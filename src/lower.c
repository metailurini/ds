#include "ds.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum { SYM_BOOL, SYM_INT, SYM_STRING, SYM_COMMAND_RESULT, SYM_ARRAY, SYM_MAP, SYM_FUNCTION, SYM_TOPLEVEL_PREDECLARED, SYM_UNKNOWN } SymKind;

typedef struct {
    char *name;
    SymKind kind;
} Symbol;

typedef struct Scope Scope;
struct Scope {
    Scope *parent;
    Symbol *items;
    size_t len;
    size_t cap;
};

typedef struct {
    DsDiag *diag;
    Scope *scope;
    DsLowerProgram *program;
} Lower;

static bool is_result_field(DsStr field, SymKind *kind_out);
static bool decode_string_text(DsStr text, DsStr *out);

static bool str_eq(DsStr a, const char *b) {
    size_t len = strlen(b);
    return a.len == len && memcmp(a.data, b, len) == 0;
}

static bool name_eq(DsStr a, const char *b) {
    size_t len = strlen(b);
    return a.len == len && memcmp(a.data, b, len) == 0;
}

static bool is_env_name_text(DsStr name) {
    if (name.len == 0) return false;
    char c = name.data[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
    for (size_t i = 1; i < name.len; i++) {
        c = name.data[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

static bool split_member_name(DsStr name, DsStr *ns, DsStr *member) {
    for (size_t i = 0; i < name.len; i++) {
        if (name.data[i] == '.') {
            ns->data = name.data;
            ns->len = i;
            member->data = name.data + i + 1;
            member->len = name.len - i - 1;
            return true;
        }
    }
    return false;
}

static bool stdlib_return_kind(const DsStdlibHelper *helper, SymKind *kind) {
    if (!helper) return false;
    switch (helper->return_kind) {
        case DS_STDLIB_RETURN_BOOL: *kind = SYM_BOOL; return true;
        case DS_STDLIB_RETURN_INT: *kind = SYM_INT; return true;
        case DS_STDLIB_RETURN_STRING: *kind = SYM_STRING; return true;
        case DS_STDLIB_RETURN_ARRAY: *kind = SYM_ARRAY; return true;
        case DS_STDLIB_RETURN_MAP: *kind = SYM_MAP; return true;
        case DS_STDLIB_RETURN_COMMAND_RESULT: *kind = SYM_COMMAND_RESULT; return true;
        case DS_STDLIB_RETURN_STATEMENT_ONLY: *kind = SYM_UNKNOWN; return true;
    }
    return false;
}

static DsStr str_clone(DsStr s) {
    DsStr out = {ds_str_dup_range(s.data, s.len), s.len};
    return out;
}

static void scope_init(Scope *scope, Scope *parent) {
    scope->parent = parent;
    scope->items = NULL;
    scope->len = 0;
    scope->cap = 0;
}

static void scope_free(Scope *scope) {
    for (size_t i = 0; i < scope->len; i++) free(scope->items[i].name);
    free(scope->items);
}

static Symbol *scope_find_current(Scope *scope, DsStr name) {
    for (size_t i = 0; i < scope->len; i++) {
        if (name_eq(name, scope->items[i].name)) return &scope->items[i];
    }
    return NULL;
}

static Symbol *scope_find(Scope *scope, DsStr name) {
    for (Scope *s = scope; s; s = s->parent) {
        Symbol *sym = scope_find_current(s, name);
        if (sym) return sym;
    }
    return NULL;
}

static void scope_define(Lower *lower, Scope *scope, DsStr name, SymKind kind, DsSpan span) {
    Symbol *current = scope_find_current(scope, name);
    if (current && current->kind == SYM_TOPLEVEL_PREDECLARED) {
        current->kind = kind;
        return;
    }
    if (scope_find(scope, name)) {
        ds_diag_error(lower->diag, span, "duplicate variable `%.*s` in this scope", (int)name.len, name.data);
        return;
    }
    if (scope->len == scope->cap) {
        scope->cap = scope->cap ? scope->cap * 2 : 16;
        scope->items = (Symbol *)ds_xrealloc(scope->items, scope->cap * sizeof(Symbol));
    }
    scope->items[scope->len].name = ds_str_dup_range(name.data, name.len);
    scope->items[scope->len].kind = kind;
    scope->len++;
}

static bool validate_interpolation(Lower *lower, DsStr text, DsSpan span) {
    if (text.len < 2 || text.data[0] != '"' || text.data[text.len - 1] != '"') return true;
    for (size_t i = 1; i + 1 < text.len; i++) {
        char c = text.data[i];
        if (c == '\\') {
            if (i + 1 < text.len - 1) i++;
            continue;
        }
        if (c != '{') continue;
        size_t start = i + 1;
        size_t j = start;
        if (j < text.len - 1 && ((text.data[j] >= 'A' && text.data[j] <= 'Z') || (text.data[j] >= 'a' && text.data[j] <= 'z') || text.data[j] == '_')) {
            j++;
            while (j < text.len - 1 && ((text.data[j] >= 'A' && text.data[j] <= 'Z') || (text.data[j] >= 'a' && text.data[j] <= 'z') || (text.data[j] >= '0' && text.data[j] <= '9') || text.data[j] == '_')) j++;
            if (j < text.len - 1 && (text.data[j] == '}' || text.data[j] == '.')) {
                DsStr name = {text.data + start, j - start};
                Symbol *sym = scope_find(lower->scope, name);
                if (!sym) {
                    ds_diag_error(lower->diag, span, "unknown interpolation variable `%.*s`", (int)name.len, name.data);
                    return false;
                }
                if (text.data[j] == '.') {
                    size_t field_start = ++j;
                    if (j < text.len - 1 && ((text.data[j] >= 'A' && text.data[j] <= 'Z') || (text.data[j] >= 'a' && text.data[j] <= 'z') || text.data[j] == '_')) {
                        j++;
                        while (j < text.len - 1 && ((text.data[j] >= 'A' && text.data[j] <= 'Z') || (text.data[j] >= 'a' && text.data[j] <= 'z') || (text.data[j] >= '0' && text.data[j] <= '9') || text.data[j] == '_')) j++;
                    }
                    if (j >= text.len - 1 || text.data[j] != '}') {
                        ds_diag_error(lower->diag, span, "unsupported string interpolation; expected `{name}` or `{name.field}`");
                        return false;
                    }
                    DsStr field = {text.data + field_start, j - field_start};
                    SymKind field_kind = SYM_UNKNOWN;
                    if (sym->kind != SYM_COMMAND_RESULT) {
                        ds_diag_error(lower->diag, span, "field interpolation is only supported on command results in v0.7.0");
                        return false;
                    }
                    if (!is_result_field(field, &field_kind)) {
                        ds_diag_error(lower->diag, span, "unknown command result field `%.*s`", (int)field.len, field.data);
                        return false;
                    }
                }
                i = j;
                continue;
            }
        }
        ds_diag_error(lower->diag, span, "unsupported string interpolation; expected `{name}` or `{name.field}`");
        return false;
    }
    return true;
}

static DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);

static bool collection_element_supported_in_bash(const DsLowerExpr *expr) {
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

static bool text_contains_recursive_glob(DsStr text) {
    if (text.len < 2) return false;
    for (size_t i = 0; i + 1 < text.len; i++) {
        if (text.data[i] == '*' && text.data[i + 1] == '*') return true;
    }
    return false;
}

static void validate_glob_pattern_arg(Lower *lower, DsStr helper_name, const DsExpr *arg) {
    if (!(str_eq(helper_name, "glob") || str_eq(helper_name, "glob!"))) return;
    if (!arg || arg->kind != DS_EXPR_STRING) return;
    DsStr decoded = {0};
    if (decode_string_text(arg->as.text, &decoded)) {
        if (text_contains_recursive_glob(decoded)) {
            ds_diag_error(lower->diag, arg->span,
                          "recursive `**` glob patterns are deferred in v0.11.0");
        }
        free(decoded.data);
    }
}

static bool is_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
static bool validate_cmd_word(Lower *lower, DsStr word, DsSpan span);
static void lower_expr_vec_push(DsLowerExprVec *vec, DsLowerExpr *expr);

static void lower_map_entry_vec_push(DsLowerMapEntryVec *vec, DsLowerMapEntry entry) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerMapEntry *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerMapEntry));
    }
    vec->items[vec->len++] = entry;
}

