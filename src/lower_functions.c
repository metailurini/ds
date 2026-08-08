#include "lower_internal.h"
#include "ds_interpolation.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

bool expr_is_literal_default(const DsExpr *expr) {
    return expr && (expr->kind == DS_EXPR_STRING || expr->kind == DS_EXPR_INT || expr->kind == DS_EXPR_BOOL);
}

void collect_function_signature(Lower *lower, const DsStmt *stmt, DsLowerProgram *program) {
    if (stmt->kind != DS_STMT_FN) return;
    if (ds_stdlib_is_name(stmt->as.fn_stmt.name) || ds_stdlib_is_namespace(stmt->as.fn_stmt.name)) {
        ds_diag_error(lower->diag, stmt->span,
                      "function `%.*s` conflicts with a v0.11.0 standard-library helper name",
                      (int)stmt->as.fn_stmt.name.len, stmt->as.fn_stmt.name.data);
        return;
    }
    if (find_function(program, stmt->as.fn_stmt.name)) {
        ds_diag_error(lower->diag, stmt->span, "duplicate function `%.*s`", (int)stmt->as.fn_stmt.name.len, stmt->as.fn_stmt.name.data);
        return;
    }
    scope_define(lower, lower->scope, stmt->as.fn_stmt.name, SYM_FUNCTION, stmt->span);
    DsLowerFn fn;
    memset(&fn, 0, sizeof(fn));
    fn.name = str_clone(stmt->as.fn_stmt.name);
    fn.span = stmt->span;
    bool seen_default = false;
    Scope param_names;
    scope_init(&param_names, NULL);
    for (size_t i = 0; i < stmt->as.fn_stmt.params.len; i++) {
        const DsFnParam *param = &stmt->as.fn_stmt.params.items[i];
        if (scope_find_current(&param_names, param->name)) {
            ds_diag_error(lower->diag, param->span, "duplicate parameter `%.*s`", (int)param->name.len, param->name.data);
        }
        Symbol dummy = {0};
        (void)dummy;
        Scope *saved = lower->scope;
        lower->scope = &param_names;
        scope_define(lower, &param_names, param->name, SYM_UNKNOWN, param->span);
        lower->scope = saved;
        if (param->has_type) {
            ds_diag_error(lower->diag, param->span, "typed function parameters are deferred in v0.9.0; omit the type annotation");
        }
        DsLowerFnParam out;
        memset(&out, 0, sizeof(out));
        out.name = str_clone(param->name);
        out.span = param->span;
        if (param->default_value) {
            seen_default = true;
            if (!expr_is_literal_default(param->default_value)) {
                ds_diag_error(lower->diag, param->default_value->span, "function parameter defaults must be string, int, or bool literals in v0.9.0");
            }
            SymKind default_kind = SYM_UNKNOWN;
            out.has_default = true;
            out.default_value = lower_expr(lower, param->default_value, &default_kind);
            out.default_kind = lower_value_kind_from_sym(default_kind);
        } else {
            if (seen_default) {
                ds_diag_error(lower->diag, param->span,
                              "required parameter `%.*s` cannot follow a default parameter",
                              (int)param->name.len, param->name.data);
            }
            fn.required_count++;
        }
        lower_fn_param_vec_push(&fn.params, out);
    }
    scope_free(&param_names);
    lower_fn_vec_push(&program->functions, fn);
}

