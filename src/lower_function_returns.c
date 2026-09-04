#include "lower_functions.h"
#include "lower_expr.h"
#include "lower_kinds.h"
#include "lower_schema.h"

typedef struct {
    DsStr name;
    DsLowerValueKind kind;
    bool is_row;
    bool is_row_array;
    DsLowerRowSchema row_schema;
} AstKindBinding;

typedef struct {
    AstKindBinding *items;
    size_t len;
    size_t cap;
} AstKindEnv;

static AstKindBinding *ast_kind_env_get_or_add(AstKindEnv *env, DsStr name) {
    for (size_t i = env->len; i > 0; i--) {
        if (ds_str_eq(env->items[i - 1].name, name)) return &env->items[i - 1];
    }
    DS_VEC_PUSH(env, ((AstKindBinding){.name = name}), 8);
    return &env->items[env->len - 1];
}

static void ast_kind_env_push(AstKindEnv *env, DsStr name, DsLowerValueKind kind) {
    if (kind == DS_LOWER_VALUE_UNKNOWN) return;
    AstKindBinding *binding = ast_kind_env_get_or_add(env, name);
    row_schema_free(&binding->row_schema);
    binding->kind = kind;
    binding->is_row = false;
    binding->is_row_array = false;
}

static void ast_kind_env_push_row_schema(AstKindEnv *env, DsStr name, DsLowerValueKind kind, const DsLowerRowSchema *schema, bool is_array) {
    if (!schema || (kind != DS_LOWER_VALUE_MAP && kind != DS_LOWER_VALUE_ARRAY)) return;
    AstKindBinding *binding = ast_kind_env_get_or_add(env, name);
    row_schema_free(&binding->row_schema);
    binding->kind = kind;
    binding->is_row = !is_array;
    binding->is_row_array = is_array;
    row_schema_clone(schema, &binding->row_schema);
}

static bool ast_kind_env_find(const AstKindEnv *env, DsStr name, DsLowerValueKind *kind_out) {
    for (size_t i = env->len; i > 0; i--) {
        if (ds_str_eq(env->items[i - 1].name, name)) {
            *kind_out = env->items[i - 1].kind;
            return true;
        }
    }
    return false;
}

static bool ast_kind_env_find_row_schema(const AstKindEnv *env, DsStr name, bool want_array, DsLowerRowSchema *schema_out) {
    for (size_t i = env->len; i > 0; i--) {
        const AstKindBinding *binding = &env->items[i - 1];
        if (!ds_str_eq(binding->name, name)) continue;
        if (want_array ? binding->is_row_array : binding->is_row) {
            row_schema_clone(&binding->row_schema, schema_out);
            return true;
        }
        return false;
    }
    return false;
}

static AstKindEnv ast_kind_env_clone(const AstKindEnv *env) {
    AstKindEnv copy = {0};
    if (env->len > 0) {
        copy.items = (AstKindBinding *)ds_xcalloc(env->len, sizeof(AstKindBinding));
        copy.len = env->len;
        copy.cap = env->len;
        for (size_t i = 0; i < env->len; i++) {
            copy.items[i].name = env->items[i].name;
            copy.items[i].kind = env->items[i].kind;
            copy.items[i].is_row = env->items[i].is_row;
            copy.items[i].is_row_array = env->items[i].is_row_array;
            row_schema_clone(&env->items[i].row_schema, &copy.items[i].row_schema);
        }
    }
    return copy;
}

static void ast_kind_env_free(AstKindEnv *env) {
    for (size_t i = 0; i < env->len; i++) row_schema_free(&env->items[i].row_schema);
    free(env->items);
    *env = (AstKindEnv){0};
}

