#include "lower_internal.h"


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
    DS_VEC_PUSH(&program->functions, fn, 8);
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
    } else if (stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == DS_EXPR_MAP) {
        element_kind = SYM_UNKNOWN;
        for (size_t i = 0; i < stmt->as.let_stmt.value->as.map.entries.len; i++) {
            const DsExpr *value = stmt->as.let_stmt.value->as.map.entries.items[i].value;
            SymKind current = SYM_UNKNOWN;
            if (value->kind == DS_EXPR_STRING) current = SYM_STRING;
            else if (value->kind == DS_EXPR_INT) current = SYM_INT;
            else if (value->kind == DS_EXPR_BOOL) current = SYM_BOOL;
            else { element_kind = SYM_UNKNOWN; break; }
            if (i == 0) element_kind = current;
            else if (element_kind != current) { element_kind = SYM_UNKNOWN; break; }
        }
    }
    scope_define_array(lower, lower->scope, stmt->as.let_stmt.name, SYM_TOPLEVEL_PREDECLARED, element_kind, stmt->span);
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
