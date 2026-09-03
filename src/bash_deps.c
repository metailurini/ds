#include "bash_internal.h"
#include "bash_helpers.h"
#include "ds_signal.h"

enum {
    DS_BASH_REGEX_BASE_HELPER = 1 << 0,
    DS_BASH_REGEX_MATCH_HELPER = 1 << 1,
    DS_BASH_REGEX_REPLACE_HELPER = 1 << 2,
};

typedef struct {
    bool run_and_membership;
    bool command_run;
} ExprScan;

static void collect_expr(BashDeps *deps, const DsLowerExpr *expr, ExprScan scan);
static void collect_command(BashDeps *deps, const DsLowerCommand *command);
static void collect_stmt(BashDeps *deps, const DsLowerStmt *stmt);

static bool stdlib_call_uses_base_helpers(DsStr name) {
    DsStdlibNamespace ns = ds_stdlib_namespace(name);
    return ds_stdlib_is_name(name) &&
           ns != DS_STDLIB_NAMESPACE_STRING &&
           ns != DS_STDLIB_NAMESPACE_REGEX &&
           !ds_stdlib_is_glob_helper(name);
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

static bool glob_call_uses_helper(DsStr name, const DsLowerExprVec *args, bool recursive_only) {
    if (!ds_stdlib_is_glob_helper(name)) return false;
    if (!recursive_only || args->len == 0) return true;
    return literal_glob_arg_is_recursive(args->items[0]);
}

static unsigned regex_call_helper_mask(DsStr name) {
    if (ds_str_eq_cstr(name, "regex.match")) return DS_BASH_REGEX_BASE_HELPER | DS_BASH_REGEX_MATCH_HELPER;
    if (ds_str_eq_cstr(name, "regex.replace")) return DS_BASH_REGEX_BASE_HELPER | DS_BASH_REGEX_REPLACE_HELPER;
    return 0;
}

static void collect_regex_helpers(BashDeps *deps, unsigned mask) {
    deps->uses_regex_base_helpers |= (mask & DS_BASH_REGEX_BASE_HELPER) != 0;
    deps->uses_regex_match_helpers |= (mask & DS_BASH_REGEX_MATCH_HELPER) != 0;
    deps->uses_regex_replace_helpers |= (mask & DS_BASH_REGEX_REPLACE_HELPER) != 0;
}

static bool scalar_stdlib_call_needs_capture(const DsLowerExpr *expr) {
    return expr && expr->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(expr->as.call.name) &&
           !stdlib_returns_array(expr->as.call.name) && expr->as.call.return_kind != DS_LOWER_VALUE_MAP;
}

static bool expr_needs_type_tags_for_truthiness(const DsLowerExpr *expr) {
    while (expr && expr->kind == DS_LOWER_EXPR_UNARY && expr->as.unary.op == DS_UNARY_NOT) {
        expr = expr->as.unary.right;
    }
    return expr && (expr->kind == DS_LOWER_EXPR_IDENT || expr->kind == DS_LOWER_EXPR_INDEX);
}

static void collect_call(BashDeps *deps, DsStr name, const DsLowerExprVec *args) {
    deps->uses_stdlib |= stdlib_call_uses_base_helpers(name);
    deps->string_helper_mask |= ds_stdlib_bash_helper_mask(name);
    deps->uses_glob_helpers |= glob_call_uses_helper(name, args, false);
    deps->uses_recursive_glob_helpers |= glob_call_uses_helper(name, args, true);
    collect_regex_helpers(deps, regex_call_helper_mask(name));
}

static void collect_expr(BashDeps *deps, const DsLowerExpr *expr, ExprScan scan) {
    if (!expr) return;

    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING:
            return;
        case DS_LOWER_EXPR_RUN:
            if (scan.run_and_membership) deps->uses_run = true;
            deps->has_command |= scan.command_run;
            collect_command(deps, &expr->as.run);
            return;
        case DS_LOWER_EXPR_FIELD:
            collect_expr(deps, expr->as.field.object, scan);
            return;
        case DS_LOWER_EXPR_INDEX:
            if (!(expr->as.index.row_field_access && expr->as.index.object &&
                  expr->as.index.object->kind == DS_LOWER_EXPR_IDENT)) {
                deps->uses_collection_index = true;
                deps->uses_array_helpers |= expr->as.index.object_is_array;
                deps->uses_map_helpers |= expr->as.index.object_is_map;
            }
            collect_expr(deps, expr->as.index.object, scan);
            collect_expr(deps, expr->as.index.index, scan);
            return;
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) collect_expr(deps, expr->as.array.elements.items[i], scan);
            return;
        case DS_LOWER_EXPR_MAP:
            deps->uses_map_literal = true;
            for (size_t i = 0; i < expr->as.map.entries.len; i++) collect_expr(deps, expr->as.map.entries.items[i].value, scan);
            return;
        case DS_LOWER_EXPR_UNARY:
            deps->uses_int_helpers |= expr->as.unary.op == DS_UNARY_NEGATE;
            collect_expr(deps, expr->as.unary.right, scan);
            return;
        case DS_LOWER_EXPR_BINARY:
            deps->uses_int_helpers |= ds_binary_op_is_arithmetic(expr->as.binary.op);
            if (scan.run_and_membership) deps->uses_membership |= expr->as.binary.op == DS_BINARY_IN;
            if (expr->as.binary.op == DS_BINARY_MATCHES && expr->as.binary.right->kind != DS_LOWER_EXPR_REGEX) {
                deps->uses_regex_base_helpers = true;
            }
            collect_expr(deps, expr->as.binary.left, scan);
            collect_expr(deps, expr->as.binary.right, scan);
            return;
        case DS_LOWER_EXPR_RANGE:
            collect_expr(deps, expr->as.range.start, scan);
            collect_expr(deps, expr->as.range.end, scan);
            return;
        case DS_LOWER_EXPR_CALL:
            collect_call(deps, expr->as.call.name, &expr->as.call.args);
            deps->uses_function_value_helpers |= expr->as.call.is_user_function;
            for (size_t i = 0; i < expr->as.call.args.len; i++) collect_expr(deps, expr->as.call.args.items[i], scan);
            return;
        case DS_LOWER_EXPR_INTERP_TEXT:
            return;
        case DS_LOWER_EXPR_INTERP_FORMAT:
            switch (expr->as.interp_format.spec.kind) {
                case DS_INTERP_FORMAT_UPPER: deps->string_helper_mask |= DS_BASH_STRING_HELPER_UPPER; break;
                case DS_INTERP_FORMAT_LOWER: deps->string_helper_mask |= DS_BASH_STRING_HELPER_LOWER; break;
                case DS_INTERP_FORMAT_TRIM: deps->string_helper_mask |= DS_BASH_STRING_HELPER_TRIM; break;
                case DS_INTERP_FORMAT_ALIGN_CENTER: deps->string_helper_mask |= DS_BASH_STRING_HELPER_FORMAT_CENTER; break;
                case DS_INTERP_FORMAT_ALIGN_LEFT:
                case DS_INTERP_FORMAT_ALIGN_RIGHT:
                case DS_INTERP_FORMAT_INT_DECIMAL:
                case DS_INTERP_FORMAT_INT_FIXED:
                    break;
            }
            collect_expr(deps, expr->as.interp_format.value, scan);
            return;
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) collect_expr(deps, expr->as.interp.parts.items[i], scan);
            return;
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_INT:
        case DS_LOWER_EXPR_BOOL:
        case DS_LOWER_EXPR_REGEX:
        case DS_LOWER_EXPR_ERROR:
            return;
    }
}