static bool ast_expr_kind_known(Lower *lower, const AstKindEnv *env, const DsExpr *expr, DsLowerValueKind *kind_out) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_EXPR_STRING:
            *kind_out = DS_LOWER_VALUE_STRING;
            return true;
        case DS_EXPR_INT:
            *kind_out = DS_LOWER_VALUE_INT;
            return true;
        case DS_EXPR_BOOL:
            *kind_out = DS_LOWER_VALUE_BOOL;
            return true;
        case DS_EXPR_UNARY:
            if (expr->as.unary.op == DS_UNARY_NEGATE) {
                DsLowerValueKind right = DS_LOWER_VALUE_UNKNOWN;
                if (!ast_expr_kind_known(lower, env, expr->as.unary.right, &right) || right != DS_LOWER_VALUE_INT) return false;
                *kind_out = DS_LOWER_VALUE_INT;
                return true;
            }
            if (expr->as.unary.op == DS_UNARY_NOT) {
                *kind_out = DS_LOWER_VALUE_BOOL;
                return true;
            }
            return false;
        case DS_EXPR_BINARY:
            if (ds_binary_op_is_arithmetic(expr->as.binary.op)) {
                DsLowerValueKind left = DS_LOWER_VALUE_UNKNOWN;
                DsLowerValueKind right = DS_LOWER_VALUE_UNKNOWN;
                if (!ast_expr_kind_known(lower, env, expr->as.binary.left, &left) ||
                    !ast_expr_kind_known(lower, env, expr->as.binary.right, &right) ||
                    left != DS_LOWER_VALUE_INT || right != DS_LOWER_VALUE_INT) return false;
                *kind_out = DS_LOWER_VALUE_INT;
                return true;
            }
            if (ds_binary_op_is_comparison(expr->as.binary.op)) {
                *kind_out = DS_LOWER_VALUE_BOOL;
                return true;
            }
            return false;
        case DS_EXPR_CALL: {
            const DsStdlibHelper *helper = ds_stdlib_lookup(expr->as.call.name);
            DsLowerValueKind lowered = lower_stdlib_return_value_kind(helper);
            if (lowered != DS_LOWER_VALUE_UNKNOWN) {
                *kind_out = lowered;
                return true;
            }
            DsLowerFn *fn = find_function(lower->program, expr->as.call.name);
            if (fn && fn->has_return && fn->all_paths_return && fn->return_kind != DS_LOWER_VALUE_UNKNOWN) {
                *kind_out = fn->return_kind;
                return true;
            }
            return false;
        }
        case DS_EXPR_IDENT:
            return ast_kind_env_find(env, expr->as.text, kind_out);
        case DS_EXPR_RUN:
            *kind_out = DS_LOWER_VALUE_COMMAND_RESULT;
            return true;
        case DS_EXPR_ARRAY:
            *kind_out = DS_LOWER_VALUE_ARRAY;
            return true;
        case DS_EXPR_MAP:
            *kind_out = DS_LOWER_VALUE_MAP;
            return true;
        case DS_EXPR_REGEX:
        case DS_EXPR_RANGE:
        case DS_EXPR_FIELD:
        case DS_EXPR_INDEX:
        case DS_EXPR_ERROR:
            return false;
    }
    return false;
}

static bool ast_stmt_all_paths_return(const DsStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_STMT_RETURN:
            return true;
        case DS_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (ast_stmt_all_paths_return(stmt->as.block_stmt.statements.items[i])) return true;
            }
            return false;
        case DS_STMT_IF:
            return stmt->as.if_stmt.else_branch &&
                   ast_stmt_all_paths_return(stmt->as.if_stmt.then_branch) &&
                   ast_stmt_all_paths_return(stmt->as.if_stmt.else_branch);
        case DS_STMT_CASE: {
            bool has_default = false;
            if (stmt->as.case_stmt.arms.len == 0) return false;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                bool arm_default = false;
                for (size_t j = 0; j < arm->patterns.len; j++) {
                    if (arm->patterns.items[j].kind == DS_CASE_PATTERN_DEFAULT) arm_default = true;
                }
                has_default = has_default || arm_default;
                if (!ast_stmt_all_paths_return(arm->body)) return false;
            }
            return has_default;
        }
        default:
            return false;
    }
}

