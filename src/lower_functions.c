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
    SymKind element_kind = SYM_UNKNOWN;
    if (stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == DS_EXPR_ARRAY) {
        element_kind = SYM_UNKNOWN;
        for (size_t i = 0; i < stmt->as.let_stmt.value->as.array.elements.len; i++) {
            const DsExpr *elem = stmt->as.let_stmt.value->as.array.elements.items[i];
            SymKind current = SYM_UNKNOWN;
            if (elem->kind == DS_EXPR_STRING) current = SYM_STRING;
            else if (elem->kind == DS_EXPR_INT) current = SYM_INT;
            else if (elem->kind == DS_EXPR_BOOL) current = SYM_BOOL;
            else { element_kind = SYM_UNKNOWN; break; }
            if (i == 0) element_kind = current;
            else if (element_kind != current) { element_kind = SYM_UNKNOWN; break; }
        }
    }
    scope_define_array(lower, lower->scope, stmt->as.let_stmt.name, SYM_TOPLEVEL_PREDECLARED, element_kind, stmt->span);
}

typedef struct {
    DsStr name;
    DsLowerValueKind kind;
} AstKindBinding;

typedef struct {
    AstKindBinding *items;
    size_t len;
    size_t cap;
} AstKindEnv;

static bool ast_str_eq(DsStr a, DsStr b) {
    return a.len == b.len && memcmp(a.data, b.data, a.len) == 0;
}

static void ast_kind_env_push(AstKindEnv *env, DsStr name, DsLowerValueKind kind) {
    if (kind == DS_LOWER_VALUE_UNKNOWN) return;
    for (size_t i = env->len; i > 0; i--) {
        if (ast_str_eq(env->items[i - 1].name, name)) {
            env->items[i - 1].kind = kind;
            return;
        }
    }
    if (env->len == env->cap) {
        env->cap = env->cap ? env->cap * 2 : 8;
        env->items = (AstKindBinding *)ds_xrealloc(env->items, env->cap * sizeof(AstKindBinding));
    }
    env->items[env->len++] = (AstKindBinding){name, kind};
}

static bool ast_kind_env_find(const AstKindEnv *env, DsStr name, DsLowerValueKind *kind_out) {
    for (size_t i = env->len; i > 0; i--) {
        if (ast_str_eq(env->items[i - 1].name, name)) {
            *kind_out = env->items[i - 1].kind;
            return true;
        }
    }
    return false;
}

static AstKindEnv ast_kind_env_clone(const AstKindEnv *env) {
    AstKindEnv copy = {0};
    if (env->len > 0) {
        copy.items = (AstKindBinding *)ds_xcalloc(env->len, sizeof(AstKindBinding));
        memcpy(copy.items, env->items, env->len * sizeof(AstKindBinding));
        copy.len = env->len;
        copy.cap = env->len;
    }
    return copy;
}

static void ast_kind_env_free(AstKindEnv *env) {
    free(env->items);
    env->items = NULL;
    env->len = 0;
    env->cap = 0;
}

