#include "bash_internal.h"
#include "ds_signal.h"
#include "ds_command_facts.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

bool emit_block_body(BashEmitter *e, const DsLowerStmt *block, int indent) {
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) {
        if (!emit_stmt(e, block->as.block_stmt.statements.items[i], indent)) return false;
    }
    return true;
}

static bool can_emit_direct_signal_command(const DsCommand *command) {
    return command && !ds_command_is_pipeline(command) && command->redirect.kind == DS_REDIRECT_NONE && command->stages.items[0].words.len > 0;
}

static bool is_regex_match_call(const DsLowerExpr *value) {
    return value && value->kind == DS_LOWER_EXPR_CALL && str_eq(value->as.call.name, "regex.match");
}

static bool emit_regex_match_map_call(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent, bool local_decl, bool declare_target) {
    emit_indent(&e->out, indent);
    if (declare_target) emit_bash_decl_prefix(&e->out, local_decl, "-A");
    emit_var_name(&e->out, name);
    buf_append(&e->out, "=()\n");
    emit_indent(&e->out, indent);
    if (declare_target) emit_bash_decl_prefix(&e->out, local_decl, "-A");
    bash_emit_map_value_type_var_name(&e->out, name);
    buf_append(&e->out, "=()\n");
    emit_indent(&e->out, indent);
    buf_append(&e->out, "__ds_regex_match_into ");
    emit_var_name(&e->out, name);
    buf_append(&e->out, " ");
    bash_emit_map_value_type_var_name(&e->out, name);
    if (!emit_call_args(e, &value->as.call.args, &e->out)) return false;
    buf_append(&e->out, "\n");
    return true;
}

static bool emit_direct_signal_command(BashEmitter *e, const DsCommand *command, DsSpan span, int indent) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "__ds_run_direct_command ");
    emit_source_loc(&e->out, e->source, span);
    buf_append(&e->out, " 1");
    for (size_t i = 0; i < command->stages.items[0].words.len; i++) {
        buf_append(&e->out, " ");
        if (!emit_command_word(e, command->stages.items[0].words.items[i], &e->out)) return false;
    }
    if (e->handler_depth > 0) buf_append(&e->out, " || return $?\n\n");
    else buf_append(&e->out, "\n\n");
    return true;
}

static bool emit_signal_pipeline(BashEmitter *e, const DsCommand *command, DsSpan span, int indent) {
    EmitBuf pipeline = {0};
    if (!emit_command_pipeline(e, command, &pipeline, span)) {
        free(pipeline.data);
        return false;
    }
    emit_indent(&e->out, indent);
    buf_append(&e->out, "__ds_run_pipeline ");
    emit_source_loc(&e->out, e->source, span);
    buf_append(&e->out, command->redirect.kind == DS_REDIRECT_NONE ? " 1" : " 0");
    buf_append(&e->out, " ");
    bash_single_quote(&e->out, pipeline.data ? pipeline.data : "", pipeline.len);
    free(pipeline.data);
    if (e->handler_depth > 0) buf_append(&e->out, " || return $?\n\n");
    else buf_append(&e->out, "\n\n");
    return true;
}

static bool emit_control_command(BashEmitter *e, const DsCommand *command, DsSpan span, int indent) {
    const char *helper = bash_command_is_control(command, "exit") ? "__ds_control_exit" : "__ds_control_fail";
    emit_indent(&e->out, indent);
    buf_append(&e->out, helper);
    buf_append(&e->out, " ");
    emit_source_loc(&e->out, e->source, span);
    for (size_t i = 1; i < command->stages.items[0].words.len; i++) {
        buf_append(&e->out, " ");
        if (!emit_command_word(e, command->stages.items[0].words.items[i], &e->out)) return false;
    }
    if (e->handler_depth > 0) buf_append(&e->out, "; return $?\n\n");
    else buf_append(&e->out, "\n\n");
    return true;
}

static bool emit_assignment_rhs(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent) {
    if (value->kind == DS_LOWER_EXPR_INTERP) {
        bool has_user_call = false;
        for (size_t i = 0; i < value->as.interp.parts.len; i++) {
            if (bash_is_user_function_call_expr(value->as.interp.parts.items[i])) { has_user_call = true; break; }
        }
        if (has_user_call) {
            char **temps = calloc(value->as.interp.parts.len ? value->as.interp.parts.len : 1, sizeof(char *));
            if (!temps) return false;
            for (size_t i = 0; i < value->as.interp.parts.len; i++) {
                const DsLowerExpr *part = value->as.interp.parts.items[i];
                if (!bash_is_user_function_call_expr(part)) continue;
                char tmp[64];
                bash_temp_ds_name(tmp, sizeof(tmp), "interp", e->temp_counter++);
                temps[i] = ds_str_dup_cstr(tmp);
                if (!temps[i]) { free(temps); return false; }
                DsStr raw = {temps[i], strlen(temps[i])};
                emit_indent(&e->out, indent);
                emit_bash_decl_prefix(&e->out, e->function_depth, "");
                emit_var_name(&e->out, raw);
                buf_append(&e->out, "=\"\"\n");
                if (!bash_emit_user_call_into_raw_var(e, part, raw, indent)) {
                    for (size_t j = 0; j < value->as.interp.parts.len; j++) free(temps[j]);
                    free(temps);
                    return false;
                }
            }
            emit_indent(&e->out, indent);
            emit_var_name(&e->out, name);
            buf_append(&e->out, "=");
            if (value->as.interp.parts.len == 0) buf_append(&e->out, "\"\"");
            for (size_t i = 0; i < value->as.interp.parts.len; i++) {
                const DsLowerExpr *part = value->as.interp.parts.items[i];
                if (temps[i]) {
                    DsStr raw = {temps[i], strlen(temps[i])};
                    buf_append(&e->out, "\"$");
                    emit_var_name(&e->out, raw);
                    buf_append(&e->out, "\"");
                } else if (!emit_value_expr(e, part, &e->out)) {
                    for (size_t j = 0; j < value->as.interp.parts.len; j++) free(temps[j]);
                    free(temps);
                    return false;
                }
            }
            buf_append(&e->out, "\n");
            for (size_t i = 0; i < value->as.interp.parts.len; i++) free(temps[i]);
            free(temps);
            bash_emit_type_assignment_for_expr(e, name, value, indent, false);
            buf_append(&e->out, "\n");
            return true;
        }
    }
    if (value->kind == DS_LOWER_EXPR_RUN) {
        if (ds_command_is_pipeline(&value->as.run)) {
            if (!bash_emit_capture_pipeline_assignment(e, name, &value->as.run, value->span, indent)) return false;
            bash_emit_type_assignment_for_expr(e, name, value, indent, false);
            buf_append(&e->out, "\n");
            return true;
        }
        emit_indent(&e->out, indent);
        buf_append(&e->out, "__ds_capture ");
        emit_var_name(&e->out, name);
        if (!emit_capture_command(e, &value->as.run, &e->out, value->span)) return false;
        buf_append(&e->out, "\n");
        bash_emit_type_assignment_for_expr(e, name, value, indent, false);
        buf_append(&e->out, "\n");
        return true;
    }
    if (is_regex_match_call(value)) {
        if (!emit_regex_match_map_call(e, name, value, indent, false, false)) return false;
        bash_emit_type_assignment_for_expr(e, name, value, indent, false);
        buf_append(&e->out, "\n");
        return true;
    }
    if (value->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(value->as.call.name) && !stdlib_returns_array(value->as.call.name)) {
        emit_indent(&e->out, indent);
        emit_var_name(&e->out, name);
        buf_append(&e->out, "=\"\"\n");
        emit_indent(&e->out, indent);
        buf_append(&e->out, "__ds_stdlib_capture ");
        emit_var_name(&e->out, name);
        buf_append(&e->out, " ");
        if (!emit_stdlib_call(e, value, &e->out)) return false;
        buf_append(&e->out, "\n");
        bash_emit_type_assignment_for_expr(e, name, value, indent, false);
        buf_append(&e->out, "\n");
        return true;
    }
    if (value->kind == DS_LOWER_EXPR_CALL && value->as.call.is_user_function) {
        emit_indent(&e->out, indent);
        emit_var_name(&e->out, name);
        buf_append(&e->out, "=\"\"\n");
        if (!bash_emit_user_function_value_call_into(e, name, value, indent)) return false;
        bash_emit_type_assignment_for_expr(e, name, value, indent, false);
        buf_append(&e->out, "\n");
        return true;
    }
    emit_indent(&e->out, indent);
    emit_var_name(&e->out, name);
    buf_append(&e->out, "=");
    if (!emit_value_expr(e, value, &e->out)) return false;
    buf_append(&e->out, "\n");
    bash_emit_type_assignment_for_expr(e, name, value, indent, false);
    buf_append(&e->out, "\n");
    return true;
}

