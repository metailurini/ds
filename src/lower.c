#include "lower_internal.h"

#include <stdlib.h>

DsLowerProgram *ds_lower_program(const DsAst *ast, DsDiag *diag) {
    Scope root;
    scope_init(&root, NULL);
    DsLowerProgram *program = (DsLowerProgram *)ds_xcalloc(1, sizeof(DsLowerProgram));
    Lower lower = {diag, &root, program, 0};
    program->span = ast->span;
    program->has_script = ast->has_script;
    if (ast->has_script) {
        for (size_t i = 0; i < ast->script.declarations.len; i++) lower_script_decl(&lower, &ast->script.declarations.items[i], program);
    }
    for (size_t i = 0; i < ast->statements.len; i++) collect_function_signature(&lower, ast->statements.items[i], program);
    for (size_t i = 0; i < ast->statements.len; i++) collect_top_level_let_signature(&lower, ast->statements.items[i]);
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
