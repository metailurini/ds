#include "lower_internal.h"

bool is_env_name_text(DsStr name) {
    if (name.len == 0 || !ds_is_ident_start(name.data[0])) return false;
    for (size_t i = 1; i < name.len; i++) if (!ds_is_ident_continue(name.data[i])) return false;
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

void scope_init(Scope *scope, Scope *parent) {
    *scope = (Scope){.parent = parent};
}

void scope_free(Scope *scope) {
    for (size_t i = 0; i < scope->len; i++) {
        free(scope->items[i].name);
        row_schema_free(&scope->items[i].row_schema);
    }
    free(scope->items);
}

Symbol *scope_find_current(Scope *scope, DsStr name) {
    for (size_t i = 0; i < scope->len; i++) {
        if (lower_str_eq(name, scope->items[i].name)) return &scope->items[i];
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

Symbol *lower_resolve_value_symbol(Lower *lower, DsStr name, DsSpan span, const char *unknown_kind) {
    Symbol *sym = scope_find(lower->scope, name);
    if (!sym) {
        if (find_function(lower->program, name)) {
            ds_diag_error(lower->diag, span, "function `%.*s` cannot be used as a variable in v0.9.0",
                          (int)name.len, name.data);
        } else {
            ds_diag_error(lower->diag, span, "unknown %s `%.*s`", unknown_kind, (int)name.len, name.data);
        }
        return NULL;
    }
    if (sym->kind == SYM_FUNCTION) {
        ds_diag_error(lower->diag, span, "function `%.*s` cannot be used as a variable in v0.9.0",
                      (int)name.len, name.data);
        return NULL;
    }
    return sym;
}

void scope_define(Lower *lower, Scope *scope, DsStr name, SymKind kind, DsSpan span) {
    scope_define_array(lower, scope, name, kind, SYM_UNKNOWN, span);
}

void scope_define_array(Lower *lower, Scope *scope, DsStr name, SymKind kind, SymKind element_kind, DsSpan span) {
    if (lower_str_eq(name, "env")) {
        ds_diag_error(lower->diag, span, "`env` is a reserved environment namespace in v0.27.0");
        return;
    }
    Symbol *current = scope_find_current(scope, name);
    if (current && current->kind == SYM_TOPLEVEL_PREDECLARED) {
        current->kind = kind;
        current->element_kind = element_kind;
        current->is_row = false;
        current->is_row_array = false;
        current->saw_scalar_array_value = kind == SYM_ARRAY && element_kind != SYM_UNKNOWN && element_kind != SYM_MAP;
        row_schema_free(&current->row_schema);
        current->dynamic_scalar = false;
        current->function_depth = lower->function_depth;
        return;
    }
    if (scope_find_current(scope, name)) {
        ds_diag_error(lower->diag, span, "duplicate variable `%.*s` in this scope", (int)name.len, name.data);
        return;
    }
    DS_GROW_ARRAY(scope->items, scope->len, scope->cap, 16);
    scope->items[scope->len].name = ds_str_dup_range(name.data, name.len);
    scope->items[scope->len].kind = kind;
    scope->items[scope->len].element_kind = element_kind;
    scope->items[scope->len].is_row = false;
    scope->items[scope->len].is_row_array = false;
    scope->items[scope->len].saw_scalar_array_value = kind == SYM_ARRAY && element_kind != SYM_UNKNOWN && element_kind != SYM_MAP;
    row_schema_init(&scope->items[scope->len].row_schema);
    scope->items[scope->len].dynamic_scalar = false;
    scope->items[scope->len].function_depth = lower->function_depth;
    scope->len++;
}

void symbol_set_row(Symbol *sym, const DsLowerRowSchema *schema) {
    if (!sym) return;
    row_schema_free(&sym->row_schema);
    row_schema_init(&sym->row_schema);
    if (schema) row_schema_clone(schema, &sym->row_schema);
    sym->kind = SYM_MAP;
    sym->element_kind = SYM_UNKNOWN;
    for (size_t i = 0; schema && i < schema->len; i++) {
        SymKind field_kind = sym_kind_from_lower_value_kind(schema->items[i].kind);
        if (sym->element_kind == SYM_UNKNOWN) sym->element_kind = field_kind;
        else if (sym->element_kind != field_kind) { sym->element_kind = SYM_UNKNOWN; break; }
    }
    sym->is_row = true;
    sym->is_row_array = false;
    sym->saw_scalar_array_value = false;
}

void symbol_set_row_array(Symbol *sym, const DsLowerRowSchema *schema) {
    if (!sym) return;
    row_schema_free(&sym->row_schema);
    row_schema_init(&sym->row_schema);
    if (schema) row_schema_clone(schema, &sym->row_schema);
    sym->kind = SYM_ARRAY;
    sym->element_kind = SYM_MAP;
    sym->is_row = false;
    sym->is_row_array = true;
    sym->saw_scalar_array_value = false;
}

void scope_define_row(Lower *lower, Scope *scope, DsStr name, DsLowerRowSchema schema, DsSpan span) {
    scope_define_array(lower, scope, name, SYM_MAP, SYM_UNKNOWN, span);
    Symbol *sym = scope_find_current(scope, name);
    if (sym) symbol_set_row(sym, &schema);
}

void scope_define_row_array(Lower *lower, Scope *scope, DsStr name, DsLowerRowSchema schema, DsSpan span) {
    scope_define_array(lower, scope, name, SYM_ARRAY, SYM_MAP, span);
    Symbol *sym = scope_find_current(scope, name);
    if (sym) symbol_set_row_array(sym, &schema);
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
        if (ds_str_eq(fn->name, name)) return fn;
    }
    return NULL;
}

int find_function_index(DsLowerProgram *program, DsStr name) {
    for (size_t i = 0; i < program->functions.len; i++) {
        DsLowerFn *fn = &program->functions.items[i];
        if (ds_str_eq(fn->name, name)) return (int)i;
    }
    return -1;
}

void lower_stmt_vec_push(DsLowerStmtVec *vec, DsLowerStmt *stmt) {
    DS_VEC_PUSH(vec, stmt, 16);
}

void lower_expr_vec_push(DsLowerExprVec *vec, DsLowerExpr *expr) {
    DS_VEC_PUSH(vec, expr, 8);
}

void lower_fn_param_vec_push(DsLowerFnParamVec *vec, DsLowerFnParam param) {
    DS_VEC_PUSH(vec, param, 8);
}

void lower_fn_vec_push(DsLowerFnVec *vec, DsLowerFn fn) {
    DS_VEC_PUSH(vec, fn, 8);
}

void lower_test_vec_push(DsLowerTestVec *vec, DsLowerTest test) {
    DS_VEC_PUSH(vec, test, 8);
}

void lower_decl_vec_push(DsLowerScriptDeclVec *vec, DsLowerScriptDecl decl) {
    DS_VEC_PUSH(vec, decl, 8);
}

void lower_case_pattern_vec_push(DsLowerCasePatternVec *vec, DsLowerCasePattern pattern) {
    DS_VEC_PUSH(vec, pattern, 4);
}

void lower_case_arm_vec_push(DsLowerCaseArmVec *vec, DsLowerCaseArm arm) {
    DS_VEC_PUSH(vec, arm, 4);
}
