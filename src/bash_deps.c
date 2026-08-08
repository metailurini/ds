#include "bash_internal.h"
#include "bash_helpers.h"
#include "ds_signal.h"
#include "ds_command_facts.h"

#include <stdlib.h>
#include <string.h>

typedef bool (*ExprUsePredicate)(const DsLowerExpr *expr, void *context);

static bool expr_uses(const DsLowerExpr *expr, ExprUsePredicate predicate, void *context) {
    if (!expr) return false;
    if (predicate(expr, context)) return true;
    switch (expr->kind) {
        case DS_LOWER_EXPR_FIELD:
            return expr_uses(expr->as.field.object, predicate, context);
        case DS_LOWER_EXPR_INDEX:
            return expr_uses(expr->as.index.object, predicate, context) || expr_uses(expr->as.index.index, predicate, context);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) if (expr_uses(expr->as.array.elements.items[i], predicate, context)) return true;
            return false;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) if (expr_uses(expr->as.map.entries.items[i].value, predicate, context)) return true;
            return false;
        case DS_LOWER_EXPR_UNARY:
            return expr_uses(expr->as.unary.right, predicate, context);
        case DS_LOWER_EXPR_BINARY:
            return expr_uses(expr->as.binary.left, predicate, context) || expr_uses(expr->as.binary.right, predicate, context);
        case DS_LOWER_EXPR_RANGE:
            return expr_uses(expr->as.range.start, predicate, context) || expr_uses(expr->as.range.end, predicate, context);
        case DS_LOWER_EXPR_CALL:
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses(expr->as.call.args.items[i], predicate, context)) return true;
            return false;
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) if (expr_uses(expr->as.interp.parts.items[i], predicate, context)) return true;
            return false;
        default:
            return false;
    }
}

static bool expr_is_run(const DsLowerExpr *expr, void *context) {
    (void)context;
    return expr->kind == DS_LOWER_EXPR_RUN;
}

static bool expr_uses_run(const DsLowerExpr *expr) {
    return expr_uses(expr, expr_is_run, NULL);
}

static bool expr_is_pipeline_run(const DsLowerExpr *expr, void *context) {
    (void)context;
    return expr->kind == DS_LOWER_EXPR_RUN && ds_command_is_pipeline(&expr->as.run);
}

static bool expr_uses_pipeline_run(const DsLowerExpr *expr) {
    return expr_uses(expr, expr_is_pipeline_run, NULL);
}

static bool stdlib_call_uses_base_helpers(DsStr name) {
    DsStdlibNamespace ns = ds_stdlib_namespace(name);
    return ds_stdlib_is_name(name) &&
           ns != DS_STDLIB_NAMESPACE_STRING &&
           ns != DS_STDLIB_NAMESPACE_REGEX &&
           !ds_stdlib_is_glob_helper(name);
}

static unsigned string_literal_helper_mask(DsStr text) {
    unsigned mask = 0;
    if (text.len < 2 || text.data[0] != '"') return 0;
    for (size_t i = 0; i < text.len; i++) {
        if (text.data[i] != ':') continue;
        if (i + 1 < text.len && text.data[i + 1] == '^') mask |= DS_BASH_STRING_HELPER_FORMAT_CENTER;
        if (i + 5 < text.len && memcmp(text.data + i + 1, "trim", 4) == 0 && text.data[i + 5] == '}') mask |= DS_BASH_STRING_HELPER_TRIM;
        if (i + 6 < text.len && memcmp(text.data + i + 1, "upper", 5) == 0 && text.data[i + 6] == '}') mask |= DS_BASH_STRING_HELPER_UPPER;
        if (i + 6 < text.len && memcmp(text.data + i + 1, "lower", 5) == 0 && text.data[i + 6] == '}') mask |= DS_BASH_STRING_HELPER_LOWER;
    }
    return mask;
}

static bool string_literal_contains_index_interpolation(DsStr text) {
    if (text.len < 2 || text.data[0] != '"') return false;
    for (size_t i = 0; i < text.len; i++) {
        if (text.data[i] != '{') continue;
        bool saw_bracket = false;
        for (size_t j = i + 1; j < text.len && text.data[j] != '}'; j++) {
            if (text.data[j] == '[') saw_bracket = true;
        }
        if (saw_bracket) return true;
    }
    return false;
}