static DsStr map_key_decode(const DsMapEntry *entry) {
    DsStr out = {0};
    if (entry->quoted_key) decode_string_text(entry->key, &out);
    else out = str_clone(entry->key);
    return out;
}

static bool map_has_duplicate_key(const DsLowerMapEntryVec *entries, DsStr key) {
    for (size_t i = 0; i < entries->len; i++) {
        if (entries->items[i].key.len == key.len && memcmp(entries->items[i].key.data, key.data, key.len) == 0) return true;
    }
    return false;
}

static DsLowerFn *find_function(DsLowerProgram *program, DsStr name) {
    for (size_t i = 0; i < program->functions.len; i++) {
        DsLowerFn *fn = &program->functions.items[i];
        if (fn->name.len == name.len && memcmp(fn->name.data, name.data, name.len) == 0) return fn;
    }
    return NULL;
}

static int find_function_index(DsLowerProgram *program, DsStr name) {
    for (size_t i = 0; i < program->functions.len; i++) {
        DsLowerFn *fn = &program->functions.items[i];
        if (fn->name.len == name.len && memcmp(fn->name.data, name.data, name.len) == 0) return (int)i;
    }
    return -1;
}

static DsLowerExpr *expr_new(DsLowerExprKind kind, DsSpan span) {
    DsLowerExpr *expr = (DsLowerExpr *)ds_xcalloc(1, sizeof(DsLowerExpr));
    expr->kind = kind;
    expr->span = span;
    return expr;
}

static bool is_result_field(DsStr field, SymKind *kind_out) {
    const DsCommandResultField *desc = ds_command_result_field_lookup(field);
    if (!desc) return false;
    switch (desc->kind) {
        case DS_COMMAND_RESULT_FIELD_STRING: *kind_out = SYM_STRING; return true;
        case DS_COMMAND_RESULT_FIELD_INT: *kind_out = SYM_INT; return true;
        case DS_COMMAND_RESULT_FIELD_BOOL: *kind_out = SYM_BOOL; return true;
    }
    return false;
}

static DsLowerExpr *lower_binary_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    SymKind left_kind = SYM_UNKNOWN;
    SymKind right_kind = SYM_UNKNOWN;
    DsLowerExpr *left = lower_expr(lower, expr->as.binary.left, &left_kind);
    DsLowerExpr *right = lower_expr(lower, expr->as.binary.right, &right_kind);
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_BINARY, expr->span);
    out->as.binary.left = left;
    out->as.binary.op = str_clone(expr->as.binary.op);
    out->as.binary.right = right;
    if (str_eq(expr->as.binary.op, "==") || str_eq(expr->as.binary.op, "!=") ||
        str_eq(expr->as.binary.op, ">") || str_eq(expr->as.binary.op, ">=") ||
        str_eq(expr->as.binary.op, "<") || str_eq(expr->as.binary.op, "<=")) {
        *kind_out = SYM_BOOL;
        return out;
    }
    ds_diag_error(lower->diag, expr->span,
                  "this expression cannot be emitted as a Bash assignment in v0.2.0; unsupported operator `%.*s` in v0.3.0",
                  (int)expr->as.binary.op.len, expr->as.binary.op.data);
    *kind_out = SYM_UNKNOWN;
    return out;
}

static DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    *kind_out = SYM_UNKNOWN;
    if (!expr) return expr_new(DS_LOWER_EXPR_ERROR, (DsSpan){0});
    switch (expr->kind) {
        case DS_EXPR_IDENT: {
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
        case DS_EXPR_STRING: {
            validate_interpolation(lower, expr->as.text, expr->span);
            *kind_out = SYM_STRING;
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_STRING, expr->span);
            out->as.text = str_clone(expr->as.text);
            return out;
        }
        case DS_EXPR_INT: {
            *kind_out = SYM_INT;
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INT, expr->span);
            out->as.text = str_clone(expr->as.text);
            return out;
        }
        case DS_EXPR_BOOL: {
            *kind_out = SYM_BOOL;
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_BOOL, expr->span);
            out->as.boolean = expr->as.boolean;
            return out;
        }
        case DS_EXPR_RUN: {
            if (expr->as.run.words.len == 0) {
                ds_diag_error(lower->diag, expr->span, "expected command after `run`");
            }
            for (size_t i = 0; i < expr->as.run.words.len; i++) validate_cmd_word(lower, expr->as.run.words.items[i].text, expr->as.run.words.items[i].span);
            *kind_out = SYM_COMMAND_RESULT;
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_RUN, expr->span);
            DsCommand command;
            ds_command_init(&command, DS_COMMAND_CAPTURE, expr->span);
            ds_word_vec_clone(&command.words, &expr->as.run.words);
            ds_command_clone(&out->as.run, &command);
            ds_command_free(&command);
            return out;
        }
        case DS_EXPR_FIELD: {
            if (expr->as.field.object && expr->as.field.object->kind == DS_EXPR_IDENT && str_eq(expr->as.field.object->as.text, "env")) {
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
                if (!is_result_field(expr->as.field.field, &field_kind)) {
                    ds_diag_error(lower->diag, expr->span, "unknown command result field `%.*s`", (int)expr->as.field.field.len, expr->as.field.field.data);
                }
            } else if (object_kind == SYM_MAP) {
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
            } else {
                ds_diag_error(lower->diag, expr->span, "field access is only supported on command results and maps in v0.10.0");
            }
            *kind_out = field_kind;
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_FIELD, expr->span);
            out->as.field.object = object;
            out->as.field.field = str_clone(expr->as.field.field);
            return out;
        }
        case DS_EXPR_UNARY: {
            SymKind right_kind = SYM_UNKNOWN;
            DsLowerExpr *right = lower_expr(lower, expr->as.unary.right, &right_kind);
            if (!str_eq(expr->as.unary.op, "!")) {
                ds_diag_error(lower->diag, expr->span, "unsupported unary operator `%.*s` in v0.3.0", (int)expr->as.unary.op.len, expr->as.unary.op.data);
                *kind_out = SYM_UNKNOWN;
            } else {
                *kind_out = SYM_BOOL;
            }
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_UNARY, expr->span);
            out->as.unary.op = str_clone(expr->as.unary.op);
            out->as.unary.right = right;
            return out;
        }
        case DS_EXPR_BINARY:
            return lower_binary_expr(lower, expr, kind_out);
        case DS_EXPR_CALL: {
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_CALL, expr->span);
            out->as.call.name = str_clone(expr->as.call.name);
            for (size_t i = 0; i < expr->as.call.args.len; i++) {
                SymKind arg_kind = SYM_UNKNOWN;
                lower_expr_vec_push(&out->as.call.args, lower_expr(lower, expr->as.call.args.items[i], &arg_kind));
                if (ds_stdlib_is_name(expr->as.call.name) && arg_kind != SYM_STRING && arg_kind != SYM_UNKNOWN) {
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
                    if (stdlib_helper->min_arity == stdlib_helper->max_arity) ds_diag_error(lower->diag, expr->span, "helper `%.*s` expects %zu arguments but got %zu", (int)expr->as.call.name.len, expr->as.call.name.data, stdlib_helper->min_arity, expr->as.call.args.len);
                    else ds_diag_error(lower->diag, expr->span, "helper `%.*s` expects %zu to %zu arguments but got %zu", (int)expr->as.call.name.len, expr->as.call.name.data, stdlib_helper->min_arity, stdlib_helper->max_arity, expr->as.call.args.len);
                } else if (stdlib_helper->statement_only) {
                    ds_diag_error(lower->diag, expr->span, "helper `%.*s` is statement-only in v0.11.0", (int)expr->as.call.name.len, expr->as.call.name.data);
                }
                if (stdlib_helper->validates_env_name && expr->as.call.args.len > 0 && expr->as.call.args.items[0]->kind == DS_EXPR_STRING) {
                    DsStr decoded = {0};
                    if (decode_string_text(expr->as.call.args.items[0]->as.text, &decoded)) {
                        if (!is_env_name_text(decoded)) ds_diag_error(lower->diag, expr->as.call.args.items[0]->span, "invalid environment variable name `%.*s` in v0.11.0", (int)decoded.len, decoded.data);
                        free(decoded.data);
                    }
                }
                if (expr->as.call.args.len > 0) validate_glob_pattern_arg(lower, expr->as.call.name, expr->as.call.args.items[0]);
                *kind_out = ret;
                return out;
            }
            DsStr ns = {0}, member = {0};
            if (split_member_name(expr->as.call.name, &ns, &member) && ds_stdlib_is_namespace(ns)) {
                ds_diag_error(lower->diag, expr->span, "unknown standard-library helper `%.*s`", (int)expr->as.call.name.len, expr->as.call.name.data);
                return out;
            }
            ds_diag_error(lower->diag, expr->span, "function calls do not produce values in v0.9.0");
            return out;
        }
        case DS_EXPR_ARRAY: {
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
        case DS_EXPR_MAP: {
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
        case DS_EXPR_INDEX: {
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
            } else if (obj_kind == SYM_MAP) {
                out->as.index.object_is_map = true;
                if (expr->as.index.index && expr->as.index.index->kind == DS_EXPR_STRING) {
                    out->as.index.map_key_literal = true;
                    decode_string_text(expr->as.index.index->as.text, &out->as.index.map_key);
                } else if (idx_kind != SYM_STRING && idx_kind != SYM_UNKNOWN) {
                    ds_diag_error(lower->diag, expr->as.index.index->span, "map index must be a string in v0.10.0");
                }
            } else {
                ds_diag_error(lower->diag, expr->span, "indexing requires an array or map in v0.10.0");
            }
            *kind_out = SYM_UNKNOWN;
            return out;
        }
        case DS_EXPR_ERROR:
            return expr_new(DS_LOWER_EXPR_ERROR, expr->span);
    }
    return expr_new(DS_LOWER_EXPR_ERROR, expr->span);
}