static bool ast_expr_kind_known(Lower *lower, const AstKindEnv *env, const DsExpr *expr, DsLowerValueKind *kind_out) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_EXPR_STRING:
            *kind_out = DS_LOWER_VALUE_STRING;
            return true;
        case DS_EXPR_INT:
            *kind_out = DS_LOWER_VALUE_INT;
            return true;
        case DS_EXPR_BOOL:
            *kind_out = DS_LOWER_VALUE_BOOL;
            return true;
        case DS_EXPR_UNARY:
            if (lower_str_eq(expr->as.unary.op, "-")) {
                DsLowerValueKind right = DS_LOWER_VALUE_UNKNOWN;
                if (!ast_expr_kind_known(lower, env, expr->as.unary.right, &right) || right != DS_LOWER_VALUE_INT) return false;
                *kind_out = DS_LOWER_VALUE_INT;
                return true;
            }
            if (lower_str_eq(expr->as.unary.op, "!")) {
                *kind_out = DS_LOWER_VALUE_BOOL;
                return true;
            }
            return false;
        case DS_EXPR_BINARY:
            if (lower_str_eq(expr->as.binary.op, "+") || lower_str_eq(expr->as.binary.op, "-") ||
                lower_str_eq(expr->as.binary.op, "*") || lower_str_eq(expr->as.binary.op, "/") ||
                lower_str_eq(expr->as.binary.op, "%") || lower_str_eq(expr->as.binary.op, "**")) {
                DsLowerValueKind left = DS_LOWER_VALUE_UNKNOWN;
                DsLowerValueKind right = DS_LOWER_VALUE_UNKNOWN;
                if (!ast_expr_kind_known(lower, env, expr->as.binary.left, &left) ||
                    !ast_expr_kind_known(lower, env, expr->as.binary.right, &right) ||
                    left != DS_LOWER_VALUE_INT || right != DS_LOWER_VALUE_INT) return false;
                *kind_out = DS_LOWER_VALUE_INT;
                return true;
            }
            if (lower_str_eq(expr->as.binary.op, "==") || lower_str_eq(expr->as.binary.op, "!=") ||
                lower_str_eq(expr->as.binary.op, "===") || lower_str_eq(expr->as.binary.op, "!==") ||
                lower_str_eq(expr->as.binary.op, ">") || lower_str_eq(expr->as.binary.op, ">=") ||
                lower_str_eq(expr->as.binary.op, "<") || lower_str_eq(expr->as.binary.op, "<=")) {
                *kind_out = DS_LOWER_VALUE_BOOL;
                return true;
            }
            return false;
        case DS_EXPR_CALL: {
            const DsStdlibHelper *helper = ds_stdlib_lookup(expr->as.call.name);
            DsLowerValueKind lowered = lower_stdlib_return_value_kind(helper);
            if (lowered != DS_LOWER_VALUE_UNKNOWN) {
                *kind_out = lowered;
                return true;
            }
            DsLowerFn *fn = find_function(lower->program, expr->as.call.name);
            if (fn && fn->has_return && fn->all_paths_return && fn->return_kind != DS_LOWER_VALUE_UNKNOWN) {
                *kind_out = fn->return_kind;
                return true;
            }
            return false;
        }
        case DS_EXPR_IDENT:
            return ast_kind_env_find(env, expr->as.text, kind_out);
        case DS_EXPR_RUN:
            *kind_out = DS_LOWER_VALUE_COMMAND_RESULT;
            return true;
        case DS_EXPR_ARRAY:
            *kind_out = DS_LOWER_VALUE_ARRAY;
            return true;
        case DS_EXPR_MAP:
            *kind_out = DS_LOWER_VALUE_MAP;
            return true;
        case DS_EXPR_REGEX:
        case DS_EXPR_RANGE:
        case DS_EXPR_FIELD:
        case DS_EXPR_INDEX:
        case DS_EXPR_ERROR:
            return false;
    }
    return false;
}

static bool ast_stmt_all_paths_return(const DsStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_STMT_RETURN:
            return true;
        case DS_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (ast_stmt_all_paths_return(stmt->as.block_stmt.statements.items[i])) return true;
            }
            return false;
        case DS_STMT_IF:
            return stmt->as.if_stmt.else_branch &&
                   ast_stmt_all_paths_return(stmt->as.if_stmt.then_branch) &&
                   ast_stmt_all_paths_return(stmt->as.if_stmt.else_branch);
        case DS_STMT_CASE: {
            bool has_default = false;
            if (stmt->as.case_stmt.arms.len == 0) return false;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                bool arm_default = false;
                for (size_t j = 0; j < arm->patterns.len; j++) {
                    if (arm->patterns.items[j].kind == DS_CASE_PATTERN_DEFAULT) arm_default = true;
                }
                has_default = has_default || arm_default;
                if (!ast_stmt_all_paths_return(arm->body)) return false;
            }
            return has_default;
        }
        default:
            return false;
    }
}