static bool expr_is_base_stdlib_call(const DsLowerExpr *expr, void *context) {
    (void)context;
    return expr->kind == DS_LOWER_EXPR_CALL && stdlib_call_uses_base_helpers(expr->as.call.name);
}

static bool expr_uses_stdlib(const DsLowerExpr *expr) {
    return expr_uses(expr, expr_is_base_stdlib_call, NULL);
}

static bool call_name_is_glob(DsStr name) {
    return str_eq(name, "glob") || str_eq(name, "glob!");
}

static bool literal_glob_arg_is_recursive(const DsLowerExpr *expr) {
    if (!expr || expr->kind != DS_LOWER_EXPR_STRING) return true;
    char *decoded = NULL;
    size_t len = 0;
    if (!decode_string_literal(NULL, expr, &decoded, &len)) return true;
    DsStr text = {decoded, len};
    bool recursive = ds_glob_pattern_contains_recursive(text);
    free(decoded);
    return recursive;
}

static bool expr_uses_glob_helper(const DsLowerExpr *expr, bool recursive_only) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_CALL:
            if (call_name_is_glob(expr->as.call.name)) {
                if (!recursive_only) return true;
                if (expr->as.call.args.len == 0) return true;
                return literal_glob_arg_is_recursive(expr->as.call.args.items[0]);
            }
            for (size_t i = 0; i < expr->as.call.args.len; i++) if (expr_uses_glob_helper(expr->as.call.args.items[i], recursive_only)) return true;
            return false;
        case DS_LOWER_EXPR_FIELD: return expr_uses_glob_helper(expr->as.field.object, recursive_only);
        case DS_LOWER_EXPR_INDEX: return expr_uses_glob_helper(expr->as.index.object, recursive_only) || expr_uses_glob_helper(expr->as.index.index, recursive_only);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) if (expr_uses_glob_helper(expr->as.array.elements.items[i], recursive_only)) return true;
            return false;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) if (expr_uses_glob_helper(expr->as.map.entries.items[i].value, recursive_only)) return true;
            return false;
        case DS_LOWER_EXPR_UNARY: return expr_uses_glob_helper(expr->as.unary.right, recursive_only);
        case DS_LOWER_EXPR_BINARY: return expr_uses_glob_helper(expr->as.binary.left, recursive_only) || expr_uses_glob_helper(expr->as.binary.right, recursive_only);
        case DS_LOWER_EXPR_RANGE: return expr_uses_glob_helper(expr->as.range.start, recursive_only) || expr_uses_glob_helper(expr->as.range.end, recursive_only);
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) if (expr_uses_glob_helper(expr->as.interp.parts.items[i], recursive_only)) return true;
            return false;
        default: return false;
    }
}

static bool stmt_uses_glob_helper(const DsLowerStmt *stmt, bool recursive_only) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_glob_helper(stmt->as.let_stmt.value, recursive_only);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_glob_helper(stmt->as.assign_stmt.value, recursive_only);
        case DS_LOWER_STMT_INDEX_ASSIGN: return expr_uses_glob_helper(stmt->as.index_assign_stmt.index, recursive_only) || expr_uses_glob_helper(stmt->as.index_assign_stmt.value, recursive_only);
        case DS_LOWER_STMT_IF:
            return expr_uses_glob_helper(stmt->as.if_stmt.condition, recursive_only) || stmt_uses_glob_helper(stmt->as.if_stmt.then_branch, recursive_only) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_glob_helper(stmt->as.if_stmt.else_branch, recursive_only));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_glob_helper(stmt->as.block_stmt.statements.items[i], recursive_only)) return true;
            return false;
        case DS_LOWER_STMT_CALL:
            if (call_name_is_glob(stmt->as.call_stmt.name)) {
                if (!recursive_only) return true;
                if (stmt->as.call_stmt.args.len == 0) return true;
                return literal_glob_arg_is_recursive(stmt->as.call_stmt.args.items[0]);
            }
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) if (expr_uses_glob_helper(stmt->as.call_stmt.args.items[i], recursive_only)) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return expr_uses_glob_helper(stmt->as.for_stmt.iterable, recursive_only) || stmt_uses_glob_helper(stmt->as.for_stmt.body, recursive_only);
        case DS_LOWER_STMT_WHILE: return expr_uses_glob_helper(stmt->as.while_stmt.condition, recursive_only) || stmt_uses_glob_helper(stmt->as.while_stmt.body, recursive_only);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_glob_helper(stmt->as.case_stmt.selector, recursive_only)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_glob_helper(stmt->as.case_stmt.arms.items[i].body, recursive_only)) return true;
            return false;
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE: return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_glob_helper(stmt->as.push_stmt.value, recursive_only);
        case DS_LOWER_STMT_ASSERT: return expr_uses_glob_helper(stmt->as.assert_stmt.condition, recursive_only);
        case DS_LOWER_STMT_RETURN: return expr_uses_glob_helper(stmt->as.return_stmt.value, recursive_only);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_uses_glob_helper(stmt->as.handler_stmt.body, recursive_only);
        case DS_LOWER_STMT_CMD: return false;
    }
    return false;
}

