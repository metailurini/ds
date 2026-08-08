#include "bash_internal.h"
#include "bash_helpers.h"
#include "ds_signal.h"
#include "ds_command_facts.h"

typedef bool (*ExprUsePredicate)(const DsLowerExpr *expr, void *context);
typedef bool (*StmtPredicate)(const DsLowerStmt *stmt);
typedef bool (*ExprUseQuery)(const DsLowerExpr *expr);
typedef unsigned (*ExprMaskPredicate)(const DsLowerExpr *expr);
typedef unsigned (*StmtMaskPredicate)(const DsLowerStmt *stmt);

static bool stmt_uses(const DsLowerStmt *stmt, StmtPredicate predicate, ExprUseQuery query, bool scan_call_args);
static bool program_uses_stmt(const DsLowerProgram *program, StmtPredicate predicate);
static unsigned program_mask(const DsLowerProgram *program, StmtMaskPredicate predicate);

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

#define DEFINE_EXPR_USES(name, predicate) \
    static bool name(const DsLowerExpr *expr) { return expr_uses(expr, predicate, NULL); }

#define DEFINE_SIMPLE_EXPR_USES(predicate, query, condition) \
    static bool predicate(const DsLowerExpr *expr, void *context) { \
        (void)context; \
        return (condition); \
    } \
    DEFINE_EXPR_USES(query, predicate)

typedef struct {
    ExprMaskPredicate predicate;
    unsigned mask;
} ExprMaskContext;

static bool expr_accumulate_mask(const DsLowerExpr *expr, void *context) {
    ExprMaskContext *ctx = (ExprMaskContext *)context;
    ctx->mask |= ctx->predicate(expr);
    return false;
}

static unsigned expr_mask(const DsLowerExpr *expr, ExprMaskPredicate predicate) {
    ExprMaskContext ctx = {predicate, 0};
    expr_uses(expr, expr_accumulate_mask, &ctx);
    return ctx.mask;
}

DEFINE_SIMPLE_EXPR_USES(expr_is_run, expr_uses_run, expr->kind == DS_LOWER_EXPR_RUN)
DEFINE_SIMPLE_EXPR_USES(expr_is_pipeline_run, expr_uses_pipeline_run,
                        expr->kind == DS_LOWER_EXPR_RUN && ds_command_is_pipeline(&expr->as.run))

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

DEFINE_SIMPLE_EXPR_USES(expr_is_base_stdlib_call, expr_uses_stdlib,
                        expr->kind == DS_LOWER_EXPR_CALL && stdlib_call_uses_base_helpers(expr->as.call.name))

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

static bool expr_is_glob_call(const DsLowerExpr *expr, void *context) {
    bool recursive_only = *(const bool *)context;
    return expr->kind == DS_LOWER_EXPR_CALL && glob_call_uses_helper(expr->as.call.name, &expr->as.call.args, recursive_only);
}

#define DEFINE_GLOB_USES(expr_name, stmt_call_name, stmt_name, recursive_only) \
    static bool expr_name(const DsLowerExpr *expr) { bool recursive = (recursive_only); return expr_uses(expr, expr_is_glob_call, &recursive); } \
    static bool stmt_call_name(const DsLowerStmt *stmt) { return stmt->kind == DS_LOWER_STMT_CALL && glob_call_uses_helper(stmt->as.call_stmt.name, &stmt->as.call_stmt.args, (recursive_only)); } \
    static bool stmt_name(const DsLowerStmt *stmt) { return stmt_uses(stmt, stmt_call_name, expr_name, true); }

DEFINE_GLOB_USES(expr_uses_glob, stmt_is_glob_call, stmt_uses_glob, false)
DEFINE_GLOB_USES(expr_uses_recursive_glob, stmt_is_recursive_glob_call, stmt_uses_recursive_glob, true)

