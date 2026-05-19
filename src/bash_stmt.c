#include "bash_internal.h"

#include <stdlib.h>

bool emit_block_body(BashEmitter *e, const DsLowerStmt *block, int indent) {
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) {
        if (!emit_stmt(e, block->as.block_stmt.statements.items[i], indent)) return false;
    }
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
        emit_indent(&e->out, 1);
        buf_appendf(&e->out, "if [[ $# -gt %zu ]]; then ", i);
        emit_var_name(&e->out, param->name);
        buf_appendf(&e->out, "=\"${%zu}\"; else ", i + 1);
        emit_var_name(&e->out, param->name);
        buf_append(&e->out, "=");
        if (param->has_default) {
            if (!emit_function_default(e, param->default_value, &e->out)) { symbols_truncate(&e->symbols, symbol_mark); return false; }
        } else {
            buf_append(&e->out, "\"\"");
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
                buf_append(&e->out, "__ds_capture ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                if (!emit_capture_words(e, &stmt->as.let_stmt.value->as.run.words, &e->out, stmt->as.let_stmt.value->span)) return false;
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
                buf_append(&e->out, "mapfile -t ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
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
            buf_append(&e->out, "\n\n");
            if (!symbol_exists(&e->symbols, stmt->as.let_stmt.name)) {
                DsStr copy = {ds_str_dup_range(stmt->as.let_stmt.name.data, stmt->as.let_stmt.name.len), stmt->as.let_stmt.name.len};
                symbol_vec_push(&e->symbols, copy);
            }
            return true;

        case DS_LOWER_STMT_CMD:
            emit_indent(&e->out, indent);
            buf_append(&e->out, "__ds_trace_cmd ");
            emit_source_loc(&e->out, e->source, stmt->span);
            for (size_t i = 0; i < stmt->as.cmd_stmt.words.len; i++) {
                buf_append(&e->out, " ");
                if (!emit_command_word(e, stmt->as.cmd_stmt.words.items[i], &e->out)) return false;
            }
            if (!emit_trace_redirect_args(e, &stmt->as.cmd_stmt.redirect, &e->out)) return false;
            buf_append(&e->out, "\n");
            emit_indent(&e->out, indent);
            for (size_t i = 0; i < stmt->as.cmd_stmt.words.len; i++) {
                if (i > 0) buf_append(&e->out, " ");
                if (!emit_command_word(e, stmt->as.cmd_stmt.words.items[i], &e->out)) return false;
            }
            if (!emit_redirect(e, &stmt->as.cmd_stmt.redirect, &e->out, stmt->span)) return false;
            buf_append(&e->out, " || __ds_fail ");
            emit_source_loc(&e->out, e->source, stmt->span);
            buf_append(&e->out, " \"$?\"\n\n");
            return true;

        case DS_LOWER_STMT_CALL:
            emit_indent(&e->out, indent);
            if (ds_stdlib_is_name(stmt->as.call_stmt.name)) emit_stdlib_helper_name(&e->out, stmt->as.call_stmt.name);
            else emit_fn_name(&e->out, stmt->as.call_stmt.name);
            if (!emit_call_args(e, &stmt->as.call_stmt.args, &e->out)) return false;
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

        case DS_LOWER_STMT_BLOCK:
            return emit_block_body(e, stmt, indent);
        case DS_LOWER_STMT_ASSERT:
            ds_diag_error(e->diag, stmt->span, "assert statements are only emitted by the test runner in v0.14.0");
            return false;
    }
    return true;
}