static bool ast_collect_return_kind(Lower *lower, const DsStmt *stmt, AstKindEnv *env, DsLowerValueKind *kind, bool *saw_return) {
    if (!stmt) return true;
    switch (stmt->kind) {
        case DS_STMT_RETURN: {
            DsLowerValueKind found = DS_LOWER_VALUE_UNKNOWN;
            if (!ast_expr_kind_known(lower, env, stmt->as.return_stmt.value, &found)) return false;
            if (*saw_return && *kind != found) return false;
            *saw_return = true;
            *kind = found;
            return true;
        }
        case DS_STMT_LET: {
            DsLowerValueKind found = DS_LOWER_VALUE_UNKNOWN;
            if (ast_expr_kind_known(lower, env, stmt->as.let_stmt.value, &found)) {
                ast_kind_env_push(env, stmt->as.let_stmt.name, found);
            }
            return true;
        }
        case DS_STMT_ASSIGN: {
            DsLowerValueKind found = DS_LOWER_VALUE_UNKNOWN;
            if (ast_expr_kind_known(lower, env, stmt->as.assign_stmt.value, &found)) {
                ast_kind_env_push(env, stmt->as.assign_stmt.name, found);
            }
            return true;
        }
        case DS_STMT_INDEX_ASSIGN:
            return true;
        case DS_STMT_BLOCK:
        {
            AstKindEnv block_env = ast_kind_env_clone(env);
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (!ast_collect_return_kind(lower, stmt->as.block_stmt.statements.items[i], &block_env, kind, saw_return)) {
                    ast_kind_env_free(&block_env);
                    return false;
                }
            }
            ast_kind_env_free(&block_env);
            return true;
        }
        case DS_STMT_IF:
        {
            AstKindEnv then_env = ast_kind_env_clone(env);
            AstKindEnv else_env = ast_kind_env_clone(env);
            bool ok = ast_collect_return_kind(lower, stmt->as.if_stmt.then_branch, &then_env, kind, saw_return) &&
                      ast_collect_return_kind(lower, stmt->as.if_stmt.else_branch, &else_env, kind, saw_return);
            ast_kind_env_free(&then_env);
            ast_kind_env_free(&else_env);
            return ok;
        }
        case DS_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                AstKindEnv arm_env = ast_kind_env_clone(env);
                bool ok = ast_collect_return_kind(lower, stmt->as.case_stmt.arms.items[i].body, &arm_env, kind, saw_return);
                ast_kind_env_free(&arm_env);
                if (!ok) return false;
            }
            return true;
        default:
            return true;
    }
}

static DsLowerValueKind ast_literal_default_kind(const DsExpr *expr) {
    if (!expr) return DS_LOWER_VALUE_UNKNOWN;
    switch (expr->kind) {
        case DS_EXPR_STRING: return DS_LOWER_VALUE_STRING;
        case DS_EXPR_INT: return DS_LOWER_VALUE_INT;
        case DS_EXPR_BOOL: return DS_LOWER_VALUE_BOOL;
        default: return DS_LOWER_VALUE_UNKNOWN;
    }
}

static bool ast_expr_row_schema_known(Lower *lower, const AstKindEnv *env, const DsExpr *expr, DsLowerRowSchema *schema_out);
static bool ast_expr_row_array_schema_known(Lower *lower, const AstKindEnv *env, const DsExpr *expr, DsLowerRowSchema *schema_out);