static unsigned expr_string_helper_bit(const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING:
            return string_literal_helper_mask(expr->as.text);
        case DS_LOWER_EXPR_CALL:
            return ds_stdlib_bash_helper_mask(expr->as.call.name);
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

DEFINE_SIMPLE_EXPR_USES(expr_is_collection_index, expr_uses_collection_index,
                        expr->kind == DS_LOWER_EXPR_INDEX ||
                        (expr->kind == DS_LOWER_EXPR_STRING && string_literal_contains_index_interpolation(expr->as.text)))

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

DEFINE_SIMPLE_EXPR_USES(expr_is_map_literal, expr_uses_map_literal, expr->kind == DS_LOWER_EXPR_MAP)

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

DEFINE_SIMPLE_EXPR_USES(expr_is_int_helper, expr_uses_int_helpers,
                        (expr->kind == DS_LOWER_EXPR_BINARY && bash_is_int_binary_op(expr->as.binary.op)) ||
                        (expr->kind == DS_LOWER_EXPR_UNARY && ds_str_eq_cstr(expr->as.unary.op, "-")))
DEFINE_SIMPLE_EXPR_USES(expr_is_user_function_call, expr_uses_function_value_helpers,
                        expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.is_user_function)

static bool stmt_uses(const DsLowerStmt *stmt, StmtPredicate predicate, ExprUseQuery query, bool scan_call_args) {
    if (!stmt) return false;
    if (predicate && predicate(stmt)) return true;
#define EXPR_USES(value) (query && query(value))
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return EXPR_USES(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return EXPR_USES(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN:
            return EXPR_USES(stmt->as.index_assign_stmt.index) || EXPR_USES(stmt->as.index_assign_stmt.value);
        case DS_LOWER_STMT_IF:
            return EXPR_USES(stmt->as.if_stmt.condition) ||
                   stmt_uses(stmt->as.if_stmt.then_branch, predicate, query, scan_call_args) ||
                   stmt_uses(stmt->as.if_stmt.else_branch, predicate, query, scan_call_args);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (stmt_uses(stmt->as.block_stmt.statements.items[i], predicate, query, scan_call_args)) return true;
            }
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            return EXPR_USES(stmt->as.for_stmt.iterable) || stmt_uses(stmt->as.for_stmt.body, predicate, query, scan_call_args);
        case DS_LOWER_STMT_WHILE:
            return EXPR_USES(stmt->as.while_stmt.condition) || stmt_uses(stmt->as.while_stmt.body, predicate, query, scan_call_args);
        case DS_LOWER_STMT_CASE:
            if (EXPR_USES(stmt->as.case_stmt.selector)) return true;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                if (stmt_uses(stmt->as.case_stmt.arms.items[i].body, predicate, query, scan_call_args)) return true;
            }
            return false;
        case DS_LOWER_STMT_PUSH: return EXPR_USES(stmt->as.push_stmt.value);
        case DS_LOWER_STMT_ASSERT: return EXPR_USES(stmt->as.assert_stmt.condition);
        case DS_LOWER_STMT_RETURN: return EXPR_USES(stmt->as.return_stmt.value);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return stmt_uses(stmt->as.handler_stmt.body, predicate, query, scan_call_args);
        case DS_LOWER_STMT_CALL:
            if (!scan_call_args) return false;
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) {
                if (EXPR_USES(stmt->as.call_stmt.args.items[i])) return true;
            }
            return false;
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return false;
        default:
            return false;
    }
#undef EXPR_USES
}

#define DEFINE_STMT_USES_NESTED(name, predicate) \
    static bool name(const DsLowerStmt *stmt) { return stmt_uses(stmt, predicate, NULL, false); }

#define DEFINE_STMT_EXPR_USES(name, query, scan_call_args) \
    static bool name(const DsLowerStmt *stmt) { return stmt_uses(stmt, NULL, query, (scan_call_args)); }

#define DEFINE_STMT_EXPR_NESTED_USES(name, query, predicate, scan_call_args) \
    static bool name(const DsLowerStmt *stmt) { return stmt_uses(stmt, predicate, query, (scan_call_args)); }