void collect_top_level_let_signature(Lower *lower, const DsStmt *stmt) {
    if (stmt->kind != DS_STMT_LET) return;
    if (scope_find_current(lower->scope, stmt->as.let_stmt.name)) {
        return;
    }
    SymKind element_kind = SYM_UNKNOWN;
    if (stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == DS_EXPR_ARRAY) {
        element_kind = SYM_UNKNOWN;
        for (size_t i = 0; i < stmt->as.let_stmt.value->as.array.elements.len; i++) {
            const DsExpr *elem = stmt->as.let_stmt.value->as.array.elements.items[i];
            SymKind current = SYM_UNKNOWN;
            if (elem->kind == DS_EXPR_STRING) current = SYM_STRING;
            else if (elem->kind == DS_EXPR_INT) current = SYM_INT;
            else if (elem->kind == DS_EXPR_BOOL) current = SYM_BOOL;
            else { element_kind = SYM_UNKNOWN; break; }
            if (i == 0) element_kind = current;
            else if (element_kind != current) { element_kind = SYM_UNKNOWN; break; }
        }
    } else if (stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == DS_EXPR_MAP) {
        element_kind = SYM_UNKNOWN;
        for (size_t i = 0; i < stmt->as.let_stmt.value->as.map.entries.len; i++) {
            const DsExpr *value = stmt->as.let_stmt.value->as.map.entries.items[i].value;
            SymKind current = SYM_UNKNOWN;
            if (value->kind == DS_EXPR_STRING) current = SYM_STRING;
            else if (value->kind == DS_EXPR_INT) current = SYM_INT;
            else if (value->kind == DS_EXPR_BOOL) current = SYM_BOOL;
            else { element_kind = SYM_UNKNOWN; break; }
            if (i == 0) element_kind = current;
            else if (element_kind != current) { element_kind = SYM_UNKNOWN; break; }
        }
    }
    scope_define_array(lower, lower->scope, stmt->as.let_stmt.name, SYM_TOPLEVEL_PREDECLARED, element_kind, stmt->span);
}

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

static void ast_kind_env_push(AstKindEnv *env, DsStr name, DsLowerValueKind kind) {
    if (kind == DS_LOWER_VALUE_UNKNOWN) return;
    for (size_t i = env->len; i > 0; i--) {
        if (ds_str_eq(env->items[i - 1].name, name)) {
            row_schema_free(&env->items[i - 1].row_schema);
            env->items[i - 1].kind = kind;
            env->items[i - 1].is_row = false;
            env->items[i - 1].is_row_array = false;
            return;
        }
    }
    AstKindBinding binding;
    memset(&binding, 0, sizeof(binding));
    binding.name = name;
    binding.kind = kind;
    row_schema_init(&binding.row_schema);
    DS_VEC_PUSH(env, binding, 8);
}

static void ast_kind_env_push_row_schema(AstKindEnv *env, DsStr name, DsLowerValueKind kind, const DsLowerRowSchema *schema, bool is_array) {
    if (!schema || (kind != DS_LOWER_VALUE_MAP && kind != DS_LOWER_VALUE_ARRAY)) return;
    for (size_t i = env->len; i > 0; i--) {
        if (ds_str_eq(env->items[i - 1].name, name)) {
            row_schema_free(&env->items[i - 1].row_schema);
            env->items[i - 1].kind = kind;
            env->items[i - 1].is_row = !is_array;
            env->items[i - 1].is_row_array = is_array;
            row_schema_clone(schema, &env->items[i - 1].row_schema);
            return;
        }
    }
    AstKindBinding binding;
    memset(&binding, 0, sizeof(binding));
    binding.name = name;
    binding.kind = kind;
    binding.is_row = !is_array;
    binding.is_row_array = is_array;
    row_schema_clone(schema, &binding.row_schema);
    DS_VEC_PUSH(env, binding, 8);
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
    env->items = NULL;
    env->len = 0;
    env->cap = 0;
}

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

static bool infer_is_scalar(DsLowerValueKind kind) {
    return kind == DS_LOWER_VALUE_STRING || kind == DS_LOWER_VALUE_INT || kind == DS_LOWER_VALUE_BOOL;
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
    env->items = NULL;
    env->len = 0;
    env->cap = 0;
}

static const char *infer_kind_name(DsLowerValueKind kind) {
    return ds_lower_value_kind_name(kind);
}

static void infer_constrain_param(InferCtx *ctx, size_t param_index, DsLowerValueKind expected, DsSpan span, const char *reason) {
    if (!infer_is_scalar(expected) || param_index >= ctx->fn->params.len) return;
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
                      infer_kind_name(current), infer_kind_name(expected),
                      reason && reason[0] ? " from " : "",
                      reason && reason[0] ? reason : "");
    }
}

