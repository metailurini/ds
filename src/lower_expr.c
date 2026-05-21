#include "lower_internal.h"

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

static bool parse_format_limit(DsStr spec, size_t start, size_t end, size_t *value) {
    if (start >= end) return false;
    size_t out = 0;
    for (size_t i = start; i < end; i++) {
        if (spec.data[i] < '0' || spec.data[i] > '9') return false;
        size_t digit = (size_t)(spec.data[i] - '0');
        if (out > 1024 / 10 || (out == 1024 / 10 && digit > 1024 % 10)) return false;
        out = out * 10 + digit;
    }
    if (out < 1 || out > 1024) return false;
    *value = out;
    return true;
}

static bool is_supported_format_spec(DsStr spec, SymKind kind) {
    if (spec.len == 0) return false;
    if ((spec.len == 5 && memcmp(spec.data, "upper", 5) == 0) ||
        (spec.len == 5 && memcmp(spec.data, "lower", 5) == 0) ||
        (spec.len == 4 && memcmp(spec.data, "trim", 4) == 0)) return kind == SYM_STRING;
    size_t i = 0;
    if (spec.data[0] == '<' || spec.data[0] == '>' || spec.data[0] == '^') {
        i = 1;
        if (i >= spec.len) return false;
        while (i < spec.len && spec.data[i] >= '0' && spec.data[i] <= '9') i++;
        size_t width = 0;
        return i == spec.len && parse_format_limit(spec, 1, i, &width) && kind == SYM_STRING;
    }
    bool zero = false;
    if (i < spec.len && spec.data[i] == '0') { zero = true; i++; }
    size_t digits_start = i;
    while (i < spec.len && spec.data[i] >= '0' && spec.data[i] <= '9') i++;
    if (i < spec.len && spec.data[i] == 'd') {
        size_t width = 0;
        return i + 1 == spec.len && parse_format_limit(spec, digits_start, i, &width) && kind == SYM_INT;
    }
    if (zero) return false;
    if (i < spec.len && spec.data[i] == '.') {
        i++;
        size_t prec_start = i;
        while (i < spec.len && spec.data[i] >= '0' && spec.data[i] <= '9') i++;
        size_t precision = 0;
        return i < spec.len && spec.data[i] == 'f' && i + 1 == spec.len && parse_format_limit(spec, prec_start, i, &precision) && kind == SYM_INT;
    }
    return false;
}

bool validate_interpolation(Lower *lower, DsStr text, DsSpan span) {
    DsStr decoded = {0};
    if (!lower_decode_string_text(text, &decoded)) return true;
    for (size_t i = 0; i < decoded.len; i++) {
        char c = decoded.data[i];
        if (c != '{') continue;
        size_t start = i + 1;
        size_t j = start;
        if (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || decoded.data[j] == '_')) {
            j++;
            while (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || (decoded.data[j] >= '0' && decoded.data[j] <= '9') || decoded.data[j] == '_')) j++;
            DsStr name = {decoded.data + start, j - start};
            Symbol *sym = scope_find(lower->scope, name);
            if (!sym) {
                ds_diag_error(lower->diag, span, "unknown interpolation variable `%.*s`", (int)name.len, name.data);
                free(decoded.data);
                return false;
            }
            SymKind value_kind = sym->kind;
            if (j < decoded.len && decoded.data[j] == '.') {
                size_t field_start = ++j;
                if (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || decoded.data[j] == '_')) {
                    j++;
                    while (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || (decoded.data[j] >= '0' && decoded.data[j] <= '9') || decoded.data[j] == '_')) j++;
                }
                DsStr field = {decoded.data + field_start, j - field_start};
                if (sym->kind != SYM_COMMAND_RESULT) {
                    ds_diag_error(lower->diag, span, "field interpolation is only supported on command results in v0.7.0");
                    free(decoded.data);
                    return false;
                }
                if (!command_result_field_kind(field, &value_kind)) {
                    ds_diag_error(lower->diag, span, "unknown command result field `%.*s`", (int)field.len, field.data);
                    free(decoded.data);
                    return false;
                }
            }
            if (j < decoded.len && decoded.data[j] == ':') {
                size_t spec_start = ++j;
                while (j < decoded.len && decoded.data[j] != '}') j++;
                if (j >= decoded.len) break;
                DsStr spec = {decoded.data + spec_start, j - spec_start};
                if (!is_supported_format_spec(spec, value_kind)) {
                    ds_diag_error(lower->diag, span, "unsupported interpolation format specifier `%.*s`; supported: upper, lower, trim, <N, >N, ^N, Nd, 0Nd, .Pf, N.Pf", (int)spec.len, spec.data);
                    free(decoded.data);
                    return false;
                }
            }
            if (j < decoded.len && decoded.data[j] == '}') { i = j; continue; }
        }
        ds_diag_error(lower->diag, span, "unsupported string interpolation; expected `{name}`, `{name.field}`, or a supported `:specifier`");
        free(decoded.data);
        return false;
    }
    free(decoded.data);
    return true;
}

