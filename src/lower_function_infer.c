#include "lower_functions.h"
#include "lower_kinds.h"
#include "ds_runtime.h"
#include "ds_interpolation.h"
#include "lower_interp_parser.h"

typedef enum {
    INFER_BIND_NONE,
    INFER_BIND_PARAM,
    INFER_BIND_KIND
} InferBindingKind;

typedef struct {
    InferBindingKind kind;
    size_t param_index;
    DsLowerValueKind value_kind;
} InferBinding;

typedef struct {
    DsStr name;
    InferBinding binding;
} InferEnvItem;

typedef struct {
    InferEnvItem *items;
    size_t len;
    size_t cap;
} InferEnv;

typedef struct {
    Lower *lower;
    DsLowerFn *fn;
    bool changed;
} InferCtx;

static InferBinding infer_none(void) {
    InferBinding out = {0};
    out.kind = INFER_BIND_NONE;
    out.value_kind = DS_LOWER_VALUE_UNKNOWN;
    return out;
}

static InferBinding infer_kind(DsLowerValueKind kind) {
    InferBinding out = {0};
    out.kind = INFER_BIND_KIND;
    out.value_kind = kind;
    return out;
}

static InferBinding infer_param(size_t index) {
    InferBinding out = {0};
    out.kind = INFER_BIND_PARAM;
    out.param_index = index;
    out.value_kind = DS_LOWER_VALUE_UNKNOWN;
    return out;
}

static bool infer_binding_known_kind(InferCtx *ctx, InferBinding binding, DsLowerValueKind *kind_out) {
    if (binding.kind == INFER_BIND_KIND) {
        *kind_out = binding.value_kind;
        return binding.value_kind != DS_LOWER_VALUE_UNKNOWN;
    }
    if (binding.kind == INFER_BIND_PARAM && binding.param_index < ctx->fn->params.len) {
        *kind_out = lower_fn_param_expected_kind(&ctx->fn->params.items[binding.param_index]);
        return *kind_out != DS_LOWER_VALUE_UNKNOWN;
    }
    return false;
}

static void infer_env_push(InferEnv *env, DsStr name, InferBinding binding) {
    for (size_t i = env->len; i > 0; i--) {
        if (ds_str_eq(env->items[i - 1].name, name)) {
            env->items[i - 1].binding = binding;
            return;
        }
    }
    DS_VEC_PUSH(env, ((InferEnvItem){name, binding}), 8);
}

static bool infer_env_find(const InferEnv *env, DsStr name, InferBinding *binding_out) {
    for (size_t i = env->len; i > 0; i--) {
        if (ds_str_eq(env->items[i - 1].name, name)) {
            *binding_out = env->items[i - 1].binding;
            return true;
        }
    }
    return false;
}

static InferEnv infer_env_clone(const InferEnv *env) {
    InferEnv copy = {0};
    if (env->len > 0) {
        copy.items = (InferEnvItem *)ds_xcalloc(env->len, sizeof(InferEnvItem));
        memcpy(copy.items, env->items, env->len * sizeof(InferEnvItem));
        copy.len = env->len;
        copy.cap = env->len;
    }
    return copy;
}

static void infer_env_free(InferEnv *env) {
    free(env->items);
    *env = (InferEnv){0};
}

static void infer_constrain_param(InferCtx *ctx, size_t param_index, DsLowerValueKind expected, DsSpan span, const char *reason) {
    if (!lower_value_kind_is_scalar(expected) || param_index >= ctx->fn->params.len) return;
    DsLowerFnParam *param = &ctx->fn->params.items[param_index];
    DsLowerValueKind current = lower_fn_param_expected_kind(param);
    if (current == DS_LOWER_VALUE_UNKNOWN) {
        param->inferred_kind = expected;
        ctx->changed = true;
        return;
    }
    if (current != expected) {
        ds_diag_error(ctx->lower->diag, span,
                      "parameter `%.*s` inferred as %s but later used as %s%s%s",
                      (int)param->name.len, param->name.data,
                      ds_lower_value_kind_name(current), ds_lower_value_kind_name(expected),
                      reason && reason[0] ? " from " : "",
                      reason && reason[0] ? reason : "");
    }
}