static unsigned stmt_mask(const DsLowerStmt *stmt, ExprMaskPredicate expr_query, StmtMaskPredicate stmt_query) {
    if (!stmt) return 0;
    unsigned mask = stmt_query ? stmt_query(stmt) : 0;
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return mask | expr_mask(stmt->as.let_stmt.value, expr_query);
        case DS_LOWER_STMT_ASSIGN: return mask | expr_mask(stmt->as.assign_stmt.value, expr_query);
        case DS_LOWER_STMT_INDEX_ASSIGN:
            return mask | expr_mask(stmt->as.index_assign_stmt.index, expr_query) | expr_mask(stmt->as.index_assign_stmt.value, expr_query);
        case DS_LOWER_STMT_IF:
            return mask | expr_mask(stmt->as.if_stmt.condition, expr_query) |
                   stmt_mask(stmt->as.if_stmt.then_branch, expr_query, stmt_query) |
                   stmt_mask(stmt->as.if_stmt.else_branch, expr_query, stmt_query);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) mask |= stmt_mask(stmt->as.block_stmt.statements.items[i], expr_query, stmt_query);
            return mask;
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            return mask | expr_mask(stmt->as.for_stmt.iterable, expr_query) | stmt_mask(stmt->as.for_stmt.body, expr_query, stmt_query);
        case DS_LOWER_STMT_WHILE:
            return mask | expr_mask(stmt->as.while_stmt.condition, expr_query) | stmt_mask(stmt->as.while_stmt.body, expr_query, stmt_query);
        case DS_LOWER_STMT_CASE:
            mask |= expr_mask(stmt->as.case_stmt.selector, expr_query);
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) mask |= stmt_mask(stmt->as.case_stmt.arms.items[i].body, expr_query, stmt_query);
            return mask;
        case DS_LOWER_STMT_PUSH: return mask | expr_mask(stmt->as.push_stmt.value, expr_query);
        case DS_LOWER_STMT_ASSERT: return mask | expr_mask(stmt->as.assert_stmt.condition, expr_query);
        case DS_LOWER_STMT_RETURN: return mask | expr_mask(stmt->as.return_stmt.value, expr_query);
        case DS_LOWER_STMT_DEFER:
        case DS_LOWER_STMT_TRAP:
            return mask | stmt_mask(stmt->as.handler_stmt.body, expr_query, stmt_query);
        case DS_LOWER_STMT_CALL:
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) mask |= expr_mask(stmt->as.call_stmt.args.items[i], expr_query);
            return mask;
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_BREAK:
        case DS_LOWER_STMT_CONTINUE:
            return mask;
    }
    return mask;
}

DEFINE_STMT_EXPR_USES(stmt_uses_run, expr_uses_run, false)
DEFINE_STMT_EXPR_USES(stmt_uses_pipeline_run, expr_uses_pipeline_run, false)

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

DEFINE_STMT_USES_NESTED(stmt_has_command, stmt_is_command)

bool program_has_command(const DsLowerProgram *program) {
    return program_uses_stmt(program, stmt_has_command);
}

static bool stmt_is_base_stdlib_call(const DsLowerStmt *stmt) {
    return stmt->kind == DS_LOWER_STMT_CALL && stdlib_call_uses_base_helpers(stmt->as.call_stmt.name);
}

DEFINE_STMT_EXPR_NESTED_USES(stmt_uses_stdlib, expr_uses_stdlib, stmt_is_base_stdlib_call, true)

static unsigned stmt_string_helper_bit(const DsLowerStmt *stmt) {
    if (stmt->kind == DS_LOWER_STMT_CALL) return ds_stdlib_bash_helper_mask(stmt->as.call_stmt.name);
    if (stmt->kind == DS_LOWER_STMT_CMD) return command_string_helper_mask(&stmt->as.cmd_stmt);
    return 0;
}