bool collection_element_supported_in_bash(const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INT:
        case DS_LOWER_EXPR_BOOL:
        case DS_LOWER_EXPR_FIELD:
        case DS_LOWER_EXPR_INDEX:
            return true;
        default:
            return false;
    }
}

bool text_contains_recursive_glob(DsStr text) {
    if (text.len < 2) return false;
    for (size_t i = 0; i + 1 < text.len; i++) {
        if (text.data[i] == '*' && text.data[i + 1] == '*') return true;
    }
    return false;
}

void validate_glob_pattern_arg(Lower *lower, DsStr helper_name, const DsExpr *arg) {
    if (!(lower_str_eq(helper_name, "glob") || lower_str_eq(helper_name, "glob!"))) return;
    if (!arg || arg->kind != DS_EXPR_STRING) return;
    DsStr decoded = {0};
    if (lower_decode_string_text(arg->as.text, &decoded)) {
        if (text_contains_recursive_glob(decoded)) {
            ds_diag_error(lower->diag, arg->span,
                          "recursive `**` glob patterns are deferred in v0.11.0");
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

DsLowerExpr *lower_binary_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    SymKind left_kind = SYM_UNKNOWN;
    SymKind right_kind = SYM_UNKNOWN;
    DsLowerExpr *left = lower_expr(lower, expr->as.binary.left, &left_kind);
    DsLowerExpr *right = lower_expr(lower, expr->as.binary.right, &right_kind);
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_BINARY, expr->span);
    out->as.binary.left = left;
    out->as.binary.op = str_clone(expr->as.binary.op);
    out->as.binary.right = right;
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
        *kind_out = sym->kind;
    }
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_IDENT, expr->span);
    out->as.text = str_clone(expr->as.text);
    return out;
}

DsLowerExpr *lower_string_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    validate_interpolation(lower, expr->as.text, expr->span);
    *kind_out = SYM_STRING;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_STRING, expr->span);
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

DsLowerExpr *lower_run_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    if (expr->as.run.stages.len == 0) {
        ds_diag_error(lower->diag, expr->span, "expected command after `run`");
    }
    for (size_t s = 0; s < expr->as.run.stages.len; s++) {
        if (expr->as.run.stages.items[s].words.len == 0) ds_diag_error(lower->diag, expr->as.run.stages.items[s].span, "empty pipeline stage");
        for (size_t i = 0; i < expr->as.run.stages.items[s].words.len; i++) validate_cmd_word(lower, expr->as.run.stages.items[s].words.items[i].text, expr->as.run.stages.items[s].words.items[i].span);
    }
    if (expr->as.run.redirect.kind != DS_REDIRECT_NONE) ds_diag_error(lower->diag, expr->as.run.redirect.op_span, "captured `run` commands do not support redirection");
    *kind_out = SYM_COMMAND_RESULT;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_RUN, expr->span);
    ds_command_clone(&out->as.run, &expr->as.run);
    return out;
}

