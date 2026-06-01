#include "bash_internal.h"
#include "ds_interpolation.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void buf_reserve(EmitBuf *buf, size_t need) {
    if (need <= buf->cap) return;
    size_t cap = buf->cap ? buf->cap : 256;
    while (cap < need) cap *= 2;
    buf->data = (char *)ds_xrealloc(buf->data, cap);
    buf->cap = cap;
}

void buf_append_len(EmitBuf *buf, const char *data, size_t len) {
    buf_reserve(buf, buf->len + len + 1);
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

void buf_append(EmitBuf *buf, const char *text) {
    buf_append_len(buf, text, strlen(text));
}

void buf_appendf(EmitBuf *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        va_end(args);
        return;
    }
    size_t start = buf->len;
    buf_reserve(buf, buf->len + (size_t)n + 1);
    vsnprintf(buf->data + start, (size_t)n + 1, fmt, args);
    va_end(args);
    buf->len += (size_t)n;
}

void symbol_vec_push(SymbolVec *vec, DsStr name) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 16;
        vec->items = (DsStr *)ds_xrealloc(vec->items, vec->cap * sizeof(DsStr));
    }
    vec->items[vec->len++] = name;
}

bool str_eq(DsStr a, const char *b) {
    size_t len = strlen(b);
    return a.len == len && memcmp(a.data, b, len) == 0;
}
bool symbol_exists(const SymbolVec *symbols, DsStr name) {
    for (size_t i = 0; i < symbols->len; i++) {
        DsStr existing = symbols->items[i];
        if (existing.len == name.len && memcmp(existing.data, name.data, name.len) == 0) return true;
    }
    return false;
}

void free_symbols(SymbolVec *symbols) {
    for (size_t i = 0; i < symbols->len; i++) free(symbols->items[i].data);
    free(symbols->items);
}

void symbols_truncate(SymbolVec *symbols, size_t len) {
    while (symbols->len > len) free(symbols->items[--symbols->len].data);
}

void emit_indent(EmitBuf *out, int indent) {
    for (int i = 0; i < indent; i++) buf_append(out, "  ");
}

bool is_safe_identifier(DsStr name) {
    if (name.len == 0) return false;
    char first = name.data[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) return false;
    for (size_t i = 1; i < name.len; i++) {
        char c = name.data[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

void emit_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_");
    buf_append_len(out, name.data, name.len);
}

void emit_fn_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_fn_");
    buf_append_len(out, name.data, name.len);
}

void emit_stdlib_helper_name(EmitBuf *out, DsStr name) {
    const DsStdlibHelper *helper = ds_stdlib_lookup(name);
    if (helper) buf_append(out, helper->bash_name);
}

bool stdlib_returns_array(DsStr name) {
    const DsStdlibHelper *helper = ds_stdlib_lookup(name);
    return helper && helper->return_kind == DS_STDLIB_RETURN_ARRAY;
}
void bash_single_quote(EmitBuf *out, const char *data, size_t len) {
    buf_append(out, "'");
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\'') buf_append(out, "'\\''");
        else buf_append_len(out, &data[i], 1);
    }
    buf_append(out, "'");
}

void emit_source_loc(EmitBuf *out, const DsSource *fallback, DsSpan span) {
    const DsSource *source = span.source ? span.source : fallback;
    const char *path = source && source->path ? source->path : "<source>";
    char buf[64];
    bash_single_quote(out, path, strlen(path));
    buf_append(out, ":");
    snprintf(buf, sizeof(buf), "%d:%d", span.start.line, span.start.column);
    buf_append(out, buf);
}
bool decode_string_literal(DsDiag *diag, const DsLowerExpr *expr, char **out_data, size_t *out_len) {
    DsStr text = expr->as.text;
    if (text.len >= 6 && memcmp(text.data, "\"\"\"", 3) == 0 && memcmp(text.data + text.len - 3, "\"\"\"", 3) == 0) {
        size_t len = text.len - 6;
        char *buf = ds_str_dup_range(text.data + 3, len);
        *out_data = buf;
        *out_len = len;
        return true;
    }
    if (text.len < 2 || text.data[0] != '"' || text.data[text.len - 1] != '"') {
        ds_diag_error(diag, expr->span, "internal Bash invariant failed: non-string literal reached Bash string decoding after lowering");
        return false;
    }

    char *buf = (char *)ds_xcalloc(text.len, 1);
    size_t len = 0;
    for (size_t i = 1; i + 1 < text.len; i++) {
        char c = text.data[i];
        if (c == '\\' && i + 1 < text.len - 1) {
            char escaped = text.data[++i];
            if (escaped == 'n') c = '\n';
            else if (escaped == 't') c = '\t';
            else if (escaped == '"') c = '"';
            else if (escaped == '\\') c = '\\';
            else c = escaped;
        }
        buf[len++] = c;
    }
    buf[len] = '\0';
    *out_data = buf;
    *out_len = len;
    return true;
}