static bool validate_cmd_word(Lower *lower, DsStr word, DsSpan span) {
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
            if (!is_result_field(field, &field_kind)) {
                ds_diag_error(lower->diag, field_span, "unsupported command result field `%.*s`", (int)field.len, field.data);
                return false;
            }
        }
    }
    return true;
}

static void lower_stmt_vec_push(DsLowerStmtVec *vec, DsLowerStmt *stmt) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 16;
        vec->items = (DsLowerStmt **)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerStmt *));
    }
    vec->items[vec->len++] = stmt;
}

static void lower_expr_vec_push(DsLowerExprVec *vec, DsLowerExpr *expr) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerExpr **)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerExpr *));
    }
    vec->items[vec->len++] = expr;
}

static void lower_fn_param_vec_push(DsLowerFnParamVec *vec, DsLowerFnParam param) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerFnParam *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerFnParam));
    }
    vec->items[vec->len++] = param;
}

static void lower_fn_vec_push(DsLowerFnVec *vec, DsLowerFn fn) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerFn *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerFn));
    }
    vec->items[vec->len++] = fn;
}

static void lower_test_vec_push(DsLowerTestVec *vec, DsLowerTest test) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerTest *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerTest));
    }
    vec->items[vec->len++] = test;
}

static void lower_decl_vec_push(DsLowerScriptDeclVec *vec, DsLowerScriptDecl decl) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerScriptDecl *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerScriptDecl));
    }
    vec->items[vec->len++] = decl;
}

static bool parse_i64(DsStr text, int64_t *out) {
    char *tmp = ds_str_dup_range(text.data, text.len);
    char *end = NULL;
    errno = 0;
    long long value = strtoll(tmp, &end, 10);
    bool ok = errno != ERANGE && end && *end == '\0';
    if (ok) *out = (int64_t)value;
    free(tmp);
    return ok;
}

static bool decode_string_text(DsStr text, DsStr *out) {
    out->data = NULL;
    out->len = 0;
    if (text.len < 2 || text.data[0] != '"' || text.data[text.len - 1] != '"') return false;
    char *buf = (char *)ds_xcalloc(text.len, 1);
    size_t len = 0;
    for (size_t i = 1; i + 1 < text.len; i++) {
        char c = text.data[i];
        if (c == '\\' && i + 1 < text.len - 1) {
            char escaped = text.data[++i];
            if (escaped == 'n') c = '\n';
            else if (escaped == 't') c = '\t';
            else if (escaped == '"') c = '"';
            else if (escaped == '\\') c = '\\';
            else c = escaped;
        }
        buf[len++] = c;
    }
    out->data = buf;
    out->len = len;
    return true;
}

static SymKind script_type_to_sym(DsScriptType type) {
    switch (type) {
        case DS_SCRIPT_TYPE_STRING: return SYM_STRING;
        case DS_SCRIPT_TYPE_INT: return SYM_INT;
        case DS_SCRIPT_TYPE_BOOL: return SYM_BOOL;
    }
    return SYM_UNKNOWN;
}

static const char *script_type_name(DsScriptType type) {
    switch (type) {
        case DS_SCRIPT_TYPE_STRING: return "string";
        case DS_SCRIPT_TYPE_INT: return "int";
        case DS_SCRIPT_TYPE_BOOL: return "bool";
    }
    return "unknown";
}