static bool emit_collection_ident_copy(BashEmitter *e, DsStr dest, const DsLowerExpr *source, DsLowerValueKind kind, int indent) {
    if (!source || source->kind != DS_LOWER_EXPR_IDENT) return false;
    bool local_decl = e->function_depth > 0;
    if (kind == DS_LOWER_VALUE_ARRAY) {
        emit_indent(&e->out, indent);
        emit_bash_decl_prefix(&e->out, local_decl, "-a");
        emit_var_name(&e->out, dest);
        buf_append(&e->out, "=(\"${");
        emit_var_name(&e->out, source->as.text);
        buf_append(&e->out, "[@]}\")\n");

        emit_indent(&e->out, indent);
        emit_bash_decl_prefix(&e->out, local_decl, "-a");
        bash_emit_elem_type_var_name(&e->out, dest);
        buf_append(&e->out, "=()\n");
        emit_indent(&e->out, indent);
        buf_append(&e->out, "if declare -p ");
        bash_emit_elem_type_var_name(&e->out, source->as.text);
        buf_append(&e->out, " >/dev/null 2>&1; then ");
        bash_emit_elem_type_var_name(&e->out, dest);
        buf_append(&e->out, "=(\"${");
        bash_emit_elem_type_var_name(&e->out, source->as.text);
        buf_append(&e->out, "[@]}\"); fi\n");
        return true;
    }
    if (kind == DS_LOWER_VALUE_MAP) {
        char key_buf[64];
        bash_temp_ds_name(key_buf, sizeof(key_buf), "copy_key", e->temp_counter++);
        DsStr key_tmp = {key_buf, strlen(key_buf)};

        emit_indent(&e->out, indent);
        emit_bash_decl_prefix(&e->out, local_decl, "-A");
        emit_var_name(&e->out, dest);
        buf_append(&e->out, "=()\n");
        emit_indent(&e->out, indent);
        if (local_decl) {
            emit_bash_decl_prefix(&e->out, 1, "");
            emit_var_name(&e->out, key_tmp);
            buf_append(&e->out, "\n");
            emit_indent(&e->out, indent);
        }
        buf_append(&e->out, "for ");
        emit_var_name(&e->out, key_tmp);
        buf_append(&e->out, " in \"${!");
        emit_var_name(&e->out, source->as.text);
        buf_append(&e->out, "[@]}\"; do ");
        emit_var_name(&e->out, dest);
        buf_append(&e->out, "[\"$");
        emit_var_name(&e->out, key_tmp);
        buf_append(&e->out, "\"]=\"${");
        emit_var_name(&e->out, source->as.text);
        buf_append(&e->out, "[$");
        emit_var_name(&e->out, key_tmp);
        buf_append(&e->out, "]}\"; done\n");

        emit_indent(&e->out, indent);
        emit_bash_decl_prefix(&e->out, local_decl, "-A");
        bash_emit_map_value_type_var_name(&e->out, dest);
        buf_append(&e->out, "=()\n");
        emit_indent(&e->out, indent);
        buf_append(&e->out, "if declare -p ");
        bash_emit_map_value_type_var_name(&e->out, source->as.text);
        buf_append(&e->out, " >/dev/null 2>&1; then for ");
        emit_var_name(&e->out, key_tmp);
        buf_append(&e->out, " in \"${!");
        bash_emit_map_value_type_var_name(&e->out, source->as.text);
        buf_append(&e->out, "[@]}\"; do ");
        bash_emit_map_value_type_var_name(&e->out, dest);
        buf_append(&e->out, "[\"$");
        emit_var_name(&e->out, key_tmp);
        buf_append(&e->out, "\"]=\"${");
        bash_emit_map_value_type_var_name(&e->out, source->as.text);
        buf_append(&e->out, "[$");
        emit_var_name(&e->out, key_tmp);
        buf_append(&e->out, "]}\"; done; fi\n");
        return true;
    }
    return false;
}

static bool emit_index_assignment(BashEmitter *e, const DsLowerStmt *stmt, int indent) {
    if (!is_safe_identifier(stmt->as.index_assign_stmt.name)) {
        ds_diag_error(e->diag, stmt->span, "internal Bash invariant failed: unsafe index assignment target `%.*s` reached Bash emission", (int)stmt->as.index_assign_stmt.name.len, stmt->as.index_assign_stmt.name.data);
        return false;
    }
    char index_buf[64], value_buf[64];
    size_t id = e->temp_counter++;
    bash_temp_ds_name(index_buf, sizeof(index_buf), "idx", id);
    bash_temp_ds_name(value_buf, sizeof(value_buf), "idx_value", id);
    DsStr index_tmp = {index_buf, strlen(index_buf)};
    DsStr value_tmp = {value_buf, strlen(value_buf)};

    emit_indent(&e->out, indent);
    emit_bash_decl_prefix(&e->out, e->function_depth, "");
    emit_var_name(&e->out, index_tmp);
    buf_append(&e->out, "=");
    if (!emit_value_expr(e, stmt->as.index_assign_stmt.index, &e->out)) return false;
    buf_append(&e->out, "\n");

    emit_indent(&e->out, indent);
    emit_bash_decl_prefix(&e->out, e->function_depth, "");
    emit_var_name(&e->out, value_tmp);
    buf_append(&e->out, "=\"\"\n");
    if (!emit_assignment_rhs(e, value_tmp, stmt->as.index_assign_stmt.value, indent)) return false;
    bash_emit_type_assignment_for_expr_required(e, value_tmp, stmt->as.index_assign_stmt.value, indent, e->function_depth > 0);

    if (stmt->as.index_assign_stmt.target_is_array) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, "__ds_array_set ");
        emit_var_name(&e->out, stmt->as.index_assign_stmt.name);
        buf_append(&e->out, " \"$");
        emit_var_name(&e->out, index_tmp);
        buf_append(&e->out, "\" \"$");
        emit_var_name(&e->out, value_tmp);
        buf_append(&e->out, "\"\n");
        emit_indent(&e->out, indent);
        bash_emit_elem_type_var_name(&e->out, stmt->as.index_assign_stmt.name);
        buf_append(&e->out, "[$");
        emit_var_name(&e->out, index_tmp);
        buf_append(&e->out, "]=");
        buf_append(&e->out, "\"${");
        bash_emit_type_var_name(&e->out, value_tmp);
        buf_append(&e->out, ":-unknown}\"");
        buf_append(&e->out, "\n\n");
    } else if (stmt->as.index_assign_stmt.target_is_map) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, "__ds_map_set ");
        emit_var_name(&e->out, stmt->as.index_assign_stmt.name);
        buf_append(&e->out, " \"$");
        emit_var_name(&e->out, index_tmp);
        buf_append(&e->out, "\" \"$");
        emit_var_name(&e->out, value_tmp);
        buf_append(&e->out, "\"\n");
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -p ");
        bash_emit_map_value_type_var_name(&e->out, stmt->as.index_assign_stmt.name);
        buf_append(&e->out, " >/dev/null 2>&1 || declare -A ");
        bash_emit_map_value_type_var_name(&e->out, stmt->as.index_assign_stmt.name);
        buf_append(&e->out, "=()\n");
        emit_indent(&e->out, indent);
        bash_emit_map_value_type_var_name(&e->out, stmt->as.index_assign_stmt.name);
        buf_append(&e->out, "[$");
        emit_var_name(&e->out, index_tmp);
        buf_append(&e->out, "]=");
        buf_append(&e->out, "\"${");
        bash_emit_type_var_name(&e->out, value_tmp);
        buf_append(&e->out, ":-unknown}\"");
        buf_append(&e->out, "\n\n");
    }
    return true;
}