static bool ast_expr_row_schema_known(Lower *lower, const AstKindEnv *env, const DsExpr *expr, DsLowerRowSchema *schema_out) {
    if (!expr) return false;
    if (expr->kind == DS_EXPR_IDENT) return ast_kind_env_find_row_schema(env, expr->as.text, false, schema_out);
    if (expr->kind == DS_EXPR_CALL) {
        DsLowerFn *fn = find_function(lower->program, expr->as.call.name);
        if (fn && fn->returns_row) {
            row_schema_clone(&fn->row_schema, schema_out);
            return true;
        }
        return false;
    }
    if (expr->kind == DS_EXPR_INDEX) {
        DsLowerRowSchema schema = {0};
        if (!ast_expr_row_array_schema_known(lower, env, expr->as.index.object, &schema)) return false;
        DsLowerValueKind index_kind = DS_LOWER_VALUE_UNKNOWN;
        bool ok = ast_expr_kind_known(lower, env, expr->as.index.index, &index_kind) && index_kind == DS_LOWER_VALUE_INT;
        if (ok) row_schema_clone(&schema, schema_out);
        row_schema_free(&schema);
        return ok;
    }
    if (expr->kind != DS_EXPR_MAP) return false;
    row_schema_init(schema_out);
    for (size_t i = 0; i < expr->as.map.entries.len; i++) {
        const DsMapEntry *entry = &expr->as.map.entries.items[i];
        DsLowerValueKind kind = DS_LOWER_VALUE_UNKNOWN;
        if (!ast_expr_kind_known(lower, env, entry->value, &kind) || !lower_value_kind_is_scalar(kind)) {
            row_schema_free(schema_out);
            return false;
        }
        DsStr key = lower_map_key_decode(entry);
        if (!key.data || row_schema_find(schema_out, key)) {
            free(key.data);
            row_schema_free(schema_out);
            return false;
        }
        row_schema_push(schema_out, key, kind);
        free(key.data);
    }
    return true;
}

static bool ast_expr_row_array_schema_known(Lower *lower, const AstKindEnv *env, const DsExpr *expr, DsLowerRowSchema *schema_out) {
    if (!expr) return false;
    if (expr->kind == DS_EXPR_IDENT) return ast_kind_env_find_row_schema(env, expr->as.text, true, schema_out);
    if (expr->kind == DS_EXPR_CALL) {
        if (ds_str_eq_cstr(expr->as.call.name, "string.sort_by") && expr->as.call.args.len > 0) {
            return ast_expr_row_array_schema_known(lower, env, expr->as.call.args.items[0], schema_out);
        }
        DsLowerFn *fn = find_function(lower->program, expr->as.call.name);
        if (fn && fn->returns_row_array) {
            row_schema_clone(&fn->row_schema, schema_out);
            return true;
        }
        return false;
    }
    if (expr->kind != DS_EXPR_ARRAY || expr->as.array.elements.len == 0) return false;
    DsLowerRowSchema common = {0};
    bool saw = false;
    for (size_t i = 0; i < expr->as.array.elements.len; i++) {
        DsLowerRowSchema current = {0};
        if (!ast_expr_row_schema_known(lower, env, expr->as.array.elements.items[i], &current)) {
            if (saw) row_schema_free(&common);
            return false;
        }
        if (!saw) {
            row_schema_clone(&current, &common);
            saw = true;
        } else if (!row_schema_equal(&common, &current)) {
            row_schema_free(&current);
            row_schema_free(&common);
            return false;
        }
        row_schema_free(&current);
    }
    if (!saw) return false;
    row_schema_clone(&common, schema_out);
    row_schema_free(&common);
    return true;
}

static void ast_kind_env_push_expr(AstKindEnv *env, Lower *lower, DsStr name, const DsExpr *expr) {
    DsLowerRowSchema row_schema = {0};
    if (ast_expr_row_schema_known(lower, env, expr, &row_schema)) {
        ast_kind_env_push_row_schema(env, name, DS_LOWER_VALUE_MAP, &row_schema, false);
        row_schema_free(&row_schema);
        return;
    }
    if (ast_expr_row_array_schema_known(lower, env, expr, &row_schema)) {
        ast_kind_env_push_row_schema(env, name, DS_LOWER_VALUE_ARRAY, &row_schema, true);
        row_schema_free(&row_schema);
        return;
    }
    DsLowerValueKind found = DS_LOWER_VALUE_UNKNOWN;
    if (ast_expr_kind_known(lower, env, expr, &found)) ast_kind_env_push(env, name, found);
}

