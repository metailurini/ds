#include "lower_internal.h"
#include "ds_command_facts.h"
#include "ds_regex.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool int_literal_in_range(DsStr text) {
    static const char max_text[] = "9223372036854775807";
    size_t start = 0;
    while (start + 1 < text.len && text.data[start] == '0') start++;
    size_t len = text.len - start;
    if (len < sizeof(max_text) - 1) return true;
    if (len > sizeof(max_text) - 1) return false;
    return memcmp(text.data + start, max_text, len) <= 0;
}

bool text_contains_recursive_glob(DsStr text) {
    return ds_glob_pattern_contains_recursive(text);
}

void validate_glob_pattern_arg(Lower *lower, DsStr helper_name, const DsExpr *arg) {
    if (!(lower_str_eq(helper_name, "glob") || lower_str_eq(helper_name, "glob!"))) return;
    if (!arg || arg->kind != DS_EXPR_STRING) return;
    DsStr decoded = {0};
    if (lower_decode_string_text(arg->as.text, &decoded)) {
        size_t recursive_count = 0;
        DsGlobPatternStatus status = ds_glob_pattern_validate(decoded, &recursive_count);
        if (status != DS_GLOB_PATTERN_OK) {
            ds_diag_error(lower->diag, arg->span, "%s", ds_glob_pattern_status_message(status));
        }
        free(decoded.data);
    }
}

bool is_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

void lower_map_entry_vec_push(DsLowerMapEntryVec *vec, DsLowerMapEntry entry) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerMapEntry *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerMapEntry));
    }
    vec->items[vec->len++] = entry;
}

DsStr map_key_decode(const DsMapEntry *entry) {
    DsStr out = {0};
    if (entry->quoted_key) lower_decode_string_text(entry->key, &out);
    else out = str_clone(entry->key);
    return out;
}

bool map_has_duplicate_key(const DsLowerMapEntryVec *entries, DsStr key) {
    for (size_t i = 0; i < entries->len; i++) {
        if (entries->items[i].key.len == key.len && memcmp(entries->items[i].key.data, key.data, key.len) == 0) return true;
    }
    return false;
}

static const DsLowerRowSchema *ident_row_schema(Lower *lower, const DsLowerExpr *expr, bool want_array) {
    if (!expr || expr->kind != DS_LOWER_EXPR_IDENT) return NULL;
    Symbol *sym = scope_find(lower->scope, expr->as.text);
    if (!sym) return NULL;
    if (want_array) return sym->is_row_array ? &sym->row_schema : NULL;
    return sym->is_row ? &sym->row_schema : NULL;
}

static const DsLowerRowSchema *expr_row_schema_full(Lower *lower, const DsLowerExpr *expr) {
    const DsLowerRowSchema *schema = NULL;
    if (lower_expr_row_schema(expr, &schema)) return schema;
    return ident_row_schema(lower, expr, false);
}

static const DsLowerRowSchema *expr_row_array_schema_full(Lower *lower, const DsLowerExpr *expr) {
    const DsLowerRowSchema *schema = NULL;
    if (lower_expr_row_array_schema(expr, &schema)) return schema;
    return ident_row_schema(lower, expr, true);
}

DsLowerExpr *expr_new(DsLowerExprKind kind, DsSpan span) {
    DsLowerExpr *expr = (DsLowerExpr *)ds_xcalloc(1, sizeof(DsLowerExpr));
    expr->kind = kind;
    expr->span = span;
    return expr;
}

bool command_result_field_kind(DsStr field, SymKind *kind_out) {
    const DsCommandResultField *desc = ds_command_result_field_lookup(field);
    if (!desc) return false;
    switch (desc->kind) {
        case DS_COMMAND_RESULT_FIELD_STRING: *kind_out = SYM_STRING; return true;
        case DS_COMMAND_RESULT_FIELD_INT: *kind_out = SYM_INT; return true;
        case DS_COMMAND_RESULT_FIELD_BOOL: *kind_out = SYM_BOOL; return true;
    }
    return false;
}

bool lower_expr_produces_command_result(const DsLowerExpr *expr) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_RUN) return true;
    return expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.return_kind == DS_LOWER_VALUE_COMMAND_RESULT;
}

bool lower_expr_is_portable_command_result_return(const DsLowerExpr *expr) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_RUN || expr->kind == DS_LOWER_EXPR_IDENT) return true;
    return expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.is_user_function &&
           expr->as.call.return_kind == DS_LOWER_VALUE_COMMAND_RESULT;
}

DsLowerValueKind lower_value_kind_from_sym(SymKind kind) {
    switch (kind) {
        case SYM_BOOL: return DS_LOWER_VALUE_BOOL;
        case SYM_INT: return DS_LOWER_VALUE_INT;
        case SYM_STRING: return DS_LOWER_VALUE_STRING;
        case SYM_COMMAND_RESULT: return DS_LOWER_VALUE_COMMAND_RESULT;
        case SYM_ARRAY: return DS_LOWER_VALUE_ARRAY;
        case SYM_MAP: return DS_LOWER_VALUE_MAP;
        case SYM_FUNCTION:
        case SYM_TOPLEVEL_PREDECLARED:
        case SYM_UNKNOWN:
            return DS_LOWER_VALUE_UNKNOWN;
    }
    return DS_LOWER_VALUE_UNKNOWN;
}

SymKind sym_kind_from_lower_value_kind(DsLowerValueKind kind) {
    switch (kind) {
        case DS_LOWER_VALUE_BOOL: return SYM_BOOL;
        case DS_LOWER_VALUE_INT: return SYM_INT;
        case DS_LOWER_VALUE_STRING: return SYM_STRING;
        case DS_LOWER_VALUE_COMMAND_RESULT: return SYM_COMMAND_RESULT;
        case DS_LOWER_VALUE_ARRAY: return SYM_ARRAY;
        case DS_LOWER_VALUE_MAP: return SYM_MAP;
        case DS_LOWER_VALUE_UNKNOWN:
            return SYM_UNKNOWN;
    }
    return SYM_UNKNOWN;
}

static bool is_scalar_sym_kind(SymKind kind) {
    return kind == SYM_STRING || kind == SYM_INT || kind == SYM_BOOL;
}

static DsLowerValueKind lower_param_expected_kind(const DsLowerFnParam *param) {
    if (!param) return DS_LOWER_VALUE_UNKNOWN;
    return param->has_default ? param->default_kind : param->inferred_kind;
}

void validate_user_call_arg_kinds(Lower *lower, const DsLowerFn *fn, const DsExprVec *args, const SymKind *arg_kinds) {
    if (!fn || !args || !arg_kinds) return;
    size_t count = args->len < fn->params.len ? args->len : fn->params.len;
    for (size_t i = 0; i < count; i++) {
        SymKind expected = sym_kind_from_lower_value_kind(lower_param_expected_kind(&fn->params.items[i]));
        SymKind actual = arg_kinds[i];
        if (!is_scalar_sym_kind(expected)) continue;
        if (actual == SYM_UNKNOWN) {
            ds_diag_error(lower->diag, args->items[i]->span,
                          "cannot prove argument kind for inferred %s parameter `%.*s`",
                          ds_lower_value_kind_name(lower_value_kind_from_sym(expected)),
                          (int)fn->params.items[i].name.len, fn->params.items[i].name.data);
            continue;
        }
        if (!is_scalar_sym_kind(actual) || expected == actual) continue;
        ds_diag_error(lower->diag, args->items[i]->span,
                      "function `%.*s` expects argument %zu `%.*s` to be %s, got %s",
                      (int)fn->name.len, fn->name.data,
                      i + 1,
                      (int)fn->params.items[i].name.len, fn->params.items[i].name.data,
                      ds_lower_value_kind_name(lower_value_kind_from_sym(expected)),
                      ds_lower_value_kind_name(lower_value_kind_from_sym(actual)));
    }
}