static unsigned stmt_string_helper_mask(const DsLowerStmt *stmt) {
    return stmt_mask(stmt, expr_string_helper_bit, stmt_string_helper_bit);
}

static bool scalar_stdlib_call_needs_capture(const DsLowerExpr *expr) {
    return expr && expr->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(expr->as.call.name) &&
           !stdlib_returns_array(expr->as.call.name) && expr->as.call.return_kind != DS_LOWER_VALUE_MAP;
}

static bool stmt_has_stdlib_capture(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return scalar_stdlib_call_needs_capture(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_ASSIGN: return scalar_stdlib_call_needs_capture(stmt->as.assign_stmt.value);
        case DS_LOWER_STMT_INDEX_ASSIGN: return scalar_stdlib_call_needs_capture(stmt->as.index_assign_stmt.value);
        default: return false;
    }
}

DEFINE_STMT_USES_NESTED(stmt_uses_stdlib_capture, stmt_has_stdlib_capture)

static bool stmt_needs_collection_index(const DsLowerStmt *stmt) {
    return stmt->kind == DS_LOWER_STMT_INDEX_ASSIGN ||
           (stmt->kind == DS_LOWER_STMT_CMD && command_uses_collection_index(&stmt->as.cmd_stmt));
}

DEFINE_STMT_EXPR_NESTED_USES(stmt_uses_collection_index, expr_uses_collection_index, stmt_needs_collection_index, true)

DEFINE_SIMPLE_EXPR_USES(expr_is_array_helper, expr_uses_array_helper,
                        (expr->kind == DS_LOWER_EXPR_INDEX && expr->as.index.object_is_array) ||
                        (expr->kind == DS_LOWER_EXPR_STRING && string_literal_contains_index_interpolation(expr->as.text)))
DEFINE_SIMPLE_EXPR_USES(expr_is_map_helper, expr_uses_map_helper,
                        (expr->kind == DS_LOWER_EXPR_INDEX && expr->as.index.object_is_map) ||
                        (expr->kind == DS_LOWER_EXPR_STRING && string_literal_contains_index_interpolation(expr->as.text)))

static bool stmt_needs_array_helper(const DsLowerStmt *stmt) {
    return (stmt->kind == DS_LOWER_STMT_INDEX_ASSIGN && stmt->as.index_assign_stmt.target_is_array) ||
           (stmt->kind == DS_LOWER_STMT_CMD && command_uses_collection_index(&stmt->as.cmd_stmt));
}

DEFINE_STMT_EXPR_NESTED_USES(stmt_uses_array_helper, expr_uses_array_helper, stmt_needs_array_helper, true)

static bool stmt_needs_map_helper(const DsLowerStmt *stmt) {
    return stmt->kind == DS_LOWER_STMT_FOR_MAP ||
           (stmt->kind == DS_LOWER_STMT_INDEX_ASSIGN && stmt->as.index_assign_stmt.target_is_map) ||
           (stmt->kind == DS_LOWER_STMT_CMD && command_uses_collection_index(&stmt->as.cmd_stmt));
}

DEFINE_STMT_EXPR_NESTED_USES(stmt_uses_map_helper, expr_uses_map_helper, stmt_needs_map_helper, true)

static bool stmt_has_map_value(const DsLowerStmt *stmt) {
    return stmt->kind == DS_LOWER_STMT_LET && stmt->as.let_stmt.value_kind == DS_LOWER_VALUE_MAP;
}

DEFINE_STMT_EXPR_NESTED_USES(stmt_uses_map_literal, expr_uses_map_literal, stmt_has_map_value, true)

static bool program_uses_stmt(const DsLowerProgram *program, StmtPredicate predicate) {
    for (size_t i = 0; i < program->functions.len; i++) {
        if (program->functions.items[i].body && predicate(program->functions.items[i].body)) return true;
    }
    for (size_t i = 0; i < program->statements.len; i++) {
        if (predicate(program->statements.items[i])) return true;
    }
    return false;
}

