#include "bash_internal.h"

#include <stdlib.h>
#include <string.h>

bool emit_block_body(BashEmitter *e, const DsLowerStmt *block, int indent) {
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) {
        if (!emit_stmt(e, block->as.block_stmt.statements.items[i], indent)) return false;
    }
    return true;
}

static const char *lower_value_type_name(DsLowerValueKind kind);

static const char *handler_signal_name(DsHandlerSignal signal) {
    switch (signal) {
        case DS_HANDLER_EXIT: return "EXIT";
        case DS_HANDLER_INT: return "INT";
        case DS_HANDLER_TERM: return "TERM";
    }
    return "EXIT";
}

static const char *expr_type_name(const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING: return "string";
        case DS_LOWER_EXPR_INTERP: return "string";
        case DS_LOWER_EXPR_INT: return "int";
        case DS_LOWER_EXPR_BOOL: return "bool";
        case DS_LOWER_EXPR_REGEX: return "unknown";
        case DS_LOWER_EXPR_ARRAY: return "array";
        case DS_LOWER_EXPR_MAP: return "map";
        case DS_LOWER_EXPR_RANGE: return "unknown";
        case DS_LOWER_EXPR_RUN: return "command_result";
        case DS_LOWER_EXPR_BINARY:
            if (str_eq(expr->as.binary.op, "+") || str_eq(expr->as.binary.op, "-") || str_eq(expr->as.binary.op, "*") || str_eq(expr->as.binary.op, "/") || str_eq(expr->as.binary.op, "%") || str_eq(expr->as.binary.op, "**")) return "int";
            return "bool";
        case DS_LOWER_EXPR_UNARY:
            if (str_eq(expr->as.unary.op, "!")) return "bool";
            if (str_eq(expr->as.unary.op, "-")) return "int";
            return "unknown";
        case DS_LOWER_EXPR_CALL:
            if (ds_stdlib_is_name(expr->as.call.name)) {
                const DsStdlibHelper *helper = ds_stdlib_lookup(expr->as.call.name);
                if (!helper) return "unknown";
                switch (helper->return_kind) {
                    case DS_STDLIB_RETURN_BOOL: return "bool";
                    case DS_STDLIB_RETURN_INT: return "int";
                    case DS_STDLIB_RETURN_STRING: return "string";
                    case DS_STDLIB_RETURN_ARRAY: return "array";
                    case DS_STDLIB_RETURN_MAP: return "map";
                    case DS_STDLIB_RETURN_COMMAND_RESULT: return "command_result";
                    case DS_STDLIB_RETURN_STATEMENT_ONLY: return "unknown";
                }
            }
            return lower_value_type_name(expr->as.call.return_kind);
            return "unknown";
        case DS_LOWER_EXPR_FIELD: {
            const DsCommandResultField *desc = ds_command_result_field_lookup(expr->as.field.field);
            if (!desc) return "unknown";
            switch (desc->kind) {
                case DS_COMMAND_RESULT_FIELD_STRING: return "string";
                case DS_COMMAND_RESULT_FIELD_INT: return "int";
                case DS_COMMAND_RESULT_FIELD_BOOL: return "bool";
            }
            return "unknown";
        }
        case DS_LOWER_EXPR_INDEX:
            switch (expr->as.index.element_kind) {
                case DS_LOWER_VALUE_BOOL: return "bool";
                case DS_LOWER_VALUE_INT: return "int";
                case DS_LOWER_VALUE_STRING: return "string";
                case DS_LOWER_VALUE_ARRAY: return "array";
                case DS_LOWER_VALUE_MAP: return "map";
                case DS_LOWER_VALUE_COMMAND_RESULT: return "command_result";
                case DS_LOWER_VALUE_UNKNOWN: return "unknown";
            }
            return "unknown";
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_ERROR:
            return "unknown";
    }
    return "unknown";
}

static bool is_control_command(const DsCommand *command, const char *name) {
    if (!command || command->stages.len != 1) return false;
    if (command->redirect.kind != DS_REDIRECT_NONE) return false;
    if (command->stages.items[0].words.len == 0) return false;
    DsStr first = command->stages.items[0].words.items[0].text;
    return str_eq(first, name);
}