static bool ast_collect_return_schema(Lower *lower, const DsStmt *stmt, AstKindEnv *env, DsLowerValueKind *kind, DsLowerRowSchema *schema, bool *saw_return);

static bool ast_record_return_schema(Lower *lower, AstKindEnv *env, const DsExpr *expr, DsLowerValueKind *kind, DsLowerRowSchema *schema, bool *saw_return) {
    DsLowerRowSchema found_schema = {0};
    DsLowerValueKind found_kind = DS_LOWER_VALUE_UNKNOWN;
    if (ast_expr_row_schema_known(lower, env, expr, &found_schema)) found_kind = DS_LOWER_VALUE_MAP;
    else if (ast_expr_row_array_schema_known(lower, env, expr, &found_schema)) found_kind = DS_LOWER_VALUE_ARRAY;
    else return false;

    if (*saw_return && (*kind != found_kind || !row_schema_equal(schema, &found_schema))) {
        row_schema_free(&found_schema);
        return false;
    }
    if (!*saw_return) {
        *kind = found_kind;
        row_schema_clone(&found_schema, schema);
    }
    *saw_return = true;
    row_schema_free(&found_schema);
    return true;
}

static bool ast_collect_return_schema(Lower *lower, const DsStmt *stmt, AstKindEnv *env, DsLowerValueKind *kind, DsLowerRowSchema *schema, bool *saw_return) {
    if (!stmt) return true;
    switch (stmt->kind) {
        case DS_STMT_RETURN:
            return ast_record_return_schema(lower, env, stmt->as.return_stmt.value, kind, schema, saw_return);
        case DS_STMT_LET:
            ast_kind_env_push_expr(env, lower, stmt->as.let_stmt.name, stmt->as.let_stmt.value);
            return true;
        case DS_STMT_ASSIGN:
            ast_kind_env_push_expr(env, lower, stmt->as.assign_stmt.name, stmt->as.assign_stmt.value);
            return true;
        case DS_STMT_PUSH: {
            DsLowerRowSchema row_schema = {0};
            if (ast_expr_row_schema_known(lower, env, stmt->as.push_stmt.value, &row_schema)) {
                DsLowerRowSchema existing = {0};
                if (ast_kind_env_find_row_schema(env, stmt->as.push_stmt.name, true, &existing)) {
                    row_schema_free(&existing);
                } else {
                    ast_kind_env_push_row_schema(env, stmt->as.push_stmt.name, DS_LOWER_VALUE_ARRAY, &row_schema, true);
                }
                row_schema_free(&row_schema);
            }
            return true;
        }
        case DS_STMT_BLOCK: {
            AstKindEnv block_env = ast_kind_env_clone(env);
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (!ast_collect_return_schema(lower, stmt->as.block_stmt.statements.items[i], &block_env, kind, schema, saw_return)) {
                    ast_kind_env_free(&block_env);
                    return false;
                }
            }
            ast_kind_env_free(&block_env);
            return true;
        }
        case DS_STMT_IF: {
            AstKindEnv then_env = ast_kind_env_clone(env);
            AstKindEnv else_env = ast_kind_env_clone(env);
            bool ok = ast_collect_return_schema(lower, stmt->as.if_stmt.then_branch, &then_env, kind, schema, saw_return) &&
                      ast_collect_return_schema(lower, stmt->as.if_stmt.else_branch, &else_env, kind, schema, saw_return);
            ast_kind_env_free(&then_env);
            ast_kind_env_free(&else_env);
            return ok;
        }
        case DS_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                AstKindEnv arm_env = ast_kind_env_clone(env);
                bool ok = ast_collect_return_schema(lower, stmt->as.case_stmt.arms.items[i].body, &arm_env, kind, schema, saw_return);
                ast_kind_env_free(&arm_env);
                if (!ok) return false;
            }
            return true;
        default:
            return true;
    }
}

