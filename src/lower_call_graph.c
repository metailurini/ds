#include "lower_internal.h"
#include "lower_functions.h"

static bool function_body_reaches(Lower *lower, size_t current_index, size_t target_index,
                                  bool *seen, DsSpan *cycle_span);
static bool stmt_reaches_function(Lower *lower, const DsLowerStmt *stmt, size_t target_index,
                                  bool *seen, DsSpan *cycle_span);

static int find_function_index(const DsLowerProgram *program, DsStr name) {
    for (size_t i = 0; i < program->functions.len; i++) {
        if (ds_str_eq(program->functions.items[i].name, name)) return (int)i;
    }
    return -1;
}

static bool expr_reaches_function(Lower *lower, const DsLowerExpr *expr, size_t target_index, bool *seen, DsSpan *cycle_span) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_CALL: {
            if (expr->as.call.is_user_function) {
                int callee = find_function_index(lower->program, expr->as.call.name);
                if (callee >= 0) {
                    if ((size_t)callee == target_index) {
                        *cycle_span = expr->span;
                        return true;
                    }
                    if (function_body_reaches(lower, (size_t)callee, target_index, seen, cycle_span)) return true;
                }
            }
            for (size_t i = 0; i < expr->as.call.args.len; i++) {
                if (expr_reaches_function(lower, expr->as.call.args.items[i], target_index, seen, cycle_span)) return true;
            }
            return false;
        }
        case DS_LOWER_EXPR_FIELD:
            return expr_reaches_function(lower, expr->as.field.object, target_index, seen, cycle_span);
        case DS_LOWER_EXPR_UNARY:
            return expr_reaches_function(lower, expr->as.unary.right, target_index, seen, cycle_span);
        case DS_LOWER_EXPR_BINARY:
            return expr_reaches_function(lower, expr->as.binary.left, target_index, seen, cycle_span) ||
                   expr_reaches_function(lower, expr->as.binary.right, target_index, seen, cycle_span);
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) {
                if (expr_reaches_function(lower, expr->as.array.elements.items[i], target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) {
                if (expr_reaches_function(lower, expr->as.map.entries.items[i].value, target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_EXPR_INDEX:
            return expr_reaches_function(lower, expr->as.index.object, target_index, seen, cycle_span) ||
                   expr_reaches_function(lower, expr->as.index.index, target_index, seen, cycle_span);
        case DS_LOWER_EXPR_INTERP_FORMAT:
            return expr_reaches_function(lower, expr->as.interp_format.value, target_index, seen, cycle_span);
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) {
                if (expr_reaches_function(lower, expr->as.interp.parts.items[i], target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INTERP_TEXT:
        case DS_LOWER_EXPR_INT:
        case DS_LOWER_EXPR_BOOL:
        case DS_LOWER_EXPR_RUN:
        case DS_LOWER_EXPR_REGEX:
        case DS_LOWER_EXPR_ERROR:
            return false;
        case DS_LOWER_EXPR_RANGE:
            return expr_reaches_function(lower, expr->as.range.start, target_index, seen, cycle_span) ||
                   expr_reaches_function(lower, expr->as.range.end, target_index, seen, cycle_span);
    }
    return false;
}

static bool stmt_reaches_function(Lower *lower, const DsLowerStmt *stmt, size_t target_index, bool *seen, DsSpan *cycle_span) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_CALL: {
            int callee = find_function_index(lower->program, stmt->as.call_stmt.name);
            if (callee < 0) return false;
            if ((size_t)callee == target_index) {
                *cycle_span = stmt->span;
                return true;
            }
            return function_body_reaches(lower, (size_t)callee, target_index, seen, cycle_span);
        }
        case DS_LOWER_STMT_IF:
            if (expr_reaches_function(lower, stmt->as.if_stmt.condition, target_index, seen, cycle_span)) return true;
            if (stmt_reaches_function(lower, stmt->as.if_stmt.then_branch, target_index, seen, cycle_span)) return true;
            return stmt_reaches_function(lower, stmt->as.if_stmt.else_branch, target_index, seen, cycle_span);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (stmt_reaches_function(lower, stmt->as.block_stmt.statements.items[i], target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            if (expr_reaches_function(lower, stmt->as.for_stmt.iterable, target_index, seen, cycle_span)) return true;
            return stmt_reaches_function(lower, stmt->as.for_stmt.body, target_index, seen, cycle_span);
        case DS_LOWER_STMT_WHILE:
            if (expr_reaches_function(lower, stmt->as.while_stmt.condition, target_index, seen, cycle_span)) return true;
            return stmt_reaches_function(lower, stmt->as.while_stmt.body, target_index, seen, cycle_span);
        case DS_LOWER_STMT_CASE:
            if (expr_reaches_function(lower, stmt->as.case_stmt.selector, target_index, seen, cycle_span)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                if (stmt_reaches_function(lower, stmt->as.case_stmt.arms.items[i].body, target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_STMT_LET:
            return expr_reaches_function(lower, stmt->as.let_stmt.value, target_index, seen, cycle_span);
        case DS_LOWER_STMT_ASSIGN:
            return expr_reaches_function(lower, stmt->as.assign_stmt.value, target_index, seen, cycle_span);
        case DS_LOWER_STMT_INDEX_ASSIGN:
            return expr_reaches_function(lower, stmt->as.index_assign_stmt.index, target_index, seen, cycle_span) ||
                   expr_reaches_function(lower, stmt->as.index_assign_stmt.value, target_index, seen, cycle_span);
        case DS_LOWER_STMT_PUSH:
            return expr_reaches_function(lower, stmt->as.push_stmt.value, target_index, seen, cycle_span);
        case DS_LOWER_STMT_ASSERT:
            return expr_reaches_function(lower, stmt->as.assert_stmt.condition, target_index, seen, cycle_span);
        case DS_LOWER_STMT_RETURN:
            return expr_reaches_function(lower, stmt->as.return_stmt.value, target_index, seen, cycle_span);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return stmt_reaches_function(lower, stmt->as.handler_stmt.body, target_index, seen, cycle_span);
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return false;
    }
    return false;
}

static bool function_body_reaches(Lower *lower, size_t current_index, size_t target_index, bool *seen, DsSpan *cycle_span) {
    if (current_index >= lower->program->functions.len) return false;
    if (seen[current_index]) return false;
    seen[current_index] = true;
    return stmt_reaches_function(lower, lower->program->functions.items[current_index].body, target_index, seen, cycle_span);
}

void lower_functions_validate_call_graph(Lower *lower) {
    if (lower->program->functions.len == 0) return;
    bool *seen = (bool *)ds_xcalloc(lower->program->functions.len, sizeof(bool));
    for (size_t i = 0; i < lower->program->functions.len; i++) {
        memset(seen, 0, lower->program->functions.len * sizeof(bool));
        DsSpan cycle_span = lower->program->functions.items[i].span;
        if (function_body_reaches(lower, i, i, seen, &cycle_span)) {
            DsLowerFn *fn = &lower->program->functions.items[i];
            ds_diag_error(lower->diag, cycle_span,
                          "recursive function calls are deferred in v0.9.0; `%.*s` participates in a recursion cycle",
                          (int)fn->name.len, fn->name.data);
        }
    }
    free(seen);
}

