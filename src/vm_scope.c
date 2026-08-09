#include "vm_internal.h"

VmScope *scope_new(VmScope *parent) {
    VmScope *scope = (VmScope *)ds_xcalloc(1, sizeof(VmScope));
    if (!ds_map_init(&scope->vars)) {
        free(scope);
        return NULL;
    }
    scope->parent = parent;
    return scope;
}

static void scope_free_one(VmScope *scope) {
    if (!scope) return;
    ds_map_free(&scope->vars);
    free(scope);
}

void scope_free_chain(VmScope *scope) {
    while (scope) {
        VmScope *parent = scope->parent;
        scope_free_one(scope);
        scope = parent;
    }
}

bool vm_push_scope(Vm *vm) {
    VmScope *scope = scope_new(vm->scope);
    if (!scope) return false;
    vm->scope = scope;
    return true;
}

void vm_pop_scope(Vm *vm) {
    if (!vm->scope || !vm->scope->parent) return;
    VmScope *old = vm->scope;
    vm->scope = old->parent;
    old->parent = NULL;
    scope_free_one(old);
}

static void vm_push_return(Vm *vm, size_t ip, int dst, VmScope *caller_scope) {
    vm->returns = ds_grow_array(vm->returns, vm->return_len, &vm->return_cap, sizeof(*vm->returns), 8);
    vm->returns[vm->return_len++] = (VmReturnFrame){ip, dst, caller_scope};
}

static DsLowerValueKind vm_value_lower_kind(const DsValue *value) {
    if (!value) return DS_LOWER_VALUE_UNKNOWN;
    switch (value->kind) {
        case DS_VALUE_BOOL: return DS_LOWER_VALUE_BOOL;
        case DS_VALUE_INT: return DS_LOWER_VALUE_INT;
        case DS_VALUE_STRING: return DS_LOWER_VALUE_STRING;
        case DS_VALUE_ARRAY: return DS_LOWER_VALUE_ARRAY;
        case DS_VALUE_MAP: return DS_LOWER_VALUE_MAP;
        case DS_VALUE_COMMAND_RESULT: return DS_LOWER_VALUE_COMMAND_RESULT;
        case DS_VALUE_NULL: return DS_LOWER_VALUE_UNKNOWN;
    }
    return DS_LOWER_VALUE_UNKNOWN;
}

static bool vm_param_kind_matches(DsLowerValueKind expected, const DsValue *value) {
    if (expected == DS_LOWER_VALUE_UNKNOWN) return true;
    return vm_value_lower_kind(value) == expected;
}

void vm_pop_to_scope(Vm *vm, VmScope *target) {
    while (vm->scope && vm->scope != target) vm_pop_scope(vm);
}

bool vm_pop_return(Vm *vm, VmReturnFrame *out) {
    if (vm->return_len == 0) return false;
    *out = vm->returns[--vm->return_len];
    return true;
}

bool call_function(Vm *vm, Instr *ins, size_t next_ip, size_t *target_ip) {
    if (ins->target < 0 || (size_t)ins->target >= vm->program->function_len) {
        ds_diag_error(vm->diag, ins->span, "internal VM invariant failed: unknown function call target after lowering");
        return false;
    }
    FnMeta *fn = &vm->program->functions[ins->target];
    VmScope *caller_scope = vm->scope;
    VmScope *scope = scope_new(caller_scope);
    if (!scope) {
        ds_diag_error(vm->diag, ins->span, "failed to initialize function scope");
        return false;
    }
    for (size_t i = 0; i < fn->param_count; i++) {
        DsValue value = ds_value_null();
        if (i < ins->arg_count) value = ds_value_copy(&vm->regs[ins->args[i]]);
        else if (fn->params[i].has_default) value = ds_value_copy(&fn->params[i].default_value);
        else {
            scope_free_one(scope);
            ds_diag_error(vm->diag, ins->span, "function `%s` missing argument `%s`", fn->name, fn->params[i].name);
            return false;
        }
        if (!vm_param_kind_matches(fn->params[i].expected_kind, &value)) {
            DsLowerValueKind actual = vm_value_lower_kind(&value);
            ds_value_free(&value);
            scope_free_one(scope);
            ds_diag_error(vm->diag, ins->span,
                          "function `%s` expects argument %zu `%s` to be %s, got %s",
                          fn->name, i + 1, fn->params[i].name,
                          ds_lower_value_kind_name(fn->params[i].expected_kind),
                          ds_lower_value_kind_name(actual));
            return false;
        }
        DsStr key = {fn->params[i].name, strlen(fn->params[i].name)};
        if (!ds_map_set(&scope->vars, key, value)) {
            scope_free_one(scope);
            ds_diag_error(vm->diag, ins->span, "failed to bind function argument `%s`", fn->params[i].name);
            return false;
        }
    }
    vm->scope = scope;
    vm_push_return(vm, next_ip, ins->dst, caller_scope);
    *target_ip = fn->target;
    return true;
}

bool lookup_var(Vm *vm, const char *name, DsValue *out, DsSpan span) {
    DsStr key = {(char *)name, strlen(name)};
    for (VmScope *scope = vm->scope; scope; scope = scope->parent) {
        DsValue *found = ds_map_get(&scope->vars, key);
        if (found) {
            *out = ds_value_copy(found);
            return true;
        }
    }
    {
        ds_diag_error(vm->diag, span, "internal VM invariant failed: unknown variable `%s` after lowering", name);
        return false;
    }
}

DsValue *lookup_var_ref(Vm *vm, const char *name) {
    DsStr key = {(char *)name, strlen(name)};
    for (VmScope *scope = vm->scope; scope; scope = scope->parent) {
        DsValue *found = ds_map_get(&scope->vars, key);
        if (found) return found;
    }
    return NULL;
}
