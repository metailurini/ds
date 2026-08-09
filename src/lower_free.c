#include "lower_internal.h"

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
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INT:
            free(expr->as.text.data);
            break;
        case DS_LOWER_EXPR_REGEX:
            free(expr->as.regex.data);
            break;
        case DS_LOWER_EXPR_RUN:
            ds_command_free(&expr->as.run);
            break;
        case DS_LOWER_EXPR_FIELD:
            lower_expr_free(expr->as.field.object);
            free(expr->as.field.field.data);
            break;
        case DS_LOWER_EXPR_UNARY:
            lower_expr_free(expr->as.unary.right);
            break;
        case DS_LOWER_EXPR_BINARY:
            lower_expr_free(expr->as.binary.left);
            lower_expr_free(expr->as.binary.right);
            break;
        case DS_LOWER_EXPR_CALL:
            free(expr->as.call.name.data);
            lower_expr_vec_free(&expr->as.call.args);
            row_schema_free(&expr->as.call.row_schema);
            break;
        case DS_LOWER_EXPR_INTERP:
            lower_expr_vec_free(&expr->as.interp.parts);
            break;
        case DS_LOWER_EXPR_ARRAY:
            lower_expr_vec_free(&expr->as.array.elements);
            row_schema_free(&expr->as.array.row_schema);
            break;
        case DS_LOWER_EXPR_MAP:
            lower_map_entry_vec_free(&expr->as.map.entries);
            row_schema_free(&expr->as.map.row_schema);
            break;
        case DS_LOWER_EXPR_INDEX:
            lower_expr_free(expr->as.index.object);
            lower_expr_free(expr->as.index.index);
            free(expr->as.index.map_key.data);
            row_schema_free(&expr->as.index.row_schema);
            break;
        case DS_LOWER_EXPR_RANGE:
            lower_expr_free(expr->as.range.start);
            lower_expr_free(expr->as.range.end);
            break;
        case DS_LOWER_EXPR_BOOL:
        case DS_LOWER_EXPR_ERROR:
            break;
    }
    free(expr);
}

void lower_stmt_free(DsLowerStmt *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET:
            free(stmt->as.let_stmt.name.data);
            lower_expr_free(stmt->as.let_stmt.value);
            row_schema_free(&stmt->as.let_stmt.row_schema);
            break;
        case DS_LOWER_STMT_ASSIGN:
            free(stmt->as.assign_stmt.name.data);
            lower_expr_free(stmt->as.assign_stmt.value);
            break;
        case DS_LOWER_STMT_INDEX_ASSIGN:
            free(stmt->as.index_assign_stmt.name.data);
            lower_expr_free(stmt->as.index_assign_stmt.index);
            lower_expr_free(stmt->as.index_assign_stmt.value);
            break;
        case DS_LOWER_STMT_IF:
            lower_expr_free(stmt->as.if_stmt.condition);
            lower_stmt_free(stmt->as.if_stmt.then_branch);
            lower_stmt_free(stmt->as.if_stmt.else_branch);
            break;
        case DS_LOWER_STMT_BLOCK:
            lower_stmt_vec_free(&stmt->as.block_stmt.statements);
            break;
        case DS_LOWER_STMT_CMD:
            ds_command_free(&stmt->as.cmd_stmt);
            break;
        case DS_LOWER_STMT_CALL:
            free(stmt->as.call_stmt.name.data);
            lower_expr_vec_free(&stmt->as.call_stmt.args);
            break;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            free(stmt->as.for_stmt.name.data);
            free(stmt->as.for_stmt.value_name.data);
            lower_expr_free(stmt->as.for_stmt.iterable);
            lower_stmt_free(stmt->as.for_stmt.body);
            row_schema_free(&stmt->as.for_stmt.row_schema);
            break;
        case DS_LOWER_STMT_WHILE:
            lower_expr_free(stmt->as.while_stmt.condition);
            lower_stmt_free(stmt->as.while_stmt.body);
            break;
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            break;
        case DS_LOWER_STMT_CASE:
            lower_expr_free(stmt->as.case_stmt.selector);
            lower_case_arm_vec_free(&stmt->as.case_stmt.arms);
            break;
        case DS_LOWER_STMT_PUSH:
            free(stmt->as.push_stmt.name.data);
            lower_expr_free(stmt->as.push_stmt.value);
            row_schema_free(&stmt->as.push_stmt.row_schema);
            break;
        case DS_LOWER_STMT_ASSERT:
            lower_expr_free(stmt->as.assert_stmt.condition);
            break;
        case DS_LOWER_STMT_RETURN:
            lower_expr_free(stmt->as.return_stmt.value);
            row_schema_free(&stmt->as.return_stmt.row_schema);
            break;
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            lower_stmt_free(stmt->as.handler_stmt.body);
            break;
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