DsLowerExpr *lower_map_field_expr(const DsExpr *expr, DsLowerExpr *object, SymKind *kind_out) {
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
    *kind_out = SYM_UNKNOWN;
    return out;
}

DsLowerExpr *lower_field_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    if (expr->as.field.object && expr->as.field.object->kind == DS_EXPR_IDENT && lower_str_eq(expr->as.field.object->as.text, "env")) {
        ds_diag_error(lower->diag, expr->span, "direct `env.NAME` access is deferred in v0.11.0; use `env.get(\"NAME\")` instead");
        DsLowerExpr *out = expr_new(DS_LOWER_EXPR_FIELD, expr->span);
        out->as.field.object = expr_new(DS_LOWER_EXPR_ERROR, expr->as.field.object->span);
        out->as.field.field = str_clone(expr->as.field.field);
        *kind_out = SYM_UNKNOWN;
        return out;
    }
    SymKind object_kind = SYM_UNKNOWN;
    DsLowerExpr *object = lower_expr(lower, expr->as.field.object, &object_kind);
    SymKind field_kind = SYM_UNKNOWN;
    if (object_kind == SYM_COMMAND_RESULT) {
        if (!command_result_field_kind(expr->as.field.field, &field_kind)) {
            ds_diag_error(lower->diag, expr->span, "unknown command result field `%.*s`", (int)expr->as.field.field.len, expr->as.field.field.data);
        }
    } else if (object_kind == SYM_MAP) {
        return lower_map_field_expr(expr, object, kind_out);
    } else {
        ds_diag_error(lower->diag, expr->span, "field access is only supported on command results and maps in v0.10.0");
    }
    *kind_out = field_kind;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_FIELD, expr->span);
    out->as.field.object = object;
    out->as.field.field = str_clone(expr->as.field.field);
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
           lower_str_eq(name, "string.starts_with") || lower_str_eq(name, "string.ends_with");
}

static bool string_helper_arg_count_ok(DsStr name, size_t argc, size_t *expected) {
    if (lower_str_eq(name, "string.trim") || lower_str_eq(name, "string.upper") || lower_str_eq(name, "string.lower")) { *expected = 1; return argc == 1; }
    if (lower_str_eq(name, "string.replace")) { *expected = 3; return argc == 3; }
    *expected = 2;
    return argc == 2;
}

