#include "lower_internal.h"

#include <regex.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>


static bool lower_expr_is_named_storage_ref(const DsLowerExpr *expr) {
    return expr && expr->kind == DS_LOWER_EXPR_IDENT;
}

static void lower_reject_temporary_collection_access(Lower *lower, DsSpan span) {
    /*
     * VM bytecode can evaluate fields/indexes from temporary structured values,
     * but standalone Bash currently has one canonical representation for those
     * values: named storage. Rejecting the temporary form here keeps the parity
     * contract owned by lowering instead of letting Bash emission become the
     * semantic validator.
     */
    ds_diag_error(lower->diag, span,
                  "collection and command-result field/index access requires a named binding for VM/Bash parity; bind the value to a variable first");
}

static bool lower_expr_is_portable_collection_index(const DsLowerExpr *expr, bool map_index) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_IDENT) return true;
    return map_index ? expr->kind == DS_LOWER_EXPR_STRING : expr->kind == DS_LOWER_EXPR_INT;
}

static void lower_reject_computed_collection_index(Lower *lower, DsSpan span) {
    /*
     * VM bytecode can evaluate computed index expressions directly. Standalone
     * Bash emission currently renders portable collection indexing only from a
     * literal index/key or a named variable. Keep that language restriction in
     * lowering so the Bash emitter does not become the semantic gatekeeper.
     */
    ds_diag_error(lower->diag, span,
                  "collection index expression must be a literal or variable for VM/Bash parity; bind the computed index to a variable first");
}

static bool int_literal_in_range(DsStr text) {
    static const char max_text[] = "9223372036854775807";
    size_t start = 0;
    while (start + 1 < text.len && text.data[start] == '0') start++;
    size_t len = text.len - start;
    if (len < sizeof(max_text) - 1) return true;
    if (len > sizeof(max_text) - 1) return false;
    return memcmp(text.data + start, max_text, len) <= 0;
}

bool collection_element_supported_in_bash(const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INT:
        case DS_LOWER_EXPR_BOOL:
        case DS_LOWER_EXPR_FIELD:
        case DS_LOWER_EXPR_INDEX:
            return true;
        default:
            return false;
    }
}

bool text_contains_recursive_glob(DsStr text) {
    if (text.len < 2) return false;
    for (size_t i = 0; i + 1 < text.len; i++) {
        if (text.data[i] == '*' && text.data[i + 1] == '*') return true;
    }
    return false;
}

void validate_glob_pattern_arg(Lower *lower, DsStr helper_name, const DsExpr *arg) {
    if (!(lower_str_eq(helper_name, "glob") || lower_str_eq(helper_name, "glob!"))) return;
    if (!arg || arg->kind != DS_EXPR_STRING) return;
    DsStr decoded = {0};
    if (lower_decode_string_text(arg->as.text, &decoded)) {
        if (text_contains_recursive_glob(decoded)) {
            ds_diag_error(lower->diag, arg->span,
                          "recursive `**` glob patterns are deferred in v0.11.0");
        }
        free(decoded.data);
    }
}

bool is_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

void lower_map_entry_vec_push(DsLowerMapEntryVec *vec, DsLowerMapEntry entry) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerMapEntry *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerMapEntry));
    }
    vec->items[vec->len++] = entry;
}

DsStr map_key_decode(const DsMapEntry *entry) {
    DsStr out = {0};
    if (entry->quoted_key) lower_decode_string_text(entry->key, &out);
    else out = str_clone(entry->key);
    return out;
}

bool map_has_duplicate_key(const DsLowerMapEntryVec *entries, DsStr key) {
    for (size_t i = 0; i < entries->len; i++) {
        if (entries->items[i].key.len == key.len && memcmp(entries->items[i].key.data, key.data, key.len) == 0) return true;
    }
    return false;
}

DsLowerExpr *expr_new(DsLowerExprKind kind, DsSpan span) {
    DsLowerExpr *expr = (DsLowerExpr *)ds_xcalloc(1, sizeof(DsLowerExpr));
    expr->kind = kind;
    expr->span = span;
    return expr;
}

bool command_result_field_kind(DsStr field, SymKind *kind_out) {
    const DsCommandResultField *desc = ds_command_result_field_lookup(field);
    if (!desc) return false;
    switch (desc->kind) {
        case DS_COMMAND_RESULT_FIELD_STRING: *kind_out = SYM_STRING; return true;
        case DS_COMMAND_RESULT_FIELD_INT: *kind_out = SYM_INT; return true;
        case DS_COMMAND_RESULT_FIELD_BOOL: *kind_out = SYM_BOOL; return true;
    }
    return false;
}

bool lower_expr_produces_command_result(const DsLowerExpr *expr) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_RUN) return true;
    return expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.return_kind == DS_LOWER_VALUE_COMMAND_RESULT;
}

bool lower_expr_is_portable_command_result_return(const DsLowerExpr *expr) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_RUN || expr->kind == DS_LOWER_EXPR_IDENT) return true;
    return expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.is_user_function &&
           expr->as.call.return_kind == DS_LOWER_VALUE_COMMAND_RESULT;
}

DsLowerValueKind lower_value_kind_from_sym(SymKind kind) {
    switch (kind) {
        case SYM_BOOL: return DS_LOWER_VALUE_BOOL;
        case SYM_INT: return DS_LOWER_VALUE_INT;
        case SYM_STRING: return DS_LOWER_VALUE_STRING;
        case SYM_COMMAND_RESULT: return DS_LOWER_VALUE_COMMAND_RESULT;
        case SYM_ARRAY: return DS_LOWER_VALUE_ARRAY;
        case SYM_MAP: return DS_LOWER_VALUE_MAP;
        case SYM_FUNCTION:
        case SYM_TOPLEVEL_PREDECLARED:
        case SYM_UNKNOWN:
            return DS_LOWER_VALUE_UNKNOWN;
    }
    return DS_LOWER_VALUE_UNKNOWN;
}

