#include "bash_internal.h"
#include "ds_interpolation.h"

void buf_append(EmitBuf *buf, const char *text) {
    ds_string_append_cstr(buf, text);
}

const char *emit_buf_data(const EmitBuf *buf) {
    return ds_string_data(buf);
}

void buf_append_dsstr(EmitBuf *buf, DsStr value) {
    ds_string_append_range(buf, ds_str_data(value), value.len);
}

void emit_bash_decl_prefix(EmitBuf *out, int function_depth, const char *decl_flags) {
    bool has_flags = decl_flags && decl_flags[0];
    if (function_depth > 0) buf_append(out, "local");
    else if (has_flags) buf_append(out, "declare");
    else return;
    if (has_flags) {
        buf_append(out, " ");
        buf_append(out, decl_flags);
    }
    buf_append(out, " ");
}

bool bash_invariant_fail(BashEmitter *e, DsSpan span, const char *message) {
    ds_diag_error(e->diag, span, "internal Bash invariant failed: %s", message);
    return false;
}

bool symbol_exists(const SymbolVec *symbols, DsStr name) {
    for (size_t i = 0; i < symbols->len; i++) {
        DsStr existing = symbols->items[i];
        if (ds_str_eq(existing, name)) return true;
    }
    return false;
}

void bash_register_symbol(BashEmitter *e, DsStr name) {
    DS_VEC_PUSH(&e->symbols, ds_str_clone(name), 16);
}

void free_symbols(SymbolVec *symbols) {
    ds_free_str_array(symbols->items, symbols->len);
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
    buf_append_dsstr(out, name);
}

void emit_fn_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_fn_");
    buf_append_dsstr(out, name);
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
        else ds_string_append_range(out, &data[i], 1);
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
    DsStr decoded = {0};
    if (!ds_decode_string_text(expr->as.text, &decoded)) {
        ds_diag_error(diag, expr->span, "internal Bash invariant failed: non-string literal reached Bash string decoding after lowering");
        return false;
    }
    *out_data = decoded.data;
    *out_len = decoded.len;
    return true;
}