static void validate_user_function_value_call(Lower *lower, const DsLowerFn *fn, DsSpan span) {
    /*
     * Function return kind is a lowerer/HIR contract. Expression-position calls
     * consume the function metadata collected and finalized by lowering; VM and
     * Bash backends must not rediscover whether the function is value-capable.
     */
    if (!fn->has_return) {
        ds_diag_error(lower->diag, span, "function `%.*s` does not return a value", (int)fn->name.len, fn->name.data);
    } else if (!fn->all_paths_return) {
        ds_diag_error(lower->diag, span, "function `%.*s` cannot be used as a value because not all control paths return in v0.21.0", (int)fn->name.len, fn->name.data);
    } else if (fn->contains_plain_command) {
        ds_diag_error(lower->diag, span,
                      "function `%.*s` cannot be used as a value because it contains plain command statements in v0.25.0; redirect debug output away from stdout, capture command output with `run`, or call it as a statement",
                      (int)fn->name.len, fn->name.data);
    }
}

DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);
DsLowerExpr *lower_regex_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out, bool allowed_matches_rhs);
static void lower_validate_static_regex_pattern(Lower *lower, const DsExpr *arg, DsStr decoded);

static bool helper_is_regex_match(DsStr name) { return lower_str_eq(name, "regex.match"); }
static bool helper_is_regex_replace(DsStr name) { return lower_str_eq(name, "regex.replace"); }
static bool helper_is_regex(DsStr name) { return helper_is_regex_match(name) || helper_is_regex_replace(name); }

static bool ast_binary_op_is_comparison_like(const DsExpr *expr) {
    if (!expr || expr->kind != DS_EXPR_BINARY) return false;
    return lower_str_eq(expr->as.binary.op, "in") || lower_str_eq(expr->as.binary.op, "matches") ||
           lower_str_eq(expr->as.binary.op, "==") || lower_str_eq(expr->as.binary.op, "!=") ||
           lower_str_eq(expr->as.binary.op, ">") || lower_str_eq(expr->as.binary.op, ">=") ||
           lower_str_eq(expr->as.binary.op, "<") || lower_str_eq(expr->as.binary.op, "<=");
}

DsLowerExpr *lower_binary_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    SymKind left_kind = SYM_UNKNOWN;
    SymKind right_kind = SYM_UNKNOWN;
    DsLowerExpr *left = lower_expr(lower, expr->as.binary.left, &left_kind);
    DsLowerExpr *right = NULL;
    if (lower_str_eq(expr->as.binary.op, "matches") && expr->as.binary.right->kind == DS_EXPR_REGEX) right = lower_regex_expr(lower, expr->as.binary.right, &right_kind, true);
    else right = lower_expr(lower, expr->as.binary.right, &right_kind);
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_BINARY, expr->span);
    out->as.binary.left = left;
    out->as.binary.op = str_clone(expr->as.binary.op);
    out->as.binary.right = right;
    out->as.binary.left_kind = lower_value_kind_from_sym(left_kind);
    out->as.binary.right_kind = lower_value_kind_from_sym(right_kind);
    if (ast_binary_op_is_comparison_like(expr) &&
        (ast_binary_op_is_comparison_like(expr->as.binary.left) || ast_binary_op_is_comparison_like(expr->as.binary.right))) {
        ds_diag_error(lower->diag, expr->span, "ambiguous comparison chain in v0.23.0; add parentheses around `in`, `matches`, or comparison operands");
    }
    if (lower_str_eq(expr->as.binary.op, "in")) {
        if (right_kind != SYM_ARRAY && right_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->as.binary.right->span, "right operand of `in` must be an array in v0.23.0");
        SymKind element_kind = infer_array_element_kind(lower, right);
        bool empty_array_literal = right && right->kind == DS_LOWER_EXPR_ARRAY && right->as.array.elements.len == 0;
        if (right && right->kind == DS_LOWER_EXPR_ARRAY && element_kind == SYM_UNKNOWN && !empty_array_literal) {
            ds_diag_error(lower->diag, expr->as.binary.right->span, "`in` over heterogeneous or unknown-element arrays is deferred in v0.23.0");
        }
        if (right_kind == SYM_ARRAY && element_kind != SYM_UNKNOWN && !is_scalar_sym_kind(element_kind)) ds_diag_error(lower->diag, expr->span, "`in` supports only scalar arrays in v0.23.0");
        out->as.binary.right_element_kind = lower_value_kind_from_sym(element_kind);
        *kind_out = SYM_BOOL;
        return out;
    }
    if (lower_str_eq(expr->as.binary.op, "matches")) {
        if (left_kind != SYM_STRING && left_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->as.binary.left->span, "left operand of `matches` must be a string in v0.32.0");
        if (right->kind == DS_LOWER_EXPR_STRING && expr->as.binary.right->kind == DS_EXPR_STRING) {
            DsStr decoded = {0};
            if (lower_decode_string_text(expr->as.binary.right->as.text, &decoded)) {
                lower_validate_static_regex_pattern(lower, expr->as.binary.right, decoded);
                free(decoded.data);
            }
        } else if (expr->as.binary.right->kind != DS_EXPR_REGEX && right_kind != SYM_STRING && right_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, expr->as.binary.right->span, "right operand of `matches` must be a regex literal or string pattern in v0.32.0");
        }
        *kind_out = SYM_BOOL;
        return out;
    }
    if (lower_str_eq(expr->as.binary.op, "&&") || lower_str_eq(expr->as.binary.op, "||")) {
        if (left_kind == SYM_ARRAY || left_kind == SYM_MAP || left_kind == SYM_COMMAND_RESULT) {
            ds_diag_error(lower->diag, expr->as.binary.left->span, "logical operator `%.*s` requires scalar operands in v0.23.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
        }
        if (right_kind == SYM_ARRAY || right_kind == SYM_MAP || right_kind == SYM_COMMAND_RESULT) {
            ds_diag_error(lower->diag, expr->as.binary.right->span, "logical operator `%.*s` requires scalar operands in v0.23.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
        }
        *kind_out = SYM_BOOL;
        return out;
    }
    if (lower_str_eq(expr->as.binary.op, "+")) {
        if (left_kind == SYM_INT && right_kind == SYM_INT) *kind_out = SYM_INT;
        else if (left_kind == SYM_STRING && right_kind == SYM_STRING) {
            ds_diag_error(lower->diag, expr->span, "string binary `+` cannot be emitted to standalone Bash with parity in v0.17.0; use interpolation instead");
            *kind_out = SYM_STRING;
        }
        else if (left_kind == SYM_UNKNOWN || right_kind == SYM_UNKNOWN) *kind_out = SYM_UNKNOWN;
        else ds_diag_error(lower->diag, expr->span, "operator `+` supports integer operands in v0.17.0; string concatenation is deferred");
        return out;
    }
    if (lower_str_eq(expr->as.binary.op, "-") || lower_str_eq(expr->as.binary.op, "*") ||
        lower_str_eq(expr->as.binary.op, "/") || lower_str_eq(expr->as.binary.op, "%") ||
        lower_str_eq(expr->as.binary.op, "**")) {
        if (left_kind != SYM_INT && left_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->as.binary.left->span, "operator `%.*s` requires integer operands in v0.21.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
        if (right_kind != SYM_INT && right_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->as.binary.right->span, "operator `%.*s` requires integer operands in v0.21.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
        *kind_out = SYM_INT;
        return out;
    }
    if (lower_str_eq(expr->as.binary.op, "==") || lower_str_eq(expr->as.binary.op, "!=") ||
        lower_str_eq(expr->as.binary.op, ">") || lower_str_eq(expr->as.binary.op, ">=") ||
        lower_str_eq(expr->as.binary.op, "<") || lower_str_eq(expr->as.binary.op, "<=")) {
        *kind_out = SYM_BOOL;
        return out;
    }
    ds_diag_error(lower->diag, expr->span,
                  "this expression cannot be emitted as a Bash assignment in v0.2.0; unsupported operator `%.*s` in v0.3.0",
                  (int)expr->as.binary.op.len, expr->as.binary.op.data);
    *kind_out = SYM_UNKNOWN;
    return out;
}

DsLowerExpr *lower_ident_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    Symbol *sym = scope_find(lower->scope, expr->as.text);
    if (!sym) {
        if (find_function(lower->program, expr->as.text)) {
            ds_diag_error(lower->diag, expr->span, "function `%.*s` cannot be used as a variable in v0.9.0",
                          (int)expr->as.text.len, expr->as.text.data);
        } else {
            ds_diag_error(lower->diag, expr->span, "unknown variable `%.*s`", (int)expr->as.text.len, expr->as.text.data);
        }
    } else if (sym->kind == SYM_FUNCTION) {
        ds_diag_error(lower->diag, expr->span, "function `%.*s` cannot be used as a variable in v0.9.0",
                      (int)expr->as.text.len, expr->as.text.data);
    } else {
        lower_validate_handler_capture(lower, sym, expr->as.text, expr->span);
        *kind_out = sym->kind == SYM_TOPLEVEL_PREDECLARED ? SYM_UNKNOWN : sym->kind;
    }
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_IDENT, expr->span);
    out->as.text = str_clone(expr->as.text);
    return out;
}

DsLowerExpr *lower_int_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    *kind_out = SYM_INT;
    if (!int_literal_in_range(expr->as.text)) {
        ds_diag_error(lower->diag, expr->span, "integer literal is outside the supported int range");
    }
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INT, expr->span);
    out->as.text = str_clone(expr->as.text);
    return out;
}