SymKind sym_kind_from_lower_value_kind(DsLowerValueKind kind) {
    switch (kind) {
        case DS_LOWER_VALUE_BOOL: return SYM_BOOL;
        case DS_LOWER_VALUE_INT: return SYM_INT;
        case DS_LOWER_VALUE_STRING: return SYM_STRING;
        case DS_LOWER_VALUE_COMMAND_RESULT: return SYM_COMMAND_RESULT;
        case DS_LOWER_VALUE_ARRAY: return SYM_ARRAY;
        case DS_LOWER_VALUE_MAP: return SYM_MAP;
        case DS_LOWER_VALUE_UNKNOWN:
            return SYM_UNKNOWN;
    }
    return SYM_UNKNOWN;
}

static bool is_scalar_sym_kind(SymKind kind) {
    return kind == SYM_STRING || kind == SYM_INT || kind == SYM_BOOL;
}

void validate_user_call_arg_kinds(Lower *lower, const DsLowerFn *fn, const DsExprVec *args, const SymKind *arg_kinds) {
    if (!fn || !args || !arg_kinds) return;
    size_t count = args->len < fn->params.len ? args->len : fn->params.len;
    for (size_t i = 0; i < count; i++) {
        if (!fn->params.items[i].has_default) continue;
        SymKind expected = sym_kind_from_lower_value_kind(fn->params.items[i].default_kind);
        SymKind actual = arg_kinds[i];
        if (!is_scalar_sym_kind(expected) || !is_scalar_sym_kind(actual) || expected == actual) continue;
        ds_diag_error(lower->diag, args->items[i]->span,
                      "function argument kind must match parameter default kind in v0.21.0; bind or convert the value to a statically compatible kind first");
    }
}

static void validate_user_function_value_call(Lower *lower, const DsLowerFn *fn, DsSpan span) {
    /*
     * Function return kind is a lowerer/HIR contract. Expression-position calls
     * consume the function metadata collected and finalized by lowering; VM and
     * Bash backends must not rediscover whether the function is value-capable.
     */
    if (!fn->has_return) {
        ds_diag_error(lower->diag, span, "function `%.*s` does not return a value", (int)fn->name.len, fn->name.data);
    } else if (!fn->all_paths_return) {
        ds_diag_error(lower->diag, span, "function `%.*s` cannot be used as a value because not all control paths return in v0.21.0", (int)fn->name.len, fn->name.data);
    } else if (fn->contains_plain_command) {
        ds_diag_error(lower->diag, span,
                      "function `%.*s` cannot be used as a value because it contains plain command statements in v0.25.0; redirect debug output away from stdout, capture command output with `run`, or call it as a statement",
                      (int)fn->name.len, fn->name.data);
    }
}

DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);
DsLowerExpr *lower_regex_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out, bool allowed_matches_rhs);

static bool ast_binary_op_is_comparison_like(const DsExpr *expr) {
    if (!expr || expr->kind != DS_EXPR_BINARY) return false;
    return lower_str_eq(expr->as.binary.op, "in") || lower_str_eq(expr->as.binary.op, "matches") ||
           lower_str_eq(expr->as.binary.op, "==") || lower_str_eq(expr->as.binary.op, "!=") ||
           lower_str_eq(expr->as.binary.op, ">") || lower_str_eq(expr->as.binary.op, ">=") ||
           lower_str_eq(expr->as.binary.op, "<") || lower_str_eq(expr->as.binary.op, "<=");
}