static bool can_emit_direct_signal_command(const DsCommand *command) {
    return command && command->stages.len == 1 && command->redirect.kind == DS_REDIRECT_NONE && command->stages.items[0].words.len > 0;
}

static bool emit_direct_signal_command(BashEmitter *e, const DsCommand *command, DsSpan span, int indent) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "__ds_run_direct_command ");
    emit_source_loc(&e->out, e->source, span);
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
    buf_append(&e->out, " ");
    bash_single_quote(&e->out, pipeline.data ? pipeline.data : "", pipeline.len);
    free(pipeline.data);
    if (e->handler_depth > 0) buf_append(&e->out, " || return $?\n\n");
    else buf_append(&e->out, "\n\n");
    return true;
}

static bool emit_control_command(BashEmitter *e, const DsCommand *command, DsSpan span, int indent) {
    const char *helper = is_control_command(command, "exit") ? "__ds_control_exit" : "__ds_control_fail";
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

static const char *lower_value_type_name(DsLowerValueKind kind) {
    switch (kind) {
        case DS_LOWER_VALUE_BOOL: return "bool";
        case DS_LOWER_VALUE_INT: return "int";
        case DS_LOWER_VALUE_STRING: return "string";
        case DS_LOWER_VALUE_ARRAY: return "array";
        case DS_LOWER_VALUE_MAP: return "map";
        case DS_LOWER_VALUE_COMMAND_RESULT: return "command_result";
        case DS_LOWER_VALUE_UNKNOWN: return "unknown";
    }
    return "unknown";
}

static void emit_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_type_");
    buf_append_len(out, name.data, name.len);
}

static void emit_elem_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_elem_type_");
    buf_append_len(out, name.data, name.len);
}

static void emit_type_assignment(BashEmitter *e, DsStr name, const char *type, int indent, bool local_decl) {
    if (!e->needs_case_types) return;
    emit_indent(&e->out, indent);
    if (local_decl) buf_append(&e->out, "local ");
    emit_type_var_name(&e->out, name);
    buf_append(&e->out, "=");
    bash_single_quote(&e->out, type, strlen(type));
    buf_append(&e->out, "\n");
}

static void emit_type_assignment_for_expr(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent, bool local_decl) {
    if (!e->needs_case_types) return;
    emit_indent(&e->out, indent);
    if (local_decl) buf_append(&e->out, "local ");
    emit_type_var_name(&e->out, name);
    buf_append(&e->out, "=");
    if (value->kind == DS_LOWER_EXPR_IDENT) {
        buf_append(&e->out, "\"${");
        emit_type_var_name(&e->out, value->as.text);
        buf_append(&e->out, ":-unknown}\"");
    } else {
        const char *type = expr_type_name(value);
        bash_single_quote(&e->out, type, strlen(type));
    }
    buf_append(&e->out, "\n");
    if (value->kind == DS_LOWER_EXPR_ARRAY) {
        emit_indent(&e->out, indent);
        if (local_decl) buf_append(&e->out, "local -a ");
        else buf_append(&e->out, "declare -a ");
        emit_elem_type_var_name(&e->out, name);
        buf_append(&e->out, "=(");
        for (size_t i = 0; i < value->as.array.elements.len; i++) {
            if (i) buf_append(&e->out, " ");
            const char *type = expr_type_name(value->as.array.elements.items[i]);
            bash_single_quote(&e->out, type, strlen(type));
        }
        buf_append(&e->out, ")\n");
    }
}

