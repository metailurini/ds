#include "lower_internal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool interp_is_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool interp_is_ident_char(char c) {
    return interp_is_ident_start(c) || (c >= '0' && c <= '9');
}

static DsStr quoted_string_from_decoded(const char *data, size_t len) {
    size_t cap = len * 2 + 3;
    char *buf = (char *)ds_xcalloc(cap, 1);
    size_t n = 0;
    buf[n++] = '"';
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (n + 3 >= cap) { cap *= 2; buf = (char *)ds_xrealloc(buf, cap); }
        if (c == '\\' || c == '"') { buf[n++] = '\\'; buf[n++] = c; }
        else if (c == '\n') { buf[n++] = '\\'; buf[n++] = 'n'; }
        else if (c == '\t') { buf[n++] = '\\'; buf[n++] = 't'; }
        else buf[n++] = c;
    }
    buf[n++] = '"';
    return (DsStr){buf, n};
}

static DsExpr *temp_expr_new(DsExprKind kind, DsSpan span) {
    DsExpr *expr = (DsExpr *)ds_xcalloc(1, sizeof(DsExpr));
    expr->kind = kind;
    expr->span = span;
    return expr;
}

static void temp_expr_vec_push(DsExprVec *vec, DsExpr *expr) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 4;
        vec->items = (DsExpr **)ds_xrealloc(vec->items, vec->cap * sizeof(DsExpr *));
    }
    vec->items[vec->len++] = expr;
}

static void temp_expr_free(DsExpr *expr) {
    if (!expr) return;
    switch (expr->kind) {
        case DS_EXPR_IDENT:
        case DS_EXPR_STRING:
        case DS_EXPR_INT:
            free(expr->as.text.data);
            break;
        case DS_EXPR_REGEX:
            free(expr->as.regex.data);
            break;
        case DS_EXPR_RUN:
            ds_command_free(&expr->as.run);
            break;
        case DS_EXPR_FIELD:
            temp_expr_free(expr->as.field.object);
            free(expr->as.field.field.data);
            break;
        case DS_EXPR_UNARY:
            free(expr->as.unary.op.data);
            temp_expr_free(expr->as.unary.right);
            break;
        case DS_EXPR_BINARY:
            temp_expr_free(expr->as.binary.left);
            free(expr->as.binary.op.data);
            temp_expr_free(expr->as.binary.right);
            break;
        case DS_EXPR_CALL:
            free(expr->as.call.name.data);
            for (size_t i = 0; i < expr->as.call.args.len; i++) temp_expr_free(expr->as.call.args.items[i]);
            free(expr->as.call.args.items);
            break;
        case DS_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) temp_expr_free(expr->as.array.elements.items[i]);
            free(expr->as.array.elements.items);
            break;
        case DS_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) {
                free(expr->as.map.entries.items[i].key.data);
                temp_expr_free(expr->as.map.entries.items[i].value);
            }
            free(expr->as.map.entries.items);
            break;
        case DS_EXPR_INDEX:
            temp_expr_free(expr->as.index.object);
            temp_expr_free(expr->as.index.index);
            break;
        case DS_EXPR_RANGE:
            temp_expr_free(expr->as.range.start);
            temp_expr_free(expr->as.range.end);
            break;
        case DS_EXPR_BOOL:
        case DS_EXPR_ERROR:
            break;
    }
    free(expr);
}

