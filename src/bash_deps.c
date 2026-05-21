#include "bash_internal.h"

#include <string.h>

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
        case DS_LOWER_EXPR_CALL:
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_run(expr->as.call.args.items[i])) return true;
            return false;
        default: return false;
    }
}

static bool expr_uses_pipeline_run(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_RUN: return expr->as.run.stages.len > 1;
        case DS_LOWER_EXPR_FIELD: return expr_uses_pipeline_run(expr->as.field.object);
        case DS_LOWER_EXPR_INDEX: return expr_uses_pipeline_run(expr->as.index.object) || expr_uses_pipeline_run(expr->as.index.index);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) if (expr_uses_pipeline_run(expr->as.array.elements.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) if (expr_uses_pipeline_run(expr->as.map.entries.items[i].value)) return true;
            return false;
        case DS_LOWER_EXPR_UNARY: return expr_uses_pipeline_run(expr->as.unary.right);
        case DS_LOWER_EXPR_BINARY: return expr_uses_pipeline_run(expr->as.binary.left) || expr_uses_pipeline_run(expr->as.binary.right);
        case DS_LOWER_EXPR_CALL:
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_pipeline_run(expr->as.call.args.items[i])) return true;
            return false;
        default: return false;
    }
}

static bool string_literal_needs_stdlib(DsStr text) {
    if (text.len < 2 || text.data[0] != '"') return false;
    for (size_t i = 0; i < text.len; i++) {
        if (text.data[i] != ':') continue;
        if (i + 1 < text.len && text.data[i + 1] == '^') return true;
        if (i + 5 < text.len && memcmp(text.data + i + 1, "trim", 4) == 0 && text.data[i + 5] == '}') return true;
        if (i + 6 < text.len && memcmp(text.data + i + 1, "upper", 5) == 0 && text.data[i + 6] == '}') return true;
        if (i + 6 < text.len && memcmp(text.data + i + 1, "lower", 5) == 0 && text.data[i + 6] == '}') return true;
    }
    return false;
}

static bool expr_uses_stdlib(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING: return string_literal_needs_stdlib(expr->as.text);
        case DS_LOWER_EXPR_CALL:
            if (ds_stdlib_is_name(expr->as.call.name)) return true;
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_stdlib(expr->as.call.args.items[i])) return true;
            return false;
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

static bool command_uses_stdlib(const DsCommand *command) {
    if (!command) return false;
    for (size_t s = 0; s < command->stages.len; s++) {
        const DsCommandStage *stage = &command->stages.items[s];
        for (size_t i = 0; i < stage->words.len; i++) {
            if (string_literal_needs_stdlib(stage->words.items[i].text)) return true;
        }
    }
    return false;
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
        case DS_LOWER_EXPR_CALL:
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_collection_index(expr->as.call.args.items[i])) return true;
            return false;
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
        case DS_LOWER_EXPR_CALL:
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_map_literal(expr->as.call.args.items[i])) return true;
            return false;
        default: return false;
    }
}

static bool int_binary_op(DsStr op) {
    return str_eq(op, "+") || str_eq(op, "-") || str_eq(op, "*") ||
           str_eq(op, "/") || str_eq(op, "%") || str_eq(op, "**");
}

static bool expr_uses_int_helpers(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_BINARY:
            return int_binary_op(expr->as.binary.op) || expr_uses_int_helpers(expr->as.binary.left) || expr_uses_int_helpers(expr->as.binary.right);
        case DS_LOWER_EXPR_UNARY:
            return str_eq(expr->as.unary.op, "-") || expr_uses_int_helpers(expr->as.unary.right);
        case DS_LOWER_EXPR_CALL:
            if (expr->as.call.is_user_function) return true;
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_int_helpers(expr->as.call.args.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_FIELD: return expr_uses_int_helpers(expr->as.field.object);
        case DS_LOWER_EXPR_INDEX: return expr_uses_int_helpers(expr->as.index.object) || expr_uses_int_helpers(expr->as.index.index);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) if (expr_uses_int_helpers(expr->as.array.elements.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) if (expr_uses_int_helpers(expr->as.map.entries.items[i].value)) return true;
            return false;
        default:
            return false;
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
        case DS_LOWER_STMT_RETURN: return expr_uses_run(stmt->as.return_stmt.value);
    }
    return false;
}

