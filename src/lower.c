#include "lower_internal.h"

bool lower_sym_kind_is_scalar(SymKind kind) {
    return kind == SYM_STRING || kind == SYM_INT || kind == SYM_BOOL;
}

bool lower_value_kind_is_scalar(DsLowerValueKind kind) {
    return kind == DS_LOWER_VALUE_STRING || kind == DS_LOWER_VALUE_INT || kind == DS_LOWER_VALUE_BOOL;
}

void lower_diag_stdlib_arity_error(Lower *lower, DsSpan span, DsStr name,
                                   size_t min_arity, size_t max_arity, size_t actual) {
    if (min_arity == max_arity) {
        ds_diag_error(lower->diag, span, "helper `%.*s` expects %zu arguments but got %zu",
                      (int)name.len, name.data, min_arity, actual);
    } else {
        ds_diag_error(lower->diag, span, "helper `%.*s` expects %zu to %zu arguments but got %zu",
                      (int)name.len, name.data, min_arity, max_arity, actual);
    }
}

void lower_diag_unknown_function(Lower *lower, DsSpan span, DsStr name) {
    ds_diag_error(lower->diag, span, "unknown function `%.*s`", (int)name.len, name.data);
}

void lower_diag_unknown_stdlib_helper(Lower *lower, DsSpan span, DsStr name) {
    ds_diag_error(lower->diag, span, "unknown standard-library helper `%.*s`", (int)name.len, name.data);
}

void lower_diag_unknown_string_method(Lower *lower, DsSpan span, DsStr member) {
    ds_diag_error(lower->diag, span, "unknown string method `%.*s`; supported methods are %s",
                  (int)member.len, member.data, ds_stdlib_string_method_names());
}

DsLowerValueKind lower_fn_param_expected_kind(const DsLowerFnParam *param) {
    if (!param) return DS_LOWER_VALUE_UNKNOWN;
    return param->has_default ? param->default_kind : param->inferred_kind;
}

bool lower_validate_env_name(Lower *lower, DsStr name, DsSpan span, const char *version) {
    if (is_env_name_text(name)) return true;
    ds_diag_error(lower->diag, span, "invalid environment variable name `%.*s` in %s",
                  (int)name.len, ds_str_data(name), version);
    return false;
}

DsLowerExpr *expr_new(DsLowerExprKind kind, DsSpan span) {
    DsLowerExpr *expr = (DsLowerExpr *)ds_xcalloc(1, sizeof(*expr));
    expr->kind = kind;
    expr->span = span;
    return expr;
}

DsLowerStmt *stmt_new(DsLowerStmtKind kind, DsSpan span) {
    DsLowerStmt *stmt = (DsLowerStmt *)ds_xcalloc(1, sizeof(*stmt));
    stmt->kind = kind;
    stmt->span = span;
    return stmt;
}

static void collect_test(Lower *lower, const DsStmt *stmt, DsLowerProgram *program) {
    if (stmt->kind != DS_STMT_TEST) return;
    for (size_t i = 0; i < program->tests.len; i++) {
        if (ds_str_eq(program->tests.items[i].name, stmt->as.test_stmt.name)) {
            ds_diag_error(lower->diag, stmt->span, "duplicate test `%.*s`", (int)stmt->as.test_stmt.name.len, stmt->as.test_stmt.name.data);
            return;
        }
    }
    DsLowerTest test;
    memset(&test, 0, sizeof(test));
    test.name = ds_str_clone(stmt->as.test_stmt.name);
    test.span = stmt->span;
    test.body = lower_block(lower, stmt->as.test_stmt.body, true);
    DS_VEC_PUSH(&program->tests, test, 8);
}

DsLowerProgram *ds_lower_program(const DsAst *ast, DsDiag *diag) {
    Scope root;
    scope_init(&root, NULL);
    DsLowerProgram *program = (DsLowerProgram *)ds_xcalloc(1, sizeof(DsLowerProgram));
    Lower lower;
    memset(&lower, 0, sizeof(lower));
    lower.diag = diag;
    lower.scope = &root;
    lower.program = program;
    program->span = ast->span;
    program->has_script = ast->has_script;
    if (ast->has_script) {
        for (size_t i = 0; i < ast->script.declarations.len; i++) lower_script_decl(&lower, &ast->script.declarations.items[i], program);
    }
    for (size_t i = 0; i < ast->statements.len; i++) collect_function_signature(&lower, ast->statements.items[i], program);
    for (size_t i = 0; i < ast->statements.len; i++) collect_top_level_let_signature(&lower, ast->statements.items[i]);
    infer_function_parameter_kinds(&lower, ast);
    if (diag->has_error) {
        scope_free(&root);
        ds_lower_program_free(program);
        return NULL;
    }
    predeclare_function_return_contracts(&lower, ast);
    for (size_t i = 0; i < ast->statements.len; i++) {
        if (ast->statements.items[i]->kind == DS_STMT_FN) {
            DsLowerFn *fn = find_function(program, ast->statements.items[i]->as.fn_stmt.name);
            if (fn) lower_function_body(&lower, fn, ast->statements.items[i]);
        }
    }
    for (size_t i = 0; i < ast->statements.len; i++) collect_test(&lower, ast->statements.items[i], program);
    reject_recursive_functions(&lower);
    for (size_t i = 0; i < ast->statements.len; i++) {
        if (ast->statements.items[i]->kind != DS_STMT_FN && ast->statements.items[i]->kind != DS_STMT_TEST) DS_VEC_PUSH(&program->statements, lower_stmt(&lower, ast->statements.items[i]), 16);
    }
    scope_free(&root);
    free(lower.map_loop_symbols);
    if (diag->has_error) {
        ds_lower_program_free(program);
        return NULL;
    }
    return program;
}
