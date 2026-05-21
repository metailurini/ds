#include "bash_internal.h"

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
        ds_diag_error(diag, expr->span, "invalid string literal for Bash emission");
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

static bool emit_formatted_interpolation(BashEmitter *e, DsStr name, const char *field, size_t field_len, const char *spec, size_t spec_len, EmitBuf *out) {
    if (spec_len == 0) return emit_interpolation_var(e, name, field, field_len, out);
    if (spec_len == 5 && memcmp(spec, "upper", 5) == 0) {
        buf_append(out, "$(__ds_string_upper "); emit_interpolation_var_quoted(e, name, field, field_len, out); buf_append(out, ")"); return true;
    }
    if (spec_len == 5 && memcmp(spec, "lower", 5) == 0) {
        buf_append(out, "$(__ds_string_lower "); emit_interpolation_var_quoted(e, name, field, field_len, out); buf_append(out, ")"); return true;
    }
    if (spec_len == 4 && memcmp(spec, "trim", 4) == 0) {
        buf_append(out, "$(__ds_string_trim "); emit_interpolation_var_quoted(e, name, field, field_len, out); buf_append(out, ")"); return true;
    }
    if (spec[0] == '<' || spec[0] == '>' || spec[0] == '^') {
        if (spec[0] == '^') {
            buf_append(out, "$(__ds_format_center ");
            buf_append_len(out, spec + 1, spec_len - 1);
            buf_append(out, " ");
            emit_interpolation_var_quoted(e, name, field, field_len, out);
            buf_append(out, ")");
            return true;
        }
        buf_append(out, "$(printf '");
        if (spec[0] == '<') { buf_append(out, "%-"); buf_append_len(out, spec + 1, spec_len - 1); buf_append(out, "s' "); }
        else { buf_append(out, "%"); buf_append_len(out, spec + 1, spec_len - 1); buf_append(out, "s' "); }
        emit_interpolation_var_quoted(e, name, field, field_len, out);
        buf_append(out, ")");
        return true;
    }
    buf_append(out, "$(printf '");
    buf_append(out, "%"); buf_append_len(out, spec, spec_len); buf_append(out, "' ");
    emit_interpolation_var_quoted(e, name, field, field_len, out);
    buf_append(out, ")");
    return true;
}

bool emit_interpolated_string(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    char *decoded = NULL;
    size_t len = 0;
    if (!decode_string_literal(e->diag, expr, &decoded, &len)) return false;

    buf_append(out, "\"");
    for (size_t i = 0; i < len; i++) {
        char c = decoded[i];
        if (c == '{') {
            size_t start = i + 1;
            size_t j = start;
            if (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || decoded[j] == '_')) {
                j++;
                while (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || (decoded[j] >= '0' && decoded[j] <= '9') || decoded[j] == '_')) j++;
                if (j < len && (decoded[j] == '}' || decoded[j] == '.' || decoded[j] == ':')) {
                    DsStr name = {decoded + start, j - start};
                    if (!symbol_exists(&e->symbols, name)) {
                        ds_diag_error(e->diag, expr->span, "unknown interpolation variable `%.*s`", (int)name.len, name.data);
                        free(decoded);
                        return false;
                    }
                    const char *field = NULL; size_t field_len = 0;
                    if (decoded[j] == '.') {
                        size_t field_start = ++j;
                        if (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || decoded[j] == '_')) {
                            j++;
                            while (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || (decoded[j] >= '0' && decoded[j] <= '9') || decoded[j] == '_')) j++;
                        }
                        field = decoded + field_start; field_len = j - field_start;
                    }
                    const char *spec = NULL; size_t spec_len = 0;
                    if (j < len && decoded[j] == ':') {
                        size_t spec_start = ++j;
                        while (j < len && decoded[j] != '}') j++;
                        spec = decoded + spec_start; spec_len = j - spec_start;
                    }
                    if (j >= len || decoded[j] != '}') {
                        ds_diag_error(e->diag, expr->span, "unsupported string interpolation; expected `{name}` or `{name.field}`");
                        free(decoded);
                        return false;
                    }
                    if (!emit_formatted_interpolation(e, name, field, field_len, spec, spec_len, out)) { free(decoded); return false; }
                    i = j;
                    continue;
                }
            }
            ds_diag_error(e->diag, expr->span, "unsupported string interpolation; expected `{name}` or `{name.field}`");
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