DsLowerExpr *lower_bool_expr(const DsExpr *expr, SymKind *kind_out) {
    *kind_out = SYM_BOOL;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_BOOL, expr->span);
    out->as.boolean = expr->as.boolean;
    return out;
}

static void validate_regex_literal(Lower *lower, const DsExpr *expr) {
    DsStr pat = {0}; bool insensitive = false;
    if (!ds_regex_literal_parts(expr->as.regex, &pat, &insensitive)) {
        ds_diag_error(lower->diag, expr->span, "%s", ds_regex_status_message(DS_REGEX_ERR_LITERAL_SYNTAX));
        return;
    }
    (void)insensitive;
    DsString decoded;
    if (!ds_regex_decode_literal_pattern(pat, &decoded)) return;
    size_t captures = 0;
    DsRegexStatus status = ds_regex_validate_pattern((DsStr){decoded.data, decoded.len}, &captures);
    (void)captures;
    if (status != DS_REGEX_OK) ds_diag_error(lower->diag, expr->span, "%s", ds_regex_status_message(status));
    ds_string_free(&decoded);
}

DsLowerExpr *lower_regex_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out, bool allowed_matches_rhs) {
    validate_regex_literal(lower, expr);
    if (!allowed_matches_rhs) ds_diag_error(lower->diag, expr->span, "regex literals are only supported as the right operand of `matches` or as regex helper pattern arguments in v0.32.0");
    *kind_out = SYM_UNKNOWN;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_REGEX, expr->span);
    out->as.regex = str_clone(expr->as.regex);
    return out;
}

DsLowerExpr *lower_run_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    if (expr->as.run.stages.len == 0) {
        ds_diag_error(lower->diag, expr->span, "expected command after `run`");
    }
    for (size_t s = 0; s < expr->as.run.stages.len; s++) {
        if (expr->as.run.stages.items[s].words.len == 0) ds_diag_error(lower->diag, expr->as.run.stages.items[s].span, "empty pipeline stage");
        for (size_t i = 0; i < expr->as.run.stages.items[s].words.len; i++) lower_validate_command_word(lower, expr->as.run.stages.items[s].words.items[i].text, expr->as.run.stages.items[s].words.items[i].span);
    }
    if (expr->as.run.redirect.kind != DS_REDIRECT_NONE) ds_diag_error(lower->diag, expr->as.run.redirect.op_span, "captured `run` commands do not support redirection");
    *kind_out = SYM_COMMAND_RESULT;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_RUN, expr->span);
    ds_command_clone(&out->as.run, &expr->as.run);
    return out;
}

DsLowerExpr *lower_map_field_expr(Lower *lower, const DsExpr *expr, DsLowerExpr *object, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INDEX, expr->span);
    out->as.index.object = object;
    out->as.index.object_is_map = true;
    out->as.index.index = expr_new(DS_LOWER_EXPR_STRING, expr->span);
    DsString quoted;
    ds_string_init(&quoted);
    ds_string_append_char(&quoted, '"');
    ds_string_append_range(&quoted, expr->as.field.field.data, expr->as.field.field.len);
    ds_string_append_char(&quoted, '"');
    out->as.index.index->as.text.data = quoted.data;
    out->as.index.index->as.text.len = quoted.len;
    out->as.index.map_key_literal = true;
    out->as.index.map_key = str_clone(expr->as.field.field);
    const DsLowerRowSchema *schema = expr_row_schema_full(lower, object);
    if (schema) {
        const DsLowerRowField *field = row_schema_find(schema, expr->as.field.field);
        if (!field) {
            ds_diag_error(lower->diag, expr->span, "unknown row field `%.*s`", (int)expr->as.field.field.len, expr->as.field.field.data);
            *kind_out = SYM_UNKNOWN;
        } else {
            out->as.index.element_kind = field->kind;
            *kind_out = sym_kind_from_lower_value_kind(field->kind);
        }
    } else {
        *kind_out = SYM_UNKNOWN;
    }
    return out;
}