DsLowerExpr *lower_call_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_CALL, expr->span);
    out->as.call.name = str_clone(expr->as.call.name);
    for (size_t i = 0; i < expr->as.call.args.len; i++) {
        SymKind arg_kind = SYM_UNKNOWN;
        lower_expr_vec_push(&out->as.call.args, lower_expr(lower, expr->as.call.args.items[i], &arg_kind));
        if (is_string_helper_name(expr->as.call.name)) {
            if (i == 0 && arg_kind != SYM_STRING) {
                ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "string method `%.*s` requires a string receiver", (int)expr->as.call.name.len, expr->as.call.name.data);
            } else if (i > 0 && arg_kind != SYM_STRING) {
                ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "string method `%.*s` expects string arguments", (int)expr->as.call.name.len, expr->as.call.name.data);
            }
        } else if (ds_stdlib_is_name(expr->as.call.name) && arg_kind != SYM_STRING && arg_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "standard-library helper `%.*s` expects string arguments in v0.11.0", (int)expr->as.call.name.len, expr->as.call.name.data);
        } else if (arg_kind != SYM_STRING && arg_kind != SYM_INT && arg_kind != SYM_BOOL && arg_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, expr->as.call.args.items[i]->span, "standard-library arguments must be scalar values in v0.11.0");
        }
    }
    const DsStdlibHelper *stdlib_helper = ds_stdlib_lookup(expr->as.call.name);
    if (stdlib_helper) {
        SymKind ret = SYM_UNKNOWN;
        if (!stdlib_return_kind(stdlib_helper, &ret)) {
            ds_diag_error(lower->diag, expr->span, "unknown standard-library helper `%.*s`", (int)expr->as.call.name.len, expr->as.call.name.data);
        } else if (!ds_stdlib_arity_ok(stdlib_helper, expr->as.call.args.len)) {
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
        out->as.call.return_kind = lower_value_kind_from_sym(ret);
        return out;
    }
    DsStr ns = {0}, member = {0};
    if (split_member_name(expr->as.call.name, &ns, &member) && ds_stdlib_is_namespace(ns)) {
        ds_diag_error(lower->diag, expr->span, "unknown standard-library helper `%.*s`", (int)expr->as.call.name.len, expr->as.call.name.data);
        return out;
    }
    if (split_member_name(expr->as.call.name, &ns, &member) && ns.len == 6 && memcmp(ns.data, "string", 6) == 0) {
        ds_diag_error(lower->diag, expr->span, "unknown string method `%.*s`; supported methods are trim, upper, lower, replace, contains, split, starts_with, ends_with", (int)member.len, member.data);
        return out;
    }
    DsLowerFn *fn = find_function(lower->program, expr->as.call.name);
    if (fn) {
        if (expr->as.call.args.len < fn->required_count || expr->as.call.args.len > fn->params.len) {
            ds_diag_error(lower->diag, expr->span, "function `%.*s` called with wrong number of arguments", (int)fn->name.len, fn->name.data);
        }
        if (!fn->has_return) {
            ds_diag_error(lower->diag, expr->span, "function `%.*s` does not return a value", (int)fn->name.len, fn->name.data);
        } else if (!fn->all_paths_return) {
            ds_diag_error(lower->diag, expr->span, "function `%.*s` cannot be used as a value because not all control paths return in v0.21.0", (int)fn->name.len, fn->name.data);
        }
        switch (fn->return_kind) {
            case DS_LOWER_VALUE_BOOL: *kind_out = SYM_BOOL; break;
            case DS_LOWER_VALUE_INT: *kind_out = SYM_INT; break;
            case DS_LOWER_VALUE_STRING: *kind_out = SYM_STRING; break;
            default: *kind_out = SYM_UNKNOWN; break;
        }
        out->as.call.is_user_function = true;
        out->as.call.return_kind = fn->return_kind;
        return out;
    }
    ds_diag_error(lower->diag, expr->span, "unknown function `%.*s`", (int)expr->as.call.name.len, expr->as.call.name.data);
    return out;
}

DsLowerExpr *lower_array_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_ARRAY, expr->span);
    for (size_t i = 0; i < expr->as.array.elements.len; i++) {
        SymKind elem_kind = SYM_UNKNOWN;
        DsLowerExpr *element = lower_expr(lower, expr->as.array.elements.items[i], &elem_kind);
        lower_expr_vec_push(&out->as.array.elements, element);
        if (elem_kind == SYM_ARRAY || elem_kind == SYM_MAP) {
            ds_diag_error(lower->diag, expr->as.array.elements.items[i]->span, "nested collections are deferred in v0.10.0");
        } else if (!collection_element_supported_in_bash(element)) {
            ds_diag_error(lower->diag, expr->as.array.elements.items[i]->span, "collection element expressions must be scalar Bash-emittable values in v0.10.0; bind the expression to a variable first");
        }
    }
    *kind_out = SYM_ARRAY;
    return out;
}

