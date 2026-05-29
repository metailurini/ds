#include "lower_internal.h"

#include <stdlib.h>
#include <string.h>

bool lower_str_eq(DsStr a, const char *b) {
    size_t len = strlen(b);
    return a.len == len && memcmp(a.data, b, len) == 0;
}

bool name_eq(DsStr a, const char *b) {
    size_t len = strlen(b);
    return a.len == len && memcmp(a.data, b, len) == 0;
}

bool is_env_name_text(DsStr name) {
    if (name.len == 0) return false;
    char c = name.data[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return false;
    for (size_t i = 1; i < name.len; i++) {
        c = name.data[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

bool split_member_name(DsStr name, DsStr *ns, DsStr *member) {
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

DsLowerValueKind lower_stdlib_return_value_kind(const DsStdlibHelper *helper) {
    if (!helper) return DS_LOWER_VALUE_UNKNOWN;
    switch (helper->return_kind) {
        case DS_STDLIB_RETURN_BOOL: return DS_LOWER_VALUE_BOOL;
        case DS_STDLIB_RETURN_INT: return DS_LOWER_VALUE_INT;
        case DS_STDLIB_RETURN_STRING: return DS_LOWER_VALUE_STRING;
        case DS_STDLIB_RETURN_ARRAY: return DS_LOWER_VALUE_ARRAY;
        case DS_STDLIB_RETURN_MAP: return DS_LOWER_VALUE_MAP;
        case DS_STDLIB_RETURN_COMMAND_RESULT: return DS_LOWER_VALUE_COMMAND_RESULT;
        case DS_STDLIB_RETURN_STATEMENT_ONLY: return DS_LOWER_VALUE_UNKNOWN;
    }
    return DS_LOWER_VALUE_UNKNOWN;
}

bool stdlib_return_kind(const DsStdlibHelper *helper, SymKind *kind) {
    if (!helper) return false;
    *kind = sym_kind_from_lower_value_kind(lower_stdlib_return_value_kind(helper));
    return true;
}

DsStr str_clone(DsStr s) {
    DsStr out = {ds_str_dup_range(s.data, s.len), s.len};
    return out;
}

void scope_init(Scope *scope, Scope *parent) {
    scope->parent = parent;
    scope->items = NULL;
    scope->len = 0;
    scope->cap = 0;
}

void scope_free(Scope *scope) {
    for (size_t i = 0; i < scope->len; i++) free(scope->items[i].name);
    free(scope->items);
}

Symbol *scope_find_current(Scope *scope, DsStr name) {
    for (size_t i = 0; i < scope->len; i++) {
        if (name_eq(name, scope->items[i].name)) return &scope->items[i];
    }
    return NULL;
}

Symbol *scope_find(Scope *scope, DsStr name) {
    for (Scope *s = scope; s; s = s->parent) {
        Symbol *sym = scope_find_current(s, name);
        if (sym) return sym;
    }
    return NULL;
}

void scope_define(Lower *lower, Scope *scope, DsStr name, SymKind kind, DsSpan span) {
    scope_define_array(lower, scope, name, kind, SYM_UNKNOWN, span);
}

void scope_define_array(Lower *lower, Scope *scope, DsStr name, SymKind kind, SymKind element_kind, DsSpan span) {
    if (name_eq(name, "env")) {
        ds_diag_error(lower->diag, span, "`env` is a reserved environment namespace in v0.27.0");
        return;
    }
    Symbol *current = scope_find_current(scope, name);
    if (current && current->kind == SYM_TOPLEVEL_PREDECLARED) {
        current->kind = kind;
        current->element_kind = element_kind;
        current->dynamic_scalar = false;
        current->function_depth = lower->function_depth;
        return;
    }
    if (scope_find_current(scope, name)) {
        ds_diag_error(lower->diag, span, "duplicate variable `%.*s` in this scope", (int)name.len, name.data);
        return;
    }
    if (scope->len == scope->cap) {
        scope->cap = scope->cap ? scope->cap * 2 : 16;
        scope->items = (Symbol *)ds_xrealloc(scope->items, scope->cap * sizeof(Symbol));
    }
    scope->items[scope->len].name = ds_str_dup_range(name.data, name.len);
    scope->items[scope->len].kind = kind;
    scope->items[scope->len].element_kind = element_kind;
    scope->items[scope->len].dynamic_scalar = false;
    scope->items[scope->len].function_depth = lower->function_depth;
    scope->len++;
}

bool lower_validate_handler_capture(Lower *lower, const Symbol *sym, DsStr name, DsSpan span) {
    if (!lower || !sym) return true;
    if (lower->handler_depth <= 0 || lower->handler_function_depth <= 0) return true;
    if (sym->function_depth <= 0 || sym->function_depth > lower->handler_function_depth) return true;
    ds_diag_error(lower->diag, span,
                  "cleanup handler captures function-local variable `%.*s`; function-local handler captures are deferred in v0.22.0 because VM and standalone Bash cannot preserve the local scope after the function returns",
                  (int)name.len, name.data);
    return false;
}

DsLowerFn *find_function(DsLowerProgram *program, DsStr name) {
    for (size_t i = 0; i < program->functions.len; i++) {
        DsLowerFn *fn = &program->functions.items[i];
        if (fn->name.len == name.len && memcmp(fn->name.data, name.data, name.len) == 0) return fn;
    }
    return NULL;
}

int find_function_index(DsLowerProgram *program, DsStr name) {
    for (size_t i = 0; i < program->functions.len; i++) {
        DsLowerFn *fn = &program->functions.items[i];
        if (fn->name.len == name.len && memcmp(fn->name.data, name.data, name.len) == 0) return (int)i;
    }
    return -1;
}

void lower_stmt_vec_push(DsLowerStmtVec *vec, DsLowerStmt *stmt) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 16;
        vec->items = (DsLowerStmt **)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerStmt *));
    }
    vec->items[vec->len++] = stmt;
}

void lower_expr_vec_push(DsLowerExprVec *vec, DsLowerExpr *expr) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerExpr **)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerExpr *));
    }
    vec->items[vec->len++] = expr;
}

void lower_fn_param_vec_push(DsLowerFnParamVec *vec, DsLowerFnParam param) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerFnParam *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerFnParam));
    }
    vec->items[vec->len++] = param;
}

void lower_fn_vec_push(DsLowerFnVec *vec, DsLowerFn fn) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerFn *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerFn));
    }
    vec->items[vec->len++] = fn;
}

void lower_test_vec_push(DsLowerTestVec *vec, DsLowerTest test) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerTest *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerTest));
    }
    vec->items[vec->len++] = test;
}

void lower_decl_vec_push(DsLowerScriptDeclVec *vec, DsLowerScriptDecl decl) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerScriptDecl *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerScriptDecl));
    }
    vec->items[vec->len++] = decl;
}

void lower_case_pattern_vec_push(DsLowerCasePatternVec *vec, DsLowerCasePattern pattern) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 4;
        vec->items = (DsLowerCasePattern *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerCasePattern));
    }
    vec->items[vec->len++] = pattern;
}

void lower_case_arm_vec_push(DsLowerCaseArmVec *vec, DsLowerCaseArm arm) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 4;
        vec->items = (DsLowerCaseArm *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerCaseArm));
    }
    vec->items[vec->len++] = arm;
}