DsLowerExpr *lower_field_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    if (expr->as.field.object && expr->as.field.object->kind == DS_EXPR_IDENT && lower_str_eq(expr->as.field.object->as.text, "env")) {
        if (!is_env_name_text(expr->as.field.field)) {
            ds_diag_error(lower->diag, expr->span, "invalid environment variable name `%.*s` in v0.27.0", (int)expr->as.field.field.len, expr->as.field.field.data);
        }
        DsLowerExpr *out = expr_new(DS_LOWER_EXPR_CALL, expr->span);
        out->as.call.name = (DsStr){ds_str_dup_range("env.get", 7), 7};
        DsLowerExpr *arg = expr_new(DS_LOWER_EXPR_STRING, expr->span);
        DsString quoted;
        ds_string_init(&quoted);
        ds_string_append_char(&quoted, '"');
        ds_string_append_range(&quoted, expr->as.field.field.data, expr->as.field.field.len);
        ds_string_append_char(&quoted, '"');
        arg->as.text = (DsStr){quoted.data, quoted.len};
        lower_expr_vec_push(&out->as.call.args, arg);
        out->as.call.return_kind = DS_LOWER_VALUE_STRING;
        *kind_out = SYM_STRING;
        return out;
    }
    SymKind object_kind = SYM_UNKNOWN;
    DsLowerExpr *object = lower_expr(lower, expr->as.field.object, &object_kind);
    SymKind field_kind = SYM_UNKNOWN;
    bool object_is_command_result = object_kind == SYM_COMMAND_RESULT || lower_expr_produces_command_result(object);
    if (object_is_command_result) {
        lower_validate_portable_collection_receiver(lower, object, expr->span);
        if (!command_result_field_kind(expr->as.field.field, &field_kind)) {
            ds_diag_error(lower->diag, expr->span, "unknown command result field `%.*s`", (int)expr->as.field.field.len, expr->as.field.field.data);
        }
    } else if (object_kind == SYM_MAP) {
        bool portable_row_index = object && object->kind == DS_LOWER_EXPR_INDEX && object->as.index.returns_row;
        if (!portable_row_index) lower_validate_portable_collection_receiver(lower, object, expr->span);
        return lower_map_field_expr(lower, expr, object, kind_out);
    } else {
        ds_diag_error(lower->diag, expr->span, "field access is only supported on command results and maps in v0.10.0");
    }
    *kind_out = field_kind;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_FIELD, expr->span);
    out->as.field.object = object;
    out->as.field.field = str_clone(expr->as.field.field);
    out->as.field.field_kind = lower_value_kind_from_sym(field_kind);
    return out;
}

DsLowerExpr *lower_unary_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    SymKind right_kind = SYM_UNKNOWN;
    DsLowerExpr *right = lower_expr(lower, expr->as.unary.right, &right_kind);
    if (!lower_str_eq(expr->as.unary.op, "!")) {
        if (lower_str_eq(expr->as.unary.op, "-")) {
            if (right_kind != SYM_INT && right_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->span, "unary `-` requires an integer operand in v0.21.0");
            *kind_out = SYM_INT;
        } else {
            ds_diag_error(lower->diag, expr->span, "unsupported unary operator `%.*s` in v0.3.0", (int)expr->as.unary.op.len, expr->as.unary.op.data);
            *kind_out = SYM_UNKNOWN;
        }
    } else {
        *kind_out = SYM_BOOL;
    }
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_UNARY, expr->span);
    out->as.unary.op = str_clone(expr->as.unary.op);
    out->as.unary.right = right;
    return out;
}


static bool is_string_helper_name(DsStr name) {
    return lower_str_eq(name, "string.trim") || lower_str_eq(name, "string.upper") ||
           lower_str_eq(name, "string.lower") || lower_str_eq(name, "string.replace") ||
           lower_str_eq(name, "string.contains") || lower_str_eq(name, "string.split") ||
           lower_str_eq(name, "string.starts_with") || lower_str_eq(name, "string.ends_with") ||
           lower_str_eq(name, "string.len") || lower_str_eq(name, "string.index_of") ||
           lower_str_eq(name, "string.last_index_of") || lower_str_eq(name, "string.count") ||
           lower_str_eq(name, "string.char_at") || lower_str_eq(name, "string.slice");
}

static bool string_helper_arg_count_ok(DsStr name, size_t argc, size_t *expected) {
    if (lower_str_eq(name, "string.trim") || lower_str_eq(name, "string.upper") || lower_str_eq(name, "string.lower") || lower_str_eq(name, "string.len")) { *expected = 1; return argc == 1; }
    if (lower_str_eq(name, "string.replace")) { *expected = 3; return argc == 3; }
    if (lower_str_eq(name, "string.slice")) { *expected = 3; return argc == 3; }
    *expected = 2;
    return argc == 2;
}

static bool string_helper_arg_expects_int(DsStr name, size_t arg_index) {
    if (lower_str_eq(name, "string.char_at")) return arg_index == 1;
    if (lower_str_eq(name, "string.slice")) return arg_index == 1 || arg_index == 2;
    return false;
}

static bool string_helper_arg_expects_string(DsStr name, size_t arg_index) {
    if (arg_index == 0) return true;
    if (string_helper_arg_expects_int(name, arg_index)) return false;
    return true;
}

static DsLowerExpr *lower_raw_string_expr(DsStr raw, DsSpan span) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_STRING, span);
    DsString quoted;
    ds_string_init(&quoted);
    ds_string_append_char(&quoted, '"');
    for (size_t i = 0; i < raw.len; i++) {
        char c = raw.data[i];
        if (c == '"' || c == '\\') {
            ds_string_append_char(&quoted, '\\');
            ds_string_append_char(&quoted, c);
        } else if (c == '\n') {
            ds_string_append_range(&quoted, "\\n", 2);
        } else if (c == '\t') {
            ds_string_append_range(&quoted, "\\t", 2);
        } else {
            ds_string_append_char(&quoted, c);
        }
    }
    ds_string_append_char(&quoted, '"');
    out->as.text = (DsStr){quoted.data, quoted.len};
    return out;
}

static void lower_validate_static_regex_pattern(Lower *lower, const DsExpr *arg, DsStr decoded) {
    size_t capture_count = 0;
    DsRegexStatus status = ds_regex_validate_pattern(decoded, &capture_count);
    (void)capture_count;
    if (status != DS_REGEX_OK) ds_diag_error(lower->diag, arg->span, "%s", ds_regex_status_message(status));
}

static void lower_validate_static_regex_flags(Lower *lower, const DsExpr *arg, DsStr decoded) {
    DsRegexStatus status = ds_regex_validate_flags(decoded, NULL);
    if (status != DS_REGEX_OK) ds_diag_error(lower->diag, arg->span, "%s", ds_regex_status_message(status));
}

static void lower_validate_static_regex_replacement(Lower *lower, const DsExpr *arg, DsStr decoded, size_t capture_count, bool capture_count_known) {
    DsRegexStatus status = ds_regex_validate_replacement(decoded, capture_count, capture_count_known);
    if (status != DS_REGEX_OK) ds_diag_error(lower->diag, arg->span, "%s", ds_regex_status_message(status));
}