static bool emit_interpolation_var(BashEmitter *e, DsStr name, const char *field, size_t field_len, EmitBuf *out) {
    (void)e;
    buf_append(out, "${__ds_");
    buf_append_len(out, name.data, name.len);
    if (field) { buf_append(out, "_"); buf_append_len(out, field, field_len); }
    buf_append(out, "}");
    return true;
}

static bool emit_interpolation_var_quoted(BashEmitter *e, DsStr name, const char *field, size_t field_len, EmitBuf *out) {
    buf_append(out, "\"");
    emit_interpolation_var(e, name, field, field_len, out);
    buf_append(out, "\"");
    return true;
}

static bool emit_formatted_interpolation(BashEmitter *e, DsStr name, const char *field, size_t field_len, const char *spec, size_t spec_len, DsSpan span, EmitBuf *out) {
    if (spec_len == 0) return emit_interpolation_var(e, name, field, field_len, out);
    DsInterpFormatSpec parsed;
    if (!ds_interp_parse_format_spec((DsStr){(char *)spec, spec_len}, &parsed)) {
        ds_diag_error(e->diag, span, "internal Bash interpolation invariant failed: unsupported format specifier `%.*s` after lowering", (int)spec_len, spec);
        return false;
    }
    if (parsed.kind == DS_INTERP_FORMAT_UPPER) {
        buf_append(out, "$(__ds_string_upper "); emit_interpolation_var_quoted(e, name, field, field_len, out); buf_append(out, ")"); return true;
    }
    if (parsed.kind == DS_INTERP_FORMAT_LOWER) {
        buf_append(out, "$(__ds_string_lower "); emit_interpolation_var_quoted(e, name, field, field_len, out); buf_append(out, ")"); return true;
    }
    if (parsed.kind == DS_INTERP_FORMAT_TRIM) {
        buf_append(out, "$(__ds_string_trim "); emit_interpolation_var_quoted(e, name, field, field_len, out); buf_append(out, ")"); return true;
    }
    if (parsed.kind == DS_INTERP_FORMAT_ALIGN_LEFT || parsed.kind == DS_INTERP_FORMAT_ALIGN_RIGHT || parsed.kind == DS_INTERP_FORMAT_ALIGN_CENTER) {
        char width_buf[32];
        snprintf(width_buf, sizeof(width_buf), "%d", parsed.width);
        if (parsed.kind == DS_INTERP_FORMAT_ALIGN_CENTER) {
            buf_append(out, "$(__ds_format_center ");
            buf_append(out, width_buf);
            buf_append(out, " ");
            emit_interpolation_var_quoted(e, name, field, field_len, out);
            buf_append(out, ")");
            return true;
        }
        buf_append(out, "$(printf '");
        if (parsed.kind == DS_INTERP_FORMAT_ALIGN_LEFT) { buf_append(out, "%-"); buf_append(out, width_buf); buf_append(out, "s' "); }
        else { buf_append(out, "%"); buf_append(out, width_buf); buf_append(out, "s' "); }
        emit_interpolation_var_quoted(e, name, field, field_len, out);
        buf_append(out, ")");
        return true;
    }
    buf_append(out, "$(printf '");
    buf_append(out, "%");
    if (parsed.kind == DS_INTERP_FORMAT_INT_DECIMAL) {
        if (parsed.zero_pad) buf_append(out, "0");
        buf_appendf(out, "%d", parsed.width);
        buf_append(out, "d");
    } else {
        if (parsed.width > 0) buf_appendf(out, "%d", parsed.width);
        buf_appendf(out, ".%d", parsed.precision);
        buf_append(out, "f");
    }
    buf_append(out, "' ");
    emit_interpolation_var_quoted(e, name, field, field_len, out);
    buf_append(out, ")");
    return true;
}

static void interp_skip_ws(const char *data, size_t len, size_t *i) {
    while (*i < len && (data[*i] == ' ' || data[*i] == '\t' || data[*i] == '\n' || data[*i] == '\r')) (*i)++;
}

static bool interp_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool interp_ident_char(char c) {
    return interp_ident_start(c) || (c >= '0' && c <= '9');
}