static void interp_skip_ws(const char *s, size_t len, size_t *i) {
    while (*i < len && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' || s[*i] == '\r')) (*i)++;
}

static bool parse_interp_name(const char *s, size_t len, size_t *i, DsStr *out) {
    size_t start = *i;
    if (start >= len || !interp_is_ident_start(s[start])) return false;
    (*i)++;
    while (*i < len && (interp_is_ident_char(s[*i]) || s[*i] == '.')) (*i)++;
    *out = (DsStr){(char *)s + start, *i - start};
    return true;
}

static DsExpr *temp_ident_from_str(DsStr name, DsSpan span) {
    DsExpr *expr = temp_expr_new(DS_EXPR_IDENT, span);
    expr->as.text = (DsStr){ds_str_dup_range(name.data, name.len), name.len};
    return expr;
}

static DsExpr *parse_interp_expr_bp(const char *s, size_t len, size_t *i, DsSpan span, int min_bp);

static DsExpr *parse_interp_call_after_name(const char *s, size_t len, size_t *i, DsStr name, DsSpan span) {
    if (*i >= len || s[*i] != '(') return NULL;
    (*i)++;
    DsExpr *call = temp_expr_new(DS_EXPR_CALL, span);
    call->as.call.name = (DsStr){ds_str_dup_range(name.data, name.len), name.len};
    interp_skip_ws(s, len, i);
    if (*i < len && s[*i] == ')') { (*i)++; return call; }
    while (*i < len) {
        interp_skip_ws(s, len, i);
        DsExpr *arg = parse_interp_expr_bp(s, len, i, span, 0);
        if (!arg) return call;
        temp_expr_vec_push(&call->as.call.args, arg);
        interp_skip_ws(s, len, i);
        if (*i < len && s[*i] == ',') { (*i)++; continue; }
        if (*i < len && s[*i] == ')') { (*i)++; return call; }
        return call;
    }
    return call;
}

static DsExpr *parse_interp_primary(const char *s, size_t len, size_t *i, DsSpan span) {
    interp_skip_ws(s, len, i);
    if (*i >= len) return NULL;
    size_t start = *i;
    if (s[*i] == '(') {
        (*i)++;
        DsExpr *inner = parse_interp_expr_bp(s, len, i, span, 0);
        interp_skip_ws(s, len, i);
        if (*i < len && s[*i] == ')') (*i)++;
        return inner;
    }
    if (s[*i] == '"') {
        (*i)++;
        while (*i < len) {
            if (s[*i] == '\\' && *i + 1 < len) { *i += 2; continue; }
            if (s[*i] == '"') { (*i)++; break; }
            (*i)++;
        }
        DsExpr *expr = temp_expr_new(DS_EXPR_STRING, span);
        expr->as.text = (DsStr){ds_str_dup_range(s + start, *i - start), *i - start};
        return expr;
    }
    if (s[*i] == '/') {
        (*i)++;
        bool terminated = false;
        while (*i < len) {
            if (s[*i] == '\\' && *i + 1 < len) { *i += 2; continue; }
            if (s[*i] == '/') { (*i)++; terminated = true; break; }
            if (s[*i] == '\n' || s[*i] == '\r') break;
            (*i)++;
        }
        if (terminated) {
            while (*i < len && ((s[*i] >= 'A' && s[*i] <= 'Z') || (s[*i] >= 'a' && s[*i] <= 'z'))) (*i)++;
        }
        DsExpr *expr = temp_expr_new(DS_EXPR_REGEX, span);
        expr->as.regex = (DsStr){ds_str_dup_range(s + start, *i - start), *i - start};
        return expr;
    }
    if ((s[*i] == '-' && *i + 1 < len && s[*i + 1] >= '0' && s[*i + 1] <= '9') || (s[*i] >= '0' && s[*i] <= '9')) {
        bool neg = false;
        if (s[*i] == '-') { neg = true; (*i)++; start = *i; }
        while (*i < len && s[*i] >= '0' && s[*i] <= '9') (*i)++;
        DsExpr *int_expr = temp_expr_new(DS_EXPR_INT, span);
        int_expr->as.text = (DsStr){ds_str_dup_range(s + start, *i - start), *i - start};
        if (!neg) return int_expr;
        DsExpr *unary = temp_expr_new(DS_EXPR_UNARY, span);
        unary->as.unary.op = (DsStr){ds_str_dup_range("-", 1), 1};
        unary->as.unary.right = int_expr;
        return unary;
    }
    DsStr name = {0};
    if (!parse_interp_name(s, len, i, &name)) return NULL;
    if (name.len == 4 && memcmp(name.data, "true", 4) == 0) { DsExpr *expr = temp_expr_new(DS_EXPR_BOOL, span); expr->as.boolean = true; return expr; }
    if (name.len == 5 && memcmp(name.data, "false", 5) == 0) { DsExpr *expr = temp_expr_new(DS_EXPR_BOOL, span); expr->as.boolean = false; return expr; }
    interp_skip_ws(s, len, i);
    if (*i < len && s[*i] == '(') return parse_interp_call_after_name(s, len, i, name, span);
    for (size_t dot = 0; dot < name.len; dot++) {
        if (name.data[dot] == '.') {
            DsExpr *field = temp_expr_new(DS_EXPR_FIELD, span);
            field->as.field.object = temp_ident_from_str((DsStr){name.data, dot}, span);
            field->as.field.field = (DsStr){ds_str_dup_range(name.data + dot + 1, name.len - dot - 1), name.len - dot - 1};
            return field;
        }
    }
    DsExpr *expr = temp_expr_new(DS_EXPR_IDENT, span);
    expr->as.text = (DsStr){ds_str_dup_range(name.data, name.len), name.len};
    return expr;
}

static bool interp_peek_op(const char *s, size_t len, size_t i, DsStr *op, int *left_bp, int *right_bp) {
    interp_skip_ws(s, len, &i);
    if (i >= len) return false;
    if (i + 1 < len) {
        if (s[i] == '|' && s[i + 1] == '|') { *op = (DsStr){"||", 2}; *left_bp = 1; *right_bp = 2; return true; }
        if (s[i] == '&' && s[i + 1] == '&') { *op = (DsStr){"&&", 2}; *left_bp = 2; *right_bp = 3; return true; }
        if (s[i] == '*' && s[i + 1] == '*') { *op = (DsStr){"**", 2}; *left_bp = 7; *right_bp = 6; return true; }
        if (s[i] == '=' && s[i + 1] == '=') { *op = (DsStr){"==", 2}; *left_bp = 3; *right_bp = 4; return true; }
        if (s[i] == '!' && s[i + 1] == '=') { *op = (DsStr){"!=", 2}; *left_bp = 3; *right_bp = 4; return true; }
        if (s[i] == '>' && s[i + 1] == '=') { *op = (DsStr){">=", 2}; *left_bp = 3; *right_bp = 4; return true; }
        if (s[i] == '<' && s[i + 1] == '=') { *op = (DsStr){"<=", 2}; *left_bp = 3; *right_bp = 4; return true; }
    }
    if (s[i] == '*' || s[i] == '/' || s[i] == '%') { *op = (DsStr){(char *)s + i, 1}; *left_bp = 5; *right_bp = 6; return true; }
    if (s[i] == '+' || s[i] == '-') { *op = (DsStr){(char *)s + i, 1}; *left_bp = 4; *right_bp = 5; return true; }
    if (s[i] == '>' || s[i] == '<') { *op = (DsStr){(char *)s + i, 1}; *left_bp = 3; *right_bp = 4; return true; }
    if (i + 2 <= len && memcmp(s + i, "in", 2) == 0 && (i == 0 || !interp_is_ident_char(s[i - 1])) && (i + 2 == len || !interp_is_ident_char(s[i + 2]))) {
        *op = (DsStr){"in", 2}; *left_bp = 3; *right_bp = 4; return true;
    }
    if (i + 7 <= len && memcmp(s + i, "matches", 7) == 0 && (i == 0 || !interp_is_ident_char(s[i - 1])) && (i + 7 == len || !interp_is_ident_char(s[i + 7]))) {
        *op = (DsStr){"matches", 7}; *left_bp = 3; *right_bp = 4; return true;
    }
    return false;
}

static DsExpr *parse_interp_expr_bp(const char *s, size_t len, size_t *i, DsSpan span, int min_bp) {
    interp_skip_ws(s, len, i);
    DsExpr *left = NULL;
    if (*i < len && s[*i] == '-' && !(*i + 1 < len && s[*i + 1] >= '0' && s[*i + 1] <= '9')) {
        (*i)++;
        DsExpr *right = parse_interp_expr_bp(s, len, i, span, 8);
        left = temp_expr_new(DS_EXPR_UNARY, span);
        left->as.unary.op = (DsStr){ds_str_dup_range("-", 1), 1};
        left->as.unary.right = right;
    } else {
        left = parse_interp_primary(s, len, i, span);
    }
    if (!left) return NULL;
    for (;;) {
        interp_skip_ws(s, len, i);
        if (*i >= len || s[*i] != '[') break;
        (*i)++;
        DsExpr *idx = parse_interp_expr_bp(s, len, i, span, 0);
        interp_skip_ws(s, len, i);
        if (*i < len && s[*i] == ']') (*i)++;
        DsExpr *index_expr = temp_expr_new(DS_EXPR_INDEX, span);
        index_expr->as.index.object = left;
        index_expr->as.index.index = idx;
        left = index_expr;
    }
    while (*i < len) {
        size_t op_i = *i;
        DsStr op = {0};
        int left_bp = 0, right_bp = 0;
        if (!interp_peek_op(s, len, op_i, &op, &left_bp, &right_bp) || left_bp < min_bp) break;
        interp_skip_ws(s, len, &op_i);
        op_i += op.len;
        *i = op_i;
        DsExpr *right = parse_interp_expr_bp(s, len, i, span, right_bp);
        if (!right) break;
        DsExpr *bin = temp_expr_new(DS_EXPR_BINARY, span);
        bin->as.binary.left = left;
        bin->as.binary.op = (DsStr){ds_str_dup_range(op.data, op.len), op.len};
        bin->as.binary.right = right;
        left = bin;
    }
    return left;
}

static bool decoded_needs_expr_interpolation(DsStr decoded) {
    for (size_t i = 0; i < decoded.len; i++) {
        if (decoded.data[i] != '{') continue;
        if (i + 1 < decoded.len && decoded.data[i + 1] == '{') { i++; continue; }
        size_t j = i + 1;
        interp_skip_ws(decoded.data, decoded.len, &j);
        if (j < decoded.len && (decoded.data[j] == '(' || decoded.data[j] == '-' || (decoded.data[j] >= '0' && decoded.data[j] <= '9'))) return true;
        DsStr name = {0};
        if (!parse_interp_name(decoded.data, decoded.len, &j, &name)) continue;
        interp_skip_ws(decoded.data, decoded.len, &j);
        if (j < decoded.len && decoded.data[j] == '(') return true;
        if (j < decoded.len && decoded.data[j] == '[') return true;
        while (j < decoded.len && decoded.data[j] != '}') {
            if (decoded.data[j] == '+' || decoded.data[j] == '-' || decoded.data[j] == '*' || decoded.data[j] == '/' || decoded.data[j] == '%' || decoded.data[j] == '<' || decoded.data[j] == '>' || decoded.data[j] == '=') return true;
            if (j + 2 <= decoded.len && memcmp(decoded.data + j, "in", 2) == 0 && (j == 0 || !interp_is_ident_char(decoded.data[j - 1])) && (j + 2 == decoded.len || !interp_is_ident_char(decoded.data[j + 2]))) return true;
            if (j + 7 <= decoded.len && memcmp(decoded.data + j, "matches", 7) == 0 && (j == 0 || !interp_is_ident_char(decoded.data[j - 1])) && (j + 7 == decoded.len || !interp_is_ident_char(decoded.data[j + 7]))) return true;
            j++;
        }
    }
    return false;
}

static void interp_push_literal(DsLowerExpr *out, const char *data, size_t len, DsSpan span) {
    if (len == 0) return;
    DsLowerExpr *part = expr_new(DS_LOWER_EXPR_STRING, span);
    part->as.text = quoted_string_from_decoded(data, len);
    lower_expr_vec_push(&out->as.interp.parts, part);
}

static DsLowerExpr *lower_interpolated_expr(Lower *lower, const DsExpr *expr, DsStr decoded, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INTERP, expr->span);
    size_t literal_start = 0;
    for (size_t i = 0; i < decoded.len; i++) {
        if (decoded.data[i] == '{' && i + 1 < decoded.len && decoded.data[i + 1] == '{') { i++; continue; }
        if (decoded.data[i] == '}' && i + 1 < decoded.len && decoded.data[i + 1] == '}') { i++; continue; }
        if (decoded.data[i] == '}') {
            ds_diag_error(lower->diag, expr->span, "unmatched `}` in string interpolation; use `}}` for a literal `}`");
            break;
        }
        if (decoded.data[i] != '{') continue;
        size_t j = i + 1;
        interp_skip_ws(decoded.data, decoded.len, &j);
        DsExpr *inner = parse_interp_expr_bp(decoded.data, decoded.len, &j, expr->span, 0);
        interp_skip_ws(decoded.data, decoded.len, &j);
        if (inner && j < decoded.len && decoded.data[j] == '}') {
            interp_push_literal(out, decoded.data + literal_start, i - literal_start, expr->span);
            SymKind inner_kind = SYM_UNKNOWN;
            DsLowerExpr *part = lower_expr(lower, inner, &inner_kind);
            if (inner_kind != SYM_STRING && inner_kind != SYM_INT && inner_kind != SYM_BOOL && inner_kind != SYM_UNKNOWN) {
                ds_diag_error(lower->diag, expr->span, "interpolation expression must be scalar in v0.21.0");
            }
            lower_expr_vec_push(&out->as.interp.parts, part);
            temp_expr_free(inner);
            i = j;
            literal_start = j + 1;
            continue;
        }
        temp_expr_free(inner);
        ds_diag_error(lower->diag, expr->span, "unsupported string interpolation; expected `{name}`, `{name.field}`, or `{name(args...)}`");
        break;
    }
    interp_push_literal(out, decoded.data + literal_start, decoded.len - literal_start, expr->span);
    *kind_out = SYM_STRING;
    return out;
}

DsLowerExpr *lower_string_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    DsStr decoded = {0};
    if (lower_decode_string_text(expr->as.text, &decoded)) {
        if (decoded_needs_expr_interpolation(decoded)) {
            DsLowerExpr *out = lower_interpolated_expr(lower, expr, decoded, kind_out);
            free(decoded.data);
            return out;
        }
        free(decoded.data);
    }
    lower_validate_word_interpolation(lower, expr->as.text, expr->span);
    *kind_out = SYM_STRING;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_STRING, expr->span);
    out->as.text = str_clone(expr->as.text);
    return out;
}
