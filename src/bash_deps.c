#include "bash_internal.h"
#include "ds_signal.h"
#include "ds_command_facts.h"

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
        case DS_LOWER_EXPR_RANGE: return expr_uses_run(expr->as.range.start) || expr_uses_run(expr->as.range.end);
        case DS_LOWER_EXPR_CALL:
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_run(expr->as.call.args.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) if (expr_uses_run(expr->as.interp.parts.items[i])) return true;
            return false;
        default: return false;
    }
}

static bool expr_uses_pipeline_run(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_RUN: return ds_command_is_pipeline(&expr->as.run);
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
        case DS_LOWER_EXPR_RANGE: return expr_uses_pipeline_run(expr->as.range.start) || expr_uses_pipeline_run(expr->as.range.end);
        case DS_LOWER_EXPR_CALL:
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_pipeline_run(expr->as.call.args.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) if (expr_uses_pipeline_run(expr->as.interp.parts.items[i])) return true;
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
        case DS_LOWER_EXPR_RANGE: return expr_uses_stdlib(expr->as.range.start) || expr_uses_stdlib(expr->as.range.end);
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) if (expr_uses_stdlib(expr->as.interp.parts.items[i])) return true;
            return false;
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
        case DS_LOWER_EXPR_RANGE: return expr_uses_collection_index(expr->as.range.start) || expr_uses_collection_index(expr->as.range.end);
        case DS_LOWER_EXPR_CALL:
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_collection_index(expr->as.call.args.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) if (expr_uses_collection_index(expr->as.interp.parts.items[i])) return true;
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
        case DS_LOWER_EXPR_RANGE: return expr_uses_map_literal(expr->as.range.start) || expr_uses_map_literal(expr->as.range.end);
        case DS_LOWER_EXPR_CALL:
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_map_literal(expr->as.call.args.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) if (expr_uses_map_literal(expr->as.interp.parts.items[i])) return true;
            return false;
        default: return false;
    }
}

static bool int_binary_op(DsStr op) {
    return str_eq(op, "+") || str_eq(op, "-") || str_eq(op, "*") ||
           str_eq(op, "/") || str_eq(op, "%") || str_eq(op, "**");
}

static bool word_has_arith_interp(DsStr word) {
    if (word.len < 4 || word.data[0] != '"' || word.data[word.len - 1] != '"') return false;
    for (size_t i = 1; i + 1 < word.len; i++) {
        if (word.data[i] != '{') continue;
        size_t j = i + 1;
        bool saw_op = false;
        while (j + 1 < word.len && word.data[j] != '}') {
            char c = word.data[j++];
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '(' || c == ')') saw_op = true;
        }
        if (j + 1 < word.len && word.data[j] == '}' && saw_op) return true;
    }
    return false;
}

static bool command_uses_int_helpers(const DsCommand *command) {
    for (size_t s = 0; s < command->stages.len; s++) {
        for (size_t i = 0; i < command->stages.items[s].words.len; i++) {
            if (word_has_arith_interp(command->stages.items[s].words.items[i].text)) return true;
        }
    }
    return word_has_arith_interp(command->redirect.target);
}

static bool expr_uses_int_helpers(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_BINARY:
            return int_binary_op(expr->as.binary.op) || expr_uses_int_helpers(expr->as.binary.left) || expr_uses_int_helpers(expr->as.binary.right);
        case DS_LOWER_EXPR_UNARY:
            return str_eq(expr->as.unary.op, "-") || expr_uses_int_helpers(expr->as.unary.right);
        case DS_LOWER_EXPR_CALL:
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
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) if (expr_uses_int_helpers(expr->as.interp.parts.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_RANGE:
            return expr_uses_int_helpers(expr->as.range.start) || expr_uses_int_helpers(expr->as.range.end);
        default:
            return false;
    }
}

