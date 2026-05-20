#include "bash_internal.h"

static bool expr_uses_run(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_RUN: return true;
        case DS_LOWER_EXPR_FIELD: return expr_uses_run(expr->as.field.object);
        case DS_LOWER_EXPR_INDEX: return expr_uses_run(expr->as.index.object) || expr_uses_run(expr->as.index.index);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) if (expr_uses_run(expr->as.array.elements.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) if (expr_uses_run(expr->as.map.entries.items[i].value)) return true;
            return false;
        case DS_LOWER_EXPR_UNARY: return expr_uses_run(expr->as.unary.right);
        case DS_LOWER_EXPR_BINARY: return expr_uses_run(expr->as.binary.left) || expr_uses_run(expr->as.binary.right);
        default: return false;
    }
}

static bool expr_uses_stdlib(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_CALL: return ds_stdlib_is_name(expr->as.call.name);
        case DS_LOWER_EXPR_FIELD: return expr_uses_stdlib(expr->as.field.object);
        case DS_LOWER_EXPR_INDEX: return expr_uses_stdlib(expr->as.index.object) || expr_uses_stdlib(expr->as.index.index);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) if (expr_uses_stdlib(expr->as.array.elements.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) if (expr_uses_stdlib(expr->as.map.entries.items[i].value)) return true;
            return false;
        case DS_LOWER_EXPR_UNARY: return expr_uses_stdlib(expr->as.unary.right);
        case DS_LOWER_EXPR_BINARY: return expr_uses_stdlib(expr->as.binary.left) || expr_uses_stdlib(expr->as.binary.right);
        default: return false;
    }
}

static bool expr_uses_collection_index(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_INDEX: return true;
        case DS_LOWER_EXPR_FIELD: return expr_uses_collection_index(expr->as.field.object);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) if (expr_uses_collection_index(expr->as.array.elements.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) if (expr_uses_collection_index(expr->as.map.entries.items[i].value)) return true;
            return false;
        case DS_LOWER_EXPR_UNARY: return expr_uses_collection_index(expr->as.unary.right);
        case DS_LOWER_EXPR_BINARY: return expr_uses_collection_index(expr->as.binary.left) || expr_uses_collection_index(expr->as.binary.right);
        default: return false;
    }
}

static bool expr_uses_map_literal(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_MAP: return true;
        case DS_LOWER_EXPR_INDEX: return expr_uses_map_literal(expr->as.index.object) || expr_uses_map_literal(expr->as.index.index);
        case DS_LOWER_EXPR_FIELD: return expr_uses_map_literal(expr->as.field.object);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) if (expr_uses_map_literal(expr->as.array.elements.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_UNARY: return expr_uses_map_literal(expr->as.unary.right);
        case DS_LOWER_EXPR_BINARY: return expr_uses_map_literal(expr->as.binary.left) || expr_uses_map_literal(expr->as.binary.right);
        default: return false;
    }
}

static bool stmt_uses_run(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_run(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_run(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_run(stmt->as.if_stmt.condition) || stmt_uses_run(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_run(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_run(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_CMD: return false;
        case DS_LOWER_STMT_CALL: return false;
        case DS_LOWER_STMT_FOR_ARRAY: return expr_uses_run(stmt->as.for_stmt.iterable) || stmt_uses_run(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_run(stmt->as.while_stmt.condition) || stmt_uses_run(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_run(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_run(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE: return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_run(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_run(stmt->as.assert_stmt.condition);
    }
    return false;
}

static bool stmt_has_command(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_CMD: return true;
        case DS_LOWER_STMT_LET: return stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_RUN;
        case DS_LOWER_STMT_ASSIGN: return stmt->as.assign_stmt.value && stmt->as.assign_stmt.value->kind == DS_LOWER_EXPR_RUN;
        case DS_LOWER_STMT_IF: return stmt_has_command(stmt->as.if_stmt.then_branch) || stmt_has_command(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_has_command(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY: return stmt_has_command(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return stmt_has_command(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_has_command(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_CALL:
        case DS_LOWER_STMT_PUSH:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
        case DS_LOWER_STMT_ASSERT:
            return false;
    }
    return false;
}

bool program_has_command(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (stmt_has_command(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_has_command(program->statements.items[i])) return true;
    return false;
}

static bool stmt_uses_stdlib(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_stdlib(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_stdlib(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_stdlib(stmt->as.if_stmt.condition) || stmt_uses_stdlib(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_stdlib(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_stdlib(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_CALL: return ds_stdlib_is_name(stmt->as.call_stmt.name);
        case DS_LOWER_STMT_FOR_ARRAY: return expr_uses_stdlib(stmt->as.for_stmt.iterable) || stmt_uses_stdlib(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_stdlib(stmt->as.while_stmt.condition) || stmt_uses_stdlib(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_stdlib(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_stdlib(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE: return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_stdlib(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_stdlib(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_CMD: return false;
    }
    return false;
}

static bool stmt_uses_collection_index(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_collection_index(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_collection_index(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_collection_index(stmt->as.if_stmt.condition) || stmt_uses_collection_index(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_collection_index(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_collection_index(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY: return expr_uses_collection_index(stmt->as.for_stmt.iterable) || stmt_uses_collection_index(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_collection_index(stmt->as.while_stmt.condition) || stmt_uses_collection_index(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_collection_index(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_collection_index(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE: return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_collection_index(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_collection_index(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_CALL:
            return false;
    }
    return false;
}

static bool stmt_uses_map_literal(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_map_literal(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_map_literal(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_map_literal(stmt->as.if_stmt.condition) || stmt_uses_map_literal(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_map_literal(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_map_literal(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY: return expr_uses_map_literal(stmt->as.for_stmt.iterable) || stmt_uses_map_literal(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_map_literal(stmt->as.while_stmt.condition) || stmt_uses_map_literal(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_map_literal(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_map_literal(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE: return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_map_literal(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_map_literal(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_CALL:
            return false;
    }
    return false;
}

bool program_uses_run(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (program->functions.items[i].body && stmt_uses_run(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_run(program->statements.items[i])) return true;
    return false;
}

bool program_uses_stdlib(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (program->functions.items[i].body && stmt_uses_stdlib(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_stdlib(program->statements.items[i])) return true;
    return false;
}

bool program_uses_collection_index(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (program->functions.items[i].body && stmt_uses_collection_index(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_collection_index(program->statements.items[i])) return true;
    return false;
}

bool program_uses_map_literal(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (program->functions.items[i].body && stmt_uses_map_literal(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_map_literal(program->statements.items[i])) return true;
    return false;
}

static bool stmt_uses_case(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_CASE: return true;
        case DS_LOWER_STMT_IF:
            return stmt_uses_case(stmt->as.if_stmt.then_branch) || stmt_uses_case(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_case(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY: return stmt_uses_case(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return stmt_uses_case(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_LET:
        case DS_LOWER_STMT_ASSIGN:
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_CALL:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
        case DS_LOWER_STMT_PUSH:
        case DS_LOWER_STMT_ASSERT:
            return false;
    }
    return false;
}

bool program_uses_case(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (stmt_uses_case(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_case(program->statements.items[i])) return true;
    return false;
}