static bool stmt_uses_pipeline_run(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_pipeline_run(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_pipeline_run(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_pipeline_run(stmt->as.if_stmt.condition) || stmt_uses_pipeline_run(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_pipeline_run(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_pipeline_run(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY: return expr_uses_pipeline_run(stmt->as.for_stmt.iterable) || stmt_uses_pipeline_run(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_pipeline_run(stmt->as.while_stmt.condition) || stmt_uses_pipeline_run(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_pipeline_run(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_pipeline_run(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_pipeline_run(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_pipeline_run(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_CALL:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return false;
        case DS_LOWER_STMT_RETURN:
            return expr_uses_pipeline_run(stmt->as.return_stmt.value);
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
        case DS_LOWER_STMT_RETURN:
            return expr_uses_run(stmt->as.return_stmt.value);
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
        case DS_LOWER_STMT_RETURN: return expr_uses_stdlib(stmt->as.return_stmt.value);
        case DS_LOWER_STMT_CMD: return command_uses_stdlib(&stmt->as.cmd_stmt);
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
        case DS_LOWER_STMT_RETURN: return expr_uses_collection_index(stmt->as.return_stmt.value);
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
        case DS_LOWER_STMT_RETURN: return expr_uses_map_literal(stmt->as.return_stmt.value);
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

bool program_uses_pipeline_run(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (program->functions.items[i].body && stmt_uses_pipeline_run(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_pipeline_run(program->statements.items[i])) return true;
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

static bool stmt_uses_int_helpers(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_int_helpers(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return stmt->as.assign_stmt.op != DS_LOWER_ASSIGN_SET || expr_uses_int_helpers(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_int_helpers(stmt->as.if_stmt.condition) || stmt_uses_int_helpers(stmt->as.if_stmt.then_branch) ||
                   stmt_uses_int_helpers(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_int_helpers(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY: return expr_uses_int_helpers(stmt->as.for_stmt.iterable) || stmt_uses_int_helpers(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_int_helpers(stmt->as.while_stmt.condition) || stmt_uses_int_helpers(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_int_helpers(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_int_helpers(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_int_helpers(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_int_helpers(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_RETURN: return expr_uses_int_helpers(stmt->as.return_stmt.value);
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_CALL:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return false;
    }
    return false;
}

bool program_uses_int_helpers(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (stmt_uses_int_helpers(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_int_helpers(program->statements.items[i])) return true;
    return false;
}

static bool expr_needs_type_tags_for_truthiness(const DsLowerExpr *expr) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_IDENT) return true;
    if (expr->kind == DS_LOWER_EXPR_UNARY && str_eq(expr->as.unary.op, "!")) return expr_needs_type_tags_for_truthiness(expr->as.unary.right);
    return false;
}

static bool stmt_uses_case(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_CASE: return true;
        case DS_LOWER_STMT_IF:
            return expr_needs_type_tags_for_truthiness(stmt->as.if_stmt.condition) || stmt_uses_case(stmt->as.if_stmt.then_branch) || stmt_uses_case(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_case(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY: return stmt_uses_case(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_needs_type_tags_for_truthiness(stmt->as.while_stmt.condition) || stmt_uses_case(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_LET:
        case DS_LOWER_STMT_ASSIGN:
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_CALL:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
        case DS_LOWER_STMT_PUSH:
        case DS_LOWER_STMT_ASSERT:
        case DS_LOWER_STMT_RETURN:
            return false;
    }
    return false;
}

bool program_uses_case(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (stmt_uses_case(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_case(program->statements.items[i])) return true;
    return false;
}