static void infer_constrain_binding(InferCtx *ctx, InferBinding binding, DsLowerValueKind expected, DsSpan span, const char *reason) {
    if (!lower_value_kind_is_scalar(expected)) return;
    if (binding.kind == INFER_BIND_PARAM) {
        infer_constrain_param(ctx, binding.param_index, expected, span, reason);
        return;
    }
    if (binding.kind == INFER_BIND_KIND && binding.value_kind != DS_LOWER_VALUE_UNKNOWN && binding.value_kind != expected) {
        ds_diag_error(ctx->lower->diag, span, "expected %s value but found %s", ds_lower_value_kind_name(expected), ds_lower_value_kind_name(binding.value_kind));
    }
}

static InferBinding infer_expr_binding(InferCtx *ctx, InferEnv *env, const DsExpr *expr);
static bool infer_extract_ident_arg(const char *data, size_t start, size_t end, DsStr *name_out);
static void infer_command_word_method_args(InferCtx *ctx, InferEnv *env, DsStr word, size_t arg_start, DsStr method, DsSpan span);

static void infer_constrain_expr(InferCtx *ctx, InferEnv *env, const DsExpr *expr, DsLowerValueKind expected, const char *reason) {
    InferBinding binding = infer_expr_binding(ctx, env, expr);
    infer_constrain_binding(ctx, binding, expected, expr ? expr->span : ctx->fn->span, reason);
}

static void infer_interpolation_identifier_arg(InferCtx *ctx, InferEnv *env,
                                               const DsExpr *arg, DsLowerValueKind expected,
                                               DsSpan span, const char *reason) {
    if (!arg || arg->kind != DS_EXPR_IDENT || !lower_value_kind_is_scalar(expected)) return;
    InferBinding binding = infer_none();
    if (infer_env_find(env, arg->as.text, &binding)) {
        infer_constrain_binding(ctx, binding, expected, span, reason);
    }
}

static void infer_interpolation_call(InferCtx *ctx, InferEnv *env,
                                     const DsExpr *expr, DsSpan span) {
    if (!expr || expr->kind != DS_EXPR_CALL) return;

    if (ds_stdlib_is_string_helper(expr->as.call.name)) {
        for (size_t i = 0; i < expr->as.call.args.len; i++) {
            DsLowerValueKind expected = ds_stdlib_arg_expects_int(expr->as.call.name, i)
                ? DS_LOWER_VALUE_INT : DS_LOWER_VALUE_STRING;
            const char *reason = i == 0
                ? "interpolation string helper"
                : "command interpolation string helper";
            infer_interpolation_identifier_arg(ctx, env, expr->as.call.args.items[i],
                                               expected, span, reason);
        }
        return;
    }

    DsLowerFn *callee = find_function(ctx->lower->program, expr->as.call.name);
    if (!callee) return;
    size_t count = expr->as.call.args.len < callee->params.len
        ? expr->as.call.args.len : callee->params.len;
    for (size_t i = 0; i < count; i++) {
        DsLowerValueKind expected = lower_fn_param_expected_kind(&callee->params.items[i]);
        infer_interpolation_identifier_arg(ctx, env, expr->as.call.args.items[i],
                                           expected, span, "interpolation function call");
    }
}

static void infer_interpolation_expr_text(InferCtx *ctx, InferEnv *env, DsStr text,
                                          size_t start, size_t end, DsSpan span) {
    if (start >= end) return;
    DsStr body = {text.data + start, end - start};
    size_t cursor = 0;
    DsExpr *parsed = lower_interp_parse_expr(body, 0, span, &cursor);
    if (!parsed) return;
    ds_skip_ascii_ws(body.data, body.len, &cursor);

    if (parsed->kind == DS_EXPR_IDENT && cursor < body.len && body.data[cursor] == ':') {
        DsStr spec = {body.data + cursor + 1, body.len - cursor - 1};
        DsInterpFormatSpec format;
        if (ds_interp_parse_format_spec(spec, &format)) {
            DsLowerValueKind expected = DS_LOWER_VALUE_UNKNOWN;
            switch (format.kind) {
                case DS_INTERP_FORMAT_UPPER:
                case DS_INTERP_FORMAT_LOWER:
                case DS_INTERP_FORMAT_TRIM:
                case DS_INTERP_FORMAT_ALIGN_LEFT:
                case DS_INTERP_FORMAT_ALIGN_RIGHT:
                case DS_INTERP_FORMAT_ALIGN_CENTER:
                    expected = DS_LOWER_VALUE_STRING;
                    break;
                case DS_INTERP_FORMAT_INT_DECIMAL:
                case DS_INTERP_FORMAT_INT_FIXED:
                    expected = DS_LOWER_VALUE_INT;
                    break;
            }
            infer_interpolation_identifier_arg(ctx, env, parsed, expected, span,
                                               "interpolation format specifier");
        }
        ds_expr_free(parsed);
        return;
    }

    infer_interpolation_call(ctx, env, parsed, span);
    ds_expr_free(parsed);
}