static void collect_command(BashDeps *deps, const DsLowerCommand *command) {
    if (!command) return;
    const ExprScan command_value = {true, true};
    deps->has_command = true;
    deps->uses_control_commands |= bash_command_is_control(command, NULL);
    for (size_t s = 0; s < command->stages.len; s++) {
        const DsLowerCommandWordVec *words = &command->stages.items[s].words;
        for (size_t i = 0; i < words->len; i++) {
            if (words->items[i].kind == DS_LOWER_COMMAND_WORD_VALUE) {
                collect_expr(deps, words->items[i].value, command_value);
            }
        }
    }
    collect_expr(deps, command->redirect.target, command_value);
}

static void collect_stmt(BashDeps *deps, const DsLowerStmt *stmt) {
    if (!stmt) return;
    const ExprScan normal = {true, false};
    const ExprScan command_run = {true, true};
    const ExprScan call_arg = {false, false};

    switch (stmt->kind) {
        case DS_LOWER_STMT_LET:
            deps->has_command |= stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_RUN;
            deps->uses_map_literal |= stmt->as.let_stmt.value_kind == DS_LOWER_VALUE_MAP;
            deps->uses_stdlib_capture |= scalar_stdlib_call_needs_capture(stmt->as.let_stmt.value);
            collect_expr(deps, stmt->as.let_stmt.value, normal);
            return;
        case DS_LOWER_STMT_ASSIGN:
            deps->has_command |= stmt->as.assign_stmt.value && stmt->as.assign_stmt.value->kind == DS_LOWER_EXPR_RUN;
            deps->uses_stdlib_capture |= scalar_stdlib_call_needs_capture(stmt->as.assign_stmt.value);
            deps->uses_int_helpers |= stmt->as.assign_stmt.op != DS_ASSIGN_SET;
            collect_expr(deps, stmt->as.assign_stmt.value, normal);
            return;
        case DS_LOWER_STMT_INDEX_ASSIGN:
            deps->uses_map_assignment |= stmt->as.index_assign_stmt.target_is_map;
            deps->uses_collection_index = true;
            deps->uses_array_helpers |= stmt->as.index_assign_stmt.target_is_array;
            deps->uses_map_helpers |= stmt->as.index_assign_stmt.target_is_map;
            deps->uses_stdlib_capture |= scalar_stdlib_call_needs_capture(stmt->as.index_assign_stmt.value);
            collect_expr(deps, stmt->as.index_assign_stmt.index, command_run);
            collect_expr(deps, stmt->as.index_assign_stmt.value, command_run);
            return;
        case DS_LOWER_STMT_IF:
            deps->uses_case |= expr_needs_type_tags_for_truthiness(stmt->as.if_stmt.condition);
            collect_expr(deps, stmt->as.if_stmt.condition, normal);
            collect_stmt(deps, stmt->as.if_stmt.then_branch);
            collect_stmt(deps, stmt->as.if_stmt.else_branch);
            return;
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) collect_stmt(deps, stmt->as.block_stmt.statements.items[i]);
            return;
        case DS_LOWER_STMT_CMD:
            collect_command(deps, &stmt->as.cmd_stmt);
            return;
        case DS_LOWER_STMT_CALL:
            collect_call(deps, stmt->as.call_stmt.name, &stmt->as.call_stmt.args);
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) collect_expr(deps, stmt->as.call_stmt.args.items[i], call_arg);
            return;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_RANGE:
            collect_expr(deps, stmt->as.for_stmt.iterable, normal);
            collect_stmt(deps, stmt->as.for_stmt.body);
            return;
        case DS_LOWER_STMT_FOR_MAP:
            deps->uses_map_iteration = true;
            deps->uses_map_helpers = true;
            collect_expr(deps, stmt->as.for_stmt.iterable, normal);
            collect_stmt(deps, stmt->as.for_stmt.body);
            return;
        case DS_LOWER_STMT_WHILE:
            deps->uses_case |= expr_needs_type_tags_for_truthiness(stmt->as.while_stmt.condition);
            collect_expr(deps, stmt->as.while_stmt.condition, normal);
            collect_stmt(deps, stmt->as.while_stmt.body);
            return;
        case DS_LOWER_STMT_CASE:
            deps->uses_case = true;
            collect_expr(deps, stmt->as.case_stmt.selector, normal);
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) collect_stmt(deps, stmt->as.case_stmt.arms.items[i].body);
            return;
        case DS_LOWER_STMT_PUSH:
            collect_expr(deps, stmt->as.push_stmt.value, normal);
            return;
        case DS_LOWER_STMT_ASSERT:
            collect_expr(deps, stmt->as.assert_stmt.condition, normal);
            return;
        case DS_LOWER_STMT_RETURN:
            collect_expr(deps, stmt->as.return_stmt.value, command_run);
            return;
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            deps->uses_handlers = true;
            deps->uses_signal_handlers |= ds_handler_signal_is_runtime_cleanup(stmt->as.handler_stmt.signal);
            collect_stmt(deps, stmt->as.handler_stmt.body);
            return;
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return;
    }
}

BashDeps bash_collect_deps(const DsLowerProgram *program) {
    BashDeps deps = {0};
    if (!program) return deps;

    for (size_t i = 0; i < program->functions.len; i++) {
        const DsLowerFn *fn = &program->functions.items[i];
        for (size_t j = 0; j < fn->params.len; j++) {
            const DsLowerFnParam *param = &fn->params.items[j];
            DsLowerValueKind kind = param->has_default ? param->default_kind : param->inferred_kind;
            deps.uses_function_param_types |= kind == DS_LOWER_VALUE_STRING ||
                                              kind == DS_LOWER_VALUE_INT ||
                                              kind == DS_LOWER_VALUE_BOOL;
        }
        collect_stmt(&deps, fn->body);
    }
    for (size_t i = 0; i < program->statements.len; i++) collect_stmt(&deps, program->statements.items[i]);
    return deps;
}