DsLowerExpr *lower_binary_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    SymKind left_kind = SYM_UNKNOWN;
    SymKind right_kind = SYM_UNKNOWN;
    DsLowerExpr *left = lower_expr(lower, expr->as.binary.left, &left_kind);
    DsLowerExpr *right = NULL;
    if (lower_str_eq(expr->as.binary.op, "matches") && expr->as.binary.right->kind == DS_EXPR_REGEX) right = lower_regex_expr(lower, expr->as.binary.right, &right_kind, true);
    else right = lower_expr(lower, expr->as.binary.right, &right_kind);
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_BINARY, expr->span);
    out->as.binary.left = left;
    out->as.binary.op = str_clone(expr->as.binary.op);
    out->as.binary.right = right;
    out->as.binary.left_kind = lower_value_kind_from_sym(left_kind);
    if (ast_binary_op_is_comparison_like(expr) &&
        (ast_binary_op_is_comparison_like(expr->as.binary.left) || ast_binary_op_is_comparison_like(expr->as.binary.right))) {
        ds_diag_error(lower->diag, expr->span, "ambiguous comparison chain in v0.23.0; add parentheses around `in`, `matches`, or comparison operands");
    }
    if (lower_str_eq(expr->as.binary.op, "in")) {
        if (right_kind != SYM_ARRAY && right_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->as.binary.right->span, "right operand of `in` must be an array in v0.23.0");
        SymKind element_kind = infer_array_element_kind(lower, right);
        bool empty_array_literal = right && right->kind == DS_LOWER_EXPR_ARRAY && right->as.array.elements.len == 0;
        if (right && right->kind == DS_LOWER_EXPR_ARRAY && element_kind == SYM_UNKNOWN && !empty_array_literal) {
            ds_diag_error(lower->diag, expr->as.binary.right->span, "`in` over heterogeneous or unknown-element arrays is deferred in v0.23.0");
        }
        if (right_kind == SYM_ARRAY && element_kind != SYM_UNKNOWN && !is_scalar_sym_kind(element_kind)) ds_diag_error(lower->diag, expr->span, "`in` supports only scalar arrays in v0.23.0");
        out->as.binary.right_element_kind = lower_value_kind_from_sym(element_kind);
        *kind_out = SYM_BOOL;
        return out;
    }
    if (lower_str_eq(expr->as.binary.op, "matches")) {
        if (left_kind != SYM_STRING && left_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->as.binary.left->span, "left operand of `matches` must be a string in v0.23.0");
        if (expr->as.binary.right->kind != DS_EXPR_REGEX) ds_diag_error(lower->diag, expr->as.binary.right->span, "right operand of `matches` must be a regex literal in v0.23.0");
        *kind_out = SYM_BOOL;
        return out;
    }
    if (lower_str_eq(expr->as.binary.op, "&&") || lower_str_eq(expr->as.binary.op, "||")) {
        if (left_kind == SYM_ARRAY || left_kind == SYM_MAP || left_kind == SYM_COMMAND_RESULT) {
            ds_diag_error(lower->diag, expr->as.binary.left->span, "logical operator `%.*s` requires scalar operands in v0.23.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
        }
        if (right_kind == SYM_ARRAY || right_kind == SYM_MAP || right_kind == SYM_COMMAND_RESULT) {
            ds_diag_error(lower->diag, expr->as.binary.right->span, "logical operator `%.*s` requires scalar operands in v0.23.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
        }
        *kind_out = SYM_BOOL;
        return out;
    }
    if (lower_str_eq(expr->as.binary.op, "+")) {
        if (left_kind == SYM_INT && right_kind == SYM_INT) *kind_out = SYM_INT;
        else if (left_kind == SYM_STRING && right_kind == SYM_STRING) {
            ds_diag_error(lower->diag, expr->span, "string binary `+` cannot be emitted to standalone Bash with parity in v0.17.0; use interpolation instead");
            *kind_out = SYM_STRING;
        }
        else if (left_kind == SYM_UNKNOWN || right_kind == SYM_UNKNOWN) *kind_out = SYM_UNKNOWN;
        else ds_diag_error(lower->diag, expr->span, "operator `+` supports integer operands in v0.17.0; string concatenation is deferred");
        return out;
    }
    if (lower_str_eq(expr->as.binary.op, "-") || lower_str_eq(expr->as.binary.op, "*") ||
        lower_str_eq(expr->as.binary.op, "/") || lower_str_eq(expr->as.binary.op, "%") ||
        lower_str_eq(expr->as.binary.op, "**")) {
        if (left_kind != SYM_INT && left_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->as.binary.left->span, "operator `%.*s` requires integer operands in v0.21.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
        if (right_kind != SYM_INT && right_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->as.binary.right->span, "operator `%.*s` requires integer operands in v0.21.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
        *kind_out = SYM_INT;
        return out;
    }
    if (lower_str_eq(expr->as.binary.op, "==") || lower_str_eq(expr->as.binary.op, "!=") ||
        lower_str_eq(expr->as.binary.op, ">") || lower_str_eq(expr->as.binary.op, ">=") ||
        lower_str_eq(expr->as.binary.op, "<") || lower_str_eq(expr->as.binary.op, "<=")) {
        *kind_out = SYM_BOOL;
        return out;
    }
    ds_diag_error(lower->diag, expr->span,
                  "this expression cannot be emitted as a Bash assignment in v0.2.0; unsupported operator `%.*s` in v0.3.0",
                  (int)expr->as.binary.op.len, expr->as.binary.op.data);
    *kind_out = SYM_UNKNOWN;
    return out;
}

DsLowerExpr *lower_ident_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    Symbol *sym = scope_find(lower->scope, expr->as.text);
    if (!sym) {
        if (find_function(lower->program, expr->as.text)) {
            ds_diag_error(lower->diag, expr->span, "function `%.*s` cannot be used as a variable in v0.9.0",
                          (int)expr->as.text.len, expr->as.text.data);
        } else {
            ds_diag_error(lower->diag, expr->span, "unknown variable `%.*s`", (int)expr->as.text.len, expr->as.text.data);
        }
    } else if (sym->kind == SYM_FUNCTION) {
        ds_diag_error(lower->diag, expr->span, "function `%.*s` cannot be used as a variable in v0.9.0",
                      (int)expr->as.text.len, expr->as.text.data);
    } else {
        lower_validate_handler_capture(lower, sym, expr->as.text, expr->span);
        *kind_out = sym->kind == SYM_TOPLEVEL_PREDECLARED ? SYM_UNKNOWN : sym->kind;
    }
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_IDENT, expr->span);
    out->as.text = str_clone(expr->as.text);
    return out;
}

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
        size_t j = i + 1;
        interp_skip_ws(decoded.data, decoded.len, &j);
        if (j < decoded.len && (decoded.data[j] == '(' || decoded.data[j] == '-' || (decoded.data[j] >= '0' && decoded.data[j] <= '9'))) return true;
        DsStr name = {0};
        if (!parse_interp_name(decoded.data, decoded.len, &j, &name)) continue;
        interp_skip_ws(decoded.data, decoded.len, &j);
        if (j < decoded.len && decoded.data[j] == '(') return true;
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

DsLowerExpr *lower_int_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    *kind_out = SYM_INT;
    if (!int_literal_in_range(expr->as.text)) {
        ds_diag_error(lower->diag, expr->span, "integer literal is outside the supported int range");
    }
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INT, expr->span);
    out->as.text = str_clone(expr->as.text);
    return out;
}