static bool emit_interpolation_index(BashEmitter *e, const char *decoded, size_t len, size_t *j, DsStr name, DsSpan span, EmitBuf *out) {
    (*j)++;
    interp_skip_ws(decoded, len, j);
    buf_append(out, "$(");
    if (*j < len && decoded[*j] == '"') {
        size_t literal_start = *j;
        (*j)++;
        while (*j < len) {
            if (decoded[*j] == '\\' && *j + 1 < len) { *j += 2; continue; }
            if (decoded[*j] == '"') { (*j)++; break; }
            (*j)++;
        }
        DsLowerExpr tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.kind = DS_LOWER_EXPR_STRING;
        tmp.span = span;
        tmp.as.text = (DsStr){(char *)decoded + literal_start, *j - literal_start};
        char *key = NULL;
        size_t key_len = 0;
        if (!decode_string_literal(e->diag, &tmp, &key, &key_len)) return false;
        buf_append(out, "__ds_map_get ");
        emit_var_name(out, name);
        buf_append(out, " ");
        bash_single_quote(out, key, key_len);
        free(key);
    } else if (*j < len && ((decoded[*j] == '-' && *j + 1 < len && decoded[*j + 1] >= '0' && decoded[*j + 1] <= '9') ||
                            (decoded[*j] >= '0' && decoded[*j] <= '9'))) {
        size_t index_start = *j;
        if (decoded[*j] == '-') (*j)++;
        while (*j < len && decoded[*j] >= '0' && decoded[*j] <= '9') (*j)++;
        buf_append(out, "__ds_array_get ");
        emit_var_name(out, name);
        buf_append(out, " ");
        buf_append_len(out, decoded + index_start, *j - index_start);
    } else if (*j < len && interp_ident_start(decoded[*j])) {
        size_t index_start = *j;
        (*j)++;
        while (*j < len && interp_ident_char(decoded[*j])) (*j)++;
        DsStr index_name = {(char *)decoded + index_start, *j - index_start};
        if (!symbol_exists(&e->symbols, index_name)) {
            ds_diag_error(e->diag, span, "internal Bash interpolation invariant failed: unknown interpolation index variable `%.*s`", (int)index_name.len, index_name.data);
            return false;
        }
        buf_append(out, "__ds_index_get ");
        emit_var_name(out, name);
        buf_append(out, " \"$");
        emit_var_name(out, index_name);
        buf_append(out, "\"");
    } else {
        ds_diag_error(e->diag, span, "internal Bash interpolation invariant failed: unsupported index interpolation shape");
        return false;
    }
    interp_skip_ws(decoded, len, j);
    if (*j >= len || decoded[*j] != ']') {
        ds_diag_error(e->diag, span, "internal Bash interpolation invariant failed: unsupported index interpolation shape");
        return false;
    }
    (*j)++;
    buf_append(out, ")");
    return true;
}

typedef struct {
    BashEmitter *e;
    const char *data;
    size_t len;
    size_t pos;
    DsSpan span;
} BashArithParser;

static void bash_arith_skip(BashArithParser *p) {
    while (p->pos < p->len && (p->data[p->pos] == ' ' || p->data[p->pos] == '\t')) p->pos++;
}

static bool bash_arith_parse_expr(BashArithParser *p, EmitBuf *out);

static bool bash_arith_parse_primary(BashArithParser *p, EmitBuf *out) {
    bash_arith_skip(p);
    if (p->pos >= p->len) return false;
    char c = p->data[p->pos];
    if (c == '(') {
        p->pos++;
        if (!bash_arith_parse_expr(p, out)) return false;
        bash_arith_skip(p);
        if (p->pos >= p->len || p->data[p->pos] != ')') return false;
        p->pos++;
        return true;
    }
    if (c == '-') {
        p->pos++;
        EmitBuf inner = {0};
        if (!bash_arith_parse_primary(p, &inner)) { free(inner.data); return false; }
        buf_append(out, "$(__ds_int_neg ");
        buf_append(out, inner.data ? inner.data : "");
        buf_append(out, ")");
        free(inner.data);
        return true;
    }
    if (c >= '0' && c <= '9') {
        size_t start = p->pos++;
        while (p->pos < p->len && p->data[p->pos] >= '0' && p->data[p->pos] <= '9') p->pos++;
        buf_append_len(out, p->data + start, p->pos - start);
        return true;
    }
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
        size_t start = p->pos++;
        while (p->pos < p->len && ((p->data[p->pos] >= 'A' && p->data[p->pos] <= 'Z') || (p->data[p->pos] >= 'a' && p->data[p->pos] <= 'z') || (p->data[p->pos] >= '0' && p->data[p->pos] <= '9') || p->data[p->pos] == '_')) p->pos++;
        DsStr name = {(char *)p->data + start, p->pos - start};
        if (!symbol_exists(&p->e->symbols, name)) {
            ds_diag_error(p->e->diag, p->span, "internal Bash interpolation invariant failed: unknown interpolation variable `%.*s`", (int)name.len, name.data);
            return false;
        }
        buf_append(out, "\"$");
        emit_var_name(out, name);
        buf_append(out, "\"");
        return true;
    }
    return false;
}