static bool interp_has_user_function_call(const DsLowerExpr *value) {
    if (!value || value->kind != DS_LOWER_EXPR_INTERP) return false;
    for (size_t i = 0; i < value->as.interp.parts.len; i++) {
        if (bash_is_user_function_call_expr(value->as.interp.parts.items[i])) return true;
    }
    return false;
}

static void emit_case_selector_type(BashEmitter *e, const DsLowerExpr *selector, EmitBuf *out) {
    (void)e;
    if (selector->kind == DS_LOWER_EXPR_IDENT) {
        buf_append(out, "\"${");
        bash_emit_type_var_name(out, selector->as.text);
        buf_append(out, ":-unknown}\"");
        return;
    }
    if (selector->kind == DS_LOWER_EXPR_INDEX && selector->as.index.element_kind != DS_LOWER_VALUE_UNKNOWN) {
        const char *type = ds_lower_value_kind_name(selector->as.index.element_kind);
        bash_single_quote(out, type, strlen(type));
        return;
    }
    if (selector->kind == DS_LOWER_EXPR_INDEX && selector->as.index.object && selector->as.index.object->kind == DS_LOWER_EXPR_IDENT) {
        const DsLowerExpr *object = selector->as.index.object;
        const DsLowerExpr *index = selector->as.index.index;
        buf_append(out, "\"${");
        if (selector->as.index.object_is_array) bash_emit_elem_type_var_name(out, object->as.text);
        else bash_emit_map_value_type_var_name(out, object->as.text);
        buf_append(out, "[");
        if (selector->as.index.object_is_map && selector->as.index.map_key_literal) {
            bash_single_quote(out, selector->as.index.map_key.data, selector->as.index.map_key.len);
        } else if (index->kind == DS_LOWER_EXPR_INT) {
            buf_append_dsstr(out, index->as.text);
        } else if (index->kind == DS_LOWER_EXPR_IDENT) {
            buf_append(out, "$");
            emit_var_name(out, index->as.text);
        }
        buf_append(out, "]:-unknown}\"");
        return;
    }
    bash_single_quote(out, bash_lower_expr_static_type_name(selector), strlen(bash_lower_expr_static_type_name(selector)));
}

static bool emit_case_pattern_condition(BashEmitter *e, const DsLowerExpr *selector, const DsLowerCasePattern *pattern, EmitBuf *out) {
    buf_append(out, "[[ ");
    emit_case_selector_type(e, selector, out);
    buf_append(out, " == ");
    if (pattern->kind == DS_CASE_PATTERN_STRING) bash_single_quote(out, "string", 6);
    else if (pattern->kind == DS_CASE_PATTERN_INT) bash_single_quote(out, "int", 3);
    else if (pattern->kind == DS_CASE_PATTERN_BOOL) bash_single_quote(out, "bool", 4);
    buf_append(out, " && ");
    if (!emit_condition_operand(e, selector, out)) return false;
    buf_append(out, " == ");
    if (pattern->kind == DS_CASE_PATTERN_STRING) {
        char *decoded = NULL; size_t len = 0;
        DsLowerExpr tmp = {.kind = DS_LOWER_EXPR_STRING, .span = pattern->span};
        tmp.as.text = pattern->text;
        if (!decode_string_literal(e->diag, &tmp, &decoded, &len)) return false;
        bash_single_quote(out, decoded, len);
        free(decoded);
    } else if (pattern->kind == DS_CASE_PATTERN_INT) {
        buf_append_dsstr(out, pattern->text);
    } else if (pattern->kind == DS_CASE_PATTERN_BOOL) {
        buf_append(out, pattern->boolean ? "true" : "false");
    }
    buf_append(out, " ]]");
    return true;
}

static void emit_decl_name_for_kind(EmitBuf *out, DsStr name, int kind) {
    switch (kind) {
        case 0: emit_var_name(out, name); break;
        case 1: bash_emit_type_var_name(out, name); break;
        case 2: bash_emit_elem_type_var_name(out, name); break;
        case 3: bash_emit_map_value_type_var_name(out, name); break;
    }
}

static void emit_save_decl_for_kind(BashEmitter *e, size_t loop_id, const char *label, DsStr name, int kind, int indent) {
    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "__ds_map_save_%zu_%s_%d=\n", loop_id, label, kind);
    emit_indent(&e->out, indent);
    buf_append(&e->out, "if declare -p ");
    emit_decl_name_for_kind(&e->out, name, kind);
    buf_append(&e->out, " >/dev/null 2>&1; then __ds_map_save_");
    buf_appendf(&e->out, "%zu_%s_%d", loop_id, label, kind);
    buf_append(&e->out, "=\"$(declare -p ");
    emit_decl_name_for_kind(&e->out, name, kind);
    buf_append(&e->out, ")\"; fi\n");
}

static void emit_restore_decl_for_kind(BashEmitter *e, size_t loop_id, const char *label, DsStr name, int kind, int indent) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "unset ");
    emit_decl_name_for_kind(&e->out, name, kind);
    buf_append(&e->out, "\n");
    emit_indent(&e->out, indent);
    buf_append(&e->out, "if [[ -n \"$__ds_map_save_");
    buf_appendf(&e->out, "%zu_%s_%d", loop_id, label, kind);
    buf_append(&e->out, "\" ]]; then eval \"$__ds_map_save_");
    buf_appendf(&e->out, "%zu_%s_%d", loop_id, label, kind);
    buf_append(&e->out, "\"; fi\n");
}

static void emit_unset_loop_name(BashEmitter *e, DsStr name, int indent) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "unset ");
    emit_var_name(&e->out, name);
    buf_append(&e->out, " ");
    bash_emit_type_var_name(&e->out, name);
    buf_append(&e->out, " ");
    bash_emit_elem_type_var_name(&e->out, name);
    buf_append(&e->out, " ");
    bash_emit_map_value_type_var_name(&e->out, name);
    buf_append(&e->out, " 2>/dev/null || true\n");
}

static void emit_save_loop_name(BashEmitter *e, size_t loop_id, const char *label, DsStr name, int indent) {
    for (int kind = 0; kind < 4; kind++) emit_save_decl_for_kind(e, loop_id, label, name, kind, indent);
}

static void emit_restore_loop_name(BashEmitter *e, size_t loop_id, const char *label, DsStr name, int indent) {
    for (int kind = 0; kind < 4; kind++) emit_restore_decl_for_kind(e, loop_id, label, name, kind, indent);
}

static void emit_map_loop_copy_ident(BashEmitter *e, DsStr source, DsStr raw_map, int indent, size_t loop_id) {
    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "for __ds_map_copy_key_%zu in \"${!", loop_id);
    emit_var_name(&e->out, source);
    buf_append(&e->out, "[@]}\"; do\n");

    emit_indent(&e->out, indent + 1);
    emit_var_name(&e->out, raw_map);
    buf_appendf(&e->out, "[\"$__ds_map_copy_key_%zu\"]=\"${", loop_id);
    emit_var_name(&e->out, source);
    buf_appendf(&e->out, "[$__ds_map_copy_key_%zu]}\"\n", loop_id);

    emit_indent(&e->out, indent + 1);
    buf_append(&e->out, "if declare -p ");
    bash_emit_map_value_type_var_name(&e->out, source);
    buf_append(&e->out, " >/dev/null 2>&1; then\n");
    emit_indent(&e->out, indent + 2);
    bash_emit_map_value_type_var_name(&e->out, raw_map);
    buf_appendf(&e->out, "[\"$__ds_map_copy_key_%zu\"]=\"${", loop_id);
    bash_emit_map_value_type_var_name(&e->out, source);
    buf_appendf(&e->out, "[$__ds_map_copy_key_%zu]:-unknown}\"\n", loop_id);
    emit_indent(&e->out, indent + 1);
    buf_append(&e->out, "else\n");
    emit_indent(&e->out, indent + 2);
    bash_emit_map_value_type_var_name(&e->out, raw_map);
    buf_appendf(&e->out, "[\"$__ds_map_copy_key_%zu\"]=unknown\n", loop_id);
    emit_indent(&e->out, indent + 1);
    buf_append(&e->out, "fi\n");

    emit_indent(&e->out, indent);
    buf_append(&e->out, "done\n");
}