DsLowerExpr *lower_bool_expr(const DsExpr *expr, SymKind *kind_out) {
    *kind_out = SYM_BOOL;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_BOOL, expr->span);
    out->as.boolean = expr->as.boolean;
    return out;
}

static bool regex_literal_parts(DsStr lit, DsStr *pattern, bool *insensitive) {
    *insensitive = false;
    if (lit.len < 3 || lit.data[0] != '/') return false;
    size_t end = 0;
    for (size_t i = 1; i < lit.len; i++) {
        if (lit.data[i] == '\\') { i++; continue; }
        if (lit.data[i] == '/') { end = i; break; }
    }
    if (!end) return false;
    pattern->data = lit.data + 1;
    pattern->len = end - 1;
    if (pattern->len == 0) return false;
    if (end + 1 < lit.len) {
        if (end + 2 == lit.len && lit.data[end + 1] == 'i') *insensitive = true;
        else return false;
    }
    return true;
}

static void validate_regex_literal(Lower *lower, const DsExpr *expr) {
    DsStr pat = {0}; bool insensitive = false;
    if (!regex_literal_parts(expr->as.regex, &pat, &insensitive)) {
        ds_diag_error(lower->diag, expr->span, "invalid regex literal; v0.23.0 supports `/pattern/` and `/pattern/i`");
        return;
    }
    (void)insensitive;
    for (size_t i = 0; i < pat.len; i++) {
        char c = pat.data[i];
        if (c == '\\' && i + 1 < pat.len) {
            char n = pat.data[i + 1];
            if (n == 'p' || n == 'P' || n == 'b' || n == 'd' || n == 'D' || n == 's' || n == 'S' || n == 'w' || n == 'W' || (n >= '1' && n <= '9')) ds_diag_error(lower->diag, expr->span, "unsupported regex escape in v0.23.0");
            i++;
        } else if (c == '(' && i + 1 < pat.len && pat.data[i + 1] == '?') {
            ds_diag_error(lower->diag, expr->span, "lookaround, inline flags, and non-POSIX regex groups are deferred in v0.23.0");
        } else if ((c == '*' || c == '+' || c == '?' || c == '}') && i + 1 < pat.len && pat.data[i + 1] == '?') {
            ds_diag_error(lower->diag, expr->span, "lazy regex quantifiers are deferred in v0.23.0");
        }
    }

    /*
     * Regex acceptance is a lowerer-owned VM/Bash parity gate. The VM and Bash
     * emitter both consume accepted regex HIR; they should only see patterns
     * that have already passed the conservative v0.23 surface checks here.
     */
    char *tmp = (char *)ds_xcalloc(pat.len + 1, 1);
    memcpy(tmp, pat.data, pat.len);
    regex_t re;
    int flags = REG_EXTENDED | (insensitive ? REG_ICASE : 0);
    int err = regcomp(&re, tmp, flags);
    if (err != 0) ds_diag_error(lower->diag, expr->span, "invalid regex pattern in v0.23.0");
    else regfree(&re);
    free(tmp);
}

DsLowerExpr *lower_regex_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out, bool allowed_matches_rhs) {
    validate_regex_literal(lower, expr);
    if (!allowed_matches_rhs) ds_diag_error(lower->diag, expr->span, "regex literals are only supported as the right operand of `matches` in v0.23.0");
    *kind_out = SYM_UNKNOWN;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_REGEX, expr->span);
    out->as.regex = str_clone(expr->as.regex);
    return out;
}

DsLowerExpr *lower_run_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    if (expr->as.run.stages.len == 0) {
        ds_diag_error(lower->diag, expr->span, "expected command after `run`");
    }
    for (size_t s = 0; s < expr->as.run.stages.len; s++) {
        if (expr->as.run.stages.items[s].words.len == 0) ds_diag_error(lower->diag, expr->as.run.stages.items[s].span, "empty pipeline stage");
        for (size_t i = 0; i < expr->as.run.stages.items[s].words.len; i++) lower_validate_command_word(lower, expr->as.run.stages.items[s].words.items[i].text, expr->as.run.stages.items[s].words.items[i].span);
    }
    if (expr->as.run.redirect.kind != DS_REDIRECT_NONE) ds_diag_error(lower->diag, expr->as.run.redirect.op_span, "captured `run` commands do not support redirection");
    *kind_out = SYM_COMMAND_RESULT;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_RUN, expr->span);
    ds_command_clone(&out->as.run, &expr->as.run);
    return out;
}

DsLowerExpr *lower_map_field_expr(const DsExpr *expr, DsLowerExpr *object, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INDEX, expr->span);
    out->as.index.object = object;
    out->as.index.object_is_map = true;
    out->as.index.index = expr_new(DS_LOWER_EXPR_STRING, expr->span);
    DsString quoted;
    ds_string_init(&quoted);
    ds_string_append_char(&quoted, '"');
    ds_string_append_range(&quoted, expr->as.field.field.data, expr->as.field.field.len);
    ds_string_append_char(&quoted, '"');
    out->as.index.index->as.text.data = quoted.data;
    out->as.index.index->as.text.len = quoted.len;
    out->as.index.map_key_literal = true;
    out->as.index.map_key = str_clone(expr->as.field.field);
    *kind_out = SYM_UNKNOWN;
    return out;
}

