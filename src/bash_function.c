#include "bash_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    bool active;
    char raw_name[64];
    DsLowerValueKind kind;
} MaterializedArg;

bool bash_is_user_function_call_expr(const DsLowerExpr *expr) {
    return expr && expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.is_user_function;
}

void bash_temp_ds_name(char *buf, size_t cap, const char *prefix, size_t id) {
    snprintf(buf, cap, "__%s_%zu", prefix, id);
}

bool bash_emit_user_call_into_raw_var(BashEmitter *e, const DsLowerExpr *expr, DsStr raw_name, int indent) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "__ds_call_value_into ");
    emit_var_name(&e->out, raw_name);
    buf_append(&e->out, " ");
    const char *return_type = ds_lower_value_kind_name(expr->as.call.return_kind);
    bash_single_quote(&e->out, return_type, strlen(return_type));
    buf_append(&e->out, " ");
    emit_fn_name(&e->out, expr->as.call.name);
    if (!emit_user_call_args(e, &expr->as.call.args, &e->out)) return false;
    buf_append(&e->out, "\n");
    return true;
}

static bool emit_materialized_user_call_args(BashEmitter *e, const DsLowerExprVec *args, MaterializedArg *mats, int indent) {
    for (size_t i = 0; i < args->len; i++) {
        mats[i].active = false;
        if (!bash_is_user_function_call_expr(args->items[i])) continue;
        mats[i].active = true;
        mats[i].kind = args->items[i]->as.call.return_kind;
        bash_temp_ds_name(mats[i].raw_name, sizeof(mats[i].raw_name), "arg", e->temp_counter++);
        DsStr raw = {mats[i].raw_name, strlen(mats[i].raw_name)};
        emit_indent(&e->out, indent);
        if (e->function_depth > 0) buf_append(&e->out, "local ");
        emit_var_name(&e->out, raw);
        buf_append(&e->out, "=\"\"\n");
        if (!bash_emit_user_call_into_raw_var(e, args->items[i], raw, indent)) return false;
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
            const char *type = ds_lower_value_kind_name(mats[i].kind);
            bash_single_quote(out, type, strlen(type));
            continue;
        }
        if (!emit_call_arg_expr(e, args->items[i], out)) return false;
        buf_append(out, " ");
        bash_emit_expr_type_value(e, args->items[i], out);
    }
    return true;
}

bool bash_emit_user_function_value_call_into(BashEmitter *e, DsStr name, const DsLowerExpr *call, int indent) {
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
    const char *return_type = ds_lower_value_kind_name(call->as.call.return_kind);
    bash_single_quote(&e->out, return_type, strlen(return_type));
    buf_append(&e->out, " ");
    emit_fn_name(&e->out, call->as.call.name);
    bool ok = emit_user_call_args_with_materialized(e, &call->as.call.args, mats, &e->out);
    free(mats);
    if (!ok) return false;
    buf_append(&e->out, "\n");
    return true;
}

bool bash_emit_user_call_statement(BashEmitter *e, DsStr name, const DsLowerExprVec *args, int indent) {
    MaterializedArg *mats = NULL;
    if (args->len > 0) {
        mats = calloc(args->len, sizeof(*mats));
        if (!mats) return false;
        if (!emit_materialized_user_call_args(e, args, mats, indent)) { free(mats); return false; }
    }
    emit_indent(&e->out, indent);
    emit_fn_name(&e->out, name);
    bool ok = emit_user_call_args_with_materialized(e, args, mats, &e->out);
    free(mats);
    if (!ok) return false;
    buf_append(&e->out, "\n\n");
    return true;
}

bool bash_emit_user_call_capture_return(BashEmitter *e, const DsLowerExpr *call, DsLowerValueKind return_kind, int indent) {
    MaterializedArg *mats = NULL;
    if (call->as.call.args.len > 0) {
        mats = calloc(call->as.call.args.len, sizeof(*mats));
        if (!mats) return false;
        if (!emit_materialized_user_call_args(e, &call->as.call.args, mats, indent)) { free(mats); return false; }
    }
    buf_append(&e->out, "__ds_call_value_capture ");
    const char *return_type = ds_lower_value_kind_name(return_kind);
    bash_single_quote(&e->out, return_type, strlen(return_type));
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
        ds_diag_error(e->diag, fn->span, "internal Bash invariant failed: unsafe lowered function name `%.*s` reached Bash emission", (int)fn->name.len, fn->name.data);
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
            bash_emit_type_var_name(&e->out, param->name);
            buf_append(&e->out, "\n");
        }

        emit_indent(&e->out, 1);
        buf_appendf(&e->out, "if [[ $# -gt %zu ]]; then ", i * 2);
        emit_var_name(&e->out, param->name);
        buf_appendf(&e->out, "=\"${%zu}\"", i * 2 + 1);
        if (e->needs_case_types) {
            buf_append(&e->out, "; ");
            bash_emit_type_var_name(&e->out, param->name);
            buf_append(&e->out, "=");
            buf_appendf(&e->out, "\"${%zu:-", i * 2 + 2);
            const char *type = param->has_default ? ds_lower_value_kind_name(param->default_kind) : ds_lower_value_kind_name(DS_LOWER_VALUE_UNKNOWN);
            buf_append(&e->out, type);
            buf_append(&e->out, "}\"");
        }
        buf_append(&e->out, "; else ");
        emit_var_name(&e->out, param->name);
        buf_append(&e->out, "=");
        if (param->has_default) {
            if (!emit_function_default(e, param->default_value, &e->out)) {
                symbols_truncate(&e->symbols, symbol_mark);
                return false;
            }
        } else {
            buf_append(&e->out, "\"\"");
        }
        if (e->needs_case_types) {
            buf_append(&e->out, "; ");
            bash_emit_type_var_name(&e->out, param->name);
            buf_append(&e->out, "=");
            const char *type = param->has_default ? ds_lower_value_kind_name(param->default_kind) : ds_lower_value_kind_name(DS_LOWER_VALUE_UNKNOWN);
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