void lower_functions_predeclare_return_contracts(Lower *lower, const DsAst *ast) {
    /*
     * Provisional forward-call contract discovery.
     *
     * This pass intentionally runs before function bodies are lowered so calls
     * to functions declared later can be validated in expression position. It
     * does not emit user-facing return diagnostics and is not the final source
     * of truth for return statements. Concrete return statements are still
     * lowered and validated by lower_validate_function_return_contract() in
     * lower_stmt.c, which owns source-language return-kind diagnostics.
     */
    if (!ast || lower->program->functions.len == 0) return;
    for (size_t pass = 0; pass < lower->program->functions.len; pass++) {
        bool changed = false;
        for (size_t i = 0; i < ast->statements.len; i++) {
            const DsStmt *stmt = ast->statements.items[i];
            if (stmt->kind != DS_STMT_FN) continue;
            DsLowerFn *fn = find_function(lower->program, stmt->as.fn_stmt.name);
            if (!fn) continue;
            if (!fn->has_return) {
                DsLowerValueKind kind = DS_LOWER_VALUE_UNKNOWN;
                bool saw_return = false;
                AstKindEnv env = {0};
                for (size_t j = 0; j < stmt->as.fn_stmt.params.len && j < fn->params.len; j++) {
                    const DsFnParam *param = &stmt->as.fn_stmt.params.items[j];
                    DsLowerValueKind param_kind = lower_fn_param_expected_kind(&fn->params.items[j]);
                    if (param_kind == DS_LOWER_VALUE_UNKNOWN) param_kind = ast_literal_default_kind(param->default_value);
                    ast_kind_env_push(&env, param->name, param_kind);
                }
                bool ok = ast_collect_return_kind(lower, stmt->as.fn_stmt.body, &env, &kind, &saw_return);
                ast_kind_env_free(&env);
                if (ok && saw_return) {
                    fn->has_return = true;
                    fn->return_kind = kind;
                    fn->all_paths_return = ast_stmt_all_paths_return(stmt->as.fn_stmt.body);
                    changed = true;
                }
            }
            if (!fn->returns_row && !fn->returns_row_array) {
                DsLowerValueKind schema_kind = DS_LOWER_VALUE_UNKNOWN;
                DsLowerRowSchema schema = {0};
                bool saw_schema_return = false;
                AstKindEnv env = {0};
                for (size_t j = 0; j < stmt->as.fn_stmt.params.len && j < fn->params.len; j++) {
                    const DsFnParam *param = &stmt->as.fn_stmt.params.items[j];
                    DsLowerValueKind param_kind = lower_fn_param_expected_kind(&fn->params.items[j]);
                    if (param_kind == DS_LOWER_VALUE_UNKNOWN) param_kind = ast_literal_default_kind(param->default_value);
                    ast_kind_env_push(&env, param->name, param_kind);
                }
                bool ok = ast_collect_return_schema(lower, stmt->as.fn_stmt.body, &env, &schema_kind, &schema, &saw_schema_return);
                ast_kind_env_free(&env);
                if (ok && saw_schema_return) {
                    fn->has_return = true;
                    fn->return_kind = schema_kind;
                    fn->all_paths_return = ast_stmt_all_paths_return(stmt->as.fn_stmt.body);
                    if (schema_kind == DS_LOWER_VALUE_MAP) fn->returns_row = true;
                    else if (schema_kind == DS_LOWER_VALUE_ARRAY) fn->returns_row_array = true;
                    row_schema_free(&fn->row_schema);
                    row_schema_clone(&schema, &fn->row_schema);
                    changed = true;
                }
                row_schema_free(&schema);
            }
        }
        if (!changed) break;
    }
}

