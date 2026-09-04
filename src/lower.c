#include "lower_context.h"
#include "lower_functions.h"
#include "lower_symbols.h"
#include "lower_stmt.h"
#include "lower_script.h"
#include "lower_free.h"

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
    DsLowerProgram *program = (DsLowerProgram *)ds_xcalloc(1, sizeof(DsLowerProgram));
    Lower lower;
    lower_context_init(&lower, diag, program);
    program->span = ast->span;
    program->has_script = ast->has_script;
    if (ast->has_script) {
        for (size_t i = 0; i < ast->script.declarations.len; i++) lower_script_decl(&lower, &ast->script.declarations.items[i], program);
    }
    lower_functions_collect_signatures(&lower, ast);
    lower_symbols_predeclare_top_level_bindings(&lower, ast);
    lower_functions_infer_parameter_kinds(&lower, ast);
    if (diag->has_error) {
        lower_context_free(&lower);
        ds_lower_program_free(program);
        return NULL;
    }
    lower_functions_predeclare_return_contracts(&lower, ast);
    lower_functions_lower_bodies(&lower, ast);
    for (size_t i = 0; i < ast->statements.len; i++) collect_test(&lower, ast->statements.items[i], program);
    lower_functions_validate_call_graph(&lower);
    for (size_t i = 0; i < ast->statements.len; i++) {
        if (ast->statements.items[i]->kind != DS_STMT_FN && ast->statements.items[i]->kind != DS_STMT_TEST) DS_VEC_PUSH(&program->statements, lower_stmt(&lower, ast->statements.items[i]), 16);
    }
    lower_context_free(&lower);
    if (diag->has_error) {
        ds_lower_program_free(program);
        return NULL;
    }
    return program;
}