static void infer_constrain_binding(InferCtx *ctx, InferBinding binding, DsLowerValueKind expected, DsSpan span, const char *reason) {
    if (!infer_is_scalar(expected)) return;
    if (binding.kind == INFER_BIND_PARAM) {
        infer_constrain_param(ctx, binding.param_index, expected, span, reason);
        return;
    }
    if (binding.kind == INFER_BIND_KIND && binding.value_kind != DS_LOWER_VALUE_UNKNOWN && binding.value_kind != expected) {
        ds_diag_error(ctx->lower->diag, span, "expected %s value but found %s", infer_kind_name(expected), infer_kind_name(binding.value_kind));
    }
}

static bool infer_string_helper_name(DsStr name) {
    return ds_stdlib_is_string_helper(name);
}

static bool infer_string_helper_arg_expects_int(DsStr name, size_t arg_index) {
    return ds_stdlib_arg_expects_int(name, arg_index);
}

static DsLowerValueKind infer_string_helper_return_kind(DsStr name) {
    return lower_stdlib_return_value_kind(ds_stdlib_lookup(name));
}

static InferBinding infer_expr_binding(InferCtx *ctx, InferEnv *env, const DsExpr *expr);
static bool infer_extract_ident_arg(const char *data, size_t start, size_t end, DsStr *name_out);
static void infer_command_word_method_args(InferCtx *ctx, InferEnv *env, DsStr word, size_t arg_start, DsStr method, DsSpan span);

static void infer_constrain_expr(InferCtx *ctx, InferEnv *env, const DsExpr *expr, DsLowerValueKind expected, const char *reason) {
    InferBinding binding = infer_expr_binding(ctx, env, expr);
    infer_constrain_binding(ctx, binding, expected, expr ? expr->span : ctx->fn->span, reason);
}