DsLowerExpr *lower_field_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    if (expr->as.field.object && expr->as.field.object->kind == DS_EXPR_IDENT && lower_str_eq(expr->as.field.object->as.text, "env")) {
        if (!is_env_name_text(expr->as.field.field)) {
            ds_diag_error(lower->diag, expr->span, "invalid environment variable name `%.*s` in v0.27.0", (int)expr->as.field.field.len, expr->as.field.field.data);
        }
        DsLowerExpr *out = expr_new(DS_LOWER_EXPR_CALL, expr->span);
        out->as.call.name = (DsStr){ds_str_dup_range("env.get", 7), 7};
        DsLowerExpr *arg = expr_new(DS_LOWER_EXPR_STRING, expr->span);
        DsString quoted;
        ds_string_init(&quoted);
        ds_string_append_char(&quoted, '"');
        ds_string_append_range(&quoted, expr->as.field.field.data, expr->as.field.field.len);
        ds_string_append_char(&quoted, '"');
        arg->as.text = (DsStr){quoted.data, quoted.len};
        lower_expr_vec_push(&out->as.call.args, arg);
        out->as.call.return_kind = DS_LOWER_VALUE_STRING;
        *kind_out = SYM_STRING;
        return out;
    }
    SymKind object_kind = SYM_UNKNOWN;
    DsLowerExpr *object = lower_expr(lower, expr->as.field.object, &object_kind);
    SymKind field_kind = SYM_UNKNOWN;
    bool object_is_command_result = object_kind == SYM_COMMAND_RESULT || lower_expr_produces_command_result(object);
    if (object_is_command_result) {
        if (!lower_expr_is_named_storage_ref(object)) {
            lower_reject_temporary_collection_access(lower, expr->span);
        }
        if (!command_result_field_kind(expr->as.field.field, &field_kind)) {
            ds_diag_error(lower->diag, expr->span, "unknown command result field `%.*s`", (int)expr->as.field.field.len, expr->as.field.field.data);
        }
    } else if (object_kind == SYM_MAP) {
        if (!lower_expr_is_named_storage_ref(object)) {
            lower_reject_temporary_collection_access(lower, expr->span);
        }
        return lower_map_field_expr(expr, object, kind_out);
    } else {
        ds_diag_error(lower->diag, expr->span, "field access is only supported on command results and maps in v0.10.0");
    }
    *kind_out = field_kind;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_FIELD, expr->span);
    out->as.field.object = object;
    out->as.field.field = str_clone(expr->as.field.field);
    return out;
}

DsLowerExpr *lower_unary_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    SymKind right_kind = SYM_UNKNOWN;
    DsLowerExpr *right = lower_expr(lower, expr->as.unary.right, &right_kind);
    if (!lower_str_eq(expr->as.unary.op, "!")) {
        if (lower_str_eq(expr->as.unary.op, "-")) {
            if (right_kind != SYM_INT && right_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->span, "unary `-` requires an integer operand in v0.21.0");
            *kind_out = SYM_INT;
        } else {
            ds_diag_error(lower->diag, expr->span, "unsupported unary operator `%.*s` in v0.3.0", (int)expr->as.unary.op.len, expr->as.unary.op.data);
            *kind_out = SYM_UNKNOWN;
        }
    } else {
        *kind_out = SYM_BOOL;
    }
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_UNARY, expr->span);
    out->as.unary.op = str_clone(expr->as.unary.op);
    out->as.unary.right = right;
    return out;
}


static bool is_string_helper_name(DsStr name) {
    return lower_str_eq(name, "string.trim") || lower_str_eq(name, "string.upper") ||
           lower_str_eq(name, "string.lower") || lower_str_eq(name, "string.replace") ||
           lower_str_eq(name, "string.contains") || lower_str_eq(name, "string.split") ||
           lower_str_eq(name, "string.starts_with") || lower_str_eq(name, "string.ends_with");
}

static bool string_helper_arg_count_ok(DsStr name, size_t argc, size_t *expected) {
    if (lower_str_eq(name, "string.trim") || lower_str_eq(name, "string.upper") || lower_str_eq(name, "string.lower")) { *expected = 1; return argc == 1; }
    if (lower_str_eq(name, "string.replace")) { *expected = 3; return argc == 3; }
    *expected = 2;
    return argc == 2;
}