bool emit_function(BashEmitter *e, const DsLowerFn *fn) {
    if (!is_safe_identifier(fn->name)) {
        ds_diag_error(e->diag, fn->span, "cannot emit unsafe Bash function name `%.*s`", (int)fn->name.len, fn->name.data);
        return false;
    }
    emit_fn_name(&e->out, fn->name);
    buf_append(&e->out, "() {\n");
    size_t symbol_mark = e->symbols.len;
    for (size_t i = 0; i < fn->params.len; i++) {
        const DsLowerFnParam *param = &fn->params.items[i];
        DsStr copy = {ds_str_dup_range(param->name.data, param->name.len), param->name.len};
        symbol_vec_push(&e->symbols, copy);
        emit_indent(&e->out, 1);
        buf_append(&e->out, "local ");
        emit_var_name(&e->out, param->name);
        buf_append(&e->out, "\n");
        if (e->needs_case_types) {
            emit_indent(&e->out, 1);
            buf_append(&e->out, "local ");
            emit_type_var_name(&e->out, param->name);
            buf_append(&e->out, "\n");
        }
        emit_indent(&e->out, 1);
        buf_appendf(&e->out, "if [[ $# -gt %zu ]]; then ", i * 2);
        emit_var_name(&e->out, param->name);
        buf_appendf(&e->out, "=\"${%zu}\"", i * 2 + 1);
        if (e->needs_case_types) {
            buf_append(&e->out, "; ");
            emit_type_var_name(&e->out, param->name);
            buf_append(&e->out, "=");
            buf_appendf(&e->out, "\"${%zu:-", i * 2 + 2);
            const char *type = param->has_default ? lower_value_type_name(param->default_kind) : "unknown";
            buf_append(&e->out, type);
            buf_append(&e->out, "}\"");
        }
        buf_append(&e->out, "; else ");
        emit_var_name(&e->out, param->name);
        buf_append(&e->out, "=");
        if (param->has_default) {
            if (!emit_function_default(e, param->default_value, &e->out)) { symbols_truncate(&e->symbols, symbol_mark); return false; }
        } else {
            buf_append(&e->out, "\"\"");
        }
        if (e->needs_case_types) {
            buf_append(&e->out, "; ");
            emit_type_var_name(&e->out, param->name);
            buf_append(&e->out, "=");
            const char *type = param->has_default ? lower_value_type_name(param->default_kind) : "unknown";
            bash_single_quote(&e->out, type, strlen(type));
        }
        buf_append(&e->out, "; fi\n");
    }
    int saved_depth = e->function_depth;
    e->function_depth++;
    bool ok = emit_block_body(e, fn->body, 1);
    e->function_depth = saved_depth;
    symbols_truncate(&e->symbols, symbol_mark);
    buf_append(&e->out, "}\n\n");
    return ok;
}

static void emit_result_field_name(EmitBuf *out, DsStr name, const char *field) {
    emit_var_name(out, name);
    buf_append(out, "_");
    buf_append(out, field);
}

static bool emit_capture_pipeline_assignment(BashEmitter *e, DsStr name, const DsCommand *command, DsSpan span, int indent) {
    size_t id = e->temp_counter++;

    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "__ds_tmpdir_%zu=$(mktemp -d) || __ds_error 'failed to create command capture temp dir'\n", id);
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
    buf_append(&e->out, "{ ");
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
    buf_appendf(&e->out, "rm -rf \"$__ds_tmpdir_%zu\"\n", id);
    return true;
}

static bool emit_assignment_rhs(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent) {
    if (value->kind == DS_LOWER_EXPR_RUN) {
        if (value->as.run.stages.len > 1) {
            if (!emit_capture_pipeline_assignment(e, name, &value->as.run, value->span, indent)) return false;
            emit_type_assignment_for_expr(e, name, value, indent, false);
            buf_append(&e->out, "\n");
            return true;
        }
        emit_indent(&e->out, indent);
        buf_append(&e->out, "__ds_capture ");
        emit_var_name(&e->out, name);
        if (!emit_capture_command(e, &value->as.run, &e->out, value->span)) return false;
        buf_append(&e->out, "\n");
        emit_type_assignment_for_expr(e, name, value, indent, false);
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
        emit_stdlib_helper_name(&e->out, value->as.call.name);
        if (!emit_call_args(e, &value->as.call.args, &e->out)) return false;
        buf_append(&e->out, "\n");
        emit_type_assignment_for_expr(e, name, value, indent, false);
        buf_append(&e->out, "\n");
        return true;
    }
    emit_indent(&e->out, indent);
    emit_var_name(&e->out, name);
    buf_append(&e->out, "=");
    if (!emit_value_expr(e, value, &e->out)) return false;
    buf_append(&e->out, "\n");
    emit_type_assignment_for_expr(e, name, value, indent, false);
    buf_append(&e->out, "\n");
    return true;
}