static bool lower_script_decl(Lower *lower, const DsScriptDecl *decl, DsLowerProgram *program) {
    DsLowerScriptDecl out;
    memset(&out, 0, sizeof(out));
    out.kind = decl->kind;
    out.type = decl->type;
    out.name = str_clone(decl->name);
    out.span = decl->span;

    if (decl->kind == DS_SCRIPT_DECL_ARG && decl->type == DS_SCRIPT_TYPE_BOOL) {
        ds_diag_error(lower->diag, decl->span, "bool positional args are not supported in v0.5.0");
    }
    if (decl->kind == DS_SCRIPT_DECL_FLAG && decl->type != DS_SCRIPT_TYPE_BOOL) {
        ds_diag_error(lower->diag, decl->span, "flag `%.*s` must have type `bool`", (int)decl->name.len, decl->name.data);
    }

    if (decl->default_value) {
        out.has_default = true;
        switch (decl->type) {
            case DS_SCRIPT_TYPE_STRING:
                if (decl->default_value->kind != DS_EXPR_STRING) {
                    ds_diag_error(lower->diag, decl->default_value->span, "default for `%.*s` must be a string", (int)decl->name.len, decl->name.data);
                } else {
                    decode_string_text(decl->default_value->as.text, &out.default_text);
                }
                break;
            case DS_SCRIPT_TYPE_INT:
                if (decl->default_value->kind != DS_EXPR_INT || !parse_i64(decl->default_value->as.text, &out.default_int)) {
                    ds_diag_error(lower->diag, decl->default_value->span, "default for `%.*s` must be an int", (int)decl->name.len, decl->name.data);
                }
                break;
            case DS_SCRIPT_TYPE_BOOL:
                if (decl->default_value->kind != DS_EXPR_BOOL) {
                    ds_diag_error(lower->diag, decl->default_value->span, "default for `%.*s` must be a bool", (int)decl->name.len, decl->name.data);
                } else {
                    out.default_bool = decl->default_value->as.boolean;
                    if (decl->kind == DS_SCRIPT_DECL_FLAG && out.default_bool) {
                        ds_diag_error(lower->diag, decl->default_value->span, "flag `%.*s` default `true` is deferred until `--no-name` support exists", (int)decl->name.len, decl->name.data);
                    }
                }
                break;
        }
    } else if (decl->kind != DS_SCRIPT_DECL_ARG) {
        ds_diag_error(lower->diag, decl->span, "%s `%.*s` requires a default value", decl->kind == DS_SCRIPT_DECL_OPTION ? "option" : "flag", (int)decl->name.len, decl->name.data);
    }

    scope_define(lower, lower->scope, decl->name, script_type_to_sym(decl->type), decl->span);
    lower_decl_vec_push(&program->script_decls, out);
    (void)script_type_name;
    return !lower->diag->has_error;
}

static DsLowerStmt *stmt_new(DsLowerStmtKind kind, DsSpan span) {
    DsLowerStmt *stmt = (DsLowerStmt *)ds_xcalloc(1, sizeof(DsLowerStmt));
    stmt->kind = kind;
    stmt->span = span;
    return stmt;
}

static DsLowerStmt *lower_stmt(Lower *lower, const DsStmt *stmt);

static bool expr_is_literal_default(const DsExpr *expr) {
    return expr && (expr->kind == DS_EXPR_STRING || expr->kind == DS_EXPR_INT || expr->kind == DS_EXPR_BOOL);
}