static bool bash_arith_parse_power(BashArithParser *p, EmitBuf *out) {
    EmitBuf left = {0};
    if (!bash_arith_parse_primary(p, &left)) return false;
    bash_arith_skip(p);
    if (p->pos + 1 < p->len && p->data[p->pos] == '*' && p->data[p->pos + 1] == '*') {
        p->pos += 2;
        EmitBuf right = {0};
        if (!bash_arith_parse_power(p, &right)) { free(left.data); free(right.data); return false; }
        buf_append(out, "$(__ds_int_bin '**' ");
        buf_append(out, left.data ? left.data : "");
        buf_append(out, " ");
        buf_append(out, right.data ? right.data : "");
        buf_append(out, ")");
        free(left.data); free(right.data);
        return true;
    }
    buf_append(out, left.data ? left.data : "");
    free(left.data);
    return true;
}

static bool bash_arith_parse_mul(BashArithParser *p, EmitBuf *out) {
    EmitBuf acc = {0};
    if (!bash_arith_parse_power(p, &acc)) return false;
    for (;;) {
        bash_arith_skip(p);
        if (p->pos >= p->len) break;
        char op = p->data[p->pos];
        if (!(op == '*' || op == '/' || op == '%')) break;
        if (op == '*' && p->pos + 1 < p->len && p->data[p->pos + 1] == '*') break;
        p->pos++;
        EmitBuf right = {0};
        if (!bash_arith_parse_power(p, &right)) { free(acc.data); free(right.data); return false; }
        EmitBuf next = {0};
        char op_text[2] = {op, 0};
        buf_append(&next, "$(__ds_int_bin ");
        bash_single_quote(&next, op_text, 1);
        buf_append(&next, " ");
        buf_append(&next, acc.data ? acc.data : "");
        buf_append(&next, " ");
        buf_append(&next, right.data ? right.data : "");
        buf_append(&next, ")");
        free(acc.data); free(right.data); acc = next;
    }
    buf_append(out, acc.data ? acc.data : "");
    free(acc.data);
    return true;
}

static bool bash_arith_parse_expr(BashArithParser *p, EmitBuf *out) {
    EmitBuf acc = {0};
    if (!bash_arith_parse_mul(p, &acc)) return false;
    for (;;) {
        bash_arith_skip(p);
        if (p->pos >= p->len) break;
        char op = p->data[p->pos];
        if (!(op == '+' || op == '-')) break;
        p->pos++;
        EmitBuf right = {0};
        if (!bash_arith_parse_mul(p, &right)) { free(acc.data); free(right.data); return false; }
        EmitBuf next = {0};
        char op_text[2] = {op, 0};
        buf_append(&next, "$(__ds_int_bin ");
        bash_single_quote(&next, op_text, 1);
        buf_append(&next, " ");
        buf_append(&next, acc.data ? acc.data : "");
        buf_append(&next, " ");
        buf_append(&next, right.data ? right.data : "");
        buf_append(&next, ")");
        free(acc.data); free(right.data); acc = next;
    }
    buf_append(out, acc.data ? acc.data : "");
    free(acc.data);
    return true;
}

static bool emit_arithmetic_interpolation(BashEmitter *e, const char *data, size_t len, DsSpan span, EmitBuf *out) {
    BashArithParser p = {.e = e, .data = data, .len = len, .span = span};
    EmitBuf expr = {0};
    if (!bash_arith_parse_expr(&p, &expr)) { free(expr.data); return false; }
    bash_arith_skip(&p);
    if (p.pos != len) { free(expr.data); return false; }
    buf_append(out, expr.data ? expr.data : "");
    free(expr.data);
    return true;
}