DsLowerExpr *lower_call_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_CALL, expr->span);
    out->as.call.name = str_clone(expr->as.call.name);
    SymKind *arg_kinds = expr->as.call.args.len ? (SymKind *)ds_xcalloc(expr->as.call.args.len, sizeof(SymKind)) : NULL;
    for (size_t i = 0; i < expr->as.call.args.len; i++) {
        SymKind arg_kind = SYM_UNKNOWN;
        lower_expr_vec_push(&out->as.call.args, lower_expr(lower, expr->as.call.args.items[i], &arg_kind));
        arg_kinds[i] = arg_kind;
        if (is_string_helper_name(expr->as.call.name)) {
            if (i == 0 && arg_kind != SYM_STRING) {
                ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "string method `%.*s` requires a string receiver", (int)expr->as.call.name.len, expr->as.call.name.data);
            } else if (i > 0 && arg_kind != SYM_STRING) {
                ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "string method `%.*s` expects string arguments", (int)expr->as.call.name.len, expr->as.call.name.data);
            }
        } else if (ds_stdlib_is_name(expr->as.call.name) && arg_kind != SYM_STRING && arg_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "standard-library helper `%.*s` expects string arguments in v0.11.0", (int)expr->as.call.name.len, expr->as.call.name.data);
        } else if (arg_kind != SYM_STRING && arg_kind != SYM_INT && arg_kind != SYM_BOOL && arg_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "standard-library arguments must be scalar values in v0.11.0");
        }
    }
    const DsStdlibHelper *stdlib_helper = ds_stdlib_lookup(expr->as.call.name);
    if (stdlib_helper) {
        DsLowerValueKind ret_kind = lower_stdlib_return_value_kind(stdlib_helper);
        SymKind ret = sym_kind_from_lower_value_kind(ret_kind);
        if (!ds_stdlib_arity_ok(stdlib_helper, expr->as.call.args.len)) {
            if (is_string_helper_name(expr->as.call.name)) {
                size_t expected = 0;
                string_helper_arg_count_ok(expr->as.call.name, expr->as.call.args.len, &expected);
                ds_diag_error(lower->diag, expr->span, "string method `%.*s` expects %zu arguments including receiver but got %zu", (int)expr->as.call.name.len, expr->as.call.name.data, expected, expr->as.call.args.len);
            } else if (stdlib_helper->min_arity == stdlib_helper->max_arity) ds_diag_error(lower->diag, expr->span, "helper `%.*s` expects %zu arguments but got %zu", (int)expr->as.call.name.len, expr->as.call.name.data, stdlib_helper->min_arity, expr->as.call.args.len);
            else ds_diag_error(lower->diag, expr->span, "helper `%.*s` expects %zu to %zu arguments but got %zu", (int)expr->as.call.name.len, expr->as.call.name.data, stdlib_helper->min_arity, stdlib_helper->max_arity, expr->as.call.args.len);
        } else if (stdlib_helper->statement_only) {
            ds_diag_error(lower->diag, expr->span, "helper `%.*s` is statement-only in v0.11.0", (int)expr->as.call.name.len, expr->as.call.name.data);
        }
        if (is_string_helper_name(expr->as.call.name)) {
            if ((lower_str_eq(expr->as.call.name, "string.split") || lower_str_eq(expr->as.call.name, "string.replace")) && expr->as.call.args.len > 1 && expr->as.call.args.items[1]->kind == DS_EXPR_STRING) {
                DsStr decoded = {0};
                if (lower_decode_string_text(expr->as.call.args.items[1]->as.text, &decoded)) {
                    if (decoded.len == 0) {
                        if (lower_str_eq(expr->as.call.name, "string.split")) ds_diag_error(lower->diag, expr->as.call.args.items[1]->span, "split with an empty separator is deferred in v0.19.0");
                        else ds_diag_error(lower->diag, expr->as.call.args.items[1]->span, "replace with an empty source is deferred in v0.19.0");
                    }
                    free(decoded.data);
                }
            }
        }
        if (stdlib_helper->validates_env_name && expr->as.call.args.len > 0 && expr->as.call.args.items[0]->kind == DS_EXPR_STRING) {
            DsStr decoded = {0};
            if (lower_decode_string_text(expr->as.call.args.items[0]->as.text, &decoded)) {
                if (!is_env_name_text(decoded)) ds_diag_error(lower->diag, expr->as.call.args.items[0]->span, "invalid environment variable name `%.*s` in v0.11.0", (int)decoded.len, decoded.data);
                free(decoded.data);
            }
        }
        if (expr->as.call.args.len > 0) validate_glob_pattern_arg(lower, expr->as.call.name, expr->as.call.args.items[0]);
        *kind_out = ret;
        out->as.call.return_kind = ret_kind;
        free(arg_kinds);
        return out;
    }
    DsStr ns = {0}, member = {0};
    if (split_member_name(expr->as.call.name, &ns, &member) && ds_stdlib_is_namespace(ns)) {
        ds_diag_error(lower->diag, expr->span, "unknown standard-library helper `%.*s`", (int)expr->as.call.name.len, expr->as.call.name.data);
        free(arg_kinds);
        return out;
    }
    if (split_member_name(expr->as.call.name, &ns, &member) && ns.len == 6 && memcmp(ns.data, "string", 6) == 0) {
        ds_diag_error(lower->diag, expr->span, "unknown string method `%.*s`; supported methods are trim, upper, lower, replace, contains, split, starts_with, ends_with", (int)member.len, member.data);
        free(arg_kinds);
        return out;
    }
    DsLowerFn *fn = find_function(lower->program, expr->as.call.name);
    if (fn) {
        if (expr->as.call.args.len < fn->required_count || expr->as.call.args.len > fn->params.len) {
            ds_diag_error(lower->diag, expr->span, "function `%.*s` called with wrong number of arguments", (int)fn->name.len, fn->name.data);
        }
        validate_user_call_arg_kinds(lower, fn, &expr->as.call.args, arg_kinds);
        validate_user_function_value_call(lower, fn, expr->span);
        *kind_out = sym_kind_from_lower_value_kind(fn->return_kind);
        out->as.call.is_user_function = true;
        out->as.call.return_kind = fn->return_kind;
        free(arg_kinds);
        return out;
    }
    ds_diag_error(lower->diag, expr->span, "unknown function `%.*s`", (int)expr->as.call.name.len, expr->as.call.name.data);
    free(arg_kinds);
    return out;
}