static bool command_uses_stdlib(const DsCommand *command) {
    (void)command;
    return false;
}

static unsigned expr_string_helper_mask(const DsLowerExpr *expr) {
    unsigned mask = 0;
    if (!expr) return 0;
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING:
            return string_literal_helper_mask(expr->as.text);
        case DS_LOWER_EXPR_CALL:
            mask |= ds_stdlib_bash_helper_mask(expr->as.call.name);
            for (size_t i = 0; i < expr->as.call.args.len; i++) mask |= expr_string_helper_mask(expr->as.call.args.items[i]);
            return mask;
        case DS_LOWER_EXPR_FIELD:
            return expr_string_helper_mask(expr->as.field.object);
        case DS_LOWER_EXPR_INDEX:
            return expr_string_helper_mask(expr->as.index.object) | expr_string_helper_mask(expr->as.index.index);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) mask |= expr_string_helper_mask(expr->as.array.elements.items[i]);
            return mask;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) mask |= expr_string_helper_mask(expr->as.map.entries.items[i].value);
            return mask;
        case DS_LOWER_EXPR_UNARY:
            return expr_string_helper_mask(expr->as.unary.right);
        case DS_LOWER_EXPR_BINARY:
            return expr_string_helper_mask(expr->as.binary.left) | expr_string_helper_mask(expr->as.binary.right);
        case DS_LOWER_EXPR_RANGE:
            return expr_string_helper_mask(expr->as.range.start) | expr_string_helper_mask(expr->as.range.end);
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) mask |= expr_string_helper_mask(expr->as.interp.parts.items[i]);
            return mask;
        default:
            return 0;
    }
}

static unsigned command_string_helper_mask(const DsCommand *command) {
    unsigned mask = 0;
    if (!command) return 0;
    for (size_t s = 0; s < command->stages.len; s++) {
        const DsCommandStage *stage = &command->stages.items[s];
        for (size_t i = 0; i < stage->words.len; i++) mask |= string_literal_helper_mask(stage->words.items[i].text);
    }
    mask |= string_literal_helper_mask(command->redirect.target);
    return mask;
}

static bool expr_is_collection_index(const DsLowerExpr *expr, void *context) {
    (void)context;
    return expr->kind == DS_LOWER_EXPR_INDEX ||
           (expr->kind == DS_LOWER_EXPR_STRING && string_literal_contains_index_interpolation(expr->as.text));
}

static bool expr_uses_collection_index(const DsLowerExpr *expr) {
    return expr_uses(expr, expr_is_collection_index, NULL);
}

static bool command_uses_collection_index(const DsCommand *command) {
    if (!command) return false;
    for (size_t s = 0; s < command->stages.len; s++) {
        const DsCommandStage *stage = &command->stages.items[s];
        for (size_t i = 0; i < stage->words.len; i++) {
            if (string_literal_contains_index_interpolation(stage->words.items[i].text)) return true;
        }
    }
    if (command->redirect.kind != DS_REDIRECT_NONE && string_literal_contains_index_interpolation(command->redirect.target)) return true;
    return false;
}