static unsigned program_mask(const DsLowerProgram *program, StmtMaskPredicate predicate) {
    unsigned mask = 0;
    for (size_t i = 0; i < program->functions.len; i++) mask |= predicate(program->functions.items[i].body);
    for (size_t i = 0; i < program->statements.len; i++) mask |= predicate(program->statements.items[i]);
    return mask;
}

#define DEFINE_PROGRAM_USES(name, predicate) \
    bool name(const DsLowerProgram *program) { return program_uses_stmt(program, predicate); }

#define DEFINE_SIMPLE_STMT_PROGRAM_USES(predicate, stmt_query, program_query, condition) \
    static bool predicate(const DsLowerStmt *stmt) { return (condition); } \
    DEFINE_STMT_USES_NESTED(stmt_query, predicate) \
    DEFINE_PROGRAM_USES(program_query, stmt_query)

DEFINE_PROGRAM_USES(program_uses_run, stmt_uses_run)
DEFINE_PROGRAM_USES(program_uses_pipeline_run, stmt_uses_pipeline_run)
DEFINE_PROGRAM_USES(program_uses_stdlib, stmt_uses_stdlib)

unsigned program_string_helper_mask(const DsLowerProgram *program) {
    return program_mask(program, stmt_string_helper_mask);
}

DEFINE_PROGRAM_USES(program_uses_stdlib_capture, stmt_uses_stdlib_capture)
DEFINE_PROGRAM_USES(program_uses_glob_helpers, stmt_uses_glob)
DEFINE_PROGRAM_USES(program_uses_recursive_glob_helpers, stmt_uses_recursive_glob)

enum {
    DS_BASH_REGEX_BASE_HELPER = 1 << 0,
    DS_BASH_REGEX_MATCH_HELPER = 1 << 1,
    DS_BASH_REGEX_REPLACE_HELPER = 1 << 2,
};

static unsigned regex_call_helper_mask(DsStr name) {
    if (ds_str_eq_cstr(name, "regex.match")) return DS_BASH_REGEX_BASE_HELPER | DS_BASH_REGEX_MATCH_HELPER;
    if (ds_str_eq_cstr(name, "regex.replace")) return DS_BASH_REGEX_BASE_HELPER | DS_BASH_REGEX_REPLACE_HELPER;
    return 0;
}

static unsigned expr_regex_helper_bit(const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_CALL: return regex_call_helper_mask(expr->as.call.name);
        case DS_LOWER_EXPR_BINARY:
            return ds_str_eq_cstr(expr->as.binary.op, "matches") && expr->as.binary.right->kind != DS_LOWER_EXPR_REGEX
                ? DS_BASH_REGEX_BASE_HELPER : 0;
        default: return 0;
    }
}

static unsigned stmt_regex_helper_bit(const DsLowerStmt *stmt) {
    if (stmt->kind == DS_LOWER_STMT_CALL) return regex_call_helper_mask(stmt->as.call_stmt.name);
    return 0;
}

static unsigned stmt_regex_helper_mask(const DsLowerStmt *stmt) {
    return stmt_mask(stmt, expr_regex_helper_bit, stmt_regex_helper_bit);
}

static unsigned program_regex_helper_mask(const DsLowerProgram *program) {
    return program_mask(program, stmt_regex_helper_mask);
}

#define DEFINE_PROGRAM_REGEX_USES(name, bit) \
    bool name(const DsLowerProgram *program) { return (program_regex_helper_mask(program) & (bit)) != 0; }

DEFINE_PROGRAM_REGEX_USES(program_uses_regex_base_helpers, DS_BASH_REGEX_BASE_HELPER)
DEFINE_PROGRAM_REGEX_USES(program_uses_regex_match_helpers, DS_BASH_REGEX_MATCH_HELPER)
DEFINE_PROGRAM_REGEX_USES(program_uses_regex_replace_helpers, DS_BASH_REGEX_REPLACE_HELPER)

