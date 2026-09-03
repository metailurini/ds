#include "bash_internal.h"
#include "ds_command_facts.h"

bool bash_command_is_control(const DsLowerCommand *command, const char *name) {
    if (!command || ds_lower_command_is_pipeline(command) || command->redirect.kind != DS_REDIRECT_NONE) return false;
    if (command->stages.len == 0 || command->stages.items[0].words.len == 0) return false;
    const DsLowerCommandWord *first_word = &command->stages.items[0].words.items[0];
    if (first_word->kind != DS_LOWER_COMMAND_WORD_LITERAL) return false;
    DsStr first = first_word->source_text;
    if (name) return ds_str_eq_cstr(first, name);
    return ds_str_eq_cstr(first, "fail") || ds_str_eq_cstr(first, "exit");
}

static void emit_command_literal_quoted(DsStr literal, EmitBuf *out) {
    buf_append(out, "\"");
    for (size_t i = 0; i < literal.len; i++) {
        char c = literal.data[i];
        if (c == '"' || c == '\\' || c == '$' || c == '`') buf_append(out, "\\");
        ds_string_append_range(out, &c, 1);
    }
    buf_append(out, "\"");
}

bool emit_command_word(BashEmitter *e, const DsLowerCommandWord *command_word, EmitBuf *out) {
    if (!command_word) return bash_invariant_fail(e, (DsSpan){0}, "command word should exist after lowering");
    if (command_word->kind == DS_LOWER_COMMAND_WORD_LITERAL) {
        if (command_word->source_text.len > 0 && command_word->source_text.data[0] == '"') {
            emit_command_literal_quoted(command_word->literal_text, out);
        } else {
            buf_append_dsstr(out, command_word->source_text);
        }
        return true;
    }
    if (!command_word->value) {
        return bash_invariant_fail(e, command_word->span, "command value word should have a lowered expression");
    }
    return emit_value_expr(e, command_word->value, out);
}

bool emit_redirect(BashEmitter *e, const DsLowerRedirect *redirect, EmitBuf *out, DsSpan span) {
    if (redirect->kind == DS_REDIRECT_NONE) return true;
    switch (redirect->kind) {
        case DS_REDIRECT_OUT: buf_append(out, " > "); break;
        case DS_REDIRECT_OUT_APPEND: buf_append(out, " >> "); break;
        case DS_REDIRECT_ERR: buf_append(out, " 2> "); break;
        case DS_REDIRECT_ERR_APPEND: buf_append(out, " 2>> "); break;
        case DS_REDIRECT_ALL: buf_append(out, " > "); break;
        case DS_REDIRECT_ALL_APPEND: buf_append(out, " >> "); break;
        case DS_REDIRECT_NONE: break;
    }
    if (redirect->target) {
        if (!emit_value_expr(e, redirect->target, out)) return false;
    } else {
        emit_command_literal_quoted(redirect->literal_target, out);
    }
    if (redirect->kind == DS_REDIRECT_ALL || redirect->kind == DS_REDIRECT_ALL_APPEND) buf_append(out, " 2>&1");
    (void)span;
    return true;
}

bool emit_trace_redirect_args(BashEmitter *e, const DsLowerRedirect *redirect, EmitBuf *out) {
    const char *op = ds_redirect_shell_op(redirect->kind);
    if (!op) return true;
    buf_append(out, " ");
    buf_append(out, "\"");
    buf_append(out, op);
    buf_append(out, "\"");
    buf_append(out, " ");
    if (redirect->target) return emit_value_expr(e, redirect->target, out);
    emit_command_literal_quoted(redirect->literal_target, out);
    return true;
}

bool emit_capture_words(BashEmitter *e, const DsLowerCommandWordVec *words, EmitBuf *out, DsSpan span) {
    buf_append(out, " ");
    emit_source_loc(out, e->source, span);
    for (size_t i = 0; i < words->len; i++) {
        buf_append(out, " ");
        if (!emit_command_word(e, &words->items[i], out)) return false;
    }
    return true;
}

static bool emit_stage_words(BashEmitter *e, const DsLowerCommandWordVec *words, EmitBuf *out) {
    for (size_t i = 0; i < words->len; i++) {
        if (i > 0) buf_append(out, " ");
        if (!emit_command_word(e, &words->items[i], out)) return false;
    }
    return true;
}

bool emit_command_pipeline(BashEmitter *e, const DsLowerCommand *command, EmitBuf *out, DsSpan span) {
    bool needs_group = ds_lower_command_is_pipeline(command) && command->redirect.kind != DS_REDIRECT_NONE;
    if (needs_group) buf_append(out, "{ ");
    if (!emit_command_pipeline_stages(e, command, out)) return false;
    if (needs_group) buf_append(out, "; }");
    return emit_redirect(e, &command->redirect, out, span);
}

