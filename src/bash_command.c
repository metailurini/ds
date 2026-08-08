#include "bash_internal.h"
#include "ds_command_facts.h"

bool bash_command_is_control(const DsCommand *command, const char *name) {
    if (!command || ds_command_is_pipeline(command) || command->redirect.kind != DS_REDIRECT_NONE) return false;
    if (command->stages.len == 0 || command->stages.items[0].words.len == 0) return false;
    DsStr first = command->stages.items[0].words.items[0].text;
    if (name) return str_eq(first, name);
    return str_eq(first, "fail") || str_eq(first, "exit");
}

bool emit_command_word(BashEmitter *e, DsWord command_word, EmitBuf *out) {
    /*
     * Command-word validity is a lowerer-owned M3.4 contract. Diagnostics here
     * are defensive invariants for malformed HIR that somehow bypassed lowering,
     * not source-language feature gates.
     */
    DsStr word = command_word.text;
    DsSpan span = command_word.span;
    DsCommandWordForm form = ds_command_word_analyze(word);
    if (form.kind == DS_COMMAND_WORD_QUOTED) {
        DsLowerExpr fake = {.kind = DS_LOWER_EXPR_STRING, .span = span};
        fake.as.text = word;
        return emit_interpolated_string(e, &fake, out);
    }
    if (form.kind == DS_COMMAND_WORD_VARIABLE) {
        if (word.data[0] == '$' && form.name.len + 1 < word.len) {
            ds_diag_error(e->diag, span, "internal Bash command-word invariant failed: unsupported variable suffix after lowering");
            return false;
        }
        DsStr name = form.name;
        if (!symbol_exists(&e->symbols, name)) {
            ds_diag_error(e->diag, span, "internal Bash command-word invariant failed: unknown command variable `%.*s`", (int)name.len, name.data);
            return false;
        }
        buf_append(out, "\"$");
        emit_var_name(out, name);
        buf_append(out, "\"");
        return true;
    }
    if (form.kind == DS_COMMAND_WORD_FIELD) {
        DsStr name = form.name;
        DsStr field = form.field;
        if (name.len == 3 && memcmp(name.data, "env", 3) == 0) {
            buf_append(out, "\"${");
            buf_append_dsstr(out, field);
            buf_append(out, ":-}\"");
            return true;
        }
        if (!symbol_exists(&e->symbols, name)) {
            ds_diag_error(e->diag, span, "internal Bash command-word invariant failed: unknown command variable `%.*s`", (int)name.len, name.data);
            return false;
        }
        buf_append(out, "\"$");
        emit_var_name(out, name);
        buf_append(out, "_");
        buf_append_dsstr(out, field);
        buf_append(out, "\"");
        return true;
    }
    buf_append_dsstr(out, word);
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

bool emit_trace_redirect_args(BashEmitter *e, const DsRedirect *redirect, EmitBuf *out) {
    const char *op = ds_redirect_shell_op(redirect->kind);
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

static bool emit_stage_words(BashEmitter *e, const DsWordVec *words, EmitBuf *out) {
    for (size_t i = 0; i < words->len; i++) {
        if (i > 0) buf_append(out, " ");
        if (!emit_command_word(e, words->items[i], out)) return false;
    }
    return true;
}

bool emit_command_pipeline(BashEmitter *e, const DsCommand *command, EmitBuf *out, DsSpan span) {
    bool needs_group = ds_command_is_pipeline(command) && command->redirect.kind != DS_REDIRECT_NONE;
    if (needs_group) buf_append(out, "{ ");
    if (!emit_command_pipeline_stages(e, command, out)) return false;
    if (needs_group) buf_append(out, "; }");
    return emit_redirect(e, &command->redirect, out, span);
}

bool emit_command_pipeline_stages(BashEmitter *e, const DsCommand *command, EmitBuf *out) {
    for (size_t s = 0; s < command->stages.len; s++) {
        if (s > 0) buf_append(out, " | ");
        if (!emit_stage_words(e, &command->stages.items[s].words, out)) return false;
    }
    return true;
}

bool emit_capture_command(BashEmitter *e, const DsCommand *command, EmitBuf *out, DsSpan span) {
    if (ds_command_is_pipeline(command)) {
        EmitBuf cmd = {0};
        DsCommand copy;
        ds_command_clone(&copy, command);
        ds_redirect_free(&copy.redirect);
        if (!emit_command_pipeline(e, &copy, &cmd, span)) { ds_command_free(&copy); free(cmd.data); return false; }
        ds_command_free(&copy);
        buf_append(out, " ");
        emit_source_loc(out, e->source, span);
        buf_append(out, " ");
        bash_single_quote(out, cmd.data ? cmd.data : "", cmd.len);
        free(cmd.data);
        return true;
    }
    return emit_capture_words(e, &command->stages.items[0].words, out, span);
}

static void emit_result_field_name(EmitBuf *out, DsStr name, const char *field) {
    emit_var_name(out, name);
    buf_append(out, "_");
    buf_append(out, field);
}

bool bash_emit_capture_pipeline_assignment(BashEmitter *e, DsStr name, const DsCommand *command, DsSpan span, int indent) {
    size_t id = e->temp_counter++;

    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "__ds_mktemp_dir __ds_tmpdir_%zu 'failed to create command capture temp dir'\n", id);
    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "__ds_stdout_%zu=\"$__ds_tmpdir_%zu/stdout\"\n", id, id);
    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "__ds_stderr_%zu=\"$__ds_tmpdir_%zu/stderr\"\n", id, id);

    emit_indent(&e->out, indent);
    buf_append(&e->out, "set +e\n");
    emit_indent(&e->out, indent);
    buf_append(&e->out, "__ds_trace_cmd ");
    emit_source_loc(&e->out, e->source, span);
    for (size_t s = 0; s < command->stages.len; s++) {
        if (s > 0) buf_append(&e->out, " \"|\"");
        for (size_t i = 0; i < command->stages.items[s].words.len; i++) {
            buf_append(&e->out, " ");
            if (!emit_command_word(e, command->stages.items[s].words.items[i], &e->out)) return false;
        }
    }
    buf_append(&e->out, "\n");

    emit_indent(&e->out, indent);
    buf_append(&e->out, "{ if [[ -t 0 ]]; then exec </dev/null; fi; ");
    if (!emit_command_pipeline_stages(e, command, &e->out)) return false;
    buf_appendf(&e->out, " ; } >\"$__ds_stdout_%zu\" 2>\"$__ds_stderr_%zu\"\n", id, id);
    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "__ds_code_%zu=$?\n", id);
    emit_indent(&e->out, indent);
    buf_append(&e->out, "set -e\n");

    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "__ds_data_%zu=$(cat \"$__ds_stdout_%zu\"; printf x)\n", id, id);
    emit_indent(&e->out, indent);
    buf_append(&e->out, "printf -v ");
    emit_result_field_name(&e->out, name, "stdout");
    buf_append(&e->out, " '%s' ");
    buf_appendf(&e->out, "\"${__ds_data_%zu%%x}\"\n", id);

    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "__ds_data_%zu=$(cat \"$__ds_stderr_%zu\"; printf x)\n", id, id);
    emit_indent(&e->out, indent);
    buf_append(&e->out, "printf -v ");
    emit_result_field_name(&e->out, name, "stderr");
    buf_append(&e->out, " '%s' ");
    buf_appendf(&e->out, "\"${__ds_data_%zu%%x}\"\n", id);

    emit_indent(&e->out, indent);
    buf_append(&e->out, "printf -v ");
    emit_result_field_name(&e->out, name, "code");
    buf_appendf(&e->out, " '%%s' \"$__ds_code_%zu\"\n", id);
    emit_indent(&e->out, indent);
    buf_append(&e->out, "printf -v ");
    emit_result_field_name(&e->out, name, "status");
    buf_appendf(&e->out, " '%%s' \"$__ds_code_%zu\"\n", id);

    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "if [[ $__ds_code_%zu -eq 0 ]]; then\n", id);
    emit_indent(&e->out, indent + 1);
    emit_result_field_name(&e->out, name, "ok");
    buf_append(&e->out, "=true\n");
    emit_indent(&e->out, indent + 1);
    emit_result_field_name(&e->out, name, "failed");
    buf_append(&e->out, "=false\n");
    emit_indent(&e->out, indent);
    buf_append(&e->out, "else\n");
    emit_indent(&e->out, indent + 1);
    emit_result_field_name(&e->out, name, "ok");
    buf_append(&e->out, "=false\n");
    emit_indent(&e->out, indent + 1);
    emit_result_field_name(&e->out, name, "failed");
    buf_append(&e->out, "=true\n");
    emit_indent(&e->out, indent);
    buf_append(&e->out, "fi\n");

    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "__ds_temp_remove \"$__ds_tmpdir_%zu\"\n", id);
    return true;
}