static DsLowerExpr *lower_regex_helper_call_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    bool is_match = helper_is_regex_match(expr->as.call.name);
    bool is_replace = helper_is_regex_replace(expr->as.call.name);
    size_t min_arity = is_match ? 2 : 3;
    size_t max_arity = is_match ? 3 : 4;
    if (expr->as.call.args.len < min_arity || expr->as.call.args.len > max_arity) {
        if (min_arity == max_arity) ds_diag_error(lower->diag, expr->span, "helper `%.*s` expects %zu arguments but got %zu", (int)expr->as.call.name.len, expr->as.call.name.data, min_arity, expr->as.call.args.len);
        else ds_diag_error(lower->diag, expr->span, "helper `%.*s` expects %zu to %zu arguments but got %zu", (int)expr->as.call.name.len, expr->as.call.name.data, min_arity, max_arity, expr->as.call.args.len);
    }

    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_CALL, expr->span);
    out->as.call.name = str_clone(expr->as.call.name);
    out->as.call.return_kind = is_match ? DS_LOWER_VALUE_MAP : DS_LOWER_VALUE_STRING;

    bool literal_pattern_insensitive = false;
    bool pattern_capture_count_known = false;
    size_t pattern_capture_count = 0;

    for (size_t i = 0; i < expr->as.call.args.len; i++) {
        const DsExpr *arg = expr->as.call.args.items[i];
        if (i == 1 && arg->kind == DS_EXPR_REGEX) {
            DsStr pat = {0};
            if (!ds_regex_literal_parts(arg->as.regex, &pat, &literal_pattern_insensitive)) {
                ds_diag_error(lower->diag, arg->span, "%s", ds_regex_status_message(DS_REGEX_ERR_LITERAL_SYNTAX));
                lower_expr_vec_push(&out->as.call.args, expr_new(DS_LOWER_EXPR_ERROR, arg->span));
                continue;
            }
            DsString decoded;
            if (!ds_regex_decode_literal_pattern(pat, &decoded)) {
                lower_expr_vec_push(&out->as.call.args, expr_new(DS_LOWER_EXPR_ERROR, arg->span));
                continue;
            }
            DsRegexStatus status = ds_regex_validate_pattern((DsStr){decoded.data, decoded.len}, &pattern_capture_count);
            pattern_capture_count_known = status == DS_REGEX_OK;
            if (status != DS_REGEX_OK) ds_diag_error(lower->diag, arg->span, "%s", ds_regex_status_message(status));
            lower_expr_vec_push(&out->as.call.args, lower_raw_string_expr((DsStr){decoded.data, decoded.len}, arg->span));
            ds_string_free(&decoded);
            continue;
        }

        SymKind arg_kind = SYM_UNKNOWN;
        DsLowerExpr *lowered = lower_expr(lower, arg, &arg_kind);
        lower_expr_vec_push(&out->as.call.args, lowered);
        if (i == 1 && arg->kind == DS_EXPR_STRING) {
            DsStr decoded = {0};
            if (lower_decode_string_text(arg->as.text, &decoded)) {
                size_t captures = 0;
                DsRegexStatus status = ds_regex_validate_pattern(decoded, &captures);
                if (status != DS_REGEX_OK) ds_diag_error(lower->diag, arg->span, "%s", ds_regex_status_message(status));
                else { pattern_capture_count_known = true; pattern_capture_count = captures; }
                free(decoded.data);
            }
        }
        if (i == 2 && is_replace && arg->kind == DS_EXPR_STRING) {
            DsStr decoded = {0};
            if (lower_decode_string_text(arg->as.text, &decoded)) {
                lower_validate_static_regex_replacement(lower, arg, decoded, pattern_capture_count, pattern_capture_count_known);
                free(decoded.data);
            }
        }
        size_t flags_index = is_match ? 2 : 3;
        if (i == flags_index && arg->kind == DS_EXPR_STRING) {
            DsStr decoded = {0};
            if (lower_decode_string_text(arg->as.text, &decoded)) {
                lower_validate_static_regex_flags(lower, arg, decoded);
                free(decoded.data);
            }
        }
        if ((i == 0 || i == 1 || (is_replace && i == 2) || i == flags_index) && arg_kind != SYM_STRING && arg_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, arg->span, "regex helper `%.*s` expects string arguments in v0.32.0", (int)expr->as.call.name.len, expr->as.call.name.data);
        }
    }

    if (literal_pattern_insensitive) {
        size_t flags_index = is_match ? 2 : 3;
        if (expr->as.call.args.len > flags_index) {
            ds_diag_error(lower->diag, expr->as.call.args.items[flags_index]->span, "regex flags may be specified either on the literal or as a helper argument in v0.32.0, not both");
        } else {
            lower_expr_vec_push(&out->as.call.args, lower_raw_string_expr((DsStr){"i", 1}, expr->as.call.args.items[1]->span));
        }
    }

    *kind_out = is_match ? SYM_MAP : SYM_STRING;
    return out;
}