DsLowerExpr *lower_array_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_ARRAY, expr->span);
    for (size_t i = 0; i < expr->as.array.elements.len; i++) {
        SymKind elem_kind = SYM_UNKNOWN;
        DsLowerExpr *element = lower_expr(lower, expr->as.array.elements.items[i], &elem_kind);
        lower_expr_vec_push(&out->as.array.elements, element);
        if (elem_kind == SYM_ARRAY || elem_kind == SYM_MAP) {
            ds_diag_error(lower->diag, expr->as.array.elements.items[i]->span, "nested collections are deferred in v0.10.0");
        } else if (!collection_element_supported_in_bash(element)) {
            ds_diag_error(lower->diag, expr->as.array.elements.items[i]->span, "collection element expressions must be scalar Bash-emittable values in v0.10.0; bind the expression to a variable first");
        }
    }
    *kind_out = SYM_ARRAY;
    return out;
}

DsLowerExpr *lower_map_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_MAP, expr->span);
    if (expr->as.map.entries.len == 0) {
        ds_diag_error(lower->diag, expr->span, "empty map literals are deferred in v0.10.0");
    }
    for (size_t i = 0; i < expr->as.map.entries.len; i++) {
        const DsMapEntry *entry = &expr->as.map.entries.items[i];
        DsLowerMapEntry lowered;
        memset(&lowered, 0, sizeof(lowered));
        lowered.key = map_key_decode(entry);
        lowered.span = entry->span;
        if (lowered.key.len == 0) {
            ds_diag_error(lower->diag, entry->span, "empty map keys are deferred in v0.10.0 because emitted Bash cannot represent them safely");
        }
        if (map_has_duplicate_key(&out->as.map.entries, lowered.key)) {
            ds_diag_error(lower->diag, entry->span, "duplicate map key `%.*s`", (int)lowered.key.len, lowered.key.data);
        }
        SymKind value_kind = SYM_UNKNOWN;
        lowered.value = lower_expr(lower, entry->value, &value_kind);
        if (value_kind == SYM_ARRAY || value_kind == SYM_MAP) {
            ds_diag_error(lower->diag, entry->span, "nested collections are deferred in v0.10.0");
        } else if (!collection_element_supported_in_bash(lowered.value)) {
            ds_diag_error(lower->diag, entry->value->span, "collection element expressions must be scalar Bash-emittable values in v0.10.0; bind the expression to a variable first");
        }
        lower_map_entry_vec_push(&out->as.map.entries, lowered);
    }
    *kind_out = SYM_MAP;
    return out;
}

DsLowerExpr *lower_index_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    SymKind obj_kind = SYM_UNKNOWN;
    SymKind idx_kind = SYM_UNKNOWN;
    DsLowerExpr *object = lower_expr(lower, expr->as.index.object, &obj_kind);
    DsLowerExpr *index = lower_expr(lower, expr->as.index.index, &idx_kind);
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INDEX, expr->span);
    out->as.index.object = object;
    out->as.index.index = index;
    if (obj_kind == SYM_ARRAY) {
        out->as.index.object_is_array = true;
        if (!lower_expr_is_named_storage_ref(object)) {
            lower_reject_temporary_collection_access(lower, expr->span);
        }
        if (!lower_expr_is_portable_collection_index(index, false)) {
            lower_reject_computed_collection_index(lower, expr->as.index.index->span);
        }
        if (idx_kind != SYM_INT && idx_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->as.index.index->span, "array index must be an int in v0.10.0");
        if (expr->as.index.index && expr->as.index.index->kind == DS_EXPR_UNARY && lower_str_eq(expr->as.index.index->as.unary.op, "-") &&
            expr->as.index.index->as.unary.right && expr->as.index.index->as.unary.right->kind == DS_EXPR_INT) {
            ds_diag_error(lower->diag, expr->as.index.index->span, "array index must be non-negative in v0.10.0");
        }
        SymKind element_kind = infer_array_element_kind(lower, object);
        out->as.index.element_kind = lower_value_kind_from_sym(element_kind);
        *kind_out = element_kind;
    } else if (obj_kind == SYM_MAP) {
        out->as.index.object_is_map = true;
        if (!lower_expr_is_named_storage_ref(object)) {
            lower_reject_temporary_collection_access(lower, expr->span);
        }
        if (!lower_expr_is_portable_collection_index(index, true)) {
            lower_reject_computed_collection_index(lower, expr->as.index.index->span);
        }
        if (expr->as.index.index && expr->as.index.index->kind == DS_EXPR_STRING) {
            out->as.index.map_key_literal = true;
            lower_decode_string_text(expr->as.index.index->as.text, &out->as.index.map_key);
        } else if (idx_kind != SYM_STRING && idx_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, expr->as.index.index->span, "map index must be a string in v0.10.0");
        }
    } else {
        ds_diag_error(lower->diag, expr->span, "indexing requires an array or map in v0.10.0");
    }
    return out;
}

static bool helper_returns_string_array(DsStr name) {
    return lower_str_eq(name, "string.split") || lower_str_eq(name, "glob") ||
           lower_str_eq(name, "glob!") || lower_str_eq(name, "lines");
}