static void infer_interpolated_text(InferCtx *ctx, InferEnv *env, DsStr text, DsSpan span) {
    for (size_t i = 0; i < text.len; i++) {
        if (text.data[i] != '{') continue;
        if (i + 1 < text.len && text.data[i + 1] == '{') { i++; continue; }
        size_t end = i + 1;
        while (end < text.len && text.data[end] != '}') end++;
        if (end >= text.len) return;
        infer_interpolation_expr_text(ctx, env, text, i + 1, end, span);
        i = end;
    }
}

static InferBinding infer_expr_binding(InferCtx *ctx, InferEnv *env, const DsExpr *expr) {
    if (!expr) return infer_none();
    switch (expr->kind) {
        case DS_EXPR_STRING:
            infer_interpolated_text(ctx, env, expr->as.text, expr->span);
            return infer_kind(DS_LOWER_VALUE_STRING);
        case DS_EXPR_INT: return infer_kind(DS_LOWER_VALUE_INT);
        case DS_EXPR_BOOL: return infer_kind(DS_LOWER_VALUE_BOOL);
        case DS_EXPR_IDENT: {
            InferBinding binding = infer_none();
            if (infer_env_find(env, expr->as.text, &binding)) return binding;
            return infer_none();
        }
        case DS_EXPR_FIELD: {
            InferBinding object = infer_expr_binding(ctx, env, expr->as.field.object);
            DsLowerValueKind object_kind = DS_LOWER_VALUE_UNKNOWN;
            if (infer_binding_known_kind(ctx, object, &object_kind) && object_kind == DS_LOWER_VALUE_COMMAND_RESULT) {
                SymKind field_kind = SYM_UNKNOWN;
                if (command_result_field_kind(expr->as.field.field, &field_kind)) return infer_kind(lower_value_kind_from_sym(field_kind));
            }
            return infer_none();
        }
        case DS_EXPR_UNARY:
            if (expr->as.unary.op == DS_UNARY_NEGATE) {
                infer_constrain_expr(ctx, env, expr->as.unary.right, DS_LOWER_VALUE_INT, "unary `-`");
                return infer_kind(DS_LOWER_VALUE_INT);
            }
            if (expr->as.unary.op == DS_UNARY_NOT) {
                infer_constrain_expr(ctx, env, expr->as.unary.right, DS_LOWER_VALUE_BOOL, "`!`");
                return infer_kind(DS_LOWER_VALUE_BOOL);
            }
            return infer_none();
        case DS_EXPR_BINARY: {
            if (ds_binary_op_is_arithmetic(expr->as.binary.op)) {
                infer_constrain_expr(ctx, env, expr->as.binary.left, DS_LOWER_VALUE_INT, "integer arithmetic");
                infer_constrain_expr(ctx, env, expr->as.binary.right, DS_LOWER_VALUE_INT, "integer arithmetic");
                return infer_kind(DS_LOWER_VALUE_INT);
            }
            if (ds_binary_op_is_logical(expr->as.binary.op)) {
                infer_constrain_expr(ctx, env, expr->as.binary.left, DS_LOWER_VALUE_BOOL, "logical operator");
                infer_constrain_expr(ctx, env, expr->as.binary.right, DS_LOWER_VALUE_BOOL, "logical operator");
                return infer_kind(DS_LOWER_VALUE_BOOL);
            }
            if (expr->as.binary.op == DS_BINARY_MATCHES) {
                infer_constrain_expr(ctx, env, expr->as.binary.left, DS_LOWER_VALUE_STRING, "`matches`");
                if (expr->as.binary.right && expr->as.binary.right->kind != DS_EXPR_REGEX) {
                    infer_constrain_expr(ctx, env, expr->as.binary.right, DS_LOWER_VALUE_STRING, "`matches`");
                }
                return infer_kind(DS_LOWER_VALUE_BOOL);
            }
            InferBinding left = infer_expr_binding(ctx, env, expr->as.binary.left);
            InferBinding right = infer_expr_binding(ctx, env, expr->as.binary.right);
            DsLowerValueKind left_kind = DS_LOWER_VALUE_UNKNOWN;
            DsLowerValueKind right_kind = DS_LOWER_VALUE_UNKNOWN;
            bool left_known = infer_binding_known_kind(ctx, left, &left_kind);
            bool right_known = infer_binding_known_kind(ctx, right, &right_kind);
            if (left_known && lower_value_kind_is_scalar(left_kind)) infer_constrain_binding(ctx, right, left_kind, expr->as.binary.right->span, "comparison");
            if (right_known && lower_value_kind_is_scalar(right_kind)) infer_constrain_binding(ctx, left, right_kind, expr->as.binary.left->span, "comparison");
            return infer_kind(DS_LOWER_VALUE_BOOL);
        }
        case DS_EXPR_CALL: {
            if (ds_stdlib_is_string_helper(expr->as.call.name)) {
                for (size_t i = 0; i < expr->as.call.args.len; i++) {
                    DsLowerValueKind expected = ds_stdlib_arg_expects_int(expr->as.call.name, i) ? DS_LOWER_VALUE_INT : DS_LOWER_VALUE_STRING;
                    infer_constrain_expr(ctx, env, expr->as.call.args.items[i], expected, "string helper");
                }
                return infer_kind(lower_stdlib_return_value_kind(ds_stdlib_lookup(expr->as.call.name)));
            }
            if (ds_stdlib_is_name(expr->as.call.name)) {
                const DsStdlibHelper *helper = ds_stdlib_lookup(expr->as.call.name);
                if (ds_stdlib_is_dir_walk_helper(expr->as.call.name)) {
                    if (expr->as.call.args.len > 0) infer_constrain_expr(ctx, env, expr->as.call.args.items[0], DS_LOWER_VALUE_STRING, "standard-library helper");
                    if (ds_stdlib_is_dir_walk_ext_helper(expr->as.call.name) && expr->as.call.args.len > 1) (void)infer_expr_binding(ctx, env, expr->as.call.args.items[1]);
                } else {
                    for (size_t i = 0; i < expr->as.call.args.len; i++) infer_constrain_expr(ctx, env, expr->as.call.args.items[i], DS_LOWER_VALUE_STRING, "standard-library helper");
                }
                return infer_kind(lower_stdlib_return_value_kind(helper));
            }
            DsLowerFn *callee = find_function(ctx->lower->program, expr->as.call.name);
            if (callee) {
                size_t count = expr->as.call.args.len < callee->params.len ? expr->as.call.args.len : callee->params.len;
                for (size_t i = 0; i < count; i++) {
                    DsLowerValueKind expected = lower_fn_param_expected_kind(&callee->params.items[i]);
                    if (lower_value_kind_is_scalar(expected)) infer_constrain_expr(ctx, env, expr->as.call.args.items[i], expected, "function call");
                    else (void)infer_expr_binding(ctx, env, expr->as.call.args.items[i]);
                }
                return infer_kind(callee->has_return && callee->all_paths_return ? callee->return_kind : DS_LOWER_VALUE_UNKNOWN);
            }
            for (size_t i = 0; i < expr->as.call.args.len; i++) (void)infer_expr_binding(ctx, env, expr->as.call.args.items[i]);
            return infer_none();
        }
        case DS_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) (void)infer_expr_binding(ctx, env, expr->as.array.elements.items[i]);
            return infer_kind(DS_LOWER_VALUE_ARRAY);
        case DS_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) (void)infer_expr_binding(ctx, env, expr->as.map.entries.items[i].value);
            return infer_kind(DS_LOWER_VALUE_MAP);
        case DS_EXPR_INDEX: {
            InferBinding object = infer_expr_binding(ctx, env, expr->as.index.object);
            infer_constrain_expr(ctx, env, expr->as.index.index, DS_LOWER_VALUE_INT, "index expression");
            if (expr->as.index.object && expr->as.index.object->kind == DS_EXPR_CALL &&
                ds_stdlib_array_element_kind(expr->as.index.object->as.call.name) == DS_STDLIB_ARRAY_ELEMENT_STRING) return infer_kind(DS_LOWER_VALUE_STRING);
            DsLowerValueKind object_kind = DS_LOWER_VALUE_UNKNOWN;
            if (infer_binding_known_kind(ctx, object, &object_kind) && object_kind == DS_LOWER_VALUE_ARRAY) return infer_none();
            return infer_none();
        }
        case DS_EXPR_RUN: return infer_kind(DS_LOWER_VALUE_COMMAND_RESULT);
        case DS_EXPR_RANGE:
            infer_constrain_expr(ctx, env, expr->as.range.start, DS_LOWER_VALUE_INT, "range start");
            infer_constrain_expr(ctx, env, expr->as.range.end, DS_LOWER_VALUE_INT, "range end");
            return infer_none();
        case DS_EXPR_REGEX:
        case DS_EXPR_ERROR:
            return infer_none();
    }
    return infer_none();
}