DsLowerExpr *lower_call_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    if (helper_is_regex(expr->as.call.name)) return lower_regex_helper_call_expr(lower, expr, kind_out);
    bool is_row_sort_method = lower_str_eq(expr->as.call.name, "string.sort_by");

    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_CALL, expr->span);
    out->as.call.name = str_clone(expr->as.call.name);
    SymKind *arg_kinds = expr->as.call.args.len ? (SymKind *)ds_xcalloc(expr->as.call.args.len, sizeof(SymKind)) : NULL;
    for (size_t i = 0; i < expr->as.call.args.len; i++) {
        SymKind arg_kind = SYM_UNKNOWN;
        lower_expr_vec_push(&out->as.call.args, lower_expr(lower, expr->as.call.args.items[i], &arg_kind));
        arg_kinds[i] = arg_kind;
        if (is_row_sort_method) {
            if (i > 0 && expr->as.call.args.len > 0 && arg_kinds[0] == SYM_ARRAY && arg_kind != SYM_STRING && arg_kind != SYM_UNKNOWN) {
                ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "row-array method `sort_by` expects string literal arguments in v0.37.0");
            }
        } else if (is_string_helper_name(expr->as.call.name)) {
            if (i == 0 && arg_kind != SYM_STRING) {
                ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "string method `%.*s` requires a string receiver", (int)expr->as.call.name.len, expr->as.call.name.data);
            } else if (i > 0 && string_helper_arg_expects_int(expr->as.call.name, i) && arg_kind != SYM_INT) {
                ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "string method `%.*s` expects int index arguments", (int)expr->as.call.name.len, expr->as.call.name.data);
            } else if (i > 0 && string_helper_arg_expects_string(expr->as.call.name, i) && arg_kind != SYM_STRING) {
                ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "string method `%.*s` expects string arguments", (int)expr->as.call.name.len, expr->as.call.name.data);
            }
        } else if (ds_stdlib_is_name(expr->as.call.name) && arg_kind != SYM_STRING && arg_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "standard-library helper `%.*s` expects string arguments in v0.11.0", (int)expr->as.call.name.len, expr->as.call.name.data);
        } else if (arg_kind != SYM_STRING && arg_kind != SYM_INT && arg_kind != SYM_BOOL && arg_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "standard-library arguments must be scalar values in v0.11.0");
        }
    }
    if (is_row_sort_method && expr->as.call.args.len > 0 && arg_kinds[0] == SYM_ARRAY) {
        if (expr->as.call.args.len < 2 || expr->as.call.args.len > 3) {
            ds_diag_error(lower->diag, expr->span, "row-array method `sort_by` expects a field and optional direction in v0.37.0");
        }
        const DsLowerRowSchema *schema = NULL;
        if (out->as.call.args.len > 0) schema = expr_row_array_schema_full(lower, out->as.call.args.items[0]);
        if (!schema) {
            ds_diag_error(lower->diag, expr->span, "row-array method `sort_by` requires a row-array with known schema in v0.37.0");
        }
        DsStr field = {0};
        if (expr->as.call.args.len > 1 && expr->as.call.args.items[1]->kind == DS_EXPR_STRING) {
            lower_decode_string_text(expr->as.call.args.items[1]->as.text, &field);
            if (field.len == 0) ds_diag_error(lower->diag, expr->as.call.args.items[1]->span, "sort_by field must be non-empty in v0.37.0");
            else if (schema && !row_schema_find(schema, field)) ds_diag_error(lower->diag, expr->as.call.args.items[1]->span, "unknown row field `%.*s`", (int)field.len, field.data);
        } else if (expr->as.call.args.len > 1) {
            ds_diag_error(lower->diag, expr->as.call.args.items[1]->span, "sort_by field must be a string literal in v0.37.0");
        }
        DsStr direction = {0};
        if (expr->as.call.args.len > 2 && expr->as.call.args.items[2]->kind == DS_EXPR_STRING) {
            lower_decode_string_text(expr->as.call.args.items[2]->as.text, &direction);
            if (!(direction.len == 3 && memcmp(direction.data, "asc", 3) == 0) &&
                !(direction.len == 4 && memcmp(direction.data, "desc", 4) == 0)) {
                ds_diag_error(lower->diag, expr->as.call.args.items[2]->span, "sort_by direction must be \"asc\" or \"desc\" in v0.37.0");
            }
        } else if (expr->as.call.args.len == 2) {
            DsLowerExpr *dir = expr_new(DS_LOWER_EXPR_STRING, expr->span);
            dir->as.text = (DsStr){ds_str_dup_range("\"asc\"", 5), 5};
            lower_expr_vec_push(&out->as.call.args, dir);
        } else if (expr->as.call.args.len > 2) {
            ds_diag_error(lower->diag, expr->as.call.args.items[2]->span, "sort_by direction must be a string literal in v0.37.0");
        }
        free(field.data);
        free(direction.data);
        free(out->as.call.name.data);
        out->as.call.name = (DsStr){ds_str_dup_range("rowarray.sort_by", 16), 16};
        out->as.call.return_kind = DS_LOWER_VALUE_ARRAY;
        out->as.call.returns_row_array = true;
        if (schema) row_schema_clone(schema, &out->as.call.row_schema);
        *kind_out = SYM_ARRAY;
        free(arg_kinds);
        return out;
    }
    const DsStdlibHelper *stdlib_helper = ds_stdlib_lookup(expr->as.call.name);
    if (stdlib_helper) {
        DsLowerValueKind ret_kind = lower_stdlib_return_value_kind(stdlib_helper);
        SymKind ret = sym_kind_from_lower_value_kind(ret_kind);
        if (!ds_stdlib_arity_ok(stdlib_helper, expr->as.call.args.len)) {
            if (is_string_helper_name(expr->as.call.name)) {
                size_t expected = 0;
                string_helper_arg_count_ok(expr->as.call.name, expr->as.call.args.len, &expected);
                ds_diag_error(lower->diag, expr->span, "string method `%.*s` expects %zu arguments including receiver but got %zu", (int)expr->as.call.name.len, expr->as.call.name.data, expected, expr->as.call.args.len);
            } else if (stdlib_helper->min_arity == stdlib_helper->max_arity) ds_diag_error(lower->diag, expr->span, "helper `%.*s` expects %zu arguments but got %zu", (int)expr->as.call.name.len, expr->as.call.name.data, stdlib_helper->min_arity, expr->as.call.args.len);
            else ds_diag_error(lower->diag, expr->span, "helper `%.*s` expects %zu to %zu arguments but got %zu", (int)expr->as.call.name.len, expr->as.call.name.data, stdlib_helper->min_arity, stdlib_helper->max_arity, expr->as.call.args.len);
        } else if (stdlib_helper->statement_only) {
            ds_diag_error(lower->diag, expr->span, "helper `%.*s` is statement-only in v0.11.0", (int)expr->as.call.name.len, expr->as.call.name.data);
        }
        if (is_string_helper_name(expr->as.call.name)) {
            if ((lower_str_eq(expr->as.call.name, "string.split") || lower_str_eq(expr->as.call.name, "string.replace")) && expr->as.call.args.len > 1 && expr->as.call.args.items[1]->kind == DS_EXPR_STRING) {
                DsStr decoded = {0};
                if (lower_decode_string_text(expr->as.call.args.items[1]->as.text, &decoded)) {
                    if (decoded.len == 0) {
                        if (lower_str_eq(expr->as.call.name, "string.split")) ds_diag_error(lower->diag, expr->as.call.args.items[1]->span, "split with an empty separator is deferred in v0.19.0");
                        else ds_diag_error(lower->diag, expr->as.call.args.items[1]->span, "replace with an empty source is deferred in v0.19.0");
                    }
                    free(decoded.data);
                }
            }
        }
        if (stdlib_helper->validates_env_name && expr->as.call.args.len > 0 && expr->as.call.args.items[0]->kind == DS_EXPR_STRING) {
            DsStr decoded = {0};
            if (lower_decode_string_text(expr->as.call.args.items[0]->as.text, &decoded)) {
                if (!is_env_name_text(decoded)) ds_diag_error(lower->diag, expr->as.call.args.items[0]->span, "invalid environment variable name `%.*s` in v0.11.0", (int)decoded.len, decoded.data);
                free(decoded.data);
            }
        }
        if (expr->as.call.args.len > 0) validate_glob_pattern_arg(lower, expr->as.call.name, expr->as.call.args.items[0]);
        *kind_out = ret;
        out->as.call.return_kind = ret_kind;
        free(arg_kinds);
        return out;
    }
    DsStr ns = {0}, member = {0};
    if (split_member_name(expr->as.call.name, &ns, &member) && ds_stdlib_is_namespace(ns)) {
        ds_diag_error(lower->diag, expr->span, "unknown standard-library helper `%.*s`", (int)expr->as.call.name.len, expr->as.call.name.data);
        free(arg_kinds);
        return out;
    }
    if (split_member_name(expr->as.call.name, &ns, &member) && ns.len == 6 && memcmp(ns.data, "string", 6) == 0) {
        ds_diag_error(lower->diag, expr->span, "unknown string method `%.*s`; supported methods are trim, upper, lower, replace, contains, split, starts_with, ends_with, len, index_of, last_index_of, count, char_at, slice", (int)member.len, member.data);
        free(arg_kinds);
        return out;
    }
    DsLowerFn *fn = find_function(lower->program, expr->as.call.name);
    if (fn) {
        if (expr->as.call.args.len < fn->required_count || expr->as.call.args.len > fn->params.len) {
            ds_diag_error(lower->diag, expr->span, "function `%.*s` called with wrong number of arguments", (int)fn->name.len, fn->name.data);
        }
        validate_user_call_arg_kinds(lower, fn, &expr->as.call.args, arg_kinds);
        validate_user_function_value_call(lower, fn, expr->span);
        *kind_out = sym_kind_from_lower_value_kind(fn->return_kind);
        out->as.call.is_user_function = true;
        out->as.call.return_kind = fn->return_kind;
        if (fn->returns_row) {
            out->as.call.returns_row = true;
            row_schema_clone(&fn->row_schema, &out->as.call.row_schema);
        }
        if (fn->returns_row_array) {
            out->as.call.returns_row_array = true;
            row_schema_clone(&fn->row_schema, &out->as.call.row_schema);
        }
        free(arg_kinds);
        return out;
    }
    ds_diag_error(lower->diag, expr->span, "unknown function `%.*s`", (int)expr->as.call.name.len, expr->as.call.name.data);
    free(arg_kinds);
    return out;
}