static DsLowerStmt *lower_call_stmt(Lower *lower, const DsStmt *stmt) {
    DsLowerStmt *out = stmt_new(DS_LOWER_STMT_CALL, stmt->span);
    out->as.call_stmt.name = str_clone(stmt->as.call_stmt.name);
    const DsStdlibHelper *stdlib_helper = ds_stdlib_lookup(stmt->as.call_stmt.name);
    bool stdlib = stdlib_helper != NULL;
    DsLowerFn *fn = stdlib ? NULL : find_function(lower->program, stmt->as.call_stmt.name);
    if (stdlib_helper) {
        SymKind ret = SYM_UNKNOWN;
        if (!stdlib_return_kind(stdlib_helper, &ret)) {
            ds_diag_error(lower->diag, stmt->span, "unknown standard-library helper `%.*s`", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
        } else if (!ds_stdlib_arity_ok(stdlib_helper, stmt->as.call_stmt.args.len)) {
            if (stdlib_helper->min_arity == stdlib_helper->max_arity) ds_diag_error(lower->diag, stmt->span, "helper `%.*s` expects %zu arguments but got %zu", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data, stdlib_helper->min_arity, stmt->as.call_stmt.args.len);
            else ds_diag_error(lower->diag, stmt->span, "helper `%.*s` expects %zu to %zu arguments but got %zu", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data, stdlib_helper->min_arity, stdlib_helper->max_arity, stmt->as.call_stmt.args.len);
        } else if (!stdlib_helper->statement_only) {
            ds_diag_error(lower->diag, stmt->span, "helper `%.*s` returns a value in v0.11.0; assign it with `let` or use it in an expression", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
        }
    } else if (!fn) {
        DsStr ns = {0}, member = {0};
        if (split_member_name(stmt->as.call_stmt.name, &ns, &member) && ds_stdlib_is_namespace(ns)) ds_diag_error(lower->diag, stmt->span, "unknown standard-library helper `%.*s`", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
        else if (split_member_name(stmt->as.call_stmt.name, &ns, &member)) ds_diag_error(lower->diag, stmt->span, "only `push` collection method is supported in v0.10.0");
        else ds_diag_error(lower->diag, stmt->span, "unknown function `%.*s`", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
    } else if (stmt->as.call_stmt.args.len < fn->required_count || stmt->as.call_stmt.args.len > fn->params.len) {
        if (fn->required_count == fn->params.len) {
            ds_diag_error(lower->diag, stmt->span, "function `%.*s` expects %zu arguments but got %zu",
                          (int)fn->name.len, fn->name.data, fn->params.len, stmt->as.call_stmt.args.len);
        } else {
            ds_diag_error(lower->diag, stmt->span, "function `%.*s` expects %zu to %zu arguments but got %zu",
                          (int)fn->name.len, fn->name.data, fn->required_count, fn->params.len, stmt->as.call_stmt.args.len);
        }
    }
    for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) {
        SymKind arg_kind = SYM_UNKNOWN;
        lower_expr_vec_push(&out->as.call_stmt.args, lower_expr(lower, stmt->as.call_stmt.args.items[i], &arg_kind));
        if (stdlib && arg_kind != SYM_STRING && arg_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, stmt->as.call_stmt.args.items[i]->span, "standard-library helper `%.*s` expects string arguments in v0.11.0", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
        } else if (arg_kind == SYM_ARRAY || arg_kind == SYM_MAP) {
            ds_diag_error(lower->diag, stmt->as.call_stmt.args.items[i]->span,
                          "passing collection values to functions is deferred in v0.10.0; index or bind scalar values instead");
        }
    }
    if (stdlib_helper && stdlib_helper->validates_env_name && stmt->as.call_stmt.args.len > 0 && stmt->as.call_stmt.args.items[0]->kind == DS_EXPR_STRING) {
        DsStr decoded = {0};
        if (decode_string_text(stmt->as.call_stmt.args.items[0]->as.text, &decoded)) {
            if (!is_env_name_text(decoded)) ds_diag_error(lower->diag, stmt->as.call_stmt.args.items[0]->span, "invalid environment variable name `%.*s` in v0.11.0", (int)decoded.len, decoded.data);
            free(decoded.data);
        }
    }
    if (stdlib && stmt->as.call_stmt.args.len > 0) validate_glob_pattern_arg(lower, stmt->as.call_stmt.name, stmt->as.call_stmt.args.items[0]);
    return out;
}

static DsLowerStmt *lower_block(Lower *lower, const DsStmt *block, bool child_scope) {
    DsLowerStmt *out = stmt_new(DS_LOWER_STMT_BLOCK, block->span);
    Scope *saved = lower->scope;
    Scope *local = NULL;
    if (child_scope) {
        local = (Scope *)ds_xcalloc(1, sizeof(Scope));
        scope_init(local, saved);
        lower->scope = local;
    }
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) {
        lower_stmt_vec_push(&out->as.block_stmt.statements, lower_stmt(lower, block->as.block_stmt.statements.items[i]));
    }
    if (child_scope) {
        lower->scope = saved;
        scope_free(local);
        free(local);
    }
    return out;
}

static void collect_function_signature(Lower *lower, const DsStmt *stmt, DsLowerProgram *program) {
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

static void collect_top_level_let_signature(Lower *lower, const DsStmt *stmt) {
    if (stmt->kind != DS_STMT_LET) return;
    if (scope_find_current(lower->scope, stmt->as.let_stmt.name)) {
        return;
    }
    scope_define(lower, lower->scope, stmt->as.let_stmt.name, SYM_TOPLEVEL_PREDECLARED, stmt->span);
}

static bool function_body_reaches(Lower *lower, size_t current_index, size_t target_index, bool *seen, DsSpan *cycle_span);

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
            if (stmt_reaches_function(lower, stmt->as.if_stmt.then_branch, target_index, seen, cycle_span)) return true;
            return stmt_reaches_function(lower, stmt->as.if_stmt.else_branch, target_index, seen, cycle_span);
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) {
                if (stmt_reaches_function(lower, stmt->as.block_stmt.statements.items[i], target_index, seen, cycle_span)) return true;
            }
            return false;
        case DS_LOWER_STMT_FOR_ARRAY:
            return stmt_reaches_function(lower, stmt->as.for_stmt.body, target_index, seen, cycle_span);
        case DS_LOWER_STMT_LET:
        case DS_LOWER_STMT_CMD:
        case DS_LOWER_STMT_PUSH:
            return false;
        case DS_LOWER_STMT_ASSERT:
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

static void reject_recursive_functions(Lower *lower) {
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

static void lower_function_body(Lower *lower, DsLowerFn *fn, const DsStmt *stmt) {
    Scope local;
    scope_init(&local, lower->scope);
    Scope *saved = lower->scope;
    lower->scope = &local;
    for (size_t i = 0; i < fn->params.len; i++) {
        scope_define(lower, &local, fn->params.items[i].name, SYM_UNKNOWN, fn->params.items[i].span);
    }
    fn->body = lower_block(lower, stmt->as.fn_stmt.body, false);
    lower->scope = saved;
    scope_free(&local);
}

static DsLowerStmt *lower_stmt(Lower *lower, const DsStmt *stmt) {
    switch (stmt->kind) {
        case DS_STMT_LET: {
            SymKind kind = SYM_UNKNOWN;
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_LET, stmt->span);
            out->as.let_stmt.name = str_clone(stmt->as.let_stmt.name);
            out->as.let_stmt.value = lower_expr(lower, stmt->as.let_stmt.value, &kind);
            scope_define(lower, lower->scope, stmt->as.let_stmt.name, kind, stmt->span);
            return out;
        }
        case DS_STMT_CMD: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_CMD, stmt->span);
            ds_command_init(&out->as.cmd_stmt, DS_COMMAND_PLAIN, stmt->span);
            for (size_t i = 0; i < stmt->as.cmd_stmt.words.len; i++) {
                validate_cmd_word(lower, stmt->as.cmd_stmt.words.items[i].text, stmt->as.cmd_stmt.words.items[i].span);
            }
            ds_word_vec_clone(&out->as.cmd_stmt.words, &stmt->as.cmd_stmt.words);
            if (stmt->as.cmd_stmt.redirect.kind != DS_REDIRECT_NONE) {
                if (stmt->as.cmd_stmt.redirect.target.len == 0) {
                    ds_diag_error(lower->diag, stmt->as.cmd_stmt.redirect.op_span, "expected redirection target");
                } else {
                    ds_redirect_clone(&out->as.cmd_stmt.redirect, &stmt->as.cmd_stmt.redirect);
                    validate_interpolation(lower, stmt->as.cmd_stmt.redirect.target, stmt->as.cmd_stmt.redirect.target_span);
                }
            }
            return out;
        }
        case DS_STMT_CALL:
            return lower_call_stmt(lower, stmt);
        case DS_STMT_IF: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_IF, stmt->span);
            SymKind cond_kind = SYM_UNKNOWN;
            out->as.if_stmt.condition = lower_expr(lower, stmt->as.if_stmt.condition, &cond_kind);
            out->as.if_stmt.then_branch = lower_block(lower, stmt->as.if_stmt.then_branch, true);
            if (stmt->as.if_stmt.else_branch) out->as.if_stmt.else_branch = lower_block(lower, stmt->as.if_stmt.else_branch, true);
            return out;
        }
        case DS_STMT_FOR: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_FOR_ARRAY, stmt->span);
            out->as.for_stmt.name = str_clone(stmt->as.for_stmt.key_name);
            if (stmt->as.for_stmt.has_value_name) {
                ds_diag_error(lower->diag, stmt->span, "map iteration is deferred in v0.10.0; use direct map access instead");
            }
            SymKind iterable_kind = SYM_UNKNOWN;
            out->as.for_stmt.iterable = lower_expr(lower, stmt->as.for_stmt.iterable, &iterable_kind);
            if (!stmt->as.for_stmt.has_value_name && iterable_kind != SYM_ARRAY && iterable_kind != SYM_UNKNOWN) {
                ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span, "for loop iterable must be an array in v0.10.0");
            }
            Scope local;
            scope_init(&local, lower->scope);
            Scope *saved = lower->scope;
            lower->scope = &local;
            scope_define(lower, &local, stmt->as.for_stmt.key_name, SYM_UNKNOWN, stmt->span);
            out->as.for_stmt.body = lower_block(lower, stmt->as.for_stmt.body, false);
            lower->scope = saved;
            scope_free(&local);
            return out;
        }
        case DS_STMT_PUSH: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_PUSH, stmt->span);
            out->as.push_stmt.name = str_clone(stmt->as.push_stmt.name);
            Symbol *sym = scope_find(lower->scope, stmt->as.push_stmt.name);
            if (!sym) {
                ds_diag_error(lower->diag, stmt->span, "unknown array `%.*s`", (int)stmt->as.push_stmt.name.len, stmt->as.push_stmt.name.data);
            } else if (sym->kind != SYM_ARRAY && sym->kind != SYM_UNKNOWN && sym->kind != SYM_TOPLEVEL_PREDECLARED) {
                ds_diag_error(lower->diag, stmt->span, "`push` requires an array variable in v0.10.0");
            }
            SymKind value_kind = SYM_UNKNOWN;
            out->as.push_stmt.value = lower_expr(lower, stmt->as.push_stmt.value, &value_kind);
            if (value_kind == SYM_ARRAY || value_kind == SYM_MAP) {
                ds_diag_error(lower->diag, stmt->as.push_stmt.value->span, "pushing collection values is deferred in v0.10.0");
            } else if (!collection_element_supported_in_bash(out->as.push_stmt.value)) {
                ds_diag_error(lower->diag, stmt->as.push_stmt.value->span, "collection element expressions must be scalar Bash-emittable values in v0.10.0; bind the expression to a variable first");
            }
            return out;
        }
        case DS_STMT_ASSERT: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_ASSERT, stmt->span);
            SymKind cond_kind = SYM_UNKNOWN;
            out->as.assert_stmt.condition = lower_expr(lower, stmt->as.assert_stmt.condition, &cond_kind);
            return out;
        }
        case DS_STMT_TEST:
            return stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
        case DS_STMT_BLOCK:
            return lower_block(lower, stmt, true);
        case DS_STMT_IMPORT:
            ds_diag_error(lower->diag, stmt->span, "unresolved import `%.*s`", (int)stmt->as.import_stmt.path.len, stmt->as.import_stmt.path.data);
            return stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
        case DS_STMT_FN:
            return stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
    }
    return stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
}

