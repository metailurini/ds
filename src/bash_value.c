#include "bash_internal.h"

bool bash_is_regex_match_call(const DsLowerExpr *value) {
    return value && value->kind == DS_LOWER_EXPR_CALL && ds_str_eq_cstr(value->as.call.name, "regex.match");
}

bool bash_emit_regex_match_map_call(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent, bool local_decl, bool declare_target) {
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

bool bash_emit_assignment_rhs(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent) {
    if (value->kind == DS_LOWER_EXPR_INTERP) {
        bool has_user_call = false;
        for (size_t i = 0; i < value->as.interp.parts.len; i++) {
            if (bash_is_user_function_call_expr(value->as.interp.parts.items[i])) { has_user_call = true; break; }
        }
        if (has_user_call) {
            char **temps = ds_xcalloc(value->as.interp.parts.len, sizeof(char *));
            for (size_t i = 0; i < value->as.interp.parts.len; i++) {
                const DsLowerExpr *part = value->as.interp.parts.items[i];
                if (!bash_is_user_function_call_expr(part)) continue;
                char tmp[64];
                bash_temp_ds_name(tmp, sizeof(tmp), "interp", e->temp_counter++);
                temps[i] = ds_str_dup_cstr(tmp);
                if (!temps[i]) { ds_free_cstr_array(temps, value->as.interp.parts.len); return false; }
                DsStr raw = {temps[i], strlen(temps[i])};
                emit_indent(&e->out, indent);
                emit_bash_decl_prefix(&e->out, e->function_depth, "");
                emit_var_name(&e->out, raw);
                buf_append(&e->out, "=\"\"\n");
                if (!bash_emit_user_call_into_raw_var(e, part, raw, indent)) {
                    ds_free_cstr_array(temps, value->as.interp.parts.len);
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
                    ds_free_cstr_array(temps, value->as.interp.parts.len);
                    return false;
                }
            }
            buf_append(&e->out, "\n");
            ds_free_cstr_array(temps, value->as.interp.parts.len);
            bash_emit_type_assignment_for_expr(e, name, value, indent, false);
            buf_append(&e->out, "\n");
            return true;
        }
    }
    if (value->kind == DS_LOWER_EXPR_RUN) {
        if (ds_lower_command_is_pipeline(&value->as.run)) {
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
    if (bash_is_regex_match_call(value)) {
        if (!bash_emit_regex_match_map_call(e, name, value, indent, false, false)) return false;
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

bool bash_interp_has_user_function_call(const DsLowerExpr *value) {
    if (!value || value->kind != DS_LOWER_EXPR_INTERP) return false;
    for (size_t i = 0; i < value->as.interp.parts.len; i++) {
        if (bash_is_user_function_call_expr(value->as.interp.parts.items[i])) return true;
    }
    return false;
}

bool bash_emit_collection_ident_copy(BashEmitter *e, DsStr dest, const DsLowerExpr *source, DsLowerValueKind kind, int indent) {
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