static bool ast_collect_return_kind(Lower *lower, const DsStmt *stmt, AstKindEnv *env, DsLowerValueKind *kind, bool *saw_return) {
    if (!stmt) return true;
    switch (stmt->kind) {
        case DS_STMT_RETURN: {
            DsLowerValueKind found = DS_LOWER_VALUE_UNKNOWN;
            if (!ast_expr_kind_known(lower, env, stmt->as.return_stmt.value, &found)) return false;
            if (*saw_return && *kind != found) return false;
            *saw_return = true;
            *kind = found;
            return true;
        }
        case DS_STMT_LET: {
            DsLowerValueKind found = DS_LOWER_VALUE_UNKNOWN;
            if (ast_expr_kind_known(lower, env, stmt->as.let_stmt.value, &found)) {
                ast_kind_env_push(env, stmt->as.let_stmt.name, found);
            }
            return true;
        }
        case DS_STMT_ASSIGN: {
            DsLowerValueKind found = DS_LOWER_VALUE_UNKNOWN;
            if (ast_expr_kind_known(lower, env, stmt->as.assign_stmt.value, &found)) {
                ast_kind_env_push(env, stmt->as.assign_stmt.name, found);
            }
            return true;
        }
        case DS_STMT_BLOCK:
        {
            AstKindEnv block_env = ast_kind_env_clone(env);
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (!ast_collect_return_kind(lower, stmt->as.block_stmt.statements.items[i], &block_env, kind, saw_return)) {
                    ast_kind_env_free(&block_env);
                    return false;
                }
            }
            ast_kind_env_free(&block_env);
            return true;
        }
        case DS_STMT_IF:
        {
            AstKindEnv then_env = ast_kind_env_clone(env);
            AstKindEnv else_env = ast_kind_env_clone(env);
            bool ok = ast_collect_return_kind(lower, stmt->as.if_stmt.then_branch, &then_env, kind, saw_return) &&
                      ast_collect_return_kind(lower, stmt->as.if_stmt.else_branch, &else_env, kind, saw_return);
            ast_kind_env_free(&then_env);
            ast_kind_env_free(&else_env);
            return ok;
        }
        case DS_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                AstKindEnv arm_env = ast_kind_env_clone(env);
                bool ok = ast_collect_return_kind(lower, stmt->as.case_stmt.arms.items[i].body, &arm_env, kind, saw_return);
                ast_kind_env_free(&arm_env);
                if (!ok) return false;
            }
            return true;
        default:
            return true;
    }
}

static DsLowerValueKind ast_literal_default_kind(const DsExpr *expr) {
    if (!expr) return DS_LOWER_VALUE_UNKNOWN;
    switch (expr->kind) {
        case DS_EXPR_STRING: return DS_LOWER_VALUE_STRING;
        case DS_EXPR_INT: return DS_LOWER_VALUE_INT;
        case DS_EXPR_BOOL: return DS_LOWER_VALUE_BOOL;
        default: return DS_LOWER_VALUE_UNKNOWN;
    }
}

void predeclare_function_return_contracts(Lower *lower, const DsAst *ast) {
    /*
     * Provisional forward-call contract discovery.
     *
     * This pass intentionally runs before function bodies are lowered so calls
     * to functions declared later can be validated in expression position. It
     * does not emit user-facing return diagnostics and is not the final source
     * of truth for return statements. Concrete return statements are still
     * lowered and validated by lower_validate_function_return_contract() in
     * lower_stmt.c, which owns source-language return-kind diagnostics.
     */
    if (!ast || lower->program->functions.len == 0) return;
    for (size_t pass = 0; pass < lower->program->functions.len; pass++) {
        bool changed = false;
        for (size_t i = 0; i < ast->statements.len; i++) {
            const DsStmt *stmt = ast->statements.items[i];
            if (stmt->kind != DS_STMT_FN) continue;
            DsLowerFn *fn = find_function(lower->program, stmt->as.fn_stmt.name);
            if (!fn || fn->has_return) continue;
            DsLowerValueKind kind = DS_LOWER_VALUE_UNKNOWN;
            bool saw_return = false;
            AstKindEnv env = {0};
            for (size_t j = 0; j < stmt->as.fn_stmt.params.len; j++) {
                const DsFnParam *param = &stmt->as.fn_stmt.params.items[j];
                ast_kind_env_push(&env, param->name, ast_literal_default_kind(param->default_value));
            }
            bool ok = ast_collect_return_kind(lower, stmt->as.fn_stmt.body, &env, &kind, &saw_return);
            ast_kind_env_free(&env);
            if (ok && saw_return) {
                fn->has_return = true;
                fn->return_kind = kind;
                fn->all_paths_return = ast_stmt_all_paths_return(stmt->as.fn_stmt.body);
                changed = true;
            }
        }
        if (!changed) break;
    }
}