static void lower_expr_free(DsLowerExpr *expr) {
    if (!expr) return;
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INT:
            free(expr->as.text.data);
            break;
        case DS_LOWER_EXPR_RUN:
            ds_command_free(&expr->as.run);
            break;
        case DS_LOWER_EXPR_FIELD:
            lower_expr_free(expr->as.field.object);
            free(expr->as.field.field.data);
            break;
        case DS_LOWER_EXPR_UNARY:
            free(expr->as.unary.op.data);
            lower_expr_free(expr->as.unary.right);
            break;
        case DS_LOWER_EXPR_BINARY:
            lower_expr_free(expr->as.binary.left);
            free(expr->as.binary.op.data);
            lower_expr_free(expr->as.binary.right);
            break;
        case DS_LOWER_EXPR_CALL:
            free(expr->as.call.name.data);
            for (size_t i = 0; i < expr->as.call.args.len; i++) lower_expr_free(expr->as.call.args.items[i]);
            free(expr->as.call.args.items);
            break;
        case DS_LOWER_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) lower_expr_free(expr->as.array.elements.items[i]);
            free(expr->as.array.elements.items);
            break;
        case DS_LOWER_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) {
                free(expr->as.map.entries.items[i].key.data);
                lower_expr_free(expr->as.map.entries.items[i].value);
            }
            free(expr->as.map.entries.items);
            break;
        case DS_LOWER_EXPR_INDEX:
            lower_expr_free(expr->as.index.object);
            lower_expr_free(expr->as.index.index);
            free(expr->as.index.map_key.data);
            break;
        case DS_LOWER_EXPR_BOOL:
        case DS_LOWER_EXPR_ERROR:
            break;
    }
    free(expr);
}

