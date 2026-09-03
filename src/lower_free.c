#include "lower_internal.h"

static void lower_command_free(DsLowerCommand *command) {
    if (!command) return;
    for (size_t s = 0; s < command->stages.len; s++) {
        DsLowerCommandWordVec *words = &command->stages.items[s].words;
        for (size_t i = 0; i < words->len; i++) {
            free(words->items[i].source_text.data);
            free(words->items[i].literal_text.data);
            lower_expr_free(words->items[i].value);
        }
        free(words->items);
    }
    free(command->stages.items);
    free(command->redirect.source_target.data);
    free(command->redirect.literal_target.data);
    lower_expr_free(command->redirect.target);
    *command = (DsLowerCommand){0};
}

static void lower_expr_vec_free(DsLowerExprVec *vec) {
    for (size_t i = 0; i < vec->len; i++) lower_expr_free(vec->items[i]);
    free(vec->items);
}

static void lower_map_entry_vec_free(DsLowerMapEntryVec *vec) {
    for (size_t i = 0; i < vec->len; i++) {
        free(vec->items[i].key.data);
        lower_expr_free(vec->items[i].value);
    }
    free(vec->items);
}

static void lower_stmt_vec_free(DsLowerStmtVec *vec) {
    for (size_t i = 0; i < vec->len; i++) lower_stmt_free(vec->items[i]);
    free(vec->items);
}

static void lower_case_arm_vec_free(DsLowerCaseArmVec *vec) {
    for (size_t i = 0; i < vec->len; i++) {
        ds_case_pattern_vec_free(&vec->items[i].patterns);
        lower_stmt_free(vec->items[i].body);
    }
    free(vec->items);
}

static void lower_fn_param_vec_free(DsLowerFnParamVec *vec) {
    for (size_t i = 0; i < vec->len; i++) {
        free(vec->items[i].name.data);
        lower_expr_free(vec->items[i].default_value);
    }
    free(vec->items);
}

void lower_expr_free(DsLowerExpr *expr) {
    if (!expr) return;
    switch (expr->kind) {
#include "generated/hir_expr_free.inc"
    }
    free(expr);
}

void lower_stmt_free(DsLowerStmt *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
#include "generated/hir_stmt_free.inc"
    }
    free(stmt);
}

void ds_lower_program_free(DsLowerProgram *program) {
    if (!program) return;
    for (size_t i = 0; i < program->script_decls.len; i++) {
        free(program->script_decls.items[i].name.data);
        free(program->script_decls.items[i].default_text.data);
    }
    free(program->script_decls.items);
    for (size_t i = 0; i < program->functions.len; i++) {
        free(program->functions.items[i].name.data);
        lower_fn_param_vec_free(&program->functions.items[i].params);
        lower_stmt_free(program->functions.items[i].body);
        row_schema_free(&program->functions.items[i].row_schema);
    }
    free(program->functions.items);
    for (size_t i = 0; i < program->tests.len; i++) {
        free(program->tests.items[i].name.data);
        lower_stmt_free(program->tests.items[i].body);
    }
    free(program->tests.items);
    lower_stmt_vec_free(&program->statements);
    free(program);
}