static void infer_command(InferCtx *ctx, InferEnv *env, const DsCommand *command);

static void infer_stmt(InferCtx *ctx, InferEnv *env, const DsStmt *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case DS_STMT_LET: {
            InferBinding binding = infer_expr_binding(ctx, env, stmt->as.let_stmt.value);
            infer_env_push(env, stmt->as.let_stmt.name, binding);
            return;
        }
        case DS_STMT_ASSIGN: {
            InferBinding binding = infer_expr_binding(ctx, env, stmt->as.assign_stmt.value);
            if (stmt->as.assign_stmt.op != DS_ASSIGN_SET) infer_constrain_binding(ctx, binding, DS_LOWER_VALUE_INT, stmt->as.assign_stmt.value ? stmt->as.assign_stmt.value->span : stmt->span, "compound arithmetic assignment");
            infer_env_push(env, stmt->as.assign_stmt.name, stmt->as.assign_stmt.op == DS_ASSIGN_SET ? binding : infer_kind(DS_LOWER_VALUE_INT));
            return;
        }
        case DS_STMT_INDEX_ASSIGN:
            if (stmt->as.index_assign_stmt.target && stmt->as.index_assign_stmt.target->kind == DS_EXPR_INDEX) {
                infer_constrain_expr(ctx, env, stmt->as.index_assign_stmt.target->as.index.index, DS_LOWER_VALUE_INT, "index assignment");
            }
            (void)infer_expr_binding(ctx, env, stmt->as.index_assign_stmt.value);
            return;
        case DS_STMT_IF: {
            infer_constrain_expr(ctx, env, stmt->as.if_stmt.condition, DS_LOWER_VALUE_BOOL, "if condition");
            InferEnv then_env = infer_env_clone(env);
            InferEnv else_env = infer_env_clone(env);
            infer_stmt(ctx, &then_env, stmt->as.if_stmt.then_branch);
            infer_stmt(ctx, &else_env, stmt->as.if_stmt.else_branch);
            infer_env_free(&then_env);
            infer_env_free(&else_env);
            return;
        }
        case DS_STMT_BLOCK: {
            InferEnv block_env = infer_env_clone(env);
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) infer_stmt(ctx, &block_env, stmt->as.block_stmt.statements.items[i]);
            infer_env_free(&block_env);
            return;
        }
        case DS_STMT_CALL: {
            DsLowerFn *callee = find_function(ctx->lower->program, stmt->as.call_stmt.name);
            if (callee) {
                size_t count = stmt->as.call_stmt.args.len < callee->params.len ? stmt->as.call_stmt.args.len : callee->params.len;
                for (size_t i = 0; i < count; i++) {
                    DsLowerValueKind expected = lower_fn_param_expected_kind(&callee->params.items[i]);
                    if (lower_value_kind_is_scalar(expected)) infer_constrain_expr(ctx, env, stmt->as.call_stmt.args.items[i], expected, "function call");
                    else (void)infer_expr_binding(ctx, env, stmt->as.call_stmt.args.items[i]);
                }
            } else {
                for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) (void)infer_expr_binding(ctx, env, stmt->as.call_stmt.args.items[i]);
            }
            return;
        }
        case DS_STMT_FOR: {
            if (stmt->as.for_stmt.iterable && stmt->as.for_stmt.iterable->kind == DS_EXPR_RANGE) {
                (void)infer_expr_binding(ctx, env, stmt->as.for_stmt.iterable);
            } else {
                (void)infer_expr_binding(ctx, env, stmt->as.for_stmt.iterable);
            }
            InferEnv loop_env = infer_env_clone(env);
            infer_env_push(&loop_env, stmt->as.for_stmt.key_name, stmt->as.for_stmt.iterable && stmt->as.for_stmt.iterable->kind == DS_EXPR_RANGE ? infer_kind(DS_LOWER_VALUE_INT) : infer_none());
            if (stmt->as.for_stmt.has_value_name) infer_env_push(&loop_env, stmt->as.for_stmt.value_name, infer_none());
            infer_stmt(ctx, &loop_env, stmt->as.for_stmt.body);
            infer_env_free(&loop_env);
            return;
        }
        case DS_STMT_WHILE: {
            infer_constrain_expr(ctx, env, stmt->as.while_stmt.condition, DS_LOWER_VALUE_BOOL, "while condition");
            InferEnv body_env = infer_env_clone(env);
            infer_stmt(ctx, &body_env, stmt->as.while_stmt.body);
            infer_env_free(&body_env);
            return;
        }
        case DS_STMT_CASE: {
            DsLowerValueKind pattern_kind = DS_LOWER_VALUE_UNKNOWN;
            bool mixed = false;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                for (size_t j = 0; j < arm->patterns.len; j++) {
                    DsLowerValueKind current = DS_LOWER_VALUE_UNKNOWN;
                    switch (arm->patterns.items[j].kind) {
                        case DS_CASE_PATTERN_STRING: current = DS_LOWER_VALUE_STRING; break;
                        case DS_CASE_PATTERN_INT: current = DS_LOWER_VALUE_INT; break;
                        case DS_CASE_PATTERN_BOOL: current = DS_LOWER_VALUE_BOOL; break;
                        case DS_CASE_PATTERN_DEFAULT: current = DS_LOWER_VALUE_UNKNOWN; break;
                    }
                    if (current == DS_LOWER_VALUE_UNKNOWN) continue;
                    if (pattern_kind == DS_LOWER_VALUE_UNKNOWN) pattern_kind = current;
                    else if (pattern_kind != current) mixed = true;
                }
            }
            if (!mixed && lower_value_kind_is_scalar(pattern_kind)) infer_constrain_expr(ctx, env, stmt->as.case_stmt.selector, pattern_kind, "case pattern");
            else (void)infer_expr_binding(ctx, env, stmt->as.case_stmt.selector);
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                InferEnv arm_env = infer_env_clone(env);
                infer_stmt(ctx, &arm_env, stmt->as.case_stmt.arms.items[i].body);
                infer_env_free(&arm_env);
            }
            return;
        }
        case DS_STMT_PUSH:
            (void)infer_expr_binding(ctx, env, stmt->as.push_stmt.value);
            return;
        case DS_STMT_ASSERT:
            infer_constrain_expr(ctx, env, stmt->as.assert_stmt.condition, DS_LOWER_VALUE_BOOL, "assert condition");
            return;
        case DS_STMT_RETURN:
            (void)infer_expr_binding(ctx, env, stmt->as.return_stmt.value);
            return;
        case DS_STMT_DEFER:
        case DS_STMT_TRAP: {
            InferEnv handler_env = infer_env_clone(env);
            infer_stmt(ctx, &handler_env, stmt->as.handler_stmt.body);
            infer_env_free(&handler_env);
            return;
        }
        case DS_STMT_CMD:
            infer_command(ctx, env, &stmt->as.cmd_stmt);
            return;
        case DS_STMT_IMPORT:
        case DS_STMT_FN:
        case DS_STMT_TEST:
        case DS_STMT_BREAK:
        case DS_STMT_CONTINUE:
            return;
    }
}

