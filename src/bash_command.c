#include "bash_internal.h"

bool emit_command_word(BashEmitter *e, DsWord command_word, EmitBuf *out) {
    DsStr word = command_word.text;
    DsSpan span = command_word.span;
    if (word.len >= 2 && word.data[0] == '"' && word.data[word.len - 1] == '"') {
        DsLowerExpr fake = {.kind = DS_LOWER_EXPR_STRING, .span = span};
        fake.as.text = word;
        return emit_interpolated_string(e, &fake, out);
    }
    if (word.len >= 2 && word.data[0] == '$') {
        DsStr name = {word.data + 1, word.len - 1};
        if (!symbol_exists(&e->symbols, name)) {
            ds_diag_error(e->diag, span, "unknown command variable `%.*s`", (int)name.len, name.data);
            return false;
        }
        buf_append(out, "\"$");
        emit_var_name(out, name);
        buf_append(out, "\"");
        return true;
    }
    for (size_t i = 1; i + 1 < word.len; i++) {
        if (word.data[i] == '.') {
            DsStr name = {word.data, i};
            DsStr field = {word.data + i + 1, word.len - i - 1};
            if (!symbol_exists(&e->symbols, name)) {
                ds_diag_error(e->diag, span, "unknown command variable `%.*s`", (int)name.len, name.data);
                return false;
            }
            buf_append(out, "\"$");
            emit_var_name(out, name);
            buf_append(out, "_");
            buf_append_len(out, field.data, field.len);
            buf_append(out, "\"");
            return true;
        }
    }
    buf_append_len(out, word.data, word.len);
    return true;
}

bool emit_redirect(BashEmitter *e, const DsRedirect *redirect, EmitBuf *out, DsSpan span) {
    if (redirect->kind == DS_REDIRECT_NONE) return true;
    DsLowerExpr fake = {.kind = DS_LOWER_EXPR_STRING, .span = redirect->target_span};
    fake.as.text = redirect->target;
    switch (redirect->kind) {
        case DS_REDIRECT_OUT: buf_append(out, " > "); break;
        case DS_REDIRECT_OUT_APPEND: buf_append(out, " >> "); break;
        case DS_REDIRECT_ERR: buf_append(out, " 2> "); break;
        case DS_REDIRECT_ERR_APPEND: buf_append(out, " 2>> "); break;
        case DS_REDIRECT_ALL: buf_append(out, " > "); break;
        case DS_REDIRECT_ALL_APPEND: buf_append(out, " >> "); break;
        case DS_REDIRECT_NONE: break;
    }
    if (!emit_interpolated_string(e, &fake, out)) return false;
    if (redirect->kind == DS_REDIRECT_ALL || redirect->kind == DS_REDIRECT_ALL_APPEND) buf_append(out, " 2>&1");
    (void)span;
    return true;
}

static const char *trace_redirect_op(DsRedirectKind kind) {
    switch (kind) {
        case DS_REDIRECT_OUT: return ">";
        case DS_REDIRECT_OUT_APPEND: return ">>";
        case DS_REDIRECT_ERR: return "2>";
        case DS_REDIRECT_ERR_APPEND: return "2>>";
        case DS_REDIRECT_ALL: return "&>";
        case DS_REDIRECT_ALL_APPEND: return "&>>";
        case DS_REDIRECT_NONE: return NULL;
    }
    return NULL;
}

bool emit_trace_redirect_args(BashEmitter *e, const DsRedirect *redirect, EmitBuf *out) {
    const char *op = trace_redirect_op(redirect->kind);
    if (!op) return true;
    DsLowerExpr fake = {.kind = DS_LOWER_EXPR_STRING, .span = redirect->target_span};
    fake.as.text = redirect->target;
    buf_append(out, " ");
    buf_append(out, "\"");
    buf_append(out, op);
    buf_append(out, "\"");
    buf_append(out, " ");
    return emit_interpolated_string(e, &fake, out);
}

bool emit_capture_words(BashEmitter *e, const DsWordVec *words, EmitBuf *out, DsSpan span) {
    buf_append(out, " ");
    emit_source_loc(out, e->source, span);
    for (size_t i = 0; i < words->len; i++) {
        buf_append(out, " ");
        if (!emit_command_word(e, words->items[i], out)) return false;
    }
    (void)span;
    return true;
}
