#include "bash_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

bool emit_block_body(BashEmitter *e, const DsLowerStmt *block, int indent) {
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) {
        if (!emit_stmt(e, block->as.block_stmt.statements.items[i], indent)) return false;
    }
    return true;
}

static const char *lower_value_type_name(DsLowerValueKind kind);

typedef struct {
    bool active;
    char raw_name[64];
    DsLowerValueKind kind;
} MaterializedArg;

static bool is_user_function_call_expr(const DsLowerExpr *expr) {
    return expr && expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.is_user_function;
}

static void temp_ds_name(char *buf, size_t cap, const char *prefix, size_t id) {
    snprintf(buf, cap, "__%s_%zu", prefix, id);
}

static bool is_int_binary_op(DsStr op) {
    return str_eq(op, "+") || str_eq(op, "-") || str_eq(op, "*") ||
           str_eq(op, "/") || str_eq(op, "%") || str_eq(op, "**");
}

static const char *stmt_expr_type_name(const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INTERP: return "string";
        case DS_LOWER_EXPR_INT: return "int";
        case DS_LOWER_EXPR_BOOL: return "bool";
        case DS_LOWER_EXPR_ARRAY: return "array";
        case DS_LOWER_EXPR_MAP: return "map";
        case DS_LOWER_EXPR_RUN: return "command_result";
        case DS_LOWER_EXPR_BINARY: return is_int_binary_op(expr->as.binary.op) ? "int" : "bool";
        case DS_LOWER_EXPR_UNARY:
            if (str_eq(expr->as.unary.op, "!")) return "bool";
            if (str_eq(expr->as.unary.op, "-")) return "int";
            return "unknown";
        case DS_LOWER_EXPR_CALL: return lower_value_type_name(expr->as.call.return_kind);
        case DS_LOWER_EXPR_INDEX: return lower_value_type_name(expr->as.index.element_kind);
        default: return "unknown";
    }
}

static bool emit_user_call_into_raw_var(BashEmitter *e, const DsLowerExpr *expr, DsStr raw_name, int indent) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "__ds_call_value_into ");
    emit_var_name(&e->out, raw_name);
    buf_append(&e->out, " ");
    bash_single_quote(&e->out, lower_value_type_name(expr->as.call.return_kind), strlen(lower_value_type_name(expr->as.call.return_kind)));
    buf_append(&e->out, " ");
    emit_fn_name(&e->out, expr->as.call.name);
    if (!emit_user_call_args(e, &expr->as.call.args, &e->out)) return false;
    buf_append(&e->out, "\n");
    return true;
}

static bool emit_materialized_user_call_args(BashEmitter *e, const DsLowerExprVec *args, MaterializedArg *mats, int indent) {
    for (size_t i = 0; i < args->len; i++) {
        mats[i].active = false;
        if (!is_user_function_call_expr(args->items[i])) continue;
        mats[i].active = true;
        mats[i].kind = args->items[i]->as.call.return_kind;
        temp_ds_name(mats[i].raw_name, sizeof(mats[i].raw_name), "arg", e->temp_counter++);
        DsStr raw = {mats[i].raw_name, strlen(mats[i].raw_name)};
        emit_indent(&e->out, indent);
        if (e->function_depth > 0) buf_append(&e->out, "local ");
        emit_var_name(&e->out, raw);
        buf_append(&e->out, "=\"\"\n");
        if (!emit_user_call_into_raw_var(e, args->items[i], raw, indent)) return false;
    }
    return true;
}

static bool emit_user_call_args_with_materialized(BashEmitter *e, const DsLowerExprVec *args, const MaterializedArg *mats, EmitBuf *out) {
    for (size_t i = 0; i < args->len; i++) {
        buf_append(out, " ");
        if (mats && mats[i].active) {
            DsStr raw = {(char *)mats[i].raw_name, strlen(mats[i].raw_name)};
            buf_append(out, "\"$");
            emit_var_name(out, raw);
            buf_append(out, "\" ");
            bash_single_quote(out, lower_value_type_name(mats[i].kind), strlen(lower_value_type_name(mats[i].kind)));
            continue;
        }
        if (!emit_call_arg_expr(e, args->items[i], out)) return false;
        buf_append(out, " ");
        if (args->items[i]->kind == DS_LOWER_EXPR_IDENT) {
            buf_append(out, "\"${");
            buf_append(out, "__ds_type_");
            buf_append_len(out, args->items[i]->as.text.data, args->items[i]->as.text.len);
            buf_append(out, ":-unknown}\"");
        } else {
            const char *type = stmt_expr_type_name(args->items[i]);
            bash_single_quote(out, type, strlen(type));
        }
    }
    return true;
}

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
            return lower_value_type_name(expr->as.call.return_kind);
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