DsLowerExpr *lower_map_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_MAP, expr->span);
    if (expr->as.map.entries.len == 0) {
        ds_diag_error(lower->diag, expr->span, "empty map literals are deferred in v0.10.0");
    }
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
        } else if (!collection_element_supported_in_bash(lowered.value)) {
            ds_diag_error(lower->diag, entry->value->span, "collection element expressions must be scalar Bash-emittable values in v0.10.0; bind the expression to a variable first");
        }
        lower_map_entry_vec_push(&out->as.map.entries, lowered);
    }
    *kind_out = SYM_MAP;
    return out;
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
        if (idx_kind != SYM_INT && idx_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, expr->as.index.index->span, "array index must be an int in v0.10.0");
        if (expr->as.index.index && expr->as.index.index->kind == DS_EXPR_UNARY && lower_str_eq(expr->as.index.index->as.unary.op, "-") &&
            expr->as.index.index->as.unary.right && expr->as.index.index->as.unary.right->kind == DS_EXPR_INT) {
            ds_diag_error(lower->diag, expr->as.index.index->span, "array index must be non-negative in v0.10.0");
        }
        SymKind element_kind = infer_array_element_kind(lower, object);
        out->as.index.element_kind = lower_value_kind_from_sym(element_kind);
        *kind_out = element_kind;
    } else if (obj_kind == SYM_MAP) {
        out->as.index.object_is_map = true;
        if (expr->as.index.index && expr->as.index.index->kind == DS_EXPR_STRING) {
            out->as.index.map_key_literal = true;
            lower_decode_string_text(expr->as.index.index->as.text, &out->as.index.map_key);
        } else if (idx_kind != SYM_STRING && idx_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, expr->as.index.index->span, "map index must be a string in v0.10.0");
        }
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
        case DS_LOWER_EXPR_INT: return SYM_INT;
        case DS_LOWER_EXPR_BOOL: return SYM_BOOL;
        case DS_LOWER_EXPR_RUN: return SYM_COMMAND_RESULT;
        case DS_LOWER_EXPR_UNARY:
            return lower_str_eq(expr->as.unary.op, "-") ? SYM_INT : SYM_BOOL;
        case DS_LOWER_EXPR_BINARY:
            if (lower_str_eq(expr->as.binary.op, "+") || lower_str_eq(expr->as.binary.op, "-") ||
                lower_str_eq(expr->as.binary.op, "*") || lower_str_eq(expr->as.binary.op, "/") ||
                lower_str_eq(expr->as.binary.op, "%") || lower_str_eq(expr->as.binary.op, "**")) return SYM_INT;
            if (lower_str_eq(expr->as.binary.op, "==") || lower_str_eq(expr->as.binary.op, "!=") ||
                lower_str_eq(expr->as.binary.op, ">") || lower_str_eq(expr->as.binary.op, ">=") ||
                lower_str_eq(expr->as.binary.op, "<") || lower_str_eq(expr->as.binary.op, "<=")) return SYM_BOOL;
            return SYM_UNKNOWN;
        case DS_LOWER_EXPR_CALL: {
            const DsStdlibHelper *helper = ds_stdlib_lookup(expr->as.call.name);
            SymKind ret = SYM_UNKNOWN;
            return stdlib_return_kind(helper, &ret) ? ret : SYM_UNKNOWN;
        }
        case DS_LOWER_EXPR_ARRAY: return SYM_ARRAY;
        case DS_LOWER_EXPR_MAP: return SYM_MAP;
        case DS_LOWER_EXPR_INDEX:
            if (expr->as.index.object_is_array) return infer_array_element_kind(lower, expr->as.index.object);
            return SYM_UNKNOWN;
        case DS_LOWER_EXPR_FIELD:
            if (infer_lower_expr_kind(lower, expr->as.field.object) == SYM_COMMAND_RESULT) {
                SymKind field_kind = SYM_UNKNOWN;
                if (command_result_field_kind(expr->as.field.field, &field_kind)) return field_kind;
            }
            return SYM_UNKNOWN;
        case DS_LOWER_EXPR_ERROR: return SYM_UNKNOWN;
    }
    return SYM_UNKNOWN;
}

SymKind infer_array_element_kind(Lower *lower, const DsLowerExpr *expr) {
    if (!expr) return SYM_UNKNOWN;
    if (expr->kind == DS_LOWER_EXPR_IDENT) {
        Symbol *sym = scope_find(lower->scope, expr->as.text);
        return sym ? sym->element_kind : SYM_UNKNOWN;
    }
    if (expr->kind == DS_LOWER_EXPR_CALL && helper_returns_string_array(expr->as.call.name)) return SYM_STRING;
    if (expr->kind != DS_LOWER_EXPR_ARRAY) return SYM_UNKNOWN;

    SymKind common = SYM_UNKNOWN;
    for (size_t i = 0; i < expr->as.array.elements.len; i++) {
        SymKind elem = infer_lower_expr_kind(lower, expr->as.array.elements.items[i]);
        if (elem == SYM_UNKNOWN) return SYM_UNKNOWN;
        if (common == SYM_UNKNOWN) common = elem;
        else if (common != elem) return SYM_UNKNOWN;
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
        case DS_EXPR_ERROR:
            return expr_new(DS_LOWER_EXPR_ERROR, expr->span);
    }
    return expr_new(DS_LOWER_EXPR_ERROR, expr->span);
}