static bool expr_reaches_function(Lower *lower, const DsLowerExpr *expr, size_t target_index, bool *seen, DsSpan *cycle_span) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_CALL: {
            if (expr->as.call.is_user_function) {
                int callee = find_function_index(lower->program, expr->as.call.name);
                if (callee >= 0) {
                    if ((size_t)callee == target_index) {
                        *cycle_span = expr->span;
                        return true;
                    }
                    if (function_body_reaches(lower, (size_t)callee, target_index, seen, cycle_span)) return true;
                }
            }
            for (size_t i = 0; i < expr->as.call.args.len; i++) {
                if (expr_reaches_function(lower, expr->as.call.args.items[i], target_index, seen, cycle_span)) return true;
            }
            return false;
        }
        case DS_LOWER_EXPR_FIELD:
            return expr_reaches_function(lower, expr->as.field.object, target_index, seen, cycle_span);
        case DS_LOWER_EXPR_UNARY:
            return expr_reaches_function(lower, expr->as.unary.right, target_index, seen, cycle_span);
        case DS_LOWER_EXPR_BINARY:
            return expr_reaches_function(lower, expr->as.binary.left, target_index, seen, cycle_span) ||
                   expr_reaches_function(lower, expr->as.binary.right, target_index, seen, cycle_span);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) {
                if (expr_reaches_function(lower, expr->as.array.elements.items[i], target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) {
                if (expr_reaches_function(lower, expr->as.map.entries.items[i].value, target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_EXPR_INDEX:
            return expr_reaches_function(lower, expr->as.index.object, target_index, seen, cycle_span) ||
                   expr_reaches_function(lower, expr->as.index.index, target_index, seen, cycle_span);
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) {
                if (expr_reaches_function(lower, expr->as.interp.parts.items[i], target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INT:
        case DS_LOWER_EXPR_BOOL:
        case DS_LOWER_EXPR_RUN:
        case DS_LOWER_EXPR_REGEX:
        case DS_LOWER_EXPR_ERROR:
            return false;
        case DS_LOWER_EXPR_RANGE:
            return expr_reaches_function(lower, expr->as.range.start, target_index, seen, cycle_span) ||
                   expr_reaches_function(lower, expr->as.range.end, target_index, seen, cycle_span);
    }
    return false;
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
            if (expr_reaches_function(lower, stmt->as.if_stmt.condition, target_index, seen, cycle_span)) return true;
            if (stmt_reaches_function(lower, stmt->as.if_stmt.then_branch, target_index, seen, cycle_span)) return true;
            return stmt_reaches_function(lower, stmt->as.if_stmt.else_branch, target_index, seen, cycle_span);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (stmt_reaches_function(lower, stmt->as.block_stmt.statements.items[i], target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_RANGE:
            if (expr_reaches_function(lower, stmt->as.for_stmt.iterable, target_index, seen, cycle_span)) return true;
            return stmt_reaches_function(lower, stmt->as.for_stmt.body, target_index, seen, cycle_span);
        case DS_LOWER_STMT_WHILE:
            if (expr_reaches_function(lower, stmt->as.while_stmt.condition, target_index, seen, cycle_span)) return true;
            return stmt_reaches_function(lower, stmt->as.while_stmt.body, target_index, seen, cycle_span);
        case DS_LOWER_STMT_CASE:
            if (expr_reaches_function(lower, stmt->as.case_stmt.selector, target_index, seen, cycle_span)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                if (stmt_reaches_function(lower, stmt->as.case_stmt.arms.items[i].body, target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_STMT_LET:
            return expr_reaches_function(lower, stmt->as.let_stmt.value, target_index, seen, cycle_span);
        case DS_LOWER_STMT_ASSIGN:
            return expr_reaches_function(lower, stmt->as.assign_stmt.value, target_index, seen, cycle_span);
        case DS_LOWER_STMT_PUSH:
            return expr_reaches_function(lower, stmt->as.push_stmt.value, target_index, seen, cycle_span);
        case DS_LOWER_STMT_ASSERT:
            return expr_reaches_function(lower, stmt->as.assert_stmt.condition, target_index, seen, cycle_span);
        case DS_LOWER_STMT_RETURN:
            return expr_reaches_function(lower, stmt->as.return_stmt.value, target_index, seen, cycle_span);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return stmt_reaches_function(lower, stmt->as.handler_stmt.body, target_index, seen, cycle_span);
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
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
        SymKind kind = sym_kind_from_lower_value_kind(fn->params.items[i].default_kind);
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