static void emit_map_value_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_value_type_");
    buf_append_len(out, name.data, name.len);
}

static void emit_array_element_type_entries(BashEmitter *e, const DsLowerExprVec *elements, int indent, const char *target_name) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "declare -ga ");
    buf_append(&e->out, target_name);
    buf_append(&e->out, "=(");
    for (size_t i = 0; i < elements->len; i++) {
        if (i) buf_append(&e->out, " ");
        const char *type = stmt_expr_type_name(elements->items[i]);
        bash_single_quote(&e->out, type, strlen(type));
    }
    buf_append(&e->out, ")\n");
}

static void emit_map_value_type_entries(BashEmitter *e, const DsLowerMapEntryVec *entries, int indent, const char *target_name) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "declare -gA ");
    buf_append(&e->out, target_name);
    buf_append(&e->out, "=(");
    for (size_t i = 0; i < entries->len; i++) {
        if (i) buf_append(&e->out, " ");
        buf_append(&e->out, "[");
        bash_single_quote(&e->out, entries->items[i].key.data, entries->items[i].key.len);
        buf_append(&e->out, "]=");
        const char *type = stmt_expr_type_name(entries->items[i].value);
        bash_single_quote(&e->out, type, strlen(type));
    }
    buf_append(&e->out, ")\n");
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
    } else if (value->kind == DS_LOWER_EXPR_MAP) {
        emit_indent(&e->out, indent);
        if (local_decl) buf_append(&e->out, "local -A ");
        else buf_append(&e->out, "declare -A ");
        emit_map_value_type_var_name(&e->out, name);
        buf_append(&e->out, "=(");
        for (size_t i = 0; i < value->as.map.entries.len; i++) {
            if (i) buf_append(&e->out, " ");
            buf_append(&e->out, "[");
            bash_single_quote(&e->out, value->as.map.entries.items[i].key.data, value->as.map.entries.items[i].key.len);
            buf_append(&e->out, "]=");
            const char *type = stmt_expr_type_name(value->as.map.entries.items[i].value);
            bash_single_quote(&e->out, type, strlen(type));
        }
        buf_append(&e->out, ")\n");
    } else if (value->kind == DS_LOWER_EXPR_INDEX && value->as.index.object->kind == DS_LOWER_EXPR_IDENT) {
        emit_indent(&e->out, indent);
        if (local_decl) buf_append(&e->out, "local ");
        emit_type_var_name(&e->out, name);
        buf_append(&e->out, "=");
        if (value->as.index.object_is_array) {
            buf_append(&e->out, "\"${");
            emit_elem_type_var_name(&e->out, value->as.index.object->as.text);
            buf_append(&e->out, "[");
            if (value->as.index.index->kind == DS_LOWER_EXPR_INT) {
                buf_append_len(&e->out, value->as.index.index->as.text.data, value->as.index.index->as.text.len);
            } else if (value->as.index.index->kind == DS_LOWER_EXPR_IDENT) {
                buf_append(&e->out, "$");
                emit_var_name(&e->out, value->as.index.index->as.text);
            } else {
                buf_append(&e->out, "0");
            }
            buf_append(&e->out, "]:-unknown}\"\n");
        } else if (value->as.index.object_is_map) {
            buf_append(&e->out, "\"${");
            emit_map_value_type_var_name(&e->out, value->as.index.object->as.text);
            buf_append(&e->out, "[");
            if (value->as.index.map_key_literal) {
                bash_single_quote(&e->out, value->as.index.map_key.data, value->as.index.map_key.len);
            } else if (value->as.index.index->kind == DS_LOWER_EXPR_IDENT) {
                buf_append(&e->out, "$");
                emit_var_name(&e->out, value->as.index.index->as.text);
            } else {
                buf_append(&e->out, "");
            }
            buf_append(&e->out, "]:-unknown}\"\n");
        }
    }
}