SymKind infer_lower_expr_kind(Lower *lower, const DsLowerExpr *expr) {
    if (!expr) return SYM_UNKNOWN;
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT: {
            Symbol *sym = scope_find(lower->scope, expr->as.text);
            return sym ? sym->kind : SYM_UNKNOWN;
        }
        case DS_LOWER_EXPR_STRING: return SYM_STRING;
        case DS_LOWER_EXPR_INTERP: return SYM_STRING;
        case DS_LOWER_EXPR_INT: return SYM_INT;
        case DS_LOWER_EXPR_BOOL: return SYM_BOOL;
        case DS_LOWER_EXPR_REGEX: return SYM_UNKNOWN;
        case DS_LOWER_EXPR_RUN: return SYM_COMMAND_RESULT;
        case DS_LOWER_EXPR_UNARY:
            return lower_str_eq(expr->as.unary.op, "-") ? SYM_INT : SYM_BOOL;
        case DS_LOWER_EXPR_BINARY:
            if (lower_str_eq(expr->as.binary.op, "+") || lower_str_eq(expr->as.binary.op, "-") ||
                lower_str_eq(expr->as.binary.op, "*") || lower_str_eq(expr->as.binary.op, "/") ||
                lower_str_eq(expr->as.binary.op, "%") || lower_str_eq(expr->as.binary.op, "**")) return SYM_INT;
            if (lower_str_eq(expr->as.binary.op, "in") || lower_str_eq(expr->as.binary.op, "matches") ||
                lower_str_eq(expr->as.binary.op, "&&") || lower_str_eq(expr->as.binary.op, "||") ||
                lower_str_eq(expr->as.binary.op, "==") || lower_str_eq(expr->as.binary.op, "!=") ||
                lower_str_eq(expr->as.binary.op, ">") || lower_str_eq(expr->as.binary.op, ">=") ||
                lower_str_eq(expr->as.binary.op, "<") || lower_str_eq(expr->as.binary.op, "<=")) return SYM_BOOL;
            return SYM_UNKNOWN;
        case DS_LOWER_EXPR_CALL: {
            (void)lower;
            return sym_kind_from_lower_value_kind(expr->as.call.return_kind);
        }
        case DS_LOWER_EXPR_ARRAY: return SYM_ARRAY;
        case DS_LOWER_EXPR_MAP: return SYM_MAP;
        case DS_LOWER_EXPR_INDEX:
            if (expr->as.index.object_is_array) return infer_array_element_kind(lower, expr->as.index.object);
            return SYM_UNKNOWN;
        case DS_LOWER_EXPR_FIELD:
            if (infer_lower_expr_kind(lower, expr->as.field.object) == SYM_COMMAND_RESULT) {
                SymKind field_kind = SYM_UNKNOWN;
                if (command_result_field_kind(expr->as.field.field, &field_kind)) return field_kind;
            }
            return SYM_UNKNOWN;
        case DS_LOWER_EXPR_ERROR: return SYM_UNKNOWN;
        case DS_LOWER_EXPR_RANGE: return SYM_UNKNOWN;
    }
    return SYM_UNKNOWN;
}

SymKind infer_array_element_kind(Lower *lower, const DsLowerExpr *expr) {
    if (!expr) return SYM_UNKNOWN;
    if (expr->kind == DS_LOWER_EXPR_IDENT) {
        Symbol *sym = scope_find(lower->scope, expr->as.text);
        return sym ? sym->element_kind : SYM_UNKNOWN;
    }
    if (expr->kind == DS_LOWER_EXPR_CALL && helper_returns_string_array(expr->as.call.name)) return SYM_STRING;
    if (expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.is_user_function) return SYM_UNKNOWN;
    if (expr->kind != DS_LOWER_EXPR_ARRAY) return SYM_UNKNOWN;

    SymKind common = SYM_UNKNOWN;
    for (size_t i = 0; i < expr->as.array.elements.len; i++) {
        SymKind elem = infer_lower_expr_kind(lower, expr->as.array.elements.items[i]);
        if (elem == SYM_UNKNOWN) return SYM_UNKNOWN;
        if (common == SYM_UNKNOWN) common = elem;
        else if (common != elem) return SYM_UNKNOWN;
    }
    return common;
}

DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    *kind_out = SYM_UNKNOWN;
    if (!expr) return expr_new(DS_LOWER_EXPR_ERROR, (DsSpan){0});
    switch (expr->kind) {
        case DS_EXPR_IDENT:
            return lower_ident_expr(lower, expr, kind_out);
        case DS_EXPR_STRING:
            return lower_string_expr(lower, expr, kind_out);
        case DS_EXPR_INT:
            return lower_int_expr(lower, expr, kind_out);
        case DS_EXPR_BOOL:
            return lower_bool_expr(expr, kind_out);
        case DS_EXPR_REGEX:
            return lower_regex_expr(lower, expr, kind_out, false);
        case DS_EXPR_RUN:
            return lower_run_expr(lower, expr, kind_out);
        case DS_EXPR_FIELD:
            return lower_field_expr(lower, expr, kind_out);
        case DS_EXPR_UNARY:
            return lower_unary_expr(lower, expr, kind_out);
        case DS_EXPR_BINARY:
            return lower_binary_expr(lower, expr, kind_out);
        case DS_EXPR_CALL:
            return lower_call_expr(lower, expr, kind_out);
        case DS_EXPR_ARRAY:
            return lower_array_expr(lower, expr, kind_out);
        case DS_EXPR_MAP:
            return lower_map_expr(lower, expr, kind_out);
        case DS_EXPR_INDEX:
            return lower_index_expr(lower, expr, kind_out);
        case DS_EXPR_RANGE: {
            ds_diag_error(lower->diag, expr->span, "range syntax is only supported as a `for` loop source in v0.23.0");
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_RANGE, expr->span);
            SymKind tmp = SYM_UNKNOWN;
            out->as.range.start = lower_expr(lower, expr->as.range.start, &tmp);
            out->as.range.end = lower_expr(lower, expr->as.range.end, &tmp);
            *kind_out = SYM_UNKNOWN;
            return out;
        }
        case DS_EXPR_ERROR:
            return expr_new(DS_LOWER_EXPR_ERROR, expr->span);
    }
    return expr_new(DS_LOWER_EXPR_ERROR, expr->span);
}
