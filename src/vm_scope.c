#include "vm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VmScope *scope_new(VmScope *parent) {
    VmScope *scope = (VmScope *)ds_xcalloc(1, sizeof(VmScope));
    ds_map_init(&scope->vars);
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

void vm_push_scope(Vm *vm) {
    vm->scope = scope_new(vm->scope);
}

void vm_pop_scope(Vm *vm) {
    if (!vm->scope || !vm->scope->parent) return;
    VmScope *old = vm->scope;
    vm->scope = old->parent;
    old->parent = NULL;
    scope_free_one(old);
}

static void vm_push_return(Vm *vm, size_t ip, VmScope *caller_scope) {
    if (vm->return_len == vm->return_cap) {
        vm->return_cap = vm->return_cap ? vm->return_cap * 2 : 8;
        vm->return_ips = (size_t *)ds_xrealloc(vm->return_ips, vm->return_cap * sizeof(size_t));
        vm->return_dsts = (int *)ds_xrealloc(vm->return_dsts, vm->return_cap * sizeof(int));
        vm->return_scopes = (VmScope **)ds_xrealloc(vm->return_scopes, vm->return_cap * sizeof(VmScope *));
    }
    vm->return_ips[vm->return_len] = ip;
    vm->return_dsts[vm->return_len] = -1;
    vm->return_scopes[vm->return_len] = caller_scope;
    vm->return_len++;
}

void vm_pop_to_scope(Vm *vm, VmScope *target) {
    while (vm->scope && vm->scope != target) vm_pop_scope(vm);
}

bool vm_pop_return(Vm *vm, size_t *out) {
    if (vm->return_len == 0) return false;
    *out = vm->return_ips[--vm->return_len];
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
    for (size_t i = 0; i < fn->param_count; i++) {
        DsValue value = ds_value_null();
        if (i < ins->arg_count) value = ds_value_copy(&vm->regs[ins->args[i]]);
        else if (fn->params[i].has_default) value = ds_value_copy(&fn->params[i].default_value);
        else {
            scope_free_one(scope);
            ds_diag_error(vm->diag, ins->span, "function `%s` missing argument `%s`", fn->name, fn->params[i].name);
            return false;
        }
        DsStr key = {fn->params[i].name, strlen(fn->params[i].name)};
        ds_map_set(&scope->vars, key, value);
    }
    vm->scope = scope;
    vm_push_return(vm, next_ip, caller_scope);
    vm->return_dsts[vm->return_len - 1] = ins->dst;
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