DsLowerExpr *lower_array_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_ARRAY, expr->span);
    bool saw_row = false;
    bool saw_non_row = false;
    DsLowerRowSchema common_schema;
    row_schema_init(&common_schema);
    for (size_t i = 0; i < expr->as.array.elements.len; i++) {
        SymKind elem_kind = SYM_UNKNOWN;
        DsLowerExpr *element = lower_expr(lower, expr->as.array.elements.items[i], &elem_kind);
        lower_expr_vec_push(&out->as.array.elements, element);
        const DsLowerRowSchema *element_row_schema = NULL;
        if (elem_kind == SYM_MAP && (lower_expr_row_schema(element, &element_row_schema) || (element_row_schema = ident_row_schema(lower, element, false)))) {
            const DsLowerRowSchema *schema = NULL;
            schema = element_row_schema;
            if (!saw_row) row_schema_clone(schema, &common_schema);
            else if (!row_schema_equal(&common_schema, schema)) {
                ds_diag_error(lower->diag, expr->as.array.elements.items[i]->span,
                              "row-array elements must have the same field names and scalar kinds in v0.37.0");
            }
            saw_row = true;
        } else if (elem_kind == SYM_ARRAY || elem_kind == SYM_MAP) {
            ds_diag_error(lower->diag, expr->as.array.elements.items[i]->span, "nested collections are deferred in v0.10.0");
        } else if (!lower_collection_element_is_portable(element)) {
            ds_diag_error(lower->diag, expr->as.array.elements.items[i]->span, "collection element expressions must be scalar Bash-emittable values in v0.10.0; bind the expression to a variable first");
        } else {
            saw_non_row = true;
        }
    }
    if (saw_row && saw_non_row) {
        ds_diag_error(lower->diag, expr->span, "row-array literals cannot mix rows with scalar values in v0.37.0");
    }
    if (saw_row) {
        out->as.array.is_row_array = true;
        row_schema_clone(&common_schema, &out->as.array.row_schema);
    }
    row_schema_free(&common_schema);
    *kind_out = SYM_ARRAY;
    return out;
}

DsLowerExpr *lower_map_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_MAP, expr->span);
    for (size_t i = 0; i < expr->as.map.entries.len; i++) {
        const DsMapEntry *entry = &expr->as.map.entries.items[i];
        DsLowerMapEntry lowered;
        memset(&lowered, 0, sizeof(lowered));
        lowered.key = map_key_decode(entry);
        lowered.span = entry->span;
        if (lowered.key.len == 0) {
            ds_diag_error(lower->diag, entry->span, "empty map keys are deferred in v0.10.0 because emitted Bash cannot represent them safely");
        }
        if (map_has_duplicate_key(&out->as.map.entries, lowered.key)) {
            ds_diag_error(lower->diag, entry->span, "duplicate map key `%.*s`", (int)lowered.key.len, lowered.key.data);
        }
        SymKind value_kind = SYM_UNKNOWN;
        lowered.value = lower_expr(lower, entry->value, &value_kind);
        if (value_kind == SYM_ARRAY || value_kind == SYM_MAP) {
            ds_diag_error(lower->diag, entry->span, "nested collections are deferred in v0.10.0");
        } else if (!lower_collection_row_field_is_portable(lowered.value)) {
            ds_diag_error(lower->diag, entry->value->span, "collection element expressions must be scalar Bash-emittable values in v0.10.0; bind the expression to a variable first");
        }
        lower_map_entry_vec_push(&out->as.map.entries, lowered);
    }
    DsLowerRowSchema schema;
    if (lower_map_expr_schema(lower, out, &schema)) {
        out->as.map.is_row = true;
        row_schema_clone(&schema, &out->as.map.row_schema);
        row_schema_free(&schema);
    }
    *kind_out = SYM_MAP;
    return out;
}

static bool lower_expr_is_portable_stdlib_array_result(const DsLowerExpr *expr) {
    if (!expr || expr->kind != DS_LOWER_EXPR_CALL || !ds_stdlib_is_name(expr->as.call.name)) return false;
    const DsStdlibHelper *helper = ds_stdlib_lookup(expr->as.call.name);
    return helper && helper->return_kind == DS_STDLIB_RETURN_ARRAY && lower_str_eq(expr->as.call.name, "string.split");
}

DsLowerExpr *lower_index_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    SymKind obj_kind = SYM_UNKNOWN;
    SymKind idx_kind = SYM_UNKNOWN;
    DsLowerExpr *object = lower_expr(lower, expr->as.index.object, &obj_kind);
    DsLowerExpr *index = lower_expr(lower, expr->as.index.index, &idx_kind);
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INDEX, expr->span);
    out->as.index.object = object;
    out->as.index.index = index;
    if (obj_kind == SYM_ARRAY) {
        out->as.index.object_is_array = true;
        bool portable_stdlib_array = lower_expr_is_portable_stdlib_array_result(object);
        if (!portable_stdlib_array) {
            lower_validate_portable_collection_receiver(lower, object, expr->span);
            lower_validate_portable_collection_index(lower, index, false, expr->as.index.index->span);
        }
        if (idx_kind != SYM_INT && idx_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->as.index.index->span, "array index must be an int in v0.10.0");
        if (expr->as.index.index && expr->as.index.index->kind == DS_EXPR_UNARY && lower_str_eq(expr->as.index.index->as.unary.op, "-") &&
            expr->as.index.index->as.unary.right && expr->as.index.index->as.unary.right->kind == DS_EXPR_INT) {
            ds_diag_error(lower->diag, expr->as.index.index->span, "array index must be non-negative in v0.10.0");
        }
        SymKind element_kind = infer_array_element_kind(lower, object);
        out->as.index.element_kind = lower_value_kind_from_sym(element_kind);
        if (element_kind == SYM_MAP) {
            const DsLowerRowSchema *schema = expr_row_array_schema_full(lower, object);
            if (schema) {
                out->as.index.returns_row = true;
                row_schema_clone(schema, &out->as.index.row_schema);
            } else {
                ds_diag_error(lower->diag, expr->span, "array indexing only supports scalar arrays or row arrays with known schema in v0.37.0");
            }
        }
        *kind_out = element_kind;
    } else if (obj_kind == SYM_MAP) {
        out->as.index.object_is_map = true;
        bool portable_row_index = object && object->kind == DS_LOWER_EXPR_INDEX && object->as.index.returns_row;
        if (!portable_row_index) lower_validate_portable_collection_receiver(lower, object, expr->span);
        lower_validate_portable_collection_index(lower, index, true, expr->as.index.index->span);
        if (expr->as.index.index && expr->as.index.index->kind == DS_EXPR_STRING) {
            out->as.index.map_key_literal = true;
            lower_decode_string_text(expr->as.index.index->as.text, &out->as.index.map_key);
            const DsLowerRowSchema *schema = expr_row_schema_full(lower, object);
            if (schema) {
                const DsLowerRowField *field = row_schema_find(schema, out->as.index.map_key);
                if (field) {
                    out->as.index.element_kind = field->kind;
                    *kind_out = sym_kind_from_lower_value_kind(field->kind);
                    return out;
                }
            }
        } else if (idx_kind != SYM_STRING && idx_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, expr->as.index.index->span, "map index must be a string in v0.10.0");
        }
        SymKind value_kind = infer_map_value_kind(lower, object);
        out->as.index.element_kind = lower_value_kind_from_sym(value_kind);
        *kind_out = value_kind;
    } else {
        ds_diag_error(lower->diag, expr->span, "indexing requires an array or map in v0.10.0");
    }
    return out;
}

static bool helper_returns_string_array(DsStr name) {
    return lower_str_eq(name, "string.split") || lower_str_eq(name, "glob") ||
           lower_str_eq(name, "glob!") || lower_str_eq(name, "lines");
}