static bool infer_parse_ident_span(const char *data, size_t start, size_t end, size_t *cursor, DsStr *name_out) {
    size_t i = *cursor;
    while (i < end && (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r')) i++;
    if (i >= end) return false;
    char c = data[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
    size_t name_start = i++;
    while (i < end) {
        c = data[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) break;
        i++;
    }
    *cursor = i;
    *name_out = (DsStr){(char *)data + name_start, i - name_start};
    (void)start;
    return true;
}

static size_t infer_find_matching_paren(const char *data, size_t start, size_t end) {
    int depth = 1;
    for (size_t i = start; i < end; i++) {
        if (data[i] == '"' || data[i] == '\'') {
            char quote = data[i++];
            while (i < end && data[i] != quote) {
                if (data[i] == '\\' && i + 1 < end) i += 2;
                else i++;
            }
            continue;
        }
        if (data[i] == '(') depth++;
        else if (data[i] == ')') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return end;
}

static void infer_interpolation_call_args(InferCtx *ctx, InferEnv *env, const DsLowerFn *callee, const char *data, size_t start, size_t end, DsSpan span) {
    size_t part_start = start;
    size_t arg_index = 0;
    int depth = 0;
    for (size_t i = start; i <= end; i++) {
        bool split = i == end;
        if (i < end) {
            if (data[i] == '"' || data[i] == '\'') {
                char quote = data[i++];
                while (i < end && data[i] != quote) {
                    if (data[i] == '\\' && i + 1 < end) i += 2;
                    else i++;
                }
            } else if (data[i] == '(') depth++;
            else if (data[i] == ')') depth--;
            else if (data[i] == ',' && depth == 0) split = true;
        }
        if (!split) continue;
        if (callee && arg_index < callee->params.len) {
            DsLowerValueKind expected = lower_fn_param_expected_kind(&callee->params.items[arg_index]);
            if (infer_is_scalar(expected)) {
                DsStr arg_name = {0};
                if (infer_extract_ident_arg(data, part_start, i, &arg_name)) {
                    InferBinding binding = infer_none();
                    if (infer_env_find(env, arg_name, &binding)) infer_constrain_binding(ctx, binding, expected, span, "interpolation function call");
                }
            }
        }
        part_start = i + 1;
        arg_index++;
    }
}

static bool infer_interpolation_method(InferCtx *ctx, InferEnv *env, DsStr text, size_t start, size_t end, DsSpan span) {
    size_t cursor = start;
    DsStr receiver = {0};
    if (!infer_parse_ident_span(text.data, start, end, &cursor, &receiver)) return false;
    while (cursor < end && (text.data[cursor] == ' ' || text.data[cursor] == '\t')) cursor++;
    if (cursor >= end || text.data[cursor++] != '.') return false;
    DsStr method = {0};
    if (!infer_parse_ident_span(text.data, start, end, &cursor, &method)) return false;
    while (cursor < end && (text.data[cursor] == ' ' || text.data[cursor] == '\t')) cursor++;
    if (cursor >= end || text.data[cursor] != '(') return false;
    DsString full;
    ds_string_init(&full);
    ds_string_append_cstr(&full, "string.");
    ds_string_append_range(&full, method.data, method.len);
    DsStr helper = {full.data, full.len};
    bool is_helper = infer_string_helper_name(helper);
    if (is_helper) {
        InferBinding binding = infer_none();
        if (infer_env_find(env, receiver, &binding)) infer_constrain_binding(ctx, binding, DS_LOWER_VALUE_STRING, span, "interpolation string helper");
        size_t close = infer_find_matching_paren(text.data, cursor + 1, end);
        infer_command_word_method_args(ctx, env, text, cursor + 1, helper, span);
        (void)close;
    }
    ds_string_free(&full);
    return is_helper;
}

static void infer_interpolation_expr_text(InferCtx *ctx, InferEnv *env, DsStr text, size_t start, size_t end, DsSpan span) {
    if (infer_interpolation_method(ctx, env, text, start, end, span)) return;
    size_t cursor = start;
    DsStr callee_name = {0};
    if (!infer_parse_ident_span(text.data, start, end, &cursor, &callee_name)) return;
    while (cursor < end && (text.data[cursor] == ' ' || text.data[cursor] == '\t' || text.data[cursor] == '\n' || text.data[cursor] == '\r')) cursor++;
    if (cursor < end && text.data[cursor] == ':') {
        DsStr spec = {text.data + cursor + 1, end - cursor - 1};
        DsInterpFormatSpec parsed;
        if (!ds_interp_parse_format_spec(spec, &parsed)) return;
        DsLowerValueKind expected = DS_LOWER_VALUE_UNKNOWN;
        switch (parsed.kind) {
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
        InferBinding binding = infer_none();
        if (infer_is_scalar(expected) && infer_env_find(env, callee_name, &binding)) {
            infer_constrain_binding(ctx, binding, expected, span, "interpolation format specifier");
        }
        return;
    }
    if (cursor >= end || text.data[cursor] != '(') return;
    DsLowerFn *callee = find_function(ctx->lower->program, callee_name);
    size_t close = infer_find_matching_paren(text.data, cursor + 1, end);
    if (close > end) return;
    infer_interpolation_call_args(ctx, env, callee, text.data, cursor + 1, close, span);
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
            if (lower_str_eq(expr->as.unary.op, "-")) {
                infer_constrain_expr(ctx, env, expr->as.unary.right, DS_LOWER_VALUE_INT, "unary `-`");
                return infer_kind(DS_LOWER_VALUE_INT);
            }
            if (lower_str_eq(expr->as.unary.op, "!")) {
                infer_constrain_expr(ctx, env, expr->as.unary.right, DS_LOWER_VALUE_BOOL, "`!`");
                return infer_kind(DS_LOWER_VALUE_BOOL);
            }
            return infer_none();
        case DS_EXPR_BINARY: {
            if (lower_str_eq(expr->as.binary.op, "+") || lower_str_eq(expr->as.binary.op, "-") ||
                lower_str_eq(expr->as.binary.op, "*") || lower_str_eq(expr->as.binary.op, "/") ||
                lower_str_eq(expr->as.binary.op, "%") || lower_str_eq(expr->as.binary.op, "**")) {
                infer_constrain_expr(ctx, env, expr->as.binary.left, DS_LOWER_VALUE_INT, "integer arithmetic");
                infer_constrain_expr(ctx, env, expr->as.binary.right, DS_LOWER_VALUE_INT, "integer arithmetic");
                return infer_kind(DS_LOWER_VALUE_INT);
            }
            if (lower_str_eq(expr->as.binary.op, "&&") || lower_str_eq(expr->as.binary.op, "||")) {
                infer_constrain_expr(ctx, env, expr->as.binary.left, DS_LOWER_VALUE_BOOL, "logical operator");
                infer_constrain_expr(ctx, env, expr->as.binary.right, DS_LOWER_VALUE_BOOL, "logical operator");
                return infer_kind(DS_LOWER_VALUE_BOOL);
            }
            if (lower_str_eq(expr->as.binary.op, "matches")) {
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
            if (left_known && infer_is_scalar(left_kind)) infer_constrain_binding(ctx, right, left_kind, expr->as.binary.right->span, "comparison");
            if (right_known && infer_is_scalar(right_kind)) infer_constrain_binding(ctx, left, right_kind, expr->as.binary.left->span, "comparison");
            return infer_kind(DS_LOWER_VALUE_BOOL);
        }
        case DS_EXPR_CALL: {
            if (infer_string_helper_name(expr->as.call.name)) {
                for (size_t i = 0; i < expr->as.call.args.len; i++) {
                    DsLowerValueKind expected = infer_string_helper_arg_expects_int(expr->as.call.name, i) ? DS_LOWER_VALUE_INT : DS_LOWER_VALUE_STRING;
                    infer_constrain_expr(ctx, env, expr->as.call.args.items[i], expected, "string helper");
                }
                return infer_kind(infer_string_helper_return_kind(expr->as.call.name));
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
                    if (infer_is_scalar(expected)) infer_constrain_expr(ctx, env, expr->as.call.args.items[i], expected, "function call");
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
                    if (infer_is_scalar(expected)) infer_constrain_expr(ctx, env, stmt->as.call_stmt.args.items[i], expected, "function call");
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
            if (!mixed && infer_is_scalar(pattern_kind)) infer_constrain_expr(ctx, env, stmt->as.case_stmt.selector, pattern_kind, "case pattern");
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
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') return false;
    }
    size_t end = pos + name_len;
    if (end < len) {
        char c = data[end];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') return false;
    }
    return true;
}

static bool infer_extract_ident_arg(const char *data, size_t start, size_t end, DsStr *name_out) {
    while (start < end && (data[start] == ' ' || data[start] == '\t')) start++;
    while (end > start && (data[end - 1] == ' ' || data[end - 1] == '\t')) end--;
    if (start >= end) return false;
    char c = data[start];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
    for (size_t i = start + 1; i < end; i++) {
        c = data[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
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
                    DsLowerValueKind expected = infer_string_helper_arg_expects_int(method, arg_index) ? DS_LOWER_VALUE_INT : DS_LOWER_VALUE_STRING;
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
    static const char *methods[] = {
        "trim", "upper", "lower", "replace", "contains", "split", "starts_with", "ends_with",
        "len", "index_of", "last_index_of", "count", "char_at", "slice"
    };
    for (size_t n = 0; n < env->len; n++) {
        InferBinding binding = env->items[n].binding;
        if (binding.kind == INFER_BIND_NONE) continue;
        DsStr name = env->items[n].name;
        for (size_t pos = 0; pos + name.len + 2 < word.text.len; pos++) {
            if (memcmp(word.text.data + pos, name.data, name.len) != 0 || !infer_name_boundary(word.text.data, word.text.len, pos, name.len)) continue;
            size_t dot = pos + name.len;
            if (dot >= word.text.len || word.text.data[dot] != '.') continue;
            for (size_t m = 0; m < sizeof(methods) / sizeof(methods[0]); m++) {
                size_t method_pos = dot + 1;
                size_t method_len = strlen(methods[m]);
                if (method_pos + method_len + 1 > word.text.len) continue;
                if (memcmp(word.text.data + method_pos, methods[m], method_len) != 0 || word.text.data[method_pos + method_len] != '(') continue;
                DsString full;
                ds_string_init(&full);
                ds_string_append_cstr(&full, "string.");
                ds_string_append_range(&full, methods[m], method_len);
                infer_constrain_binding(ctx, binding, DS_LOWER_VALUE_STRING, word.span, "command interpolation string helper");
                infer_command_word_method_args(ctx, env, word.text, method_pos + method_len + 1, (DsStr){full.data, full.len}, word.span);
                ds_string_free(&full);
            }
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

void infer_function_parameter_kinds(Lower *lower, const DsAst *ast) {
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
            if (lower_str_eq(expr->as.unary.op, "-")) {
                DsLowerValueKind right = DS_LOWER_VALUE_UNKNOWN;
                if (!ast_expr_kind_known(lower, env, expr->as.unary.right, &right) || right != DS_LOWER_VALUE_INT) return false;
                *kind_out = DS_LOWER_VALUE_INT;
                return true;
            }
            if (lower_str_eq(expr->as.unary.op, "!")) {
                *kind_out = DS_LOWER_VALUE_BOOL;
                return true;
            }
            return false;
        case DS_EXPR_BINARY:
            if (lower_str_eq(expr->as.binary.op, "+") || lower_str_eq(expr->as.binary.op, "-") ||
                lower_str_eq(expr->as.binary.op, "*") || lower_str_eq(expr->as.binary.op, "/") ||
                lower_str_eq(expr->as.binary.op, "%") || lower_str_eq(expr->as.binary.op, "**")) {
                DsLowerValueKind left = DS_LOWER_VALUE_UNKNOWN;
                DsLowerValueKind right = DS_LOWER_VALUE_UNKNOWN;
                if (!ast_expr_kind_known(lower, env, expr->as.binary.left, &left) ||
                    !ast_expr_kind_known(lower, env, expr->as.binary.right, &right) ||
                    left != DS_LOWER_VALUE_INT || right != DS_LOWER_VALUE_INT) return false;
                *kind_out = DS_LOWER_VALUE_INT;
                return true;
            }
            if (lower_str_eq(expr->as.binary.op, "==") || lower_str_eq(expr->as.binary.op, "!=") ||
                lower_str_eq(expr->as.binary.op, "===") || lower_str_eq(expr->as.binary.op, "!==") ||
                lower_str_eq(expr->as.binary.op, ">") || lower_str_eq(expr->as.binary.op, ">=") ||
                lower_str_eq(expr->as.binary.op, "<") || lower_str_eq(expr->as.binary.op, "<=")) {
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

static bool ast_value_kind_is_row_scalar(DsLowerValueKind kind) {
    return kind == DS_LOWER_VALUE_STRING || kind == DS_LOWER_VALUE_INT || kind == DS_LOWER_VALUE_BOOL;
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
        if (!ast_expr_kind_known(lower, env, entry->value, &kind) || !ast_value_kind_is_row_scalar(kind)) {
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
        if (lower_str_eq(expr->as.call.name, "string.sort_by") && expr->as.call.args.len > 0) {
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

void predeclare_function_return_contracts(Lower *lower, const DsAst *ast) {
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
        case DS_LOWER_EXPR_INTERP:
            for (size_t i = 0; i < expr->as.interp.parts.len; i++) {
                if (expr_reaches_function(lower, expr->as.interp.parts.items[i], target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_STRING:
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

bool stmt_reaches_function(Lower *lower, const DsLowerStmt *stmt, size_t target_index, bool *seen, DsSpan *cycle_span) {
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

bool function_body_reaches(Lower *lower, size_t current_index, size_t target_index, bool *seen, DsSpan *cycle_span) {
    if (current_index >= lower->program->functions.len) return false;
    if (seen[current_index]) return false;
    seen[current_index] = true;
    return stmt_reaches_function(lower, lower->program->functions.items[current_index].body, target_index, seen, cycle_span);
}

void reject_recursive_functions(Lower *lower) {
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

static bool stmt_all_paths_return(const DsLowerStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_RETURN:
            return true;
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (stmt_all_paths_return(stmt->as.block_stmt.statements.items[i])) return true;
            }
            return false;
        case DS_LOWER_STMT_IF:
            return stmt->as.if_stmt.else_branch &&
                   stmt_all_paths_return(stmt->as.if_stmt.then_branch) &&
                   stmt_all_paths_return(stmt->as.if_stmt.else_branch);
        case DS_LOWER_STMT_CASE: {
            bool has_default = false;
            if (stmt->as.case_stmt.arms.len == 0) return false;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsLowerCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                bool arm_default = false;
                for (size_t j = 0; j < arm->patterns.len; j++) {
                    if (arm->patterns.items[j].kind == DS_LOWER_CASE_PATTERN_DEFAULT) arm_default = true;
                }
                has_default = has_default || arm_default;
                if (!stmt_all_paths_return(arm->body)) return false;
            }
            return has_default;
        }
        default:
            return false;
    }
}

static bool stmt_contains_plain_command(const DsLowerStmt *stmt, DsSpan *span_out) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case DS_LOWER_STMT_CMD:
            *span_out = stmt->span;
            return true;
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (stmt_contains_plain_command(stmt->as.block_stmt.statements.items[i], span_out)) return true;
            }
            return false;
        case DS_LOWER_STMT_IF:
            return stmt_contains_plain_command(stmt->as.if_stmt.then_branch, span_out) ||
                   stmt_contains_plain_command(stmt->as.if_stmt.else_branch, span_out);
        case DS_LOWER_STMT_FOR_ARRAY:
        case DS_LOWER_STMT_FOR_MAP:
        case DS_LOWER_STMT_FOR_RANGE:
            return stmt_contains_plain_command(stmt->as.for_stmt.body, span_out);
        case DS_LOWER_STMT_WHILE:
            return stmt_contains_plain_command(stmt->as.while_stmt.body, span_out);
        case DS_LOWER_STMT_CASE:
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                if (stmt_contains_plain_command(stmt->as.case_stmt.arms.items[i].body, span_out)) return true;
            }
            return false;
        default:
            return false;
    }
}

void lower_function_body(Lower *lower, DsLowerFn *fn, const DsStmt *stmt) {
    Scope local;
    scope_init(&local, lower->scope);
    Scope *saved = lower->scope;
    int saved_depth = lower->loop_depth;
    int saved_fn_depth = lower->function_depth;
    DsLowerFn *saved_fn = lower->current_function;
    lower->scope = &local;
    lower->loop_depth = 0;
    lower->function_depth++;
    lower->current_function = fn;
    for (size_t i = 0; i < fn->params.len; i++) {
        SymKind kind = sym_kind_from_lower_value_kind(lower_fn_param_expected_kind(&fn->params.items[i]));
        scope_define(lower, &local, fn->params.items[i].name, kind, fn->params.items[i].span);
    }
    fn->body = lower_block(lower, stmt->as.fn_stmt.body, false);
    fn->all_paths_return = stmt_all_paths_return(fn->body);
    DsSpan command_span = fn->span;
    fn->contains_plain_command = stmt_contains_plain_command(fn->body, &command_span);
    lower->scope = saved;
    lower->loop_depth = saved_depth;
    lower->function_depth = saved_fn_depth;
    lower->current_function = saved_fn;
    scope_free(&local);
}