DEFINE_PROGRAM_USES(program_uses_collection_index, stmt_uses_collection_index)
DEFINE_PROGRAM_USES(program_uses_array_helpers, stmt_uses_array_helper)
DEFINE_PROGRAM_USES(program_uses_map_helpers, stmt_uses_map_helper)

DEFINE_SIMPLE_STMT_PROGRAM_USES(stmt_is_map_iteration, stmt_uses_map_iteration, program_uses_map_iteration,
                                stmt->kind == DS_LOWER_STMT_FOR_MAP)
DEFINE_SIMPLE_STMT_PROGRAM_USES(stmt_is_map_assignment, stmt_uses_map_assignment, program_uses_map_assignment,
                                stmt->kind == DS_LOWER_STMT_INDEX_ASSIGN && stmt->as.index_assign_stmt.target_is_map)
DEFINE_PROGRAM_USES(program_uses_map_literal, stmt_uses_map_literal)

DEFINE_SIMPLE_STMT_PROGRAM_USES(stmt_is_control_command, stmt_uses_control_commands, program_uses_control_commands,
                                stmt->kind == DS_LOWER_STMT_CMD && bash_command_is_control(&stmt->as.cmd_stmt, NULL))

static bool stmt_needs_int_helpers(const DsLowerStmt *stmt) {
    return (stmt->kind == DS_LOWER_STMT_ASSIGN && stmt->as.assign_stmt.op != DS_ASSIGN_SET) ||
           (stmt->kind == DS_LOWER_STMT_CMD && command_uses_int_helpers(&stmt->as.cmd_stmt));
}

DEFINE_STMT_EXPR_NESTED_USES(stmt_uses_int_helpers, expr_uses_int_helpers, stmt_needs_int_helpers, true)

DEFINE_PROGRAM_USES(program_uses_int_helpers, stmt_uses_int_helpers)

DEFINE_STMT_EXPR_USES(stmt_uses_function_value_helpers, expr_uses_function_value_helpers, true)

DEFINE_PROGRAM_USES(program_uses_function_value_helpers, stmt_uses_function_value_helpers)

DEFINE_SIMPLE_STMT_PROGRAM_USES(stmt_is_handler, stmt_uses_handlers, program_uses_handlers,
                                stmt->kind == DS_LOWER_STMT_DEFER || stmt->kind == DS_LOWER_STMT_TRAP)
DEFINE_SIMPLE_STMT_PROGRAM_USES(stmt_is_signal_handler, stmt_uses_signal_handlers, program_uses_signal_handlers,
                                (stmt->kind == DS_LOWER_STMT_DEFER || stmt->kind == DS_LOWER_STMT_TRAP) &&
                                ds_handler_signal_is_runtime_cleanup(stmt->as.handler_stmt.signal))

static bool expr_needs_type_tags_for_truthiness(const DsLowerExpr *expr) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_IDENT) return true;
    if (expr->kind == DS_LOWER_EXPR_INDEX) return true;
    if (expr->kind == DS_LOWER_EXPR_UNARY && ds_str_eq_cstr(expr->as.unary.op, "!")) return expr_needs_type_tags_for_truthiness(expr->as.unary.right);
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

DEFINE_STMT_USES_NESTED(stmt_uses_case, stmt_needs_case_helpers)

DEFINE_PROGRAM_USES(program_uses_case, stmt_uses_case)

DEFINE_SIMPLE_EXPR_USES(expr_is_membership, expr_uses_membership,
                        expr->kind == DS_LOWER_EXPR_BINARY && ds_str_eq_cstr(expr->as.binary.op, "in"))

DEFINE_STMT_EXPR_USES(stmt_uses_membership, expr_uses_membership, false)

DEFINE_PROGRAM_USES(program_uses_membership, stmt_uses_membership)

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