SymKind infer_lower_expr_kind(Lower *lower, const DsLowerExpr *expr) {
    if (!expr) return SYM_UNKNOWN;
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT: {
            Symbol *sym = scope_find(lower->scope, expr->as.text);
            return sym ? sym->kind : SYM_UNKNOWN;
        }
        case DS_LOWER_EXPR_STRING: return SYM_STRING;
        case DS_LOWER_EXPR_INTERP: return SYM_STRING;
        case DS_LOWER_EXPR_INT: return SYM_INT;
        case DS_LOWER_EXPR_BOOL: return SYM_BOOL;
        case DS_LOWER_EXPR_REGEX: return SYM_UNKNOWN;
        case DS_LOWER_EXPR_RUN: return SYM_COMMAND_RESULT;
        case DS_LOWER_EXPR_UNARY:
            return lower_str_eq(expr->as.unary.op, "-") ? SYM_INT : SYM_BOOL;
        case DS_LOWER_EXPR_BINARY:
            if (lower_str_eq(expr->as.binary.op, "+") || lower_str_eq(expr->as.binary.op, "-") ||
                lower_str_eq(expr->as.binary.op, "*") || lower_str_eq(expr->as.binary.op, "/") ||
                lower_str_eq(expr->as.binary.op, "%") || lower_str_eq(expr->as.binary.op, "**")) return SYM_INT;
            if (lower_str_eq(expr->as.binary.op, "in") || lower_str_eq(expr->as.binary.op, "matches") ||
                lower_str_eq(expr->as.binary.op, "&&") || lower_str_eq(expr->as.binary.op, "||") ||
                lower_str_eq(expr->as.binary.op, "==") || lower_str_eq(expr->as.binary.op, "!=") ||
                lower_str_eq(expr->as.binary.op, ">") || lower_str_eq(expr->as.binary.op, ">=") ||
                lower_str_eq(expr->as.binary.op, "<") || lower_str_eq(expr->as.binary.op, "<=")) return SYM_BOOL;
            return SYM_UNKNOWN;
        case DS_LOWER_EXPR_CALL: {
            (void)lower;
            return sym_kind_from_lower_value_kind(expr->as.call.return_kind);
        }
        case DS_LOWER_EXPR_ARRAY: return SYM_ARRAY;
        case DS_LOWER_EXPR_MAP: return SYM_MAP;
        case DS_LOWER_EXPR_INDEX:
            if (expr->as.index.returns_row) return SYM_MAP;
            if (expr->as.index.element_kind != DS_LOWER_VALUE_UNKNOWN) {
                return sym_kind_from_lower_value_kind(expr->as.index.element_kind);
            }
            if (expr->as.index.object_is_array) return infer_array_element_kind(lower, expr->as.index.object);
            return SYM_UNKNOWN;
        case DS_LOWER_EXPR_FIELD:
            if (infer_lower_expr_kind(lower, expr->as.field.object) == SYM_COMMAND_RESULT) {
                SymKind field_kind = SYM_UNKNOWN;
                if (command_result_field_kind(expr->as.field.field, &field_kind)) return field_kind;
            }
            return SYM_UNKNOWN;
        case DS_LOWER_EXPR_ERROR: return SYM_UNKNOWN;
        case DS_LOWER_EXPR_RANGE: return SYM_UNKNOWN;
    }
    return SYM_UNKNOWN;
}

SymKind infer_array_element_kind(Lower *lower, const DsLowerExpr *expr) {
    if (!expr) return SYM_UNKNOWN;
    if (expr->kind == DS_LOWER_EXPR_IDENT) {
        Symbol *sym = scope_find(lower->scope, expr->as.text);
        return sym ? sym->element_kind : SYM_UNKNOWN;
    }
    if (expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.returns_row_array) return SYM_MAP;
    if (expr->kind == DS_LOWER_EXPR_CALL && helper_returns_string_array(expr->as.call.name)) return SYM_STRING;
    if (expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.is_user_function) return SYM_UNKNOWN;
    if (expr->kind != DS_LOWER_EXPR_ARRAY) return SYM_UNKNOWN;
    if (expr->as.array.is_row_array) return SYM_MAP;

    SymKind common = SYM_UNKNOWN;
    for (size_t i = 0; i < expr->as.array.elements.len; i++) {
        SymKind elem = infer_lower_expr_kind(lower, expr->as.array.elements.items[i]);
        if (elem == SYM_UNKNOWN) return SYM_UNKNOWN;
        if (common == SYM_UNKNOWN) common = elem;
        else if (common != elem) return SYM_UNKNOWN;
    }
    return common;
}

SymKind infer_map_value_kind(Lower *lower, const DsLowerExpr *expr) {
    if (!expr) return SYM_UNKNOWN;
    if (expr->kind == DS_LOWER_EXPR_IDENT) {
        Symbol *sym = scope_find(lower->scope, expr->as.text);
        return sym ? sym->element_kind : SYM_UNKNOWN;
    }
    if (expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.is_user_function) {
        DsLowerFn *fn = find_function(lower->program, expr->as.call.name);
        return fn ? sym_kind_from_lower_value_kind(fn->return_element_kind) : SYM_UNKNOWN;
    }
    if (expr->kind != DS_LOWER_EXPR_MAP) return SYM_UNKNOWN;

    SymKind common = SYM_UNKNOWN;
    for (size_t i = 0; i < expr->as.map.entries.len; i++) {
        SymKind value = infer_lower_expr_kind(lower, expr->as.map.entries.items[i].value);
        if (value == SYM_UNKNOWN) return SYM_UNKNOWN;
        if (common == SYM_UNKNOWN) common = value;
        else if (common != value) return SYM_UNKNOWN;
    }
    return common;
}

DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    *kind_out = SYM_UNKNOWN;
    if (!expr) return expr_new(DS_LOWER_EXPR_ERROR, (DsSpan){0});
    switch (expr->kind) {
        case DS_EXPR_IDENT:
            return lower_ident_expr(lower, expr, kind_out);
        case DS_EXPR_STRING:
            return lower_string_expr(lower, expr, kind_out);
        case DS_EXPR_INT:
            return lower_int_expr(lower, expr, kind_out);
        case DS_EXPR_BOOL:
            return lower_bool_expr(expr, kind_out);
        case DS_EXPR_REGEX:
            return lower_regex_expr(lower, expr, kind_out, false);
        case DS_EXPR_RUN:
            return lower_run_expr(lower, expr, kind_out);
        case DS_EXPR_FIELD:
            return lower_field_expr(lower, expr, kind_out);
        case DS_EXPR_UNARY:
            return lower_unary_expr(lower, expr, kind_out);
        case DS_EXPR_BINARY:
            return lower_binary_expr(lower, expr, kind_out);
        case DS_EXPR_CALL:
            return lower_call_expr(lower, expr, kind_out);
        case DS_EXPR_ARRAY:
            return lower_array_expr(lower, expr, kind_out);
        case DS_EXPR_MAP:
            return lower_map_expr(lower, expr, kind_out);
        case DS_EXPR_INDEX:
            return lower_index_expr(lower, expr, kind_out);
        case DS_EXPR_RANGE: {
            ds_diag_error(lower->diag, expr->span, "range syntax is only supported as a `for` loop source in v0.23.0");
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_RANGE, expr->span);
            SymKind tmp = SYM_UNKNOWN;
            out->as.range.start = lower_expr(lower, expr->as.range.start, &tmp);
            out->as.range.end = lower_expr(lower, expr->as.range.end, &tmp);
            *kind_out = SYM_UNKNOWN;
            return out;
        }
        case DS_EXPR_ERROR:
            return expr_new(DS_LOWER_EXPR_ERROR, expr->span);
    }
    return expr_new(DS_LOWER_EXPR_ERROR, expr->span);
}