static void lower_stmt_free(DsLowerStmt *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET:
            free(stmt->as.let_stmt.name.data);
            lower_expr_free(stmt->as.let_stmt.value);
            break;
        case DS_LOWER_STMT_IF:
            lower_expr_free(stmt->as.if_stmt.condition);
            lower_stmt_free(stmt->as.if_stmt.then_branch);
            lower_stmt_free(stmt->as.if_stmt.else_branch);
            break;
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) lower_stmt_free(stmt->as.block_stmt.statements.items[i]);
            free(stmt->as.block_stmt.statements.items);
            break;
        case DS_LOWER_STMT_CMD:
            ds_command_free(&stmt->as.cmd_stmt);
            break;
        case DS_LOWER_STMT_CALL:
            free(stmt->as.call_stmt.name.data);
            for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) lower_expr_free(stmt->as.call_stmt.args.items[i]);
            free(stmt->as.call_stmt.args.items);
            break;
        case DS_LOWER_STMT_FOR_ARRAY:
            free(stmt->as.for_stmt.name.data);
            lower_expr_free(stmt->as.for_stmt.iterable);
            lower_stmt_free(stmt->as.for_stmt.body);
            break;
        case DS_LOWER_STMT_PUSH:
            free(stmt->as.push_stmt.name.data);
            lower_expr_free(stmt->as.push_stmt.value);
            break;
        case DS_LOWER_STMT_ASSERT:
            lower_expr_free(stmt->as.assert_stmt.condition);
            break;
    }
    free(stmt);
}

static void collect_test(Lower *lower, const DsStmt *stmt, DsLowerProgram *program) {
    if (stmt->kind != DS_STMT_TEST) return;
    for (size_t i = 0; i < program->tests.len; i++) {
        if (program->tests.items[i].name.len == stmt->as.test_stmt.name.len &&
            memcmp(program->tests.items[i].name.data, stmt->as.test_stmt.name.data, stmt->as.test_stmt.name.len) == 0) {
            ds_diag_error(lower->diag, stmt->span, "duplicate test `%.*s`", (int)stmt->as.test_stmt.name.len, stmt->as.test_stmt.name.data);
            return;
        }
    }
    DsLowerTest test;
    memset(&test, 0, sizeof(test));
    test.name = str_clone(stmt->as.test_stmt.name);
    test.span = stmt->span;
    test.body = lower_block(lower, stmt->as.test_stmt.body, true);
    lower_test_vec_push(&program->tests, test);
}

DsLowerProgram *ds_lower_program(const DsAst *ast, DsDiag *diag) {
    Scope root;
    scope_init(&root, NULL);
    DsLowerProgram *program = (DsLowerProgram *)ds_xcalloc(1, sizeof(DsLowerProgram));
    Lower lower = {diag, &root, program};
    program->span = ast->span;
    program->has_script = ast->has_script;
    if (ast->has_script) {
        for (size_t i = 0; i < ast->script.declarations.len; i++) lower_script_decl(&lower, &ast->script.declarations.items[i], program);
    }
    for (size_t i = 0; i < ast->statements.len; i++) collect_function_signature(&lower, ast->statements.items[i], program);
    for (size_t i = 0; i < ast->statements.len; i++) collect_top_level_let_signature(&lower, ast->statements.items[i]);
    for (size_t i = 0; i < ast->statements.len; i++) {
        if (ast->statements.items[i]->kind == DS_STMT_FN) {
            DsLowerFn *fn = find_function(program, ast->statements.items[i]->as.fn_stmt.name);
            if (fn) lower_function_body(&lower, fn, ast->statements.items[i]);
        }
    }
    for (size_t i = 0; i < ast->statements.len; i++) collect_test(&lower, ast->statements.items[i], program);
    reject_recursive_functions(&lower);
    for (size_t i = 0; i < ast->statements.len; i++) {
        if (ast->statements.items[i]->kind != DS_STMT_FN && ast->statements.items[i]->kind != DS_STMT_TEST) lower_stmt_vec_push(&program->statements, lower_stmt(&lower, ast->statements.items[i]));
    }
    scope_free(&root);
    if (diag->has_error) {
        ds_lower_program_free(program);
        return NULL;
    }
    return program;
}

bool ds_lower_validate(const DsAst *ast, DsDiag *diag) {
    DsLowerProgram *program = ds_lower_program(ast, diag);
    if (!program) return false;
    ds_lower_program_free(program);
    return true;
}

void ds_lower_program_free(DsLowerProgram *program) {
    if (!program) return;
    for (size_t i = 0; i < program->script_decls.len; i++) {
        free(program->script_decls.items[i].name.data);
        free(program->script_decls.items[i].default_text.data);
    }
    free(program->script_decls.items);
    for (size_t i = 0; i < program->functions.len; i++) {
        free(program->functions.items[i].name.data);
        for (size_t j = 0; j < program->functions.items[i].params.len; j++) {
            free(program->functions.items[i].params.items[j].name.data);
            lower_expr_free(program->functions.items[i].params.items[j].default_value);
        }
        free(program->functions.items[i].params.items);
        lower_stmt_free(program->functions.items[i].body);
    }
    free(program->functions.items);
    for (size_t i = 0; i < program->tests.len; i++) {
        free(program->tests.items[i].name.data);
        lower_stmt_free(program->tests.items[i].body);
    }
    free(program->tests.items);
    for (size_t i = 0; i < program->statements.len; i++) lower_stmt_free(program->statements.items[i]);
    free(program->statements.items);
    free(program);
}