static void emit_command_result_storage_decl(BashEmitter *e, DsStr name, int indent, bool local_decl) {
    emit_indent(&e->out, indent);
    if (local_decl) {
        buf_append(&e->out, "local ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_stdout ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_stderr ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_code ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_status ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_ok ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_failed\n");
        return;
    }
    emit_var_name(&e->out, name); buf_append(&e->out, "_stdout=\"\"; ");
    emit_var_name(&e->out, name); buf_append(&e->out, "_stderr=\"\"; ");
    emit_var_name(&e->out, name); buf_append(&e->out, "_code=0; ");
    emit_var_name(&e->out, name); buf_append(&e->out, "_status=0; ");
    emit_var_name(&e->out, name); buf_append(&e->out, "_ok=false; ");
    emit_var_name(&e->out, name); buf_append(&e->out, "_failed=true\n");
}

static bool emit_structured_target_decl(BashEmitter *e, DsStr name, DsLowerValueKind kind, int indent, bool local_decl) {
    switch (kind) {
        case DS_LOWER_VALUE_ARRAY:
            emit_indent(&e->out, indent);
            buf_append(&e->out, local_decl ? "local -a " : "declare -a ");
            emit_var_name(&e->out, name);
            buf_append(&e->out, "=()\n");
            emit_indent(&e->out, indent);
            buf_append(&e->out, local_decl ? "local -a " : "declare -a ");
            emit_elem_type_var_name(&e->out, name);
            buf_append(&e->out, "=()\n");
            return true;
        case DS_LOWER_VALUE_MAP:
            emit_indent(&e->out, indent);
            buf_append(&e->out, local_decl ? "local -A " : "declare -A ");
            emit_var_name(&e->out, name);
            buf_append(&e->out, "=()\n");
            emit_indent(&e->out, indent);
            buf_append(&e->out, local_decl ? "local -A " : "declare -A ");
            emit_map_value_type_var_name(&e->out, name);
            buf_append(&e->out, "=()\n");
            return true;
        case DS_LOWER_VALUE_COMMAND_RESULT:
            emit_command_result_storage_decl(e, name, indent, local_decl);
            return true;
        default:
            emit_indent(&e->out, indent);
            if (local_decl) buf_append(&e->out, "local ");
            emit_var_name(&e->out, name);
            buf_append(&e->out, "=\"\"\n");
            return true;
    }
}

static bool emit_user_function_value_call_into(BashEmitter *e, DsStr name, const DsLowerExpr *call, int indent) {
    MaterializedArg *mats = NULL;
    if (call->as.call.args.len > 0) {
        mats = calloc(call->as.call.args.len, sizeof(*mats));
        if (!mats) return false;
        if (!emit_materialized_user_call_args(e, &call->as.call.args, mats, indent)) { free(mats); return false; }
    }
    emit_indent(&e->out, indent);
    buf_append(&e->out, "__ds_call_value_into ");
    emit_var_name(&e->out, name);
    buf_append(&e->out, " ");
    bash_single_quote(&e->out, lower_value_type_name(call->as.call.return_kind), strlen(lower_value_type_name(call->as.call.return_kind)));
    buf_append(&e->out, " ");
    emit_fn_name(&e->out, call->as.call.name);
    bool ok = emit_user_call_args_with_materialized(e, &call->as.call.args, mats, &e->out);
    free(mats);
    if (!ok) return false;
    buf_append(&e->out, "\n");
    return true;
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
    buf_appendf(&e->out, "rm -rf \"$__ds_tmpdir_%zu\"\n", id);
    return true;
}