static bool emit_case_pattern_condition(BashEmitter *e, const DsLowerExpr *selector, const DsLowerCasePattern *pattern, EmitBuf *out) {
    buf_append(out, "[[ ");
    if (selector->kind == DS_LOWER_EXPR_IDENT) {
        buf_append(out, "\"${");
        emit_type_var_name(out, selector->as.text);
        buf_append(out, ":-unknown}\"");
    } else {
        bash_single_quote(out, expr_type_name(selector), strlen(expr_type_name(selector)));
    }
    buf_append(out, " == ");
    if (pattern->kind == DS_LOWER_CASE_PATTERN_STRING) bash_single_quote(out, "string", 6);
    else if (pattern->kind == DS_LOWER_CASE_PATTERN_INT) bash_single_quote(out, "int", 3);
    else if (pattern->kind == DS_LOWER_CASE_PATTERN_BOOL) bash_single_quote(out, "bool", 4);
    buf_append(out, " && ");
    if (!emit_condition_operand(e, selector, out)) return false;
    buf_append(out, " == ");
    if (pattern->kind == DS_LOWER_CASE_PATTERN_STRING) {
        char *decoded = NULL; size_t len = 0;
        DsLowerExpr tmp = {.kind = DS_LOWER_EXPR_STRING, .span = pattern->span};
        tmp.as.text = pattern->text;
        if (!decode_string_literal(e->diag, &tmp, &decoded, &len)) return false;
        bash_single_quote(out, decoded, len);
        free(decoded);
    } else if (pattern->kind == DS_LOWER_CASE_PATTERN_INT) {
        buf_append_len(out, pattern->text.data, pattern->text.len);
    } else if (pattern->kind == DS_LOWER_CASE_PATTERN_BOOL) {
        buf_append(out, pattern->boolean ? "true" : "false");
    }
    buf_append(out, " ]]");
    return true;
}