static bool emit_map_loop_materialize(BashEmitter *e, const DsLowerStmt *stmt, DsStr raw_map, int indent, size_t loop_id) {
    if (!bash_emit_structured_target_decl(e, raw_map, DS_LOWER_VALUE_MAP, indent, e->function_depth > 0)) return false;
    if (stmt->as.for_stmt.iterable->kind == DS_LOWER_EXPR_IDENT) {
        emit_map_loop_copy_ident(e, stmt->as.for_stmt.iterable->as.text, raw_map, indent, loop_id);
        return true;
    }
    if (stmt->as.for_stmt.iterable->kind == DS_LOWER_EXPR_CALL && stmt->as.for_stmt.iterable->as.call.is_user_function) {
        return bash_emit_user_function_value_call_into(e, raw_map, stmt->as.for_stmt.iterable, indent);
    }
    return bash_invariant_fail(e, stmt->span, "map loop iterable should be named or a supported map-returning function after lowering");
}

/* Return statement emission. Kept in bash_stmt.c so tiny statement-only logic
 * does not become a standalone micro-file; structured payload details stay in
 * bash_structured.c. */
static bool emit_command_result_return(BashEmitter *e, const DsLowerExpr *value, DsSpan span, int indent) {
    if (value->kind == DS_LOWER_EXPR_RUN) {
        if (ds_command_is_pipeline(&value->as.run)) {
            DsStr ret_name = {"return", strlen("return")};
            return bash_emit_capture_pipeline_assignment(e, ret_name, &value->as.run, value->span, indent);
        }
        emit_indent(&e->out, indent);
        buf_append(&e->out, "__ds_capture __ds_return");
        if (!emit_capture_command(e, &value->as.run, &e->out, value->span)) return false;
        buf_append(&e->out, "\n");
        return true;
    }
    if (value->kind == DS_LOWER_EXPR_IDENT) {
        bash_emit_command_result_copy_to_return(e, value->as.text, indent);
        return true;
    }
    return bash_invariant_fail(e, span, "command-result return should be run, named, or forwarded after lowering");
}

static bool emit_return_stmt(BashEmitter *e, const DsLowerStmt *stmt, int indent) {
    const DsLowerExpr *value = stmt->as.return_stmt.value;
    DsLowerValueKind kind = stmt->as.return_stmt.return_kind;

    bash_emit_return_type(e, kind, indent);
    emit_indent(&e->out, indent);

    if (value->kind == DS_LOWER_EXPR_CALL && value->as.call.is_user_function) {
        if (!bash_emit_user_call_capture_return(e, value, kind, indent)) return false;
        bash_emit_return_type(e, kind, indent);
    } else if (kind == DS_LOWER_VALUE_ARRAY) {
        if (stmt->as.return_stmt.returns_row_array) {
            if (!bash_emit_row_array_return_payload(e, value, &stmt->as.return_stmt.row_schema, stmt->span, indent)) return false;
        } else if (!bash_emit_array_return_payload(e, value, stmt->span, indent)) return false;
    } else if (kind == DS_LOWER_VALUE_MAP) {
        if (is_regex_match_call(value)) {
            char tmp_buf[64];
            bash_temp_ds_name(tmp_buf, sizeof(tmp_buf), "regex_return", e->temp_counter++);
            DsStr tmp = {tmp_buf, strlen(tmp_buf)};
            if (!emit_regex_match_map_call(e, tmp, value, indent, e->function_depth > 0, true)) return false;
            DsLowerExpr tmp_ident = {0};
            tmp_ident.kind = DS_LOWER_EXPR_IDENT;
            tmp_ident.span = value->span;
            tmp_ident.as.text = tmp;
            if (!bash_emit_map_return_payload(e, &tmp_ident, stmt->span, indent)) return false;
        } else if (!bash_emit_map_return_payload(e, value, stmt->span, indent)) return false;
    } else if (kind == DS_LOWER_VALUE_COMMAND_RESULT) {
        if (!emit_command_result_return(e, value, stmt->span, indent)) return false;
    } else {
        buf_append(&e->out, "__ds_return_value=");
        if (!emit_value_expr(e, value, &e->out)) return false;
        buf_append(&e->out, " || return $?\n");
    }

    emit_indent(&e->out, indent);
    buf_append(&e->out, "return 0\n\n");
    return true;
}