static bool expr_is_map_literal(const DsLowerExpr *expr, void *context) {
    (void)context;
    return expr->kind == DS_LOWER_EXPR_MAP;
}

static bool expr_uses_map_literal(const DsLowerExpr *expr) {
    return expr_uses(expr, expr_is_map_literal, NULL);
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

static bool expr_is_int_helper(const DsLowerExpr *expr, void *context) {
    (void)context;
    return (expr->kind == DS_LOWER_EXPR_BINARY && bash_is_int_binary_op(expr->as.binary.op)) ||
           (expr->kind == DS_LOWER_EXPR_UNARY && str_eq(expr->as.unary.op, "-"));
}

static bool expr_uses_int_helpers(const DsLowerExpr *expr) {
    return expr_uses(expr, expr_is_int_helper, NULL);
}

static bool expr_is_user_function_call(const DsLowerExpr *expr, void *context) {
    (void)context;
    return expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.is_user_function;
}

static bool expr_uses_function_value_helpers(const DsLowerExpr *expr) {
    return expr_uses(expr, expr_is_user_function_call, NULL);
}

typedef bool (*StmtPredicate)(const DsLowerStmt *stmt);

static bool stmt_uses_nested(const DsLowerStmt *stmt, StmtPredicate predicate) {
    if (!stmt) return false;
    if (predicate(stmt)) return true;
    switch (stmt->kind) {
        case DS_LOWER_STMT_IF:
            return stmt_uses_nested(stmt->as.if_stmt.then_branch, predicate) ||
                   stmt_uses_nested(stmt->as.if_stmt.else_branch, predicate);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (stmt_uses_nested(stmt->as.block_stmt.statements.items[i], predicate)) return true;
            }
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            return stmt_uses_nested(stmt->as.for_stmt.body, predicate);
        case DS_LOWER_STMT_WHILE:
            return stmt_uses_nested(stmt->as.while_stmt.body, predicate);
        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                if (stmt_uses_nested(stmt->as.case_stmt.arms.items[i].body, predicate)) return true;
            }
            return false;
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return stmt_uses_nested(stmt->as.handler_stmt.body, predicate);
        default:
            return false;
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

static bool stmt_is_command(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_CMD: return true;
        case DS_LOWER_STMT_LET: return stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_RUN;
        case DS_LOWER_STMT_ASSIGN: return stmt->as.assign_stmt.value && stmt->as.assign_stmt.value->kind == DS_LOWER_EXPR_RUN;
        case DS_LOWER_STMT_INDEX_ASSIGN: return expr_uses_run(stmt->as.index_assign_stmt.index) || expr_uses_run(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_RETURN: return expr_uses_run(stmt->as.return_stmt.value);
        default: return false;
    }
}

static bool stmt_has_command(const DsLowerStmt *stmt) {
    return stmt_uses_nested(stmt, stmt_is_command);
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
            if (stdlib_call_uses_base_helpers(stmt->as.call_stmt.name)) return true;
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

static unsigned stmt_string_helper_mask(const DsLowerStmt *stmt) {
    unsigned mask = 0;
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET:
            return expr_string_helper_mask(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN:
            return expr_string_helper_mask(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN:
            return expr_string_helper_mask(stmt->as.index_assign_stmt.index) | expr_string_helper_mask(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            mask |= expr_string_helper_mask(stmt->as.if_stmt.condition);
            mask |= stmt_string_helper_mask(stmt->as.if_stmt.then_branch);
            if (stmt->as.if_stmt.else_branch) mask |= stmt_string_helper_mask(stmt->as.if_stmt.else_branch);
            return mask;
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) mask |= stmt_string_helper_mask(stmt->as.block_stmt.statements.items[i]);
            return mask;
        case DS_LOWER_STMT_CALL:
            mask |= ds_stdlib_bash_helper_mask(stmt->as.call_stmt.name);
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) mask |= expr_string_helper_mask(stmt->as.call_stmt.args.items[i]);
            return mask;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            return expr_string_helper_mask(stmt->as.for_stmt.iterable) | stmt_string_helper_mask(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE:
            return expr_string_helper_mask(stmt->as.while_stmt.condition) | stmt_string_helper_mask(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            mask |= expr_string_helper_mask(stmt->as.case_stmt.selector);
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) mask |= stmt_string_helper_mask(stmt->as.case_stmt.arms.items[i].body);
            return mask;
        case DS_LOWER_STMT_PUSH:
            return expr_string_helper_mask(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT:
            return expr_string_helper_mask(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_RETURN:
            return expr_string_helper_mask(stmt->as.return_stmt.value);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return stmt_string_helper_mask(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_CMD:
            return command_string_helper_mask(&stmt->as.cmd_stmt);
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return 0;
    }
    return 0;
}

static bool scalar_stdlib_call_needs_capture(const DsLowerExpr *expr) {
    return expr && expr->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(expr->as.call.name) &&
           !stdlib_returns_array(expr->as.call.name) && expr->as.call.return_kind != DS_LOWER_VALUE_MAP;
}

static bool stmt_uses_stdlib_capture(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return scalar_stdlib_call_needs_capture(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return scalar_stdlib_call_needs_capture(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN:
            return scalar_stdlib_call_needs_capture(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return stmt_uses_stdlib_capture(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_stdlib_capture(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_stdlib_capture(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return stmt_uses_stdlib_capture(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return stmt_uses_stdlib_capture(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_stdlib_capture(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_uses_stdlib_capture(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_CALL:
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_PUSH:
        case DS_LOWER_STMT_ASSERT:
        case DS_LOWER_STMT_RETURN:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return false;
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
            return command_uses_collection_index(&stmt->as.cmd_stmt);
    }
    return false;
}

static bool expr_is_array_helper(const DsLowerExpr *expr, void *context) {
    (void)context;
    return (expr->kind == DS_LOWER_EXPR_INDEX && expr->as.index.object_is_array) ||
           (expr->kind == DS_LOWER_EXPR_STRING && string_literal_contains_index_interpolation(expr->as.text));
}

static bool expr_uses_array_helper(const DsLowerExpr *expr) {
    return expr_uses(expr, expr_is_array_helper, NULL);
}

static bool expr_is_map_helper(const DsLowerExpr *expr, void *context) {
    (void)context;
    return (expr->kind == DS_LOWER_EXPR_INDEX && expr->as.index.object_is_map) ||
           (expr->kind == DS_LOWER_EXPR_STRING && string_literal_contains_index_interpolation(expr->as.text));
}

static bool expr_uses_map_helper(const DsLowerExpr *expr) {
    return expr_uses(expr, expr_is_map_helper, NULL);
}

static bool command_uses_index_interpolation(const DsCommand *command) {
    return command_uses_collection_index(command);
}

static bool stmt_uses_array_helper(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_array_helper(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_array_helper(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN:
            return stmt->as.index_assign_stmt.target_is_array || expr_uses_array_helper(stmt->as.index_assign_stmt.index) || expr_uses_array_helper(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_array_helper(stmt->as.if_stmt.condition) || stmt_uses_array_helper(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_array_helper(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_array_helper(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return expr_uses_array_helper(stmt->as.for_stmt.iterable) || stmt_uses_array_helper(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_array_helper(stmt->as.while_stmt.condition) || stmt_uses_array_helper(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_array_helper(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_array_helper(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE: return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_array_helper(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_array_helper(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_RETURN: return expr_uses_array_helper(stmt->as.return_stmt.value);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_uses_array_helper(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_CALL:
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) if (expr_uses_array_helper(stmt->as.call_stmt.args.items[i])) return true;
            return false;
        case DS_LOWER_STMT_CMD: return command_uses_index_interpolation(&stmt->as.cmd_stmt);
    }
    return false;
}

static bool stmt_uses_map_helper(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_map_helper(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_uses_map_helper(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN:
            return stmt->as.index_assign_stmt.target_is_map || expr_uses_map_helper(stmt->as.index_assign_stmt.index) || expr_uses_map_helper(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_map_helper(stmt->as.if_stmt.condition) || stmt_uses_map_helper(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_map_helper(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_map_helper(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_FOR_MAP: return true;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_RANGE: return expr_uses_map_helper(stmt->as.for_stmt.iterable) || stmt_uses_map_helper(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_uses_map_helper(stmt->as.while_stmt.condition) || stmt_uses_map_helper(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
            if (expr_uses_map_helper(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) if (stmt_uses_map_helper(stmt->as.case_stmt.arms.items[i].body)) return true;
            return false;
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE: return false;
        case DS_LOWER_STMT_PUSH: return expr_uses_map_helper(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_uses_map_helper(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_RETURN: return expr_uses_map_helper(stmt->as.return_stmt.value);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_uses_map_helper(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_CALL:
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) if (expr_uses_map_helper(stmt->as.call_stmt.args.items[i])) return true;
            return false;
        case DS_LOWER_STMT_CMD: return command_uses_index_interpolation(&stmt->as.cmd_stmt);
    }
    return false;
}

static bool stmt_uses_map_literal(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return stmt->as.let_stmt.value_kind == DS_LOWER_VALUE_MAP || expr_uses_map_literal(stmt->as.let_stmt.value);
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

static bool program_uses_stmt(const DsLowerProgram *program, StmtPredicate predicate) {
    for (size_t i = 0; i < program->functions.len; i++) {
        if (program->functions.items[i].body && predicate(program->functions.items[i].body)) return true;
    }
    for (size_t i = 0; i < program->statements.len; i++) {
        if (predicate(program->statements.items[i])) return true;
    }
    return false;
}

bool program_uses_run(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_run);
}

bool program_uses_pipeline_run(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_pipeline_run);
}

bool program_uses_stdlib(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_stdlib);
}

unsigned program_string_helper_mask(const DsLowerProgram *program) {
    unsigned mask = 0;
    for (size_t i = 0; i < program->functions.len; i++) {
        if (program->functions.items[i].body) mask |= stmt_string_helper_mask(program->functions.items[i].body);
    }
    for (size_t i = 0; i < program->statements.len; i++) mask |= stmt_string_helper_mask(program->statements.items[i]);
    return mask;
}

bool program_uses_stdlib_capture(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_stdlib_capture);
}

bool program_uses_glob_helpers(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (program->functions.items[i].body && stmt_uses_glob_helper(program->functions.items[i].body, false)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_glob_helper(program->statements.items[i], false)) return true;
    return false;
}

bool program_uses_recursive_glob_helpers(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->functions.len; i++) if (program->functions.items[i].body && stmt_uses_glob_helper(program->functions.items[i].body, true)) return true;
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_glob_helper(program->statements.items[i], true)) return true;
    return false;
}

enum {
    DS_BASH_REGEX_BASE_HELPER = 1 << 0,
    DS_BASH_REGEX_MATCH_HELPER = 1 << 1,
    DS_BASH_REGEX_REPLACE_HELPER = 1 << 2,
};

static int regex_call_helper_mask(DsStr name) {
    if (str_eq(name, "regex.match")) return DS_BASH_REGEX_BASE_HELPER | DS_BASH_REGEX_MATCH_HELPER;
    if (str_eq(name, "regex.replace")) return DS_BASH_REGEX_BASE_HELPER | DS_BASH_REGEX_REPLACE_HELPER;
    return 0;
}

static int expr_regex_helper_mask(const DsLowerExpr *expr) {
    if (!expr) return 0;
    switch (expr->kind) {
        case DS_LOWER_EXPR_CALL:
        {
            int mask = regex_call_helper_mask(expr->as.call.name);
            for (size_t i = 0; i < expr->as.call.args.len; i++) mask |= expr_regex_helper_mask(expr->as.call.args.items[i]);
            return mask;
        }
        case DS_LOWER_EXPR_BINARY:
        {
            int mask = 0;
            if (str_eq(expr->as.binary.op, "matches") && expr->as.binary.right->kind != DS_LOWER_EXPR_REGEX) mask |= DS_BASH_REGEX_BASE_HELPER;
            return mask | expr_regex_helper_mask(expr->as.binary.left) | expr_regex_helper_mask(expr->as.binary.right);
        }
        case DS_LOWER_EXPR_FIELD: return expr_regex_helper_mask(expr->as.field.object);
        case DS_LOWER_EXPR_INDEX: return expr_regex_helper_mask(expr->as.index.object) | expr_regex_helper_mask(expr->as.index.index);
        case DS_LOWER_EXPR_ARRAY:
        {
            int mask = 0;
            for (size_t i = 0; i < expr->as.array.elements.len; i++) mask |= expr_regex_helper_mask(expr->as.array.elements.items[i]);
            return mask;
        }
        case DS_LOWER_EXPR_MAP:
        {
            int mask = 0;
            for (size_t i = 0; i < expr->as.map.entries.len; i++) mask |= expr_regex_helper_mask(expr->as.map.entries.items[i].value);
            return mask;
        }
        case DS_LOWER_EXPR_UNARY: return expr_regex_helper_mask(expr->as.unary.right);
        case DS_LOWER_EXPR_RANGE: return expr_regex_helper_mask(expr->as.range.start) | expr_regex_helper_mask(expr->as.range.end);
        case DS_LOWER_EXPR_INTERP:
        {
            int mask = 0;
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) mask |= expr_regex_helper_mask(expr->as.interp.parts.items[i]);
            return mask;
        }
        default: return 0;
    }
}

static int stmt_regex_helper_mask(const DsLowerStmt *stmt) {
    if (!stmt) return 0;
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_regex_helper_mask(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return expr_regex_helper_mask(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN: return expr_regex_helper_mask(stmt->as.index_assign_stmt.index) | expr_regex_helper_mask(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_regex_helper_mask(stmt->as.if_stmt.condition) | stmt_regex_helper_mask(stmt->as.if_stmt.then_branch) |
                   stmt_regex_helper_mask(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_BLOCK:
        {
            int mask = 0;
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) mask |= stmt_regex_helper_mask(stmt->as.block_stmt.statements.items[i]);
            return mask;
        }
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE: return expr_regex_helper_mask(stmt->as.for_stmt.iterable) | stmt_regex_helper_mask(stmt->as.for_stmt.body);
        case DS_LOWER_STMT_WHILE: return expr_regex_helper_mask(stmt->as.while_stmt.condition) | stmt_regex_helper_mask(stmt->as.while_stmt.body);
        case DS_LOWER_STMT_CASE:
        {
            int mask = expr_regex_helper_mask(stmt->as.case_stmt.selector);
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) mask |= stmt_regex_helper_mask(stmt->as.case_stmt.arms.items[i].body);
            return mask;
        }
        case DS_LOWER_STMT_PUSH: return expr_regex_helper_mask(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return expr_regex_helper_mask(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_RETURN: return expr_regex_helper_mask(stmt->as.return_stmt.value);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP: return stmt_regex_helper_mask(stmt->as.handler_stmt.body);
        case DS_LOWER_STMT_CALL:
        {
            int mask = regex_call_helper_mask(stmt->as.call_stmt.name);
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) mask |= expr_regex_helper_mask(stmt->as.call_stmt.args.items[i]);
            return mask;
        }
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return 0;
    }
    return 0;
}

static int program_regex_helper_mask(const DsLowerProgram *program) {
    int mask = 0;
    for (size_t i = 0; i < program->functions.len; i++) mask |= stmt_regex_helper_mask(program->functions.items[i].body);
    for (size_t i = 0; i < program->statements.len; i++) mask |= stmt_regex_helper_mask(program->statements.items[i]);
    return mask;
}

bool program_uses_regex_base_helpers(const DsLowerProgram *program) {
    return (program_regex_helper_mask(program) & DS_BASH_REGEX_BASE_HELPER) != 0;
}

bool program_uses_regex_match_helpers(const DsLowerProgram *program) {
    return (program_regex_helper_mask(program) & DS_BASH_REGEX_MATCH_HELPER) != 0;
}

bool program_uses_regex_replace_helpers(const DsLowerProgram *program) {
    return (program_regex_helper_mask(program) & DS_BASH_REGEX_REPLACE_HELPER) != 0;
}

bool program_uses_collection_index(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_collection_index);
}

bool program_uses_array_helpers(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_array_helper);
}

bool program_uses_map_helpers(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_map_helper);
}

static bool stmt_is_map_iteration(const DsLowerStmt *stmt) {
    return stmt->kind == DS_LOWER_STMT_FOR_MAP;
}

static bool stmt_uses_map_iteration(const DsLowerStmt *stmt) {
    return stmt_uses_nested(stmt, stmt_is_map_iteration);
}

bool program_uses_map_iteration(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_map_iteration);
}

static bool stmt_is_map_assignment(const DsLowerStmt *stmt) {
    return stmt->kind == DS_LOWER_STMT_INDEX_ASSIGN && stmt->as.index_assign_stmt.target_is_map;
}

static bool stmt_uses_map_assignment(const DsLowerStmt *stmt) {
    return stmt_uses_nested(stmt, stmt_is_map_assignment);
}

bool program_uses_map_assignment(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_map_assignment);
}

bool program_uses_map_literal(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_map_literal);
}

static bool stmt_is_control_command(const DsLowerStmt *stmt) {
    return stmt->kind == DS_LOWER_STMT_CMD && bash_command_is_control(&stmt->as.cmd_stmt, NULL);
}

static bool stmt_uses_control_commands(const DsLowerStmt *stmt) {
    return stmt_uses_nested(stmt, stmt_is_control_command);
}

bool program_uses_control_commands(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_control_commands);
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
    return program_uses_stmt(program, stmt_uses_int_helpers);
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
    return program_uses_stmt(program, stmt_uses_function_value_helpers);
}

static bool stmt_is_handler(const DsLowerStmt *stmt) {
    return stmt->kind == DS_LOWER_STMT_DEFER || stmt->kind == DS_LOWER_STMT_TRAP;
}

static bool stmt_uses_handlers(const DsLowerStmt *stmt) {
    return stmt_uses_nested(stmt, stmt_is_handler);
}

bool program_uses_handlers(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_handlers);
}

static bool stmt_is_signal_handler(const DsLowerStmt *stmt) {
    return stmt_is_handler(stmt) && ds_handler_signal_is_runtime_cleanup(stmt->as.handler_stmt.signal);
}

static bool stmt_uses_signal_handlers(const DsLowerStmt *stmt) {
    return stmt_uses_nested(stmt, stmt_is_signal_handler);
}

bool program_uses_signal_handlers(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_signal_handlers);
}

static bool expr_needs_type_tags_for_truthiness(const DsLowerExpr *expr) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_IDENT) return true;
    if (expr->kind == DS_LOWER_EXPR_INDEX) return true;
    if (expr->kind == DS_LOWER_EXPR_UNARY && str_eq(expr->as.unary.op, "!")) return expr_needs_type_tags_for_truthiness(expr->as.unary.right);
    return false;
}

static bool stmt_needs_case_helpers(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_CASE: return true;
        case DS_LOWER_STMT_IF: return expr_needs_type_tags_for_truthiness(stmt->as.if_stmt.condition);
        case DS_LOWER_STMT_WHILE: return expr_needs_type_tags_for_truthiness(stmt->as.while_stmt.condition);
        default: return false;
    }
}

static bool stmt_uses_case(const DsLowerStmt *stmt) {
    return stmt_uses_nested(stmt, stmt_needs_case_helpers);
}

bool program_uses_case(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_uses_case);
}

static bool expr_is_membership(const DsLowerExpr *expr, void *context) {
    (void)context;
    return expr->kind == DS_LOWER_EXPR_BINARY && str_eq(expr->as.binary.op, "in");
}

static bool expr_uses_membership(const DsLowerExpr *expr) {
    return expr_uses(expr, expr_is_membership, NULL);
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
    return program_uses_stmt(program, stmt_uses_membership);
}

bool program_uses_function_param_types(const DsLowerProgram *program) {
    if (!program) return false;
    for (size_t i = 0; i < program->functions.len; i++) {
        const DsLowerFn *fn = &program->functions.items[i];
        for (size_t j = 0; j < fn->params.len; j++) {
            const DsLowerFnParam *param = &fn->params.items[j];
            DsLowerValueKind kind = param->has_default ? param->default_kind : param->inferred_kind;
            if (kind == DS_LOWER_VALUE_STRING || kind == DS_LOWER_VALUE_INT || kind == DS_LOWER_VALUE_BOOL) return true;
        }
    }
    return false;
}
