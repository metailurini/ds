#include "lower_internal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

bool expr_is_literal_default(const DsExpr *expr) {
    return expr && (expr->kind == DS_EXPR_STRING || expr->kind == DS_EXPR_INT || expr->kind == DS_EXPR_BOOL);
}

void collect_function_signature(Lower *lower, const DsStmt *stmt, DsLowerProgram *program) {
    if (stmt->kind != DS_STMT_FN) return;
    if (ds_stdlib_is_name(stmt->as.fn_stmt.name) || ds_stdlib_is_namespace(stmt->as.fn_stmt.name)) {
        ds_diag_error(lower->diag, stmt->span,
                      "function `%.*s` conflicts with a v0.11.0 standard-library helper name",
                      (int)stmt->as.fn_stmt.name.len, stmt->as.fn_stmt.name.data);
        return;
    }
    if (find_function(program, stmt->as.fn_stmt.name)) {
        ds_diag_error(lower->diag, stmt->span, "duplicate function `%.*s`", (int)stmt->as.fn_stmt.name.len, stmt->as.fn_stmt.name.data);
        return;
    }
    scope_define(lower, lower->scope, stmt->as.fn_stmt.name, SYM_FUNCTION, stmt->span);
    DsLowerFn fn;
    memset(&fn, 0, sizeof(fn));
    fn.name = str_clone(stmt->as.fn_stmt.name);
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
        out.name = str_clone(param->name);
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
        lower_fn_param_vec_push(&fn.params, out);
    }
    scope_free(&param_names);
    lower_fn_vec_push(&program->functions, fn);
}

void collect_top_level_let_signature(Lower *lower, const DsStmt *stmt) {
    if (stmt->kind != DS_STMT_LET) return;
    if (scope_find_current(lower->scope, stmt->as.let_stmt.name)) {
        return;
    }
    scope_define(lower, lower->scope, stmt->as.let_stmt.name, SYM_TOPLEVEL_PREDECLARED, stmt->span);
}

bool stmt_reaches_function(Lower *lower, const DsLowerStmt *stmt, size_t target_index, bool *seen, DsSpan *cycle_span) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_CALL: {
            int callee = find_function_index(lower->program, stmt->as.call_stmt.name);
            if (callee < 0) return false;
            if ((size_t)callee == target_index) {
                *cycle_span = stmt->span;
                return true;
            }
            return function_body_reaches(lower, (size_t)callee, target_index, seen, cycle_span);
        }
        case DS_LOWER_STMT_IF:
            if (stmt_reaches_function(lower, stmt->as.if_stmt.then_branch, target_index, seen, cycle_span)) return true;
            return stmt_reaches_function(lower, stmt->as.if_stmt.else_branch, target_index, seen, cycle_span);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (stmt_reaches_function(lower, stmt->as.block_stmt.statements.items[i], target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
            return stmt_reaches_function(lower, stmt->as.for_stmt.body, target_index, seen, cycle_span);
        case DS_LOWER_STMT_WHILE:
            return stmt_reaches_function(lower, stmt->as.while_stmt.body, target_index, seen, cycle_span);
        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                if (stmt_reaches_function(lower, stmt->as.case_stmt.arms.items[i].body, target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_STMT_LET:
        case DS_LOWER_STMT_ASSIGN:
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_PUSH:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
        case DS_LOWER_STMT_RETURN:
            return false;
        case DS_LOWER_STMT_ASSERT:
            return false;
    }
    return false;
}

bool function_body_reaches(Lower *lower, size_t current_index, size_t target_index, bool *seen, DsSpan *cycle_span) {
    if (current_index >= lower->program->functions.len) return false;
    if (seen[current_index]) return false;
    seen[current_index] = true;
    return stmt_reaches_function(lower, lower->program->functions.items[current_index].body, target_index, seen, cycle_span);
}

void reject_recursive_functions(Lower *lower) {
    if (lower->program->functions.len == 0) return;
    bool *seen = (bool *)ds_xcalloc(lower->program->functions.len, sizeof(bool));
    for (size_t i = 0; i < lower->program->functions.len; i++) {
        memset(seen, 0, lower->program->functions.len * sizeof(bool));
        DsSpan cycle_span = lower->program->functions.items[i].span;
        if (function_body_reaches(lower, i, i, seen, &cycle_span)) {
            DsLowerFn *fn = &lower->program->functions.items[i];
            ds_diag_error(lower->diag, cycle_span,
                          "recursive function calls are deferred in v0.9.0; `%.*s` participates in a recursion cycle",
                          (int)fn->name.len, fn->name.data);
        }
    }
    free(seen);
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
                    if (arm->patterns.items[j].kind == DS_LOWER_CASE_PATTERN_DEFAULT) arm_default = true;
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

void lower_function_body(Lower *lower, DsLowerFn *fn, const DsStmt *stmt) {
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
        SymKind kind = SYM_UNKNOWN;
        switch (fn->params.items[i].default_kind) {
            case DS_LOWER_VALUE_BOOL: kind = SYM_BOOL; break;
            case DS_LOWER_VALUE_INT: kind = SYM_INT; break;
            case DS_LOWER_VALUE_STRING: kind = SYM_STRING; break;
            case DS_LOWER_VALUE_COMMAND_RESULT: kind = SYM_COMMAND_RESULT; break;
            case DS_LOWER_VALUE_ARRAY: kind = SYM_ARRAY; break;
            case DS_LOWER_VALUE_MAP: kind = SYM_MAP; break;
            case DS_LOWER_VALUE_UNKNOWN: kind = SYM_UNKNOWN; break;
        }
        scope_define(lower, &local, fn->params.items[i].name, kind, fn->params.items[i].span);
    }
    fn->body = lower_block(lower, stmt->as.fn_stmt.body, false);
    fn->all_paths_return = stmt_all_paths_return(fn->body);
    lower->scope = saved;
    lower->loop_depth = saved_depth;
    lower->function_depth = saved_fn_depth;
    lower->current_function = saved_fn;
    scope_free(&local);
}