static bool infer_name_boundary(const char *data, size_t len, size_t pos, size_t name_len) {
    if (pos > 0) {
        char c = data[pos - 1];
        if (ds_is_ident_continue(c)) return false;
    }
    size_t end = pos + name_len;
    if (end < len) {
        char c = data[end];
        if (ds_is_ident_continue(c)) return false;
    }
    return true;
}

static bool infer_extract_ident_arg(const char *data, size_t start, size_t end, DsStr *name_out) {
    while (start < end && (data[start] == ' ' || data[start] == '\t')) start++;
    while (end > start && (data[end - 1] == ' ' || data[end - 1] == '\t')) end--;
    if (start >= end) return false;
    if (!ds_is_ident_start(data[start])) return false;
    for (size_t i = start + 1; i < end; i++) {
        if (!ds_is_ident_continue(data[i])) return false;
    }
    *name_out = (DsStr){(char *)data + start, end - start};
    return true;
}

static void infer_command_word_method_args(InferCtx *ctx, InferEnv *env, DsStr word, size_t arg_start, DsStr method, DsSpan span) {
    size_t end = arg_start;
    int depth = 1;
    while (end < word.len && depth > 0) {
        if (word.data[end] == '(') depth++;
        else if (word.data[end] == ')') depth--;
        if (depth > 0) end++;
    }
    if (end > word.len) return;
    size_t part_start = arg_start;
    size_t arg_index = 1;
    for (size_t i = arg_start; i <= end; i++) {
        if (i == end || word.data[i] == ',') {
            DsStr arg_name = {0};
            if (infer_extract_ident_arg(word.data, part_start, i, &arg_name)) {
                InferBinding binding = infer_none();
                if (infer_env_find(env, arg_name, &binding)) {
                    DsLowerValueKind expected = ds_stdlib_arg_expects_int(method, arg_index) ? DS_LOWER_VALUE_INT : DS_LOWER_VALUE_STRING;
                    infer_constrain_binding(ctx, binding, expected, span, "command interpolation string helper");
                }
            }
            part_start = i + 1;
            arg_index++;
        }
    }
}

