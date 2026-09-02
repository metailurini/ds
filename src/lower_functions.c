#include "lower_internal.h"
#include "lower_functions.h"


static bool expr_is_literal_default(const DsExpr *expr) {
    return expr && (expr->kind == DS_EXPR_STRING || expr->kind == DS_EXPR_INT || expr->kind == DS_EXPR_BOOL);
}

static void collect_function_signature(Lower *lower, const DsStmt *stmt) {
    if (stmt->kind != DS_STMT_FN) return;
    if (ds_stdlib_is_name(stmt->as.fn_stmt.name) || ds_stdlib_is_namespace(stmt->as.fn_stmt.name)) {
        ds_diag_error(lower->diag, stmt->span,
                      "function `%.*s` conflicts with a v0.11.0 standard-library helper name",
                      (int)stmt->as.fn_stmt.name.len, stmt->as.fn_stmt.name.data);
        return;
    }
    if (find_function(lower->program, stmt->as.fn_stmt.name)) {
        ds_diag_error(lower->diag, stmt->span, "duplicate function `%.*s`", (int)stmt->as.fn_stmt.name.len, stmt->as.fn_stmt.name.data);
        return;
    }
    scope_define(lower, lower->scope, stmt->as.fn_stmt.name, SYM_FUNCTION, stmt->span);
    DsLowerFn fn;
    memset(&fn, 0, sizeof(fn));
    fn.name = ds_str_clone(stmt->as.fn_stmt.name);
    fn.span = stmt->span;
    bool seen_default = false;
    Scope param_names;
    scope_init(&param_names, NULL);
    for (size_t i = 0; i < stmt->as.fn_stmt.params.len; i++) {
        const DsFnParam *param = &stmt->as.fn_stmt.params.items[i];
        if (scope_find_current(&param_names, param->name)) {
            ds_diag_error(lower->diag, param->span, "duplicate parameter `%.*s`", (int)param->name.len, param->name.data);
        }
        Symbol dummy = {0};
        (void)dummy;
        Scope *saved = lower->scope;
        lower->scope = &param_names;
        scope_define(lower, &param_names, param->name, SYM_UNKNOWN, param->span);
        lower->scope = saved;
        if (param->has_type) {
            ds_diag_error(lower->diag, param->span, "typed function parameters are deferred in v0.9.0; omit the type annotation");
        }
        DsLowerFnParam out;
        memset(&out, 0, sizeof(out));
        out.name = ds_str_clone(param->name);
        out.span = param->span;
        if (param->default_value) {
            seen_default = true;
            if (!expr_is_literal_default(param->default_value)) {
                ds_diag_error(lower->diag, param->default_value->span, "function parameter defaults must be string, int, or bool literals in v0.9.0");
            }
            SymKind default_kind = SYM_UNKNOWN;
            out.has_default = true;
            out.default_value = lower_expr(lower, param->default_value, &default_kind);
            out.default_kind = lower_value_kind_from_sym(default_kind);
        } else {
            if (seen_default) {
                ds_diag_error(lower->diag, param->span,
                              "required parameter `%.*s` cannot follow a default parameter",
                              (int)param->name.len, param->name.data);
            }
            fn.required_count++;
        }
        DS_VEC_PUSH(&fn.params, out, 8);
    }
    scope_free(&param_names);
    DS_VEC_PUSH(&lower->program->functions, fn, 8);
}

void lower_functions_collect_signatures(Lower *lower, const DsAst *ast) {
    if (!lower || !ast) return;
    for (size_t i = 0; i < ast->statements.len; i++) {
        collect_function_signature(lower, ast->statements.items[i]);
    }
}

static bool stmt_all_paths_return(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_RETURN:
            return true;
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (stmt_all_paths_return(stmt->as.block_stmt.statements.items[i])) return true;
            }
            return false;
        case DS_LOWER_STMT_IF:
            return stmt->as.if_stmt.else_branch &&
                   stmt_all_paths_return(stmt->as.if_stmt.then_branch) &&
                   stmt_all_paths_return(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_CASE: {
            bool has_default = false;
            if (stmt->as.case_stmt.arms.len == 0) return false;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsLowerCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                bool arm_default = false;
                for (size_t j = 0; j < arm->patterns.len; j++) {
                    if (arm->patterns.items[j].kind == DS_CASE_PATTERN_DEFAULT) arm_default = true;
                }
                has_default = has_default || arm_default;
                if (!stmt_all_paths_return(arm->body)) return false;
            }
            return has_default;
        }
        default:
            return false;
    }
}

static bool stmt_contains_plain_command(const DsLowerStmt *stmt, DsSpan *span_out) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_CMD:
            *span_out = stmt->span;
            return true;
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (stmt_contains_plain_command(stmt->as.block_stmt.statements.items[i], span_out)) return true;
            }
            return false;
        case DS_LOWER_STMT_IF:
            return stmt_contains_plain_command(stmt->as.if_stmt.then_branch, span_out) ||
                   stmt_contains_plain_command(stmt->as.if_stmt.else_branch, span_out);
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            return stmt_contains_plain_command(stmt->as.for_stmt.body, span_out);
        case DS_LOWER_STMT_WHILE:
            return stmt_contains_plain_command(stmt->as.while_stmt.body, span_out);
        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                if (stmt_contains_plain_command(stmt->as.case_stmt.arms.items[i].body, span_out)) return true;
            }
            return false;
        default:
            return false;
    }
}

static void lower_function_body(Lower *lower, DsLowerFn *fn, const DsStmt *stmt) {
    Scope local;
    scope_init(&local, lower->scope);
    Scope *saved = lower->scope;
    int saved_depth = lower->loop_depth;
    int saved_fn_depth = lower->function_depth;
    DsLowerFn *saved_fn = lower->current_function;
    lower->scope = &local;
    lower->loop_depth = 0;
    lower->function_depth++;
    lower->current_function = fn;
    for (size_t i = 0; i < fn->params.len; i++) {
        SymKind kind = sym_kind_from_lower_value_kind(lower_fn_param_expected_kind(&fn->params.items[i]));
        scope_define(lower, &local, fn->params.items[i].name, kind, fn->params.items[i].span);
    }
    fn->body = lower_block(lower, stmt->as.fn_stmt.body, false);
    fn->all_paths_return = stmt_all_paths_return(fn->body);
    DsSpan command_span = fn->span;
    fn->contains_plain_command = stmt_contains_plain_command(fn->body, &command_span);
    lower->scope = saved;
    lower->loop_depth = saved_depth;
    lower->function_depth = saved_fn_depth;
    lower->current_function = saved_fn;
    scope_free(&local);
}

void lower_functions_lower_bodies(Lower *lower, const DsAst *ast) {
    if (!lower || !ast) return;
    for (size_t i = 0; i < ast->statements.len; i++) {
        const DsStmt *stmt = ast->statements.items[i];
        if (stmt->kind != DS_STMT_FN) continue;
        DsLowerFn *fn = find_function(lower->program, stmt->as.fn_stmt.name);
        if (fn) lower_function_body(lower, fn, stmt);
    }
}