bool emit_command_pipeline_stages(BashEmitter *e, const DsLowerCommand *command, EmitBuf *out) {
    for (size_t s = 0; s < command->stages.len; s++) {
        if (s > 0) buf_append(out, " | ");
        if (!emit_stage_words(e, &command->stages.items[s].words, out)) return false;
    }
    return true;
}

bool emit_capture_command(BashEmitter *e, const DsLowerCommand *command, EmitBuf *out, DsSpan span) {
    if (ds_lower_command_is_pipeline(command)) {
        EmitBuf cmd = {0};
        if (!emit_command_pipeline_stages(e, command, &cmd)) { free(cmd.data); return false; }
        buf_append(out, " ");
        emit_source_loc(out, e->source, span);
        buf_append(out, " ");
        bash_single_quote(out, emit_buf_data(&cmd), cmd.len);
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

bool bash_emit_capture_pipeline_assignment(BashEmitter *e, DsStr name, const DsLowerCommand *command, DsSpan span, int indent) {
    size_t id = e->temp_counter++;

    emit_indent(&e->out, indent);
    ds_string_appendf(&e->out, "__ds_mktemp_dir __ds_tmpdir_%zu 'failed to create command capture temp dir'\n", id);
    emit_indent(&e->out, indent);
    ds_string_appendf(&e->out, "__ds_stdout_%zu=\"$__ds_tmpdir_%zu/stdout\"\n", id, id);
    emit_indent(&e->out, indent);
    ds_string_appendf(&e->out, "__ds_stderr_%zu=\"$__ds_tmpdir_%zu/stderr\"\n", id, id);

    emit_indent(&e->out, indent);
    buf_append(&e->out, "set +e\n");
    emit_indent(&e->out, indent);
    buf_append(&e->out, "__ds_trace_cmd ");
    emit_source_loc(&e->out, e->source, span);
    for (size_t s = 0; s < command->stages.len; s++) {
        if (s > 0) buf_append(&e->out, " \"|\"");
        for (size_t i = 0; i < command->stages.items[s].words.len; i++) {
            buf_append(&e->out, " ");
            if (!emit_command_word(e, &command->stages.items[s].words.items[i], &e->out)) return false;
        }
    }
    buf_append(&e->out, "\n");

    emit_indent(&e->out, indent);
    buf_append(&e->out, "{ if [[ -t 0 ]]; then exec </dev/null; fi; ");
    if (!emit_command_pipeline_stages(e, command, &e->out)) return false;
    ds_string_appendf(&e->out, " ; } >\"$__ds_stdout_%zu\" 2>\"$__ds_stderr_%zu\"\n", id, id);
    emit_indent(&e->out, indent);
    ds_string_appendf(&e->out, "__ds_code_%zu=$?\n", id);
    emit_indent(&e->out, indent);
    buf_append(&e->out, "set -e\n");

    emit_indent(&e->out, indent);
    ds_string_appendf(&e->out, "__ds_data_%zu=$(cat \"$__ds_stdout_%zu\"; printf x)\n", id, id);
    emit_indent(&e->out, indent);
    buf_append(&e->out, "printf -v ");
    emit_result_field_name(&e->out, name, "stdout");
    buf_append(&e->out, " '%s' ");
    ds_string_appendf(&e->out, "\"${__ds_data_%zu%%x}\"\n", id);

    emit_indent(&e->out, indent);
    ds_string_appendf(&e->out, "__ds_data_%zu=$(cat \"$__ds_stderr_%zu\"; printf x)\n", id, id);
    emit_indent(&e->out, indent);
    buf_append(&e->out, "printf -v ");
    emit_result_field_name(&e->out, name, "stderr");
    buf_append(&e->out, " '%s' ");
    ds_string_appendf(&e->out, "\"${__ds_data_%zu%%x}\"\n", id);

    emit_indent(&e->out, indent);
    buf_append(&e->out, "printf -v ");
    emit_result_field_name(&e->out, name, "code");
    ds_string_appendf(&e->out, " '%%s' \"$__ds_code_%zu\"\n", id);
    emit_indent(&e->out, indent);
    buf_append(&e->out, "printf -v ");
    emit_result_field_name(&e->out, name, "status");
    ds_string_appendf(&e->out, " '%%s' \"$__ds_code_%zu\"\n", id);

    emit_indent(&e->out, indent);
    ds_string_appendf(&e->out, "if [[ $__ds_code_%zu -eq 0 ]]; then\n", id);
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
    ds_string_appendf(&e->out, "__ds_temp_remove \"$__ds_tmpdir_%zu\"\n", id);
    return true;
}