static bool expr_uses_function_value_helpers(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_CALL:
            if (expr->as.call.is_user_function) return true;
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_function_value_helpers(expr->as.call.args.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_BINARY:
            return expr_uses_function_value_helpers(expr->as.binary.left) || expr_uses_function_value_helpers(expr->as.binary.right);
        case DS_LOWER_EXPR_UNARY: return expr_uses_function_value_helpers(expr->as.unary.right);
        case DS_LOWER_EXPR_FIELD: return expr_uses_function_value_helpers(expr->as.field.object);
        case DS_LOWER_EXPR_INDEX: return expr_uses_function_value_helpers(expr->as.index.object) || expr_uses_function_value_helpers(expr->as.index.index);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) if (expr_uses_function_value_helpers(expr->as.array.elements.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) if (expr_uses_function_value_helpers(expr->as.map.entries.items[i].value)) return true;
            return false;
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) if (expr_uses_function_value_helpers(expr->as.interp.parts.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_RANGE:
            return expr_uses_function_value_helpers(expr->as.range.start) || expr_uses_function_value_helpers(expr->as.range.end);
        default: return false;
    }
}

static bool stmt_uses_run(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_run(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_run(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN: return expr_uses_run(stmt->as.index_assign_stmt.index) || expr_uses_run(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_run(stmt->as.if_stmt.condition) || stmt_uses_run(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_run(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_run(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_CMD: return false;
        case DS_LOWER_STMT_CALL: return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return expr_uses_run(stmt->as.for_stmt.iterable) || stmt_uses_run(stmt->as.for_stmt.body);
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
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_uses_run(stmt->as.handler_stmt.body);
    }
    return false;
}

static bool stmt_uses_pipeline_run(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_pipeline_run(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_pipeline_run(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN: return expr_uses_pipeline_run(stmt->as.index_assign_stmt.index) || expr_uses_pipeline_run(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_pipeline_run(stmt->as.if_stmt.condition) || stmt_uses_pipeline_run(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_pipeline_run(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_pipeline_run(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return expr_uses_pipeline_run(stmt->as.for_stmt.iterable) || stmt_uses_pipeline_run(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_pipeline_run(stmt->as.while_stmt.condition) || stmt_uses_pipeline_run(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_pipeline_run(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_pipeline_run(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_pipeline_run(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_pipeline_run(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_CMD: return false;
        case DS_LOWER_STMT_CALL:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return false;
        case DS_LOWER_STMT_RETURN:
            return expr_uses_pipeline_run(stmt->as.return_stmt.value);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return stmt_uses_pipeline_run(stmt->as.handler_stmt.body);
    }
    return false;
}

static bool stmt_has_command(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_CMD: return true;
        case DS_LOWER_STMT_LET: return stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_RUN;
        case DS_LOWER_STMT_ASSIGN: return stmt->as.assign_stmt.value && stmt->as.assign_stmt.value->kind == DS_LOWER_EXPR_RUN;
        case DS_LOWER_STMT_INDEX_ASSIGN: return expr_uses_run(stmt->as.index_assign_stmt.index) || expr_uses_run(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF: return stmt_has_command(stmt->as.if_stmt.then_branch) || stmt_has_command(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_has_command(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return stmt_has_command(stmt->as.for_stmt.body);
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
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return stmt_has_command(stmt->as.handler_stmt.body);
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
        case DS_LOWER_STMT_INDEX_ASSIGN: return expr_uses_stdlib(stmt->as.index_assign_stmt.index) || expr_uses_stdlib(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_stdlib(stmt->as.if_stmt.condition) || stmt_uses_stdlib(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_stdlib(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_stdlib(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_CALL:
            if (ds_stdlib_is_name(stmt->as.call_stmt.name)) return true;
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) {
                if (expr_uses_stdlib(stmt->as.call_stmt.args.items[i])) return true;
            }
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return expr_uses_stdlib(stmt->as.for_stmt.iterable) || stmt_uses_stdlib(stmt->as.for_stmt.body);
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
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_uses_stdlib(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_CMD: return command_uses_stdlib(&stmt->as.cmd_stmt);
    }
    return false;
}

static bool stmt_uses_collection_index(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_collection_index(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_collection_index(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN: return true;
        case DS_LOWER_STMT_IF:
            return expr_uses_collection_index(stmt->as.if_stmt.condition) || stmt_uses_collection_index(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_collection_index(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_collection_index(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return expr_uses_collection_index(stmt->as.for_stmt.iterable) || stmt_uses_collection_index(stmt->as.for_stmt.body);
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
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_uses_collection_index(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_CALL:
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) {
                if (expr_uses_collection_index(stmt->as.call_stmt.args.items[i])) return true;
            }
            return false;
        case DS_LOWER_STMT_CMD:
            return false;
    }
    return false;
}

static bool stmt_uses_map_literal(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_map_literal(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_map_literal(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN: return expr_uses_map_literal(stmt->as.index_assign_stmt.index) || expr_uses_map_literal(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_map_literal(stmt->as.if_stmt.condition) || stmt_uses_map_literal(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_map_literal(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_map_literal(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return expr_uses_map_literal(stmt->as.for_stmt.iterable) || stmt_uses_map_literal(stmt->as.for_stmt.body);
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
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_uses_map_literal(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_CALL:
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) {
                if (expr_uses_map_literal(stmt->as.call_stmt.args.items[i])) return true;
            }
            return false;
        case DS_LOWER_STMT_CMD:
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

static bool stmt_uses_map_iteration(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_FOR_MAP:
            return true;
        case DS_LOWER_STMT_IF:
            return stmt_uses_map_iteration(stmt->as.if_stmt.then_branch) || stmt_uses_map_iteration(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_map_iteration(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_RANGE:
            return stmt_uses_map_iteration(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE:
            return stmt_uses_map_iteration(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_map_iteration(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return stmt_uses_map_iteration(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_LET:
        case DS_LOWER_STMT_ASSIGN:
        case DS_LOWER_STMT_INDEX_ASSIGN:
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

bool program_uses_map_iteration(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (program->functions.items[i].body && stmt_uses_map_iteration(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_map_iteration(program->statements.items[i])) return true;
    return false;
}

static bool stmt_uses_map_assignment(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_INDEX_ASSIGN:
            return stmt->as.index_assign_stmt.target_is_map;
        case DS_LOWER_STMT_IF:
            return stmt_uses_map_assignment(stmt->as.if_stmt.then_branch) || stmt_uses_map_assignment(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_map_assignment(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            return stmt_uses_map_assignment(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE:
            return stmt_uses_map_assignment(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_map_assignment(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return stmt_uses_map_assignment(stmt->as.handler_stmt.body);
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

bool program_uses_map_assignment(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (program->functions.items[i].body && stmt_uses_map_assignment(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_map_assignment(program->statements.items[i])) return true;
    return false;
}

bool program_uses_map_literal(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (program->functions.items[i].body && stmt_uses_map_literal(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_map_literal(program->statements.items[i])) return true;
    return false;
}

static bool command_is_control(const DsCommand *command) {
    if (!command || ds_command_is_pipeline(command) || command->redirect.kind != DS_REDIRECT_NONE) return false;
    if (command->stages.len == 0 || command->stages.items[0].words.len == 0) return false;
    DsStr first = command->stages.items[0].words.items[0].text;
    return (first.len == 4 && memcmp(first.data, "fail", 4) == 0) ||
           (first.len == 4 && memcmp(first.data, "exit", 4) == 0);
}

static bool stmt_uses_control_commands(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_CMD:
            return command_is_control(&stmt->as.cmd_stmt);
        case DS_LOWER_STMT_IF:
            return stmt_uses_control_commands(stmt->as.if_stmt.then_branch) || stmt_uses_control_commands(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_control_commands(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            return stmt_uses_control_commands(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE:
            return stmt_uses_control_commands(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_control_commands(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return stmt_uses_control_commands(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_LET:
        case DS_LOWER_STMT_ASSIGN:
        case DS_LOWER_STMT_INDEX_ASSIGN:
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

bool program_uses_control_commands(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (program->functions.items[i].body && stmt_uses_control_commands(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_control_commands(program->statements.items[i])) return true;
    return false;
}

static bool stmt_uses_int_helpers(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_int_helpers(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return stmt->as.assign_stmt.op != DS_LOWER_ASSIGN_SET || expr_uses_int_helpers(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN: return expr_uses_int_helpers(stmt->as.index_assign_stmt.index) || expr_uses_int_helpers(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_int_helpers(stmt->as.if_stmt.condition) || stmt_uses_int_helpers(stmt->as.if_stmt.then_branch) ||
                   stmt_uses_int_helpers(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_int_helpers(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return expr_uses_int_helpers(stmt->as.for_stmt.iterable) || stmt_uses_int_helpers(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_int_helpers(stmt->as.while_stmt.condition) || stmt_uses_int_helpers(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_int_helpers(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_int_helpers(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_int_helpers(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_int_helpers(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_RETURN: return expr_uses_int_helpers(stmt->as.return_stmt.value);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_uses_int_helpers(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_CMD: return command_uses_int_helpers(&stmt->as.cmd_stmt);
        case DS_LOWER_STMT_CALL:
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) {
                if (expr_uses_int_helpers(stmt->as.call_stmt.args.items[i])) return true;
            }
            return false;
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

static bool stmt_uses_function_value_helpers(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_function_value_helpers(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_function_value_helpers(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN: return expr_uses_function_value_helpers(stmt->as.index_assign_stmt.index) || expr_uses_function_value_helpers(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_function_value_helpers(stmt->as.if_stmt.condition) || stmt_uses_function_value_helpers(stmt->as.if_stmt.then_branch) || stmt_uses_function_value_helpers(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_function_value_helpers(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return expr_uses_function_value_helpers(stmt->as.for_stmt.iterable) || stmt_uses_function_value_helpers(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_function_value_helpers(stmt->as.while_stmt.condition) || stmt_uses_function_value_helpers(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_function_value_helpers(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_function_value_helpers(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_function_value_helpers(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_function_value_helpers(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_RETURN: return expr_uses_function_value_helpers(stmt->as.return_stmt.value);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_uses_function_value_helpers(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_CALL:
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) if (expr_uses_function_value_helpers(stmt->as.call_stmt.args.items[i])) return true;
            return false;
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return false;
    }
    return false;
}

bool program_uses_function_value_helpers(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (stmt_uses_function_value_helpers(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_function_value_helpers(program->statements.items[i])) return true;
    return false;
}

static bool stmt_uses_handlers(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return true;
        case DS_LOWER_STMT_IF:
            return stmt_uses_handlers(stmt->as.if_stmt.then_branch) || stmt_uses_handlers(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_handlers(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            return stmt_uses_handlers(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE:
            return stmt_uses_handlers(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_handlers(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_LET:
        case DS_LOWER_STMT_ASSIGN:
        case DS_LOWER_STMT_INDEX_ASSIGN:
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

bool program_uses_handlers(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (stmt_uses_handlers(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_handlers(program->statements.items[i])) return true;
    return false;
}

static bool stmt_uses_signal_handlers(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return ds_handler_signal_is_runtime_cleanup(stmt->as.handler_stmt.signal) || stmt_uses_signal_handlers(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_IF:
            return stmt_uses_signal_handlers(stmt->as.if_stmt.then_branch) || stmt_uses_signal_handlers(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_signal_handlers(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            return stmt_uses_signal_handlers(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE:
            return stmt_uses_signal_handlers(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_signal_handlers(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_LET:
        case DS_LOWER_STMT_ASSIGN:
        case DS_LOWER_STMT_INDEX_ASSIGN:
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

bool program_uses_signal_handlers(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (stmt_uses_signal_handlers(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_signal_handlers(program->statements.items[i])) return true;
    return false;
}

static bool expr_needs_type_tags_for_truthiness(const DsLowerExpr *expr) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_IDENT) return true;
    if (expr->kind == DS_LOWER_EXPR_INDEX) return true;
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
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return stmt_uses_case(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_needs_type_tags_for_truthiness(stmt->as.while_stmt.condition) || stmt_uses_case(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_LET:
        case DS_LOWER_STMT_ASSIGN:
        case DS_LOWER_STMT_INDEX_ASSIGN:
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_CALL:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
        case DS_LOWER_STMT_PUSH:
        case DS_LOWER_STMT_ASSERT:
        case DS_LOWER_STMT_RETURN:
            return false;
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return stmt_uses_case(stmt->as.handler_stmt.body);
    }
    return false;
}

bool program_uses_case(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (stmt_uses_case(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_case(program->statements.items[i])) return true;
    return false;
}

static bool expr_uses_membership(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_BINARY:
            return str_eq(expr->as.binary.op, "in") || expr_uses_membership(expr->as.binary.left) || expr_uses_membership(expr->as.binary.right);
        case DS_LOWER_EXPR_UNARY: return expr_uses_membership(expr->as.unary.right);
        case DS_LOWER_EXPR_FIELD: return expr_uses_membership(expr->as.field.object);
        case DS_LOWER_EXPR_INDEX: return expr_uses_membership(expr->as.index.object) || expr_uses_membership(expr->as.index.index);
        case DS_LOWER_EXPR_CALL:
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_membership(expr->as.call.args.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) if (expr_uses_membership(expr->as.interp.parts.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) if (expr_uses_membership(expr->as.array.elements.items[i])) return true;
            return false;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) if (expr_uses_membership(expr->as.map.entries.items[i].value)) return true;
            return false;
        case DS_LOWER_EXPR_RANGE:
            return expr_uses_membership(expr->as.range.start) || expr_uses_membership(expr->as.range.end);
        default: return false;
    }
}

static bool stmt_uses_membership(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_membership(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_membership(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN: return expr_uses_membership(stmt->as.index_assign_stmt.index) || expr_uses_membership(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF: return expr_uses_membership(stmt->as.if_stmt.condition) || stmt_uses_membership(stmt->as.if_stmt.then_branch) || stmt_uses_membership(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_membership(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            return expr_uses_membership(stmt->as.for_stmt.iterable) || stmt_uses_membership(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_membership(stmt->as.while_stmt.condition) || stmt_uses_membership(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_membership(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_membership(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_membership(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_membership(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_RETURN: return expr_uses_membership(stmt->as.return_stmt.value);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_uses_membership(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_CALL:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return false;
    }
    return false;
}

bool program_uses_membership(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (stmt_uses_membership(program->functions.items[i].body)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_membership(program->statements.items[i])) return true;
    return false;
}