bool emit_stmt(BashEmitter *e, const DsLowerStmt *stmt, int indent) {
    emit_indent(&e->out, indent);
    const DsSource *stmt_source = stmt->span.source ? stmt->span.source : e->source;
    buf_appendf(&e->out, "# ds: %s:%d\n", stmt_source && stmt_source->path ? stmt_source->path : "<source>", stmt->span.start.line);

    switch (stmt->kind) {
        case DS_LOWER_STMT_LET:
            if (!is_safe_identifier(stmt->as.let_stmt.name)) {
                ds_diag_error(e->diag, stmt->span, "cannot emit unsafe Bash variable name `%.*s`", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);
                return false;
            }
            emit_indent(&e->out, indent);
            if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_RUN) {
                if (e->function_depth > 0) {
                    buf_append(&e->out, "local ");
                    emit_var_name(&e->out, stmt->as.let_stmt.name);
                    buf_append(&e->out, "_stdout ");
                    emit_var_name(&e->out, stmt->as.let_stmt.name);
                    buf_append(&e->out, "_stderr ");
                    emit_var_name(&e->out, stmt->as.let_stmt.name);
                    buf_append(&e->out, "_code ");
                    emit_var_name(&e->out, stmt->as.let_stmt.name);
                    buf_append(&e->out, "_ok ");
                    emit_var_name(&e->out, stmt->as.let_stmt.name);
                    buf_append(&e->out, "_failed\n");
                    emit_indent(&e->out, indent);
                }
                if (stmt->as.let_stmt.value->as.run.stages.len > 1) {
                    if (!emit_capture_pipeline_assignment(e, stmt->as.let_stmt.name, &stmt->as.let_stmt.value->as.run, stmt->as.let_stmt.value->span, indent)) return false;
                } else {
                    buf_append(&e->out, "__ds_capture ");
                    emit_var_name(&e->out, stmt->as.let_stmt.name);
                    if (!emit_capture_command(e, &stmt->as.let_stmt.value->as.run, &e->out, stmt->as.let_stmt.value->span)) return false;
                }
            } else if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_ARRAY) {
                if (e->function_depth > 0) buf_append(&e->out, "local -a ");
                else buf_append(&e->out, "declare -a ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=");
                if (!emit_array_elements(e, &stmt->as.let_stmt.value->as.array.elements, &e->out)) return false;
            } else if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_CALL && stdlib_returns_array(stmt->as.let_stmt.value->as.call.name)) {
                if (e->function_depth > 0) buf_append(&e->out, "local -a ");
                else buf_append(&e->out, "declare -a ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=()\n");
                emit_indent(&e->out, indent);
                size_t temp_id = e->temp_counter++;
                buf_appendf(&e->out, "__ds_iter_%zu=$(mktemp)\n", temp_id);
                emit_indent(&e->out, indent);
                emit_stdlib_helper_name(&e->out, stmt->as.let_stmt.value->as.call.name);
                if (!emit_call_args(e, &stmt->as.let_stmt.value->as.call.args, &e->out)) return false;
                buf_appendf(&e->out, " >\"$__ds_iter_%zu\"\n", temp_id);
                emit_indent(&e->out, indent);
                buf_append(&e->out, "while IFS= read -r __ds_line; do ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "+=(\"$__ds_line\"); done");
                buf_appendf(&e->out, " <\"$__ds_iter_%zu\"\n", temp_id);
                emit_indent(&e->out, indent);
                buf_appendf(&e->out, "rm -f \"$__ds_iter_%zu\"", temp_id);
            } else if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(stmt->as.let_stmt.value->as.call.name)) {
                if (e->function_depth > 0) buf_append(&e->out, "local ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=\"\"\n");
                emit_indent(&e->out, indent);
                buf_append(&e->out, "__ds_stdlib_capture ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, " ");
                emit_stdlib_helper_name(&e->out, stmt->as.let_stmt.value->as.call.name);
                if (!emit_call_args(e, &stmt->as.let_stmt.value->as.call.args, &e->out)) return false;
            } else if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_MAP) {
                if (e->function_depth > 0) buf_append(&e->out, "local -A ");
                else buf_append(&e->out, "declare -A ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=");
                if (!emit_map_entries(e, &stmt->as.let_stmt.value->as.map.entries, &e->out)) return false;
            } else {
                if (e->function_depth > 0) buf_append(&e->out, "local ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=");
                if (!emit_value_expr(e, stmt->as.let_stmt.value, &e->out)) return false;
            }
            buf_append(&e->out, "\n");
            emit_type_assignment_for_expr(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, indent, e->function_depth > 0);
            buf_append(&e->out, "\n");
            if (!symbol_exists(&e->symbols, stmt->as.let_stmt.name)) {
                DsStr copy = {ds_str_dup_range(stmt->as.let_stmt.name.data, stmt->as.let_stmt.name.len), stmt->as.let_stmt.name.len};
                symbol_vec_push(&e->symbols, copy);
            }
            return true;

        case DS_LOWER_STMT_ASSIGN:
            if (!is_safe_identifier(stmt->as.assign_stmt.name)) {
                ds_diag_error(e->diag, stmt->span, "cannot emit unsafe Bash variable name `%.*s`", (int)stmt->as.assign_stmt.name.len, stmt->as.assign_stmt.name.data);
                return false;
            }
            if (stmt->as.assign_stmt.op == DS_LOWER_ASSIGN_SET) {
                return emit_assignment_rhs(e, stmt->as.assign_stmt.name, stmt->as.assign_stmt.value, indent);
            }
            emit_indent(&e->out, indent);
            emit_var_name(&e->out, stmt->as.assign_stmt.name);
            buf_append(&e->out, "=\"$(__ds_int_bin ");
            const char *op = stmt->as.assign_stmt.op == DS_LOWER_ASSIGN_ADD ? "+" :
                             (stmt->as.assign_stmt.op == DS_LOWER_ASSIGN_SUB ? "-" :
                              (stmt->as.assign_stmt.op == DS_LOWER_ASSIGN_MUL ? "*" :
                               (stmt->as.assign_stmt.op == DS_LOWER_ASSIGN_DIV ? "/" : "%")));
            bash_single_quote(&e->out, op, strlen(op));
            buf_append(&e->out, " \"$");
            emit_var_name(&e->out, stmt->as.assign_stmt.name);
            buf_append(&e->out, "\" ");
            if (!emit_value_expr(e, stmt->as.assign_stmt.value, &e->out)) return false;
            buf_append(&e->out, ")\"\n");
            emit_type_assignment(e, stmt->as.assign_stmt.name, "int", indent, false);
            buf_append(&e->out, "\n");
            return true;

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
            if (e->has_cleanup_helpers && (is_control_command(&stmt->as.cmd_stmt, "fail") || is_control_command(&stmt->as.cmd_stmt, "exit"))) {
                return emit_control_command(e, &stmt->as.cmd_stmt, stmt->span, indent);
            }
            if (e->has_signal_handlers && can_emit_direct_signal_command(&stmt->as.cmd_stmt)) {
                return emit_direct_signal_command(e, &stmt->as.cmd_stmt, stmt->span, indent);
            }
            if (e->has_signal_handlers && stmt->as.cmd_stmt.stages.len > 1) {
                return emit_signal_pipeline(e, &stmt->as.cmd_stmt, stmt->span, indent);
            }
            emit_indent(&e->out, indent);
            if (!emit_command_pipeline(e, &stmt->as.cmd_stmt, &e->out, stmt->span)) return false;
            if (stmt->as.cmd_stmt.stages.len > 1) {
                buf_append(&e->out, " || { __ds_code=$?; printf '%s: error: pipeline failed with exit %s\\n' ");
                emit_source_loc(&e->out, e->source, stmt->span);
                buf_append(&e->out, " \"$__ds_code\" >&2; ");
                if (e->handler_depth > 0) buf_append(&e->out, "return \"$__ds_code\"; }");
                else buf_append(&e->out, "exit \"$__ds_code\"; }");
                buf_append(&e->out, "\n\n");
            } else {
                buf_append(&e->out, " || __ds_fail ");
                emit_source_loc(&e->out, e->source, stmt->span);
                buf_append(&e->out, " \"$?\"");
                if (e->handler_depth > 0) buf_append(&e->out, " || return $?\n\n");
                else buf_append(&e->out, "\n\n");
            }
            return true;

        case DS_LOWER_STMT_CALL:
            emit_indent(&e->out, indent);
            if (ds_stdlib_is_name(stmt->as.call_stmt.name)) emit_stdlib_helper_name(&e->out, stmt->as.call_stmt.name);
            else emit_fn_name(&e->out, stmt->as.call_stmt.name);
            if (ds_stdlib_is_name(stmt->as.call_stmt.name)) {
                if (!emit_call_args(e, &stmt->as.call_stmt.args, &e->out)) return false;
            } else {
                if (!emit_user_call_args(e, &stmt->as.call_stmt.args, &e->out)) return false;
            }
            buf_append(&e->out, "\n\n");
            return true;

        case DS_LOWER_STMT_PUSH:
            emit_indent(&e->out, indent);
            emit_var_name(&e->out, stmt->as.push_stmt.name);
            buf_append(&e->out, "+=(");
            if (!emit_value_expr(e, stmt->as.push_stmt.value, &e->out)) return false;
            buf_append(&e->out, ")\n\n");
            return true;

        case DS_LOWER_STMT_FOR_ARRAY: {
            emit_indent(&e->out, indent);
            if (stmt->as.for_stmt.iterable->kind != DS_LOWER_EXPR_IDENT && !(stmt->as.for_stmt.iterable->kind == DS_LOWER_EXPR_CALL && stdlib_returns_array(stmt->as.for_stmt.iterable->as.call.name))) {
                ds_diag_error(e->diag, stmt->span, "Bash emission only supports looping over named arrays in v0.10.0");
                return false;
            }
            size_t temp_id = 0;
            if (stmt->as.for_stmt.iterable->kind == DS_LOWER_EXPR_IDENT) {
                buf_append(&e->out, "for ");
                emit_var_name(&e->out, stmt->as.for_stmt.name);
                buf_append(&e->out, " in ");
                buf_append(&e->out, "\"${");
                emit_var_name(&e->out, stmt->as.for_stmt.iterable->as.text);
                buf_append(&e->out, "[@]}\"; do\n");
            } else {
                temp_id = e->temp_counter++;
                buf_appendf(&e->out, "__ds_iter_%zu=$(mktemp)\n", temp_id);
                emit_indent(&e->out, indent);
                emit_stdlib_helper_name(&e->out, stmt->as.for_stmt.iterable->as.call.name);
                if (!emit_call_args(e, &stmt->as.for_stmt.iterable->as.call.args, &e->out)) return false;
                buf_appendf(&e->out, " >\"$__ds_iter_%zu\"\n", temp_id);
                emit_indent(&e->out, indent);
                buf_append(&e->out, "while IFS= read -r ");
                emit_var_name(&e->out, stmt->as.for_stmt.name);
                buf_append(&e->out, "; do\n");
            }
            size_t mark = e->symbols.len;
            DsStr copy = {ds_str_dup_range(stmt->as.for_stmt.name.data, stmt->as.for_stmt.name.len), stmt->as.for_stmt.name.len};
            symbol_vec_push(&e->symbols, copy);
            emit_type_assignment(e, stmt->as.for_stmt.name, lower_value_type_name(stmt->as.for_stmt.element_kind), indent + 1, false);
            if (!emit_block_body(e, stmt->as.for_stmt.body, indent + 1)) { symbols_truncate(&e->symbols, mark); return false; }
            symbols_truncate(&e->symbols, mark);
            emit_indent(&e->out, indent);
            if (stmt->as.for_stmt.iterable->kind == DS_LOWER_EXPR_IDENT) buf_append(&e->out, "done\n\n");
            else {
                buf_appendf(&e->out, "done <\"$__ds_iter_%zu\"\n", temp_id);
                emit_indent(&e->out, indent);
                buf_appendf(&e->out, "rm -f \"$__ds_iter_%zu\"\n\n", temp_id);
            }
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
            DsStr copy = {ds_str_dup_range(stmt->as.for_stmt.name.data, stmt->as.for_stmt.name.len), stmt->as.for_stmt.name.len};
            symbol_vec_push(&e->symbols, copy);
            emit_type_assignment(e, stmt->as.for_stmt.name, "int", indent + 1, false);
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

        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsLowerCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                bool is_default = arm->patterns.len > 0 && arm->patterns.items[0].kind == DS_LOWER_CASE_PATTERN_DEFAULT;
                emit_indent(&e->out, indent);
                if (is_default) {
                    if (i == 0) buf_append(&e->out, "if true; then\n");
                    else buf_append(&e->out, "else\n");
                } else {
                    if (i == 0) buf_append(&e->out, "if ");
                    else buf_append(&e->out, "elif ");
                    for (size_t j = 0; j < arm->patterns.len; j++) {
                        if (j) buf_append(&e->out, " || ");
                        if (!emit_case_pattern_condition(e, stmt->as.case_stmt.selector, &arm->patterns.items[j], &e->out)) return false;
                    }
                    buf_append(&e->out, "; then\n");
                }
                if (!emit_block_body(e, arm->body, indent + 1)) return false;
            }
            emit_indent(&e->out, indent);
            buf_append(&e->out, "fi\n\n");
            return true;

        case DS_LOWER_STMT_BLOCK:
            return emit_block_body(e, stmt, indent);
        case DS_LOWER_STMT_ASSERT:
            ds_diag_error(e->diag, stmt->span, "assert statements are only emitted by the test runner in v0.14.0");
            return false;
        case DS_LOWER_STMT_RETURN:
            emit_indent(&e->out, indent);
            buf_append(&e->out, "__ds_return=");
            if (!emit_value_expr(e, stmt->as.return_stmt.value, &e->out)) return false;
            buf_append(&e->out, "\n");
            emit_indent(&e->out, indent);
            buf_append(&e->out, "return 0\n\n");
            return true;
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: {
            size_t id = e->handler_counter++;
            const char *sig = handler_signal_name(stmt->as.handler_stmt.signal);
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