static void infer_command_word(InferCtx *ctx, InferEnv *env, DsWord word) {
    if (!memchr(word.text.data, '{', word.text.len)) return;
    infer_interpolated_text(ctx, env, word.text, word.span);
    for (size_t n = 0; n < env->len; n++) {
        InferBinding binding = env->items[n].binding;
        if (binding.kind == INFER_BIND_NONE) continue;
        DsStr name = env->items[n].name;
        for (size_t pos = 0; pos + name.len + 2 < word.text.len; pos++) {
            if (memcmp(word.text.data + pos, name.data, name.len) != 0 || !infer_name_boundary(word.text.data, word.text.len, pos, name.len)) continue;
            size_t dot = pos + name.len;
            if (dot >= word.text.len || word.text.data[dot] != '.') continue;
            size_t method_pos = dot + 1;
            size_t method_end = method_pos;
            while (method_end < word.text.len && ds_is_ident_continue(word.text.data[method_end])) method_end++;
            if (method_end == method_pos || method_end >= word.text.len || word.text.data[method_end] != '(') continue;
            DsString full;
            ds_string_init(&full);
            ds_string_append_cstr(&full, "string.");
            ds_string_append_range(&full, word.text.data + method_pos, method_end - method_pos);
            DsStr method = {full.data, full.len};
            if (ds_stdlib_is_string_helper(method)) {
                infer_constrain_binding(ctx, binding, DS_LOWER_VALUE_STRING, word.span, "command interpolation string helper");
                infer_command_word_method_args(ctx, env, word.text, method_end + 1, method, word.span);
            }
            ds_string_free(&full);
        }
    }
}