bool validate_cmd_word(Lower *lower, DsStr word, DsSpan span) {
    if (word.len >= 2 && word.data[0] == '$') {
        size_t name_len = 0;
        while (1 + name_len < word.len && is_name_char(word.data[1 + name_len])) name_len++;
        DsStr name = {word.data + 1, name_len};
        Symbol *sym = scope_find(lower->scope, name);
        if (!sym) {
            if (find_function(lower->program, name)) {
                ds_diag_error(lower->diag, span, "function `%.*s` cannot be used as a variable in v0.9.0", (int)name.len, name.data);
            } else {
                ds_diag_error(lower->diag, span, "unknown command variable `%.*s`", (int)name.len, name.data);
            }
            return false;
        }
        if (sym->kind == SYM_FUNCTION) {
            ds_diag_error(lower->diag, span, "function `%.*s` cannot be used as a variable in v0.9.0", (int)name.len, name.data);
            return false;
        }
        if (name_len + 1 < word.len) {
            char suffix = word.data[name_len + 1];
            if ((sym->kind == SYM_ARRAY || sym->kind == SYM_MAP) && (suffix == '[' || suffix == '.')) {
                ds_diag_error(lower->diag, span, "collection access command arguments are deferred in v0.10.0; bind the indexed value to a variable first");
                return false;
            }
            if (suffix != '.' || sym->kind != SYM_COMMAND_RESULT) {
                ds_diag_error(lower->diag, span, "unsupported command variable suffix in v0.10.0");
                return false;
            }
        }
        if (sym->kind == SYM_ARRAY || sym->kind == SYM_MAP) {
            ds_diag_error(lower->diag, span, "collection `%.*s` cannot be passed directly as a command argument in v0.10.0; index it first", (int)name.len, name.data);
            return false;
        }
    }
    if (word.len >= 2 && word.data[0] == '"' && word.data[word.len - 1] == '"') return validate_interpolation(lower, word, span);
    for (size_t i = 1; i < word.len; i++) {
        if (word.data[i] == '.') {
            DsStr name = {word.data, i};
            DsStr field = {word.data + i + 1, word.len - i - 1};
            DsSpan field_span = span;
            field_span.start.offset = span.start.offset + (int)i + 1;
            field_span.start.column = span.start.column + (int)i + 1;
            field_span.end.offset = field_span.start.offset + (int)field.len;
            field_span.end.column = field_span.start.column + (int)field.len;
            if (field.len == 0) {
                ds_diag_error(lower->diag, field_span, "expected field name after `.`");
                return false;
            }
            SymKind field_kind = SYM_UNKNOWN;
            Symbol *sym = scope_find(lower->scope, name);
            if (!sym) {
                DsSpan name_span = span;
                name_span.end.offset = name_span.start.offset + (int)name.len;
                name_span.end.column = name_span.start.column + (int)name.len;
                ds_diag_error(lower->diag, name_span, "unknown command variable `%.*s`", (int)name.len, name.data);
                return false;
            }
            if (sym->kind != SYM_COMMAND_RESULT) {
                if (sym->kind == SYM_MAP) {
                    ds_diag_error(lower->diag, field_span, "map field command arguments are deferred in v0.10.0; bind the field to a variable first");
                    return false;
                }
                ds_diag_error(lower->diag, field_span, "field access is only supported on command results and maps in v0.10.0");
                return false;
            }
            if (!command_result_field_kind(field, &field_kind)) {
                ds_diag_error(lower->diag, field_span, "unsupported command result field `%.*s`", (int)field.len, field.data);
                return false;
            }
        }
    }
    return true;
}