bool emit_stmt(BashEmitter *e, const DsLowerStmt *stmt, int indent) {
    emit_indent(&e->out, indent);
    const DsSource *stmt_source = stmt->span.source ? stmt->span.source : e->source;
    buf_appendf(&e->out, "# ds: %s:%d\n", stmt_source && stmt_source->path ? stmt_source->path : "<source>", stmt->span.start.line);

    switch (stmt->kind) {
        case DS_LOWER_STMT_LET:
            if (!is_safe_identifier(stmt->as.let_stmt.name)) {
                ds_diag_error(e->diag, stmt->span, "internal Bash invariant failed: unsafe lowered variable name `%.*s` reached Bash emission", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);
                return false;
            }
            if (interp_has_user_function_call(stmt->as.let_stmt.value)) {
                if (e->function_depth > 0) {
                    emit_indent(&e->out, indent);
                    buf_append(&e->out, "local ");
                    emit_var_name(&e->out, stmt->as.let_stmt.name);
                    buf_append(&e->out, "=\"\"\n");
                }
                if (!emit_assignment_rhs(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, indent)) return false;
                if (!symbol_exists(&e->symbols, stmt->as.let_stmt.name)) {
                    bash_register_symbol(e, stmt->as.let_stmt.name);
                }
                return true;
            }
            emit_indent(&e->out, indent);
            if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_RUN) {
                if (e->function_depth > 0) {
                    bash_emit_command_result_storage_decl(e, stmt->as.let_stmt.name, 0, true);
                    emit_indent(&e->out, indent);
                }
                if (ds_command_is_pipeline(&stmt->as.let_stmt.value->as.run)) {
                    if (!bash_emit_capture_pipeline_assignment(e, stmt->as.let_stmt.name, &stmt->as.let_stmt.value->as.run, stmt->as.let_stmt.value->span, indent)) return false;
                } else {
                    buf_append(&e->out, "__ds_capture ");
                    emit_var_name(&e->out, stmt->as.let_stmt.name);
                    if (!emit_capture_command(e, &stmt->as.let_stmt.value->as.run, &e->out, stmt->as.let_stmt.value->span)) return false;
                }
            } else if (stmt->as.let_stmt.is_row_array && stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_IDENT) {
                if (!bash_emit_row_array_copy(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value->as.text, &stmt->as.let_stmt.row_schema, indent, e->function_depth > 0)) return false;
            } else if (stmt->as.let_stmt.is_row && stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_IDENT) {
                if (!emit_collection_ident_copy(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, DS_LOWER_VALUE_MAP, indent)) return false;
                if (!bash_emit_row_scalar_sidecars_from_map(e, stmt->as.let_stmt.name, &stmt->as.let_stmt.row_schema, indent)) return false;
            } else if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_IDENT &&
                       (stmt->as.let_stmt.value_kind == DS_LOWER_VALUE_ARRAY || stmt->as.let_stmt.value_kind == DS_LOWER_VALUE_MAP)) {
                if (!emit_collection_ident_copy(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, stmt->as.let_stmt.value_kind, indent)) return false;
            } else if (stmt->as.let_stmt.is_row_array && stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_ARRAY) {
                if (!bash_emit_row_array_literal(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, &stmt->as.let_stmt.row_schema, indent, e->function_depth > 0)) return false;
            } else if (stmt->as.let_stmt.is_row && stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_INDEX && stmt->as.let_stmt.value->as.index.returns_row) {
                if (!bash_emit_row_from_index(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, &stmt->as.let_stmt.row_schema, indent, e->function_depth > 0)) return false;
            } else if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_ARRAY) {
                emit_bash_decl_prefix(&e->out, e->function_depth, "-a");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=");
                if (!emit_array_elements(e, &stmt->as.let_stmt.value->as.array.elements, &e->out)) return false;
            } else if (is_regex_match_call(stmt->as.let_stmt.value)) {
                if (!emit_regex_match_map_call(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, indent, e->function_depth > 0, true)) return false;
            } else if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_CALL && stdlib_returns_array(stmt->as.let_stmt.value->as.call.name)) {
                emit_bash_decl_prefix(&e->out, e->function_depth, "-a");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=()\n");
                emit_indent(&e->out, indent);
                emit_bash_decl_prefix(&e->out, e->function_depth, "-a");
                bash_emit_elem_type_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=()\n");
                emit_indent(&e->out, indent);
                size_t temp_id = e->temp_counter++;
                buf_appendf(&e->out, "__ds_mktemp_file __ds_iter_%zu 'failed to create stdlib iteration temp file'\n", temp_id);
                emit_indent(&e->out, indent);
                if (!emit_stdlib_call(e, stmt->as.let_stmt.value, &e->out)) return false;
                buf_appendf(&e->out, " >\"$__ds_iter_%zu\"\n", temp_id);
                emit_indent(&e->out, indent);
                buf_append(&e->out, stdlib_array_call_uses_nul_records(stmt->as.let_stmt.value) ? "while IFS= read -r -d '' __ds_line; do " : "while IFS= read -r __ds_line; do ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "+=(\"$__ds_line\"); ");
                bash_emit_elem_type_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "+=(\"string\"); done");
                buf_appendf(&e->out, " <\"$__ds_iter_%zu\"\n", temp_id);
                emit_indent(&e->out, indent);
                buf_appendf(&e->out, "__ds_temp_remove \"$__ds_iter_%zu\"", temp_id);
            } else if (stmt->as.let_stmt.is_row_array && stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_CALL && str_eq(stmt->as.let_stmt.value->as.call.name, "rowarray.sort_by")) {
                if (!bash_emit_row_array_sort_call(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, &stmt->as.let_stmt.row_schema, indent, e->function_depth > 0)) return false;
            } else if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(stmt->as.let_stmt.value->as.call.name)) {
                emit_bash_decl_prefix(&e->out, e->function_depth, "");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=\"\"\n");
                emit_indent(&e->out, indent);
                buf_append(&e->out, "__ds_stdlib_capture ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, " ");
                if (!emit_stdlib_call(e, stmt->as.let_stmt.value, &e->out)) return false;
            } else if (stmt->as.let_stmt.is_row_array && stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_CALL && stmt->as.let_stmt.value->as.call.is_user_function) {
                if (!bash_emit_row_array_decls(e, stmt->as.let_stmt.name, &stmt->as.let_stmt.row_schema, indent, e->function_depth > 0)) return false;
                if (!bash_emit_user_function_value_call_into(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, indent)) return false;
            } else if (stmt->as.let_stmt.is_row && stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_CALL && stmt->as.let_stmt.value->as.call.is_user_function) {
                if (!bash_emit_structured_target_decl(e, stmt->as.let_stmt.name, DS_LOWER_VALUE_MAP, indent, e->function_depth > 0)) return false;
                if (!bash_emit_user_function_value_call_into(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, indent)) return false;
                if (!bash_emit_row_scalar_sidecars_from_map(e, stmt->as.let_stmt.name, &stmt->as.let_stmt.row_schema, indent)) return false;
            } else if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_CALL && stmt->as.let_stmt.value->as.call.is_user_function) {
                if (!bash_emit_structured_target_decl(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value->as.call.return_kind, indent, e->function_depth > 0)) return false;
                if (!bash_emit_user_function_value_call_into(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, indent)) return false;
            } else if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_MAP) {
                emit_bash_decl_prefix(&e->out, e->function_depth, "-A");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=");
                if (!emit_map_entries(e, &stmt->as.let_stmt.value->as.map.entries, &e->out)) return false;
                if (stmt->as.let_stmt.is_row) {
                    buf_append(&e->out, "\n");
                    for (size_t i = 0; i < stmt->as.let_stmt.row_schema.len; i++) {
                        const DsLowerRowField *field = &stmt->as.let_stmt.row_schema.items[i];
                        const DsLowerMapEntry *entry = bash_row_map_entry(stmt->as.let_stmt.value, field->name);
                        if (!entry || !is_safe_identifier(field->name)) continue;
                        emit_indent(&e->out, indent);
                        emit_var_name(&e->out, stmt->as.let_stmt.name);
                        buf_append(&e->out, "_");
                        buf_append_dsstr(&e->out, field->name);
                        buf_append(&e->out, "=");
                        if (!emit_value_expr(e, entry->value, &e->out)) return false;
                        buf_append(&e->out, "\n");
                    }
                    emit_indent(&e->out, indent);
                }
            } else {
                emit_bash_decl_prefix(&e->out, e->function_depth, "");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=");
                if (!emit_value_expr(e, stmt->as.let_stmt.value, &e->out)) return false;
            }
            buf_append(&e->out, "\n");
            bash_emit_type_assignment_for_expr(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, indent, e->function_depth > 0);
            buf_append(&e->out, "\n");
            bash_register_symbol(e, stmt->as.let_stmt.name);
            return true;

        case DS_LOWER_STMT_ASSIGN:
            if (stmt->as.assign_stmt.name.len > 4 && memcmp(stmt->as.assign_stmt.name.data, "env.", 4) == 0) {
                DsStr env_name = {stmt->as.assign_stmt.name.data + 4, stmt->as.assign_stmt.name.len - 4};
                char tmp_buf[64];
                snprintf(tmp_buf, sizeof(tmp_buf), "__env_value_%zu", e->temp_counter++);
                DsStr tmp = {tmp_buf, strlen(tmp_buf)};
                emit_indent(&e->out, indent);
                emit_var_name(&e->out, tmp);
                buf_append(&e->out, "=");
                if (stmt->as.assign_stmt.value->kind == DS_LOWER_EXPR_CALL && stmt->as.assign_stmt.value->as.call.is_user_function) {
                    buf_append(&e->out, "\n");
                    emit_indent(&e->out, indent);
                    if (!bash_emit_user_function_value_call_into(e, tmp, stmt->as.assign_stmt.value, indent)) return false;
                } else {
                    if (!emit_value_expr(e, stmt->as.assign_stmt.value, &e->out)) return false;
                    buf_append(&e->out, "\n");
                }
                emit_indent(&e->out, indent);
                buf_append(&e->out, "export ");
                buf_append_dsstr(&e->out, env_name);
                buf_append(&e->out, "=\"$");
                emit_var_name(&e->out, tmp);
                buf_append(&e->out, "\"\n");
                return true;
            }
            if (!is_safe_identifier(stmt->as.assign_stmt.name)) {
                ds_diag_error(e->diag, stmt->span, "internal Bash invariant failed: unsafe lowered variable name `%.*s` reached Bash emission", (int)stmt->as.assign_stmt.name.len, stmt->as.assign_stmt.name.data);
                return false;
            }
            if (stmt->as.assign_stmt.op == DS_LOWER_ASSIGN_SET) {
                return emit_assignment_rhs(e, stmt->as.assign_stmt.name, stmt->as.assign_stmt.value, indent);
            }
            emit_indent(&e->out, indent);
            emit_var_name(&e->out, stmt->as.assign_stmt.name);
            buf_append(&e->out, "=\"$(__ds_int_bin ");
            const char *op = ds_lower_assign_binary_op(stmt->as.assign_stmt.op);
            bash_single_quote(&e->out, op, strlen(op));
            buf_append(&e->out, " \"$");
            emit_var_name(&e->out, stmt->as.assign_stmt.name);
            buf_append(&e->out, "\" ");
            if (!emit_value_expr(e, stmt->as.assign_stmt.value, &e->out)) return false;
            buf_append(&e->out, ")\"\n");
            bash_emit_type_assignment(e, stmt->as.assign_stmt.name, "int", indent, false);
            buf_append(&e->out, "\n");
            return true;

        case DS_LOWER_STMT_INDEX_ASSIGN:
            return emit_index_assignment(e, stmt, indent);

        case DS_LOWER_STMT_CMD:
            emit_indent(&e->out, indent);
            buf_append(&e->out, "__ds_trace_cmd ");
            emit_source_loc(&e->out, e->source, stmt->span);
            for (size_t s = 0; s < stmt->as.cmd_stmt.stages.len; s++) {
                if (s > 0) buf_append(&e->out, " \"|\"");
                for (size_t i = 0; i < stmt->as.cmd_stmt.stages.items[s].words.len; i++) {
                    buf_append(&e->out, " ");
                    if (!emit_command_word(e, stmt->as.cmd_stmt.stages.items[s].words.items[i], &e->out)) return false;
                }
            }
            if (!emit_trace_redirect_args(e, &stmt->as.cmd_stmt.redirect, &e->out)) return false;
            buf_append(&e->out, "\n");
            if (bash_command_is_control(&stmt->as.cmd_stmt, NULL)) {
                return emit_control_command(e, &stmt->as.cmd_stmt, stmt->span, indent);
            }
            if (e->has_signal_handlers && can_emit_direct_signal_command(&stmt->as.cmd_stmt)) {
                return emit_direct_signal_command(e, &stmt->as.cmd_stmt, stmt->span, indent);
            }
            if (e->has_signal_handlers && ds_command_is_pipeline(&stmt->as.cmd_stmt)) {
                return emit_signal_pipeline(e, &stmt->as.cmd_stmt, stmt->span, indent);
            }
            emit_indent(&e->out, indent);
            bool is_multi = ds_command_is_pipeline(&stmt->as.cmd_stmt);
            buf_append(&e->out, "( ");
            if (is_multi) buf_append(&e->out, "if [[ -t 0 ]]; then exec </dev/null; fi; ");
            if (!emit_command_pipeline(e, &stmt->as.cmd_stmt, &e->out, stmt->span)) return false;
            if (is_multi) {
                buf_append(&e->out, " ) || { __ds_code=$?; if __ds_is_quiet_broken_pipe \"$__ds_code\" ");
                buf_append(&e->out, stmt->as.cmd_stmt.redirect.kind == DS_REDIRECT_NONE ? "1" : "0");
                buf_append(&e->out, "; then ");
                buf_append(&e->out, e->handler_depth > 0 ? "return 0; " : "exit 0; ");
                buf_append(&e->out, "fi; printf '%s: error: pipeline failed with exit %s\\n' ");
                emit_source_loc(&e->out, e->source, stmt->span);
                buf_append(&e->out, " \"$__ds_code\" >&2; ");
                buf_append(&e->out, e->handler_depth > 0 ? "return \"$__ds_code\"; }" : "exit \"$__ds_code\"; }");
                buf_append(&e->out, "\n\n");
            } else {
                buf_append(&e->out, " ) || __ds_fail ");
                emit_source_loc(&e->out, e->source, stmt->span);
                buf_append(&e->out, " \"$?\"");
                buf_append(&e->out, stmt->as.cmd_stmt.redirect.kind == DS_REDIRECT_NONE ? " 1" : " 0");
                if (e->handler_depth > 0) buf_append(&e->out, " || return $?\n\n");
                else buf_append(&e->out, "\n\n");
            }
            return true;

        case DS_LOWER_STMT_CALL:
            if (ds_stdlib_is_name(stmt->as.call_stmt.name)) {
                emit_indent(&e->out, indent);
                emit_stdlib_helper_name(&e->out, stmt->as.call_stmt.name);
                if (!emit_call_args(e, &stmt->as.call_stmt.args, &e->out)) return false;
                buf_append(&e->out, "\n\n");
                return true;
            }
            return bash_emit_user_call_statement(e, stmt->as.call_stmt.name, &stmt->as.call_stmt.args, indent);

        case DS_LOWER_STMT_PUSH:
            if (stmt->as.push_stmt.target_is_row_array) {
                if (!bash_emit_row_array_push_literal(e, stmt->as.push_stmt.name, &stmt->as.push_stmt.row_schema, stmt->as.push_stmt.value, indent)) return false;
                buf_append(&e->out, "\n");
                return true;
            }
            emit_indent(&e->out, indent);
            emit_var_name(&e->out, stmt->as.push_stmt.name);
            buf_append(&e->out, "+=(");
            if (!emit_value_expr(e, stmt->as.push_stmt.value, &e->out)) return false;
            buf_append(&e->out, ")\n\n");
            if (e->needs_case_types) {
                emit_indent(&e->out, indent);
                bash_emit_elem_type_var_name(&e->out, stmt->as.push_stmt.name);
                buf_append(&e->out, "[$((${#");
                emit_var_name(&e->out, stmt->as.push_stmt.name);
                buf_append(&e->out, "[@]} - 1))]=");
                bash_emit_collection_element_type_value(e, stmt->as.push_stmt.value, &e->out);
                buf_append(&e->out, "\n\n");
            }
            return true;

        case DS_LOWER_STMT_FOR_ARRAY: {
            emit_indent(&e->out, indent);
            if (stmt->as.for_stmt.iterates_row_array) {
                DsStr iter_name = {0};
                if (stmt->as.for_stmt.iterable->kind == DS_LOWER_EXPR_IDENT) {
                    iter_name = stmt->as.for_stmt.iterable->as.text;
                } else if (stmt->as.for_stmt.iterable->kind == DS_LOWER_EXPR_CALL && stmt->as.for_stmt.iterable->as.call.returns_row_array) {
                    char iter_buf[64];
                    bash_temp_ds_name(iter_buf, sizeof(iter_buf), "row_iter", e->temp_counter++);
                    iter_name = (DsStr){iter_buf, strlen(iter_buf)};
                    if (!bash_emit_row_array_expr_into(e, iter_name, stmt->as.for_stmt.iterable, &stmt->as.for_stmt.row_schema, indent, e->function_depth > 0)) return false;
                    emit_indent(&e->out, indent);
                } else {
                    return bash_invariant_fail(e, stmt->span, "row-array loop iterable should be named or a known row-array result after lowering");
                }
                size_t id = e->temp_counter++;
                buf_appendf(&e->out, "for __ds_row_i_%zu in \"${!", id);
                emit_var_name(&e->out, iter_name);
                buf_append(&e->out, "[@]}\"; do\n");
                emit_indent(&e->out, indent + 1);
                emit_bash_decl_prefix(&e->out, e->function_depth, "-A");
                emit_var_name(&e->out, stmt->as.for_stmt.name);
                buf_append(&e->out, "=()\n");
                emit_indent(&e->out, indent + 1);
                emit_bash_decl_prefix(&e->out, e->function_depth, "-A");
                bash_emit_map_value_type_var_name(&e->out, stmt->as.for_stmt.name);
                buf_append(&e->out, "=()\n");
                for (size_t i = 0; i < stmt->as.for_stmt.row_schema.len; i++) {
                    const DsLowerRowField *field = &stmt->as.for_stmt.row_schema.items[i];
                    emit_indent(&e->out, indent + 1);
                    emit_var_name(&e->out, stmt->as.for_stmt.name);
                    buf_append(&e->out, "[");
                    bash_single_quote(&e->out, field->name.data, field->name.len);
                    buf_append(&e->out, "]=\"${");
                    bash_emit_row_field_array_name(&e->out, iter_name, field->name);
                    buf_appendf(&e->out, "[$__ds_row_i_%zu]}\"\n", id);
                    emit_indent(&e->out, indent + 1);
                    bash_emit_map_value_type_var_name(&e->out, stmt->as.for_stmt.name);
                    buf_append(&e->out, "[");
                    bash_single_quote(&e->out, field->name.data, field->name.len);
                    buf_append(&e->out, "]=");
                    bash_single_quote(&e->out, ds_lower_value_kind_name(field->kind), strlen(ds_lower_value_kind_name(field->kind)));
                    buf_append(&e->out, "\n");
                    if (is_safe_identifier(field->name)) {
                        emit_indent(&e->out, indent + 1);
                        emit_var_name(&e->out, stmt->as.for_stmt.name);
                        buf_append(&e->out, "_");
                        buf_append_dsstr(&e->out, field->name);
                        buf_append(&e->out, "=\"${");
                        bash_emit_row_field_array_name(&e->out, iter_name, field->name);
                        buf_appendf(&e->out, "[$__ds_row_i_%zu]}\"\n", id);
                    }
                }
                size_t mark = e->symbols.len;
                bash_register_symbol(e, stmt->as.for_stmt.name);
                bash_emit_type_assignment(e, stmt->as.for_stmt.name, "map", indent + 1, false);
                if (!emit_block_body(e, stmt->as.for_stmt.body, indent + 1)) { symbols_truncate(&e->symbols, mark); return false; }
                symbols_truncate(&e->symbols, mark);
                emit_indent(&e->out, indent);
                buf_append(&e->out, "done\n\n");
                return true;
            }
            bool iterable_is_user_array_call = stmt->as.for_stmt.iterable->kind == DS_LOWER_EXPR_CALL &&
                                               stmt->as.for_stmt.iterable->as.call.is_user_function &&
                                               stmt->as.for_stmt.iterable->as.call.return_kind == DS_LOWER_VALUE_ARRAY;
            if (stmt->as.for_stmt.iterable->kind != DS_LOWER_EXPR_IDENT &&
                !iterable_is_user_array_call &&
                !(stmt->as.for_stmt.iterable->kind == DS_LOWER_EXPR_CALL && stdlib_returns_array(stmt->as.for_stmt.iterable->as.call.name))) {
                /* Lowering rejects non-portable array iterables for VM/Bash parity. */
                return bash_invariant_fail(e, stmt->span, "array loop iterable should be named, a known stdlib array result, or a supported array-returning function after lowering");
            }
            size_t temp_id = 0;
            DsStr user_iter_name = {0};
            if (stmt->as.for_stmt.iterable->kind == DS_LOWER_EXPR_IDENT) {
                buf_append(&e->out, "for ");
                emit_var_name(&e->out, stmt->as.for_stmt.name);
                buf_append(&e->out, " in ");
                buf_append(&e->out, "\"${");
                emit_var_name(&e->out, stmt->as.for_stmt.iterable->as.text);
                buf_append(&e->out, "[@]}\"; do\n");
            } else if (iterable_is_user_array_call) {
                char iter_buf[64];
                bash_temp_ds_name(iter_buf, sizeof(iter_buf), "array_iter", e->temp_counter++);
                user_iter_name = (DsStr){iter_buf, strlen(iter_buf)};
                if (!bash_emit_structured_target_decl(e, user_iter_name, DS_LOWER_VALUE_ARRAY, indent, e->function_depth > 0)) return false;
                if (!bash_emit_user_function_value_call_into(e, user_iter_name, stmt->as.for_stmt.iterable, indent)) return false;
                emit_indent(&e->out, indent);
                buf_append(&e->out, "for ");
                emit_var_name(&e->out, stmt->as.for_stmt.name);
                buf_append(&e->out, " in ");
                buf_append(&e->out, "\"${");
                emit_var_name(&e->out, user_iter_name);
                buf_append(&e->out, "[@]}\"; do\n");
            } else {
                temp_id = e->temp_counter++;
                buf_appendf(&e->out, "__ds_mktemp_file __ds_iter_%zu 'failed to create stdlib iteration temp file'\n", temp_id);
                emit_indent(&e->out, indent);
                if (!emit_stdlib_call(e, stmt->as.for_stmt.iterable, &e->out)) return false;
                buf_appendf(&e->out, " >\"$__ds_iter_%zu\"\n", temp_id);
                emit_indent(&e->out, indent);
                buf_append(&e->out, stdlib_array_call_uses_nul_records(stmt->as.for_stmt.iterable) ? "while IFS= read -r -d '' " : "while IFS= read -r ");
                emit_var_name(&e->out, stmt->as.for_stmt.name);
                buf_append(&e->out, "; do\n");
            }
            size_t mark = e->symbols.len;
            bash_register_symbol(e, stmt->as.for_stmt.name);
            bash_emit_type_assignment(e, stmt->as.for_stmt.name, ds_lower_value_kind_name(stmt->as.for_stmt.element_kind), indent + 1, false);
            if (!emit_block_body(e, stmt->as.for_stmt.body, indent + 1)) { symbols_truncate(&e->symbols, mark); return false; }
            symbols_truncate(&e->symbols, mark);
            emit_indent(&e->out, indent);
            if (stmt->as.for_stmt.iterable->kind == DS_LOWER_EXPR_IDENT || iterable_is_user_array_call) buf_append(&e->out, "done\n\n");
            else {
                buf_appendf(&e->out, "done <\"$__ds_iter_%zu\"\n", temp_id);
                emit_indent(&e->out, indent);
                buf_appendf(&e->out, "__ds_temp_remove \"$__ds_iter_%zu\"\n\n", temp_id);
            }
            return true;
        }

        case DS_LOWER_STMT_FOR_MAP: {
            size_t loop_id = e->temp_counter++;
            char raw_map_buf[64];
            char raw_keys_buf[64];
            char raw_key_buf[64];
            bash_temp_ds_name(raw_map_buf, sizeof(raw_map_buf), "map_iter", loop_id);
            bash_temp_ds_name(raw_keys_buf, sizeof(raw_keys_buf), "map_keys", loop_id);
            bash_temp_ds_name(raw_key_buf, sizeof(raw_key_buf), "map_key", loop_id);
            DsStr raw_map = {raw_map_buf, strlen(raw_map_buf)};
            DsStr raw_keys = {raw_keys_buf, strlen(raw_keys_buf)};
            DsStr raw_key = {raw_key_buf, strlen(raw_key_buf)};

            if (!emit_map_loop_materialize(e, stmt, raw_map, indent, loop_id)) return false;
            emit_indent(&e->out, indent);
            emit_bash_decl_prefix(&e->out, e->function_depth, "-a");
            emit_var_name(&e->out, raw_keys);
            buf_append(&e->out, "=()\n");
            emit_indent(&e->out, indent);
            buf_append(&e->out, "__ds_map_sorted_keys ");
            emit_var_name(&e->out, raw_map);
            buf_append(&e->out, " ");
            emit_var_name(&e->out, raw_keys);
            buf_append(&e->out, "\n");

            emit_save_loop_name(e, loop_id, "key", stmt->as.for_stmt.name, indent);
            emit_save_loop_name(e, loop_id, "value", stmt->as.for_stmt.value_name, indent);

            emit_indent(&e->out, indent);
            buf_append(&e->out, "for ");
            emit_var_name(&e->out, raw_key);
            buf_append(&e->out, " in \"${");
            emit_var_name(&e->out, raw_keys);
            buf_append(&e->out, "[@]}\"; do\n");

            emit_unset_loop_name(e, stmt->as.for_stmt.name, indent + 1);
            emit_unset_loop_name(e, stmt->as.for_stmt.value_name, indent + 1);

            emit_indent(&e->out, indent + 1);
            emit_var_name(&e->out, stmt->as.for_stmt.name);
            buf_append(&e->out, "=\"$");
            emit_var_name(&e->out, raw_key);
            buf_append(&e->out, "\"\n");
            emit_indent(&e->out, indent + 1);
            emit_var_name(&e->out, stmt->as.for_stmt.value_name);
            buf_append(&e->out, "=\"${");
            emit_var_name(&e->out, raw_map);
            buf_append(&e->out, "[$");
            emit_var_name(&e->out, raw_key);
            buf_append(&e->out, "]}\"\n");

            bash_emit_type_assignment(e, stmt->as.for_stmt.name, "string", indent + 1, false);
            emit_indent(&e->out, indent + 1);
            bash_emit_type_var_name(&e->out, stmt->as.for_stmt.value_name);
            buf_append(&e->out, "=\"${");
            bash_emit_map_value_type_var_name(&e->out, raw_map);
            buf_append(&e->out, "[$");
            emit_var_name(&e->out, raw_key);
            buf_append(&e->out, "]:-");
            buf_append(&e->out, ds_lower_value_kind_name(stmt->as.for_stmt.element_kind));
            buf_append(&e->out, "}\"\n");

            size_t mark = e->symbols.len;
            bash_register_symbol(e, stmt->as.for_stmt.name);
            bash_register_symbol(e, stmt->as.for_stmt.value_name);
            if (!emit_block_body(e, stmt->as.for_stmt.body, indent + 1)) { symbols_truncate(&e->symbols, mark); return false; }
            symbols_truncate(&e->symbols, mark);

            emit_indent(&e->out, indent);
            buf_append(&e->out, "done\n");
            emit_restore_loop_name(e, loop_id, "value", stmt->as.for_stmt.value_name, indent);
            emit_restore_loop_name(e, loop_id, "key", stmt->as.for_stmt.name, indent);
            emit_indent(&e->out, indent);
            buf_append(&e->out, "unset ");
            emit_var_name(&e->out, raw_map);
            buf_append(&e->out, " ");
            bash_emit_map_value_type_var_name(&e->out, raw_map);
            buf_append(&e->out, " ");
            emit_var_name(&e->out, raw_keys);
            buf_append(&e->out, " ");
            emit_var_name(&e->out, raw_key);
            buf_append(&e->out, " 2>/dev/null || true\n\n");
            return true;
        }

        case DS_LOWER_STMT_FOR_RANGE: {
            size_t temp_id = e->temp_counter++;
            emit_indent(&e->out, indent);
            buf_appendf(&e->out, "__ds_range_start_%zu=", temp_id);
            if (!emit_value_expr(e, stmt->as.for_stmt.iterable->as.range.start, &e->out)) return false;
            buf_append(&e->out, "\n");
            emit_indent(&e->out, indent);
            buf_appendf(&e->out, "__ds_range_end_%zu=", temp_id);
            if (!emit_value_expr(e, stmt->as.for_stmt.iterable->as.range.end, &e->out)) return false;
            buf_append(&e->out, "\n");
            emit_indent(&e->out, indent);
            buf_append(&e->out, "for (( ");
            emit_var_name(&e->out, stmt->as.for_stmt.name);
            buf_appendf(&e->out, "=__ds_range_start_%zu; ", temp_id);
            emit_var_name(&e->out, stmt->as.for_stmt.name);
            buf_appendf(&e->out, "<=__ds_range_end_%zu; ", temp_id);
            emit_var_name(&e->out, stmt->as.for_stmt.name);
            buf_append(&e->out, "++ )); do\n");
            size_t mark = e->symbols.len;
            bash_register_symbol(e, stmt->as.for_stmt.name);
            bash_emit_type_assignment(e, stmt->as.for_stmt.name, "int", indent + 1, false);
            if (!emit_block_body(e, stmt->as.for_stmt.body, indent + 1)) { symbols_truncate(&e->symbols, mark); return false; }
            symbols_truncate(&e->symbols, mark);
            emit_indent(&e->out, indent);
            buf_append(&e->out, "done\n\n");
            return true;
        }

        case DS_LOWER_STMT_IF:
            emit_indent(&e->out, indent);
            buf_append(&e->out, "if ");
            if (!emit_condition(e, stmt->as.if_stmt.condition, &e->out)) return false;
            buf_append(&e->out, "; then\n");
            if (!emit_block_body(e, stmt->as.if_stmt.then_branch, indent + 1)) return false;
            if (stmt->as.if_stmt.else_branch) {
                emit_indent(&e->out, indent);
                buf_append(&e->out, "else\n");
                if (!emit_block_body(e, stmt->as.if_stmt.else_branch, indent + 1)) return false;
            }
            emit_indent(&e->out, indent);
            buf_append(&e->out, "fi\n\n");
            return true;

        case DS_LOWER_STMT_WHILE:
            emit_indent(&e->out, indent);
            buf_append(&e->out, "while ");
            if (!emit_condition(e, stmt->as.while_stmt.condition, &e->out)) return false;
            buf_append(&e->out, "; do\n");
            if (!emit_block_body(e, stmt->as.while_stmt.body, indent + 1)) return false;
            emit_indent(&e->out, indent);
            buf_append(&e->out, "done\n\n");
            return true;

        case DS_LOWER_STMT_BREAK:
            emit_indent(&e->out, indent);
            buf_append(&e->out, "break\n\n");
            return true;

        case DS_LOWER_STMT_CONTINUE:
            emit_indent(&e->out, indent);
            buf_append(&e->out, "continue\n\n");
            return true;

        case DS_LOWER_STMT_CASE: {
            const DsLowerExpr *case_selector = stmt->as.case_stmt.selector;
            DsLowerExpr case_temp_expr;
            char case_temp_buf[64];
            size_t case_symbol_len = e->symbols.len;
            if (bash_is_user_function_call_expr(stmt->as.case_stmt.selector)) {
                bash_temp_ds_name(case_temp_buf, sizeof(case_temp_buf), "case", e->temp_counter++);
                DsStr raw = {case_temp_buf, strlen(case_temp_buf)};
                emit_indent(&e->out, indent);
                emit_bash_decl_prefix(&e->out, e->function_depth, "");
                emit_var_name(&e->out, raw);
                buf_append(&e->out, "=\"\"\n");
                if (!bash_emit_user_call_into_raw_var(e, stmt->as.case_stmt.selector, raw, indent)) return false;
                emit_indent(&e->out, indent);
                buf_append(&e->out, "__ds_type_");
                buf_append_dsstr(&e->out, raw);
                buf_append(&e->out, "=");
                bash_single_quote(&e->out, ds_lower_value_kind_name(stmt->as.case_stmt.selector->as.call.return_kind), strlen(ds_lower_value_kind_name(stmt->as.case_stmt.selector->as.call.return_kind)));
                buf_append(&e->out, "\n");
                case_temp_expr = *stmt->as.case_stmt.selector;
                case_temp_expr.kind = DS_LOWER_EXPR_IDENT;
                case_temp_expr.as.text = raw;
                bash_register_symbol(e, raw);
                case_selector = &case_temp_expr;
            }
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsLowerCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                bool is_default = arm->patterns.len > 0 && arm->patterns.items[0].kind == DS_CASE_PATTERN_DEFAULT;
                emit_indent(&e->out, indent);
                if (is_default) {
                    if (i == 0) buf_append(&e->out, "if true; then\n");
                    else buf_append(&e->out, "else\n");
                } else {
                    if (i == 0) buf_append(&e->out, "if ");
                    else buf_append(&e->out, "elif ");
                    for (size_t j = 0; j < arm->patterns.len; j++) {
                        if (j) buf_append(&e->out, " || ");
                        if (!emit_case_pattern_condition(e, case_selector, &arm->patterns.items[j], &e->out)) return false;
                    }
                    buf_append(&e->out, "; then\n");
                }
                if (!emit_block_body(e, arm->body, indent + 1)) return false;
            }
            if (case_symbol_len != e->symbols.len) symbols_truncate(&e->symbols, case_symbol_len);
            emit_indent(&e->out, indent);
            buf_append(&e->out, "fi\n\n");
            return true;
        }

        case DS_LOWER_STMT_BLOCK:
            return emit_block_body(e, stmt, indent);
        case DS_LOWER_STMT_ASSERT:
            return bash_invariant_fail(e, stmt->span, "assert statement reached standalone Bash emission; the test runner owns assert emission in v0.14.0");
        case DS_LOWER_STMT_RETURN:
            return emit_return_stmt(e, stmt, indent);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: {
            size_t id = e->handler_counter++;
            const char *sig = ds_handler_signal_name(stmt->as.handler_stmt.signal);
            emit_indent(&e->out, indent);
            buf_appendf(&e->out, "__ds_handler_%zu() {\n", id);
            e->handler_depth++;
            if (!emit_block_body(e, stmt->as.handler_stmt.body, indent + 1)) return false;
            e->handler_depth--;
            emit_indent(&e->out, indent);
            buf_append(&e->out, "}\n");
            emit_indent(&e->out, indent);
            if (stmt->kind == DS_LOWER_STMT_TRAP) {
                buf_appendf(&e->out, "__ds_trap_%s=__ds_handler_%zu\n\n", sig, id);
            } else {
                buf_appendf(&e->out, "__ds_defer_%s+=(__ds_handler_%zu)\n\n", sig, id);
            }
            return true;
        }
    }
    return true;
}