static void infer_command(InferCtx *ctx, InferEnv *env, const DsCommand *command) {
    if (!command) return;
    for (size_t s = 0; s < command->stages.len; s++) {
        const DsCommandStage *stage = &command->stages.items[s];
        for (size_t i = 0; i < stage->words.len; i++) infer_command_word(ctx, env, stage->words.items[i]);
    }
    if (command->redirect.kind != DS_REDIRECT_NONE && command->redirect.target.len > 0) {
        DsWord redirect = {command->redirect.target, command->redirect.target_span};
        infer_command_word(ctx, env, redirect);
    }
}

void lower_functions_infer_parameter_kinds(Lower *lower, const DsAst *ast) {
    if (!ast || !lower || !lower->program || lower->program->functions.len == 0) return;
    for (size_t pass = 0; pass < lower->program->functions.len + 1; pass++) {
        bool any_changed = false;
        for (size_t i = 0; i < ast->statements.len; i++) {
            const DsStmt *stmt = ast->statements.items[i];
            if (!stmt || stmt->kind != DS_STMT_FN) continue;
            DsLowerFn *fn = find_function(lower->program, stmt->as.fn_stmt.name);
            if (!fn) continue;
            InferEnv env = {0};
            for (size_t j = 0; j < fn->params.len; j++) infer_env_push(&env, fn->params.items[j].name, infer_param(j));
            InferCtx ctx = {lower, fn, false};
            infer_stmt(&ctx, &env, stmt->as.fn_stmt.body);
            any_changed = any_changed || ctx.changed;
            infer_env_free(&env);
            if (lower->diag->has_error) return;
        }
        if (!any_changed) break;
    }
}

