#include "lower_internal.h"

#include <stdlib.h>
#include <string.h>

static void collect_test(Lower *lower, const DsStmt *stmt, DsLowerProgram *program) {
    if (stmt->kind != DS_STMT_TEST) return;
    for (size_t i = 0; i < program->tests.len; i++) {
        if (program->tests.items[i].name.len == stmt->as.test_stmt.name.len &&
            memcmp(program->tests.items[i].name.data, stmt->as.test_stmt.name.data, stmt->as.test_stmt.name.len) == 0) {
            ds_diag_error(lower->diag, stmt->span, "duplicate test `%.*s`", (int)stmt->as.test_stmt.name.len, stmt->as.test_stmt.name.data);
            return;
        }
    }
    DsLowerTest test;
    memset(&test, 0, sizeof(test));
    test.name = str_clone(stmt->as.test_stmt.name);
    test.span = stmt->span;
    test.body = lower_block(lower, stmt->as.test_stmt.body, true);
    lower_test_vec_push(&program->tests, test);
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
        if (ast->statements.items[i]->kind != DS_STMT_FN && ast->statements.items[i]->kind != DS_STMT_TEST) lower_stmt_vec_push(&program->statements, lower_stmt(&lower, ast->statements.items[i]));
    }
    scope_free(&root);
    for (size_t i = 0; i < lower.map_loop_len; i++) free(lower.map_loop_names[i].data);
    free(lower.map_loop_names);
    if (diag->has_error) {
        ds_lower_program_free(program);
        return NULL;
    }
    return program;
}

bool ds_lower_validate(const DsAst *ast, DsDiag *diag) {
    DsLowerProgram *program = ds_lower_program(ast, diag);
    if (!program) return false;
    ds_lower_program_free(program);
    return true;
}