bool emit_interpolated_string(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    /*
     * Lowering owns interpolation acceptance. The Bash emitter renders accepted
     * string/command-word interpolation and keeps these diagnostics only as
     * defensive invariants for malformed HIR or stale metadata.
     */
    char *decoded = NULL;
    size_t len = 0;
    if (!decode_string_literal(e->diag, expr, &decoded, &len)) return false;

    buf_append(out, "\"");
    for (size_t i = 0; i < len; i++) {
        char c = decoded[i];
        if (c == '{') {
            if (i + 1 < len && decoded[i + 1] == '{') {
                buf_append(out, "{");
                i++;
                continue;
            }
            size_t start = i + 1;
            size_t j = start;
            if (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || decoded[j] == '_')) {
                j++;
                while (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || (decoded[j] >= '0' && decoded[j] <= '9') || decoded[j] == '_')) j++;
                if (j < len && (decoded[j] == '}' || decoded[j] == '.' || decoded[j] == ':' || decoded[j] == '[')) {
                    DsStr name = {decoded + start, j - start};
                    if (name.len == 3 && memcmp(name.data, "env", 3) == 0 && decoded[j] == '.') {
                        size_t field_start = ++j;
                        if (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || decoded[j] == '_')) {
                            j++;
                            while (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || (decoded[j] >= '0' && decoded[j] <= '9') || decoded[j] == '_')) j++;
                        }
                        DsStr field = {decoded + field_start, j - field_start};
                        if (j >= len || decoded[j] != '}') {
                            ds_diag_error(e->diag, expr->span, "internal Bash interpolation invariant failed: unsupported string interpolation shape");
                            free(decoded);
                            return false;
                        }
                        buf_append(out, "${");
                        buf_append_len(out, field.data, field.len);
                        buf_append(out, ":-}");
                        i = j;
                        continue;
                    }
                    if (!symbol_exists(&e->symbols, name)) {
                        ds_diag_error(e->diag, expr->span, "internal Bash interpolation invariant failed: unknown interpolation variable `%.*s`", (int)name.len, name.data);
                        free(decoded);
                        return false;
                    }
                    const char *field = NULL; size_t field_len = 0;
                    bool indexed_interp = false;
                    if (decoded[j] == '[') {
                        if (!emit_interpolation_index(e, decoded, len, &j, name, expr->span, out)) { free(decoded); return false; }
                        indexed_interp = true;
                    } else if (decoded[j] == '.') {
                        size_t field_start = ++j;
                        if (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || decoded[j] == '_')) {
                            j++;
                            while (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || (decoded[j] >= '0' && decoded[j] <= '9') || decoded[j] == '_')) j++;
                        }
                        field = decoded + field_start; field_len = j - field_start;
                    }
                    const char *spec = NULL; size_t spec_len = 0;
                    if (j < len && decoded[j] == ':') {
                        if (indexed_interp) {
                            ds_diag_error(e->diag, expr->span, "internal Bash interpolation invariant failed: index interpolation format specifier should be rejected by lowering");
                            free(decoded);
                            return false;
                        }
                        size_t spec_start = ++j;
                        while (j < len && decoded[j] != '}') j++;
                        spec = decoded + spec_start; spec_len = j - spec_start;
                    }
                    if (j >= len || decoded[j] != '}') {
                        ds_diag_error(e->diag, expr->span, "internal Bash interpolation invariant failed: unsupported string interpolation shape");
                        free(decoded);
                        return false;
                    }
                    if (!indexed_interp && !emit_formatted_interpolation(e, name, field, field_len, spec, spec_len, expr->span, out)) { free(decoded); return false; }
                    i = j;
                    continue;
                }
            }
            size_t arith_end = start;
            while (arith_end < len && decoded[arith_end] != '}') arith_end++;
            if (arith_end < len && emit_arithmetic_interpolation(e, decoded + start, arith_end - start, expr->span, out)) {
                i = arith_end;
                continue;
            }
            ds_diag_error(e->diag, expr->span, "internal Bash interpolation invariant failed: unsupported string interpolation shape");
            free(decoded);
            return false;
        }
        if (c == '}') {
            if (i + 1 < len && decoded[i + 1] == '}') {
                buf_append(out, "}");
                i++;
                continue;
            }
            ds_diag_error(e->diag, expr->span, "internal Bash interpolation invariant failed: unmatched literal close brace after lowering");
            free(decoded);
            return false;
        }
        if (c == '"' || c == '\\' || c == '$' || c == '`') buf_append(out, "\\");
        buf_append_len(out, &c, 1);
    }
    buf_append(out, "\"");
    free(decoded);
    return true;
}