static bool emit_assignment_rhs(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent) {
    if (value->kind == DS_LOWER_EXPR_INTERP) {
        bool has_user_call = false;
        for (size_t i = 0; i < value->as.interp.parts.len; i++) {
            if (is_user_function_call_expr(value->as.interp.parts.items[i])) { has_user_call = true; break; }
        }
        if (has_user_call) {
            char **temps = calloc(value->as.interp.parts.len ? value->as.interp.parts.len : 1, sizeof(char *));
            if (!temps) return false;
            for (size_t i = 0; i < value->as.interp.parts.len; i++) {
                const DsLowerExpr *part = value->as.interp.parts.items[i];
                if (!is_user_function_call_expr(part)) continue;
                char tmp[64];
                temp_ds_name(tmp, sizeof(tmp), "interp", e->temp_counter++);
                temps[i] = ds_str_dup_range(tmp, strlen(tmp));
                if (!temps[i]) { free(temps); return false; }
                DsStr raw = {temps[i], strlen(temps[i])};
                emit_indent(&e->out, indent);
                if (e->function_depth > 0) buf_append(&e->out, "local ");
                emit_var_name(&e->out, raw);
                buf_append(&e->out, "=\"\"\n");
                if (!emit_user_call_into_raw_var(e, part, raw, indent)) {
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
            emit_type_assignment_for_expr(e, name, value, indent, false);
            buf_append(&e->out, "\n");
            return true;
        }
    }
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
    if (value->kind == DS_LOWER_EXPR_CALL && value->as.call.is_user_function) {
        MaterializedArg *mats = NULL;
        if (value->as.call.args.len > 0) {
            mats = calloc(value->as.call.args.len, sizeof(*mats));
            if (!mats) return false;
            if (!emit_materialized_user_call_args(e, &value->as.call.args, mats, indent)) { free(mats); return false; }
        }
        emit_indent(&e->out, indent);
        emit_var_name(&e->out, name);
        buf_append(&e->out, "=\"\"\n");
        emit_indent(&e->out, indent);
        buf_append(&e->out, "__ds_call_value_into ");
        emit_var_name(&e->out, name);
        buf_append(&e->out, " ");
        bash_single_quote(&e->out, lower_value_type_name(value->as.call.return_kind), strlen(lower_value_type_name(value->as.call.return_kind)));
        buf_append(&e->out, " ");
        emit_fn_name(&e->out, value->as.call.name);
        bool ok = emit_user_call_args_with_materialized(e, &value->as.call.args, mats, &e->out);
        free(mats);
        if (!ok) return false;
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

static bool interp_has_user_function_call(const DsLowerExpr *value) {
    if (!value || value->kind != DS_LOWER_EXPR_INTERP) return false;
    for (size_t i = 0; i < value->as.interp.parts.len; i++) {
        if (is_user_function_call_expr(value->as.interp.parts.items[i])) return true;
    }
    return false;
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
            if (interp_has_user_function_call(stmt->as.let_stmt.value)) {
                if (e->function_depth > 0) {
                    emit_indent(&e->out, indent);
                    buf_append(&e->out, "local ");
                    emit_var_name(&e->out, stmt->as.let_stmt.name);
                    buf_append(&e->out, "=\"\"\n");
                }
                if (!emit_assignment_rhs(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, indent)) return false;
                if (!symbol_exists(&e->symbols, stmt->as.let_stmt.name)) {
                    DsStr copy = {ds_str_dup_range(stmt->as.let_stmt.name.data, stmt->as.let_stmt.name.len), stmt->as.let_stmt.name.len};
                    symbol_vec_push(&e->symbols, copy);
                }
                return true;
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
                if (e->function_depth > 0) buf_append(&e->out, "local -a ");
                else buf_append(&e->out, "declare -a ");
                emit_elem_type_var_name(&e->out, stmt->as.let_stmt.name);
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
                buf_append(&e->out, "+=(\"$__ds_line\"); ");
                emit_elem_type_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "+=(\"string\"); done");
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
            } else if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_CALL && stmt->as.let_stmt.value->as.call.is_user_function) {
                if (!emit_structured_target_decl(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value->as.call.return_kind, indent, e->function_depth > 0)) return false;
                if (!emit_user_function_value_call_into(e, stmt->as.let_stmt.name, stmt->as.let_stmt.value, indent)) return false;
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
                    if (!emit_user_function_value_call_into(e, tmp, stmt->as.assign_stmt.value, indent)) return false;
                } else {
                    if (!emit_value_expr(e, stmt->as.assign_stmt.value, &e->out)) return false;
                    buf_append(&e->out, "\n");
                }
                emit_indent(&e->out, indent);
                buf_append(&e->out, "export ");
                buf_append_len(&e->out, env_name.data, env_name.len);
                buf_append(&e->out, "=\"$");
                emit_var_name(&e->out, tmp);
                buf_append(&e->out, "\"\n");
                return true;
            }
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
            bool is_multi = stmt->as.cmd_stmt.stages.len > 1;
            if (is_multi) buf_append(&e->out, "( if [[ -t 0 ]]; then exec </dev/null; fi; ");
            if (!emit_command_pipeline(e, &stmt->as.cmd_stmt, &e->out, stmt->span)) return false;
            if (is_multi) {
                buf_append(&e->out, " ) || { __ds_code=$?; printf '%s: error: pipeline failed with exit %s\\n' ");
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

        case DS_LOWER_STMT_CALL: {
            MaterializedArg *call_mats = NULL;
            if (!ds_stdlib_is_name(stmt->as.call_stmt.name) && stmt->as.call_stmt.args.len > 0) {
                call_mats = calloc(stmt->as.call_stmt.args.len, sizeof(*call_mats));
                if (!call_mats) return false;
                if (!emit_materialized_user_call_args(e, &stmt->as.call_stmt.args, call_mats, indent)) { free(call_mats); return false; }
            }
            emit_indent(&e->out, indent);
            if (ds_stdlib_is_name(stmt->as.call_stmt.name)) emit_stdlib_helper_name(&e->out, stmt->as.call_stmt.name);
            else emit_fn_name(&e->out, stmt->as.call_stmt.name);
            if (ds_stdlib_is_name(stmt->as.call_stmt.name)) {
                if (!emit_call_args(e, &stmt->as.call_stmt.args, &e->out)) return false;
            } else {
                bool ok = emit_user_call_args_with_materialized(e, &stmt->as.call_stmt.args, call_mats, &e->out);
                free(call_mats);
                if (!ok) return false;
            }
            buf_append(&e->out, "\n\n");
            return true;
        }

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
                /* Lowering rejects non-portable array iterables for VM/Bash parity. */
                ds_diag_error(e->diag, stmt->span, "internal Bash invariant failed: array loop iterable should be named or a known stdlib array result after lowering");
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

        case DS_LOWER_STMT_CASE: {
            const DsLowerExpr *case_selector = stmt->as.case_stmt.selector;
            DsLowerExpr case_temp_expr;
            char case_temp_buf[64];
            size_t case_symbol_len = e->symbols.len;
            if (is_user_function_call_expr(stmt->as.case_stmt.selector)) {
                temp_ds_name(case_temp_buf, sizeof(case_temp_buf), "case", e->temp_counter++);
                DsStr raw = {case_temp_buf, strlen(case_temp_buf)};
                emit_indent(&e->out, indent);
                if (e->function_depth > 0) buf_append(&e->out, "local ");
                emit_var_name(&e->out, raw);
                buf_append(&e->out, "=\"\"\n");
                if (!emit_user_call_into_raw_var(e, stmt->as.case_stmt.selector, raw, indent)) return false;
                emit_indent(&e->out, indent);
                buf_append(&e->out, "__ds_type_");
                buf_append_len(&e->out, raw.data, raw.len);
                buf_append(&e->out, "=");
                bash_single_quote(&e->out, lower_value_type_name(stmt->as.case_stmt.selector->as.call.return_kind), strlen(lower_value_type_name(stmt->as.case_stmt.selector->as.call.return_kind)));
                buf_append(&e->out, "\n");
                case_temp_expr = *stmt->as.case_stmt.selector;
                case_temp_expr.kind = DS_LOWER_EXPR_IDENT;
                case_temp_expr.as.text = raw;
                DsStr copy = {ds_str_dup_range(raw.data, raw.len), raw.len};
                symbol_vec_push(&e->symbols, copy);
                case_selector = &case_temp_expr;
            }
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
            ds_diag_error(e->diag, stmt->span, "assert statements are only emitted by the test runner in v0.14.0");
            return false;
        case DS_LOWER_STMT_RETURN:
            emit_indent(&e->out, indent);
            buf_append(&e->out, "__ds_return_type=");
            bash_single_quote(&e->out, lower_value_type_name(stmt->as.return_stmt.return_kind), strlen(lower_value_type_name(stmt->as.return_stmt.return_kind)));
            buf_append(&e->out, "\n");
            emit_indent(&e->out, indent);
            if (stmt->as.return_stmt.value->kind == DS_LOWER_EXPR_CALL && stmt->as.return_stmt.value->as.call.is_user_function) {
                MaterializedArg *mats = NULL;
                if (stmt->as.return_stmt.value->as.call.args.len > 0) {
                    mats = calloc(stmt->as.return_stmt.value->as.call.args.len, sizeof(*mats));
                    if (!mats) return false;
                    if (!emit_materialized_user_call_args(e, &stmt->as.return_stmt.value->as.call.args, mats, indent)) { free(mats); return false; }
                }
                buf_append(&e->out, "__ds_call_value_capture ");
                bash_single_quote(&e->out, lower_value_type_name(stmt->as.return_stmt.return_kind), strlen(lower_value_type_name(stmt->as.return_stmt.return_kind)));
                buf_append(&e->out, " ");
                emit_fn_name(&e->out, stmt->as.return_stmt.value->as.call.name);
                bool ok = emit_user_call_args_with_materialized(e, &stmt->as.return_stmt.value->as.call.args, mats, &e->out);
                free(mats);
                if (!ok) return false;
                buf_append(&e->out, "\n");
                emit_indent(&e->out, indent);
                buf_append(&e->out, "__ds_return_type=");
                bash_single_quote(&e->out, lower_value_type_name(stmt->as.return_stmt.return_kind), strlen(lower_value_type_name(stmt->as.return_stmt.return_kind)));
                buf_append(&e->out, "\n");
            } else if (stmt->as.return_stmt.return_kind == DS_LOWER_VALUE_ARRAY) {
                if (stmt->as.return_stmt.value->kind == DS_LOWER_EXPR_ARRAY) {
                    buf_append(&e->out, "declare -ga __ds_return_array=");
                    if (!emit_array_elements(e, &stmt->as.return_stmt.value->as.array.elements, &e->out)) return false;
                    buf_append(&e->out, "\n");
                    emit_array_element_type_entries(e, &stmt->as.return_stmt.value->as.array.elements, indent, "__ds_return_elem_type");
                } else if (stmt->as.return_stmt.value->kind == DS_LOWER_EXPR_IDENT) {
                    buf_append(&e->out, "declare -ga __ds_return_array=(\"${");
                    emit_var_name(&e->out, stmt->as.return_stmt.value->as.text);
                    buf_append(&e->out, "[@]}\")\n");
                    emit_indent(&e->out, indent);
                    buf_append(&e->out, "declare -ga __ds_return_elem_type=(\"${");
                    emit_elem_type_var_name(&e->out, stmt->as.return_stmt.value->as.text);
                    buf_append(&e->out, "[@]}\")\n");
                } else {
                    /* Lowering owns the structured-return portability gate. */
                    ds_diag_error(e->diag, stmt->span, "internal Bash invariant failed: array return should be literal, named, or forwarded after lowering");
                    return false;
                }
            } else if (stmt->as.return_stmt.return_kind == DS_LOWER_VALUE_MAP) {
                if (stmt->as.return_stmt.value->kind == DS_LOWER_EXPR_MAP) {
                    buf_append(&e->out, "declare -gA __ds_return_map=");
                    if (!emit_map_entries(e, &stmt->as.return_stmt.value->as.map.entries, &e->out)) return false;
                    buf_append(&e->out, "\n");
                    emit_map_value_type_entries(e, &stmt->as.return_stmt.value->as.map.entries, indent, "__ds_return_value_type");
                } else if (stmt->as.return_stmt.value->kind == DS_LOWER_EXPR_IDENT) {
                    buf_append(&e->out, "declare -gA __ds_return_map=()\n");
                    emit_indent(&e->out, indent);
                    buf_append(&e->out, "for __ds_key in \"${!");
                    emit_var_name(&e->out, stmt->as.return_stmt.value->as.text);
                    buf_append(&e->out, "[@]}\"; do __ds_return_map[\"$__ds_key\"]=\"${");
                    emit_var_name(&e->out, stmt->as.return_stmt.value->as.text);
                    buf_append(&e->out, "[$__ds_key]}\"; done\n");
                    emit_indent(&e->out, indent);
                    buf_append(&e->out, "declare -gA __ds_return_value_type=()\n");
                    emit_indent(&e->out, indent);
                    buf_append(&e->out, "for __ds_key in \"${!");
                    emit_map_value_type_var_name(&e->out, stmt->as.return_stmt.value->as.text);
                    buf_append(&e->out, "[@]}\"; do __ds_return_value_type[\"$__ds_key\"]=\"${");
                    emit_map_value_type_var_name(&e->out, stmt->as.return_stmt.value->as.text);
                    buf_append(&e->out, "[$__ds_key]}\"; done\n");
                } else {
                    /* Lowering owns the structured-return portability gate. */
                    ds_diag_error(e->diag, stmt->span, "internal Bash invariant failed: map return should be literal, named, or forwarded after lowering");
                    return false;
                }
            } else if (stmt->as.return_stmt.return_kind == DS_LOWER_VALUE_COMMAND_RESULT) {
                if (stmt->as.return_stmt.value->kind == DS_LOWER_EXPR_RUN) {
                    if (stmt->as.return_stmt.value->as.run.stages.len > 1) {
                        DsStr ret_name = {"return", strlen("return")};
                        if (!emit_capture_pipeline_assignment(e, ret_name, &stmt->as.return_stmt.value->as.run, stmt->as.return_stmt.value->span, indent)) return false;
                    } else {
                        buf_append(&e->out, "__ds_capture __ds_return");
                        if (!emit_capture_command(e, &stmt->as.return_stmt.value->as.run, &e->out, stmt->as.return_stmt.value->span)) return false;
                        buf_append(&e->out, "\n");
                    }
                } else if (stmt->as.return_stmt.value->kind == DS_LOWER_EXPR_IDENT) {
                    buf_append(&e->out, "printf -v __ds_return_stdout '%s' \"$");
                    emit_var_name(&e->out, stmt->as.return_stmt.value->as.text); buf_append(&e->out, "_stdout\"\n");
                    emit_indent(&e->out, indent); buf_append(&e->out, "printf -v __ds_return_stderr '%s' \"$");
                    emit_var_name(&e->out, stmt->as.return_stmt.value->as.text); buf_append(&e->out, "_stderr\"\n");
                    emit_indent(&e->out, indent); buf_append(&e->out, "printf -v __ds_return_code '%s' \"$");
                    emit_var_name(&e->out, stmt->as.return_stmt.value->as.text); buf_append(&e->out, "_code\"\n");
                    emit_indent(&e->out, indent); buf_append(&e->out, "printf -v __ds_return_status '%s' \"$");
                    emit_var_name(&e->out, stmt->as.return_stmt.value->as.text); buf_append(&e->out, "_code\"\n");
                    emit_indent(&e->out, indent); buf_append(&e->out, "printf -v __ds_return_ok '%s' \"$");
                    emit_var_name(&e->out, stmt->as.return_stmt.value->as.text); buf_append(&e->out, "_ok\"\n");
                    emit_indent(&e->out, indent); buf_append(&e->out, "printf -v __ds_return_failed '%s' \"$");
                    emit_var_name(&e->out, stmt->as.return_stmt.value->as.text); buf_append(&e->out, "_failed\"\n");
                } else {
                    /* Lowering owns the structured-return portability gate. */
                    ds_diag_error(e->diag, stmt->span, "internal Bash invariant failed: command-result return should be run, named, or forwarded after lowering");
                    return false;
                }
            } else {
                buf_append(&e->out, "__ds_return_value=");
                if (!emit_value_expr(e, stmt->as.return_stmt.value, &e->out)) return false;
                buf_append(&e->out, " || return $?\n");
            }
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
