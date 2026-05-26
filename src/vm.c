#define _POSIX_C_SOURCE 200809L

#include "ds_signal.h"
#include "vm_internal.h"

#include <stdbool.h>
#include <signal.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t ds_vm_pending_signal = 0;

static void ds_vm_signal_handler(int sig) {
    ds_vm_pending_signal = sig;
}

static bool vm_install_signal_handler(int sig, struct sigaction *old_action) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = ds_vm_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return sigaction(sig, &action, old_action) == 0;
}

int vm_take_pending_signal(void) {
    int sig = ds_vm_pending_signal;
    ds_vm_pending_signal = 0;
    return sig;
}

void vm_note_interrupted_signal(Vm *vm, int sig) {
    if (ds_posix_signal_is_runtime_cleanup(sig)) vm->interrupted_signal = sig;
}

static bool int_add_checked(int64_t a, int64_t b, int64_t *out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    *out = a + b;
    return true;
}

static bool int_sub_checked(int64_t a, int64_t b, int64_t *out) {
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return false;
    *out = a - b;
    return true;
}

static bool int_mul_checked(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) { *out = 0; return true; }
    if (a == -1 && b == INT64_MIN) return false;
    if (b == -1 && a == INT64_MIN) return false;
    if (a > 0) {
        if (b > 0) { if (a > INT64_MAX / b) return false; }
        else { if (b < INT64_MIN / a) return false; }
    } else {
        if (b > 0) { if (a < INT64_MIN / b) return false; }
        else { if (a < INT64_MAX / b) return false; }
    }
    *out = a * b;
    return true;
}

static bool int_pow_checked(int64_t base, int64_t exp, int64_t *out) {
    if (exp < 0) return false;
    int64_t result = 1;
    int64_t factor = base;
    while (exp > 0) {
        if (exp & 1) {
            if (!int_mul_checked(result, factor, &result)) return false;
        }
        exp >>= 1;
        if (exp > 0 && !int_mul_checked(factor, factor, &factor)) return false;
    }
    *out = result;
    return true;
}

static bool value_exact_equal(const DsValue *a, const DsValue *b) {
    if (a->kind != b->kind) return false;
    return ds_value_compare(a, b) == 0;
}

static bool regex_literal_to_pattern(const DsValue *lit, DsString *pattern, int *flags) {
    ds_string_init(pattern);
    *flags = REG_EXTENDED;
    if (lit->kind != DS_VALUE_STRING || lit->as.string.len < 3 || lit->as.string.data[0] != '/') return false;
    const char *s = lit->as.string.data;
    size_t len = lit->as.string.len;
    size_t i = 1;
    for (; i < len; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < len) {
            char n = s[++i];
            if (n == '/') ds_string_append_char(pattern, '/');
            else { ds_string_append_char(pattern, '\\'); ds_string_append_char(pattern, n); }
            continue;
        }
        if (c == '/') break;
        ds_string_append_char(pattern, c);
    }
    if (i >= len || s[i] != '/' || pattern->len == 0) return false;
    if (i + 1 < len) {
        if (i + 2 == len && s[i + 1] == 'i') *flags |= REG_ICASE;
        else return false;
    }
    return true;
}

static bool ensure_regs(Vm *vm) {
    if (vm->program->next_reg <= 0) vm->program->next_reg = 1;
    vm->regs = (DsValue *)ds_xcalloc((size_t)vm->program->next_reg, sizeof(DsValue));
    for (int i = 0; i < vm->program->next_reg; i++) vm->regs[i] = ds_value_null();
    return true;
}


static void set_reg(Vm *vm, int reg, DsValue value) {
    ds_value_free(&vm->regs[reg]);
    vm->regs[reg] = value;
}

static void vm_register_handler(Vm *vm, DsHandlerSignal signal, size_t target, bool is_trap) {
    if (is_trap) {
        for (size_t i = 0; i < vm->handler_len; i++) {
            if (vm->handlers[i].is_trap && vm->handlers[i].signal == signal) {
                vm->handlers[i].target = target;
                return;
            }
        }
    }
    if (vm->handler_len == vm->handler_cap) {
        vm->handler_cap = vm->handler_cap ? vm->handler_cap * 2 : 8;
        vm->handlers = (VmHandler *)ds_xrealloc(vm->handlers, vm->handler_cap * sizeof(VmHandler));
    }
    vm->handlers[vm->handler_len++] = (VmHandler){signal, target, is_trap};
}

int ds_vm_run_program_args_options(const DsSource *source, const DsLowerProgram *lowered, int argc, char **argv, DsDiag *diag, DsVmOptions options) {
    (void)source;
    Program p;
    if (!compile_program(lowered, &p, diag)) {
        return 1;
    }
    Vm vm;
    memset(&vm, 0, sizeof(vm));
    vm.program = &p;
    vm.diag = diag;
    vm.source = source;
    vm.options = options;
    vm.scope = scope_new(NULL);
    ensure_regs(&vm);

    int rc = 0;
    struct sigaction old_int;
    struct sigaction old_term;
    memset(&old_int, 0, sizeof(old_int));
    memset(&old_term, 0, sizeof(old_term));
    bool int_installed = false;
    bool term_installed = false;
    int bind_rc = bind_script_args(&vm, lowered, argc, argv);
    if (bind_rc == 2) { rc = 0; goto done; }
    if (bind_rc != 0) { rc = bind_rc; goto done; }
    size_t ip = 0;
    bool handler_mode = false;
    int final_rc = 0;
    size_t cleanup_cursor = 0;
    bool cleanup_trap_done = false;
    DsHandlerSignal cleanup_signal = DS_HANDLER_EXIT;

    int_installed = vm_install_signal_handler(SIGINT, &old_int);
    term_installed = vm_install_signal_handler(SIGTERM, &old_term);

dispatch_loop:
    while (ip < p.instr_len) {
        if (!handler_mode && ds_vm_pending_signal) {
            int sig = vm_take_pending_signal();
            vm_note_interrupted_signal(&vm, sig);
            cleanup_signal = ds_handler_signal_from_posix(sig);
            rc = ds_handler_signal_default_status(cleanup_signal);
            goto done;
        }
        Instr *ins = &p.instrs[ip];
        trace_vm_instr(&vm, ip, ins);
        switch (ins->op) {
            case OP_LOAD_CONST:
                set_reg(&vm, ins->dst, ds_value_copy(&p.consts[ins->a]));
                ip++;
                break;
            case OP_LOAD_VAR: {
                DsValue value;
                if (!lookup_var(&vm, ins->name, &value, ins->span)) { rc = 1; goto done; }
                set_reg(&vm, ins->dst, value);
                ip++;
                break;
            }
            case OP_STORE_VAR: {
                DsStr key = {ins->name, strlen(ins->name)};
                DsValue *slot = lookup_var_ref(&vm, ins->name);
                if (slot) {
                    ds_value_free(slot);
                    *slot = ds_value_copy(&vm.regs[ins->a]);
                } else {
                    ds_map_set(&vm.scope->vars, key, ds_value_copy(&vm.regs[ins->a]));
                }
                ip++;
                break;
            }
            case OP_SET_ENV: {
                DsString rendered;
                ds_value_to_string(&vm.regs[ins->a], &rendered);
                if (setenv(ins->name, rendered.data ? rendered.data : "", 1) != 0) {
                    ds_diag_error(vm.diag, ins->span, "failed to set environment `%s`", ins->name);
                    ds_string_free(&rendered);
                    rc = 1;
                    goto done;
                }
                ds_string_free(&rendered);
                ip++;
                break;
            }
            case OP_NOT: {
                bool truth = false;
                ds_value_truthy(&vm.regs[ins->a], &truth);
                set_reg(&vm, ins->dst, ds_value_bool(!truth));
                ip++;
                break;
            }
            case OP_BINARY: {
                /*
                 * Lowering rejects statically-known operand-kind errors. The
                 * checks below are runtime/data diagnostics for values whose
                 * kind is only known while executing accepted HIR, plus
                 * arithmetic overflow checks.
                 */
                DsValue *left = &vm.regs[ins->a];
                DsValue *right = &vm.regs[ins->b];
                if (strcmp(ins->cmp, "+") == 0) {
                    if (left->kind == DS_VALUE_INT && right->kind == DS_VALUE_INT) {
                        int64_t out = 0;
                        if (!int_add_checked(left->as.integer, right->as.integer, &out)) {
                            ds_diag_error(diag, ins->span, "integer overflow in operator `+`");
                            rc = 1; goto done;
                        }
                        set_reg(&vm, ins->dst, ds_value_int(out));
                    } else if (left->kind == DS_VALUE_STRING && right->kind == DS_VALUE_STRING) {
                        DsString joined;
                        ds_string_init(&joined);
                        ds_string_append_range(&joined, left->as.string.data ? left->as.string.data : "", left->as.string.len);
                        ds_string_append_range(&joined, right->as.string.data ? right->as.string.data : "", right->as.string.len);
                        set_reg(&vm, ins->dst, ds_value_string_take(&joined));
                    } else {
                        ds_diag_error(diag, ins->span, "runtime operator `+` supports int+int or string+string");
                        rc = 1; goto done;
                    }
                } else if (strcmp(ins->cmp, "-") == 0) {
                    if (left->kind != DS_VALUE_INT || right->kind != DS_VALUE_INT) {
                        ds_diag_error(diag, ins->span, "runtime operator `-` requires integer operands");
                        rc = 1; goto done;
                    }
                    int64_t out = 0;
                    if (!int_sub_checked(left->as.integer, right->as.integer, &out)) {
                        ds_diag_error(diag, ins->span, "integer overflow in operator `-`");
                        rc = 1; goto done;
                    }
                    set_reg(&vm, ins->dst, ds_value_int(out));
                } else if (strcmp(ins->cmp, "*") == 0 || strcmp(ins->cmp, "/") == 0 || strcmp(ins->cmp, "%") == 0 || strcmp(ins->cmp, "**") == 0) {
                    if (left->kind != DS_VALUE_INT || right->kind != DS_VALUE_INT) {
                        ds_diag_error(diag, ins->span, "runtime arithmetic operator `%s` requires integer operands", ins->cmp);
                        rc = 1; goto done;
                    }
                    if ((strcmp(ins->cmp, "/") == 0 || strcmp(ins->cmp, "%") == 0) && right->as.integer == 0) {
                        ds_diag_error(diag, ins->span, "division or modulo by zero");
                        rc = 1; goto done;
                    }
                    if ((strcmp(ins->cmp, "/") == 0 || strcmp(ins->cmp, "%") == 0) && left->as.integer == INT64_MIN && right->as.integer == -1) {
                        ds_diag_error(diag, ins->span, "integer overflow in operator `%s`", ins->cmp);
                        rc = 1; goto done;
                    }
                    if (strcmp(ins->cmp, "*") == 0) {
                        int64_t out = 0;
                        if (!int_mul_checked(left->as.integer, right->as.integer, &out)) {
                            ds_diag_error(diag, ins->span, "integer overflow in operator `*`");
                            rc = 1; goto done;
                        }
                        set_reg(&vm, ins->dst, ds_value_int(out));
                    } else if (strcmp(ins->cmp, "/") == 0) set_reg(&vm, ins->dst, ds_value_int(left->as.integer / right->as.integer));
                    else if (strcmp(ins->cmp, "%") == 0) set_reg(&vm, ins->dst, ds_value_int(left->as.integer % right->as.integer));
                    else {
                        if (right->as.integer < 0) { ds_diag_error(diag, ins->span, "negative exponent runtime value is rejected in v0.21.0"); rc = 1; goto done; }
                        int64_t out = 0;
                        if (!int_pow_checked(left->as.integer, right->as.integer, &out)) {
                            ds_diag_error(diag, ins->span, "integer overflow in operator `**`");
                            rc = 1; goto done;
                        }
                        set_reg(&vm, ins->dst, ds_value_int(out));
                    }
                } else {
                    ds_diag_error(diag, ins->span, "internal VM invariant failed: unknown binary operator `%s` after lowering", ins->cmp ? ins->cmp : "");
                    rc = 1; goto done;
                }
                ip++;
                break;
            }
            case OP_COMPARE: {
                bool same_kind = vm.regs[ins->a].kind == vm.regs[ins->b].kind;
                int cmp = ds_value_compare(&vm.regs[ins->a], &vm.regs[ins->b]);
                bool result = false;
                if (strcmp(ins->cmp, "===") == 0) result = same_kind && cmp == 0;
                else if (strcmp(ins->cmp, "!==") == 0) result = !same_kind || cmp != 0;
                else if (strcmp(ins->cmp, "==") == 0) result = cmp == 0;
                else if (strcmp(ins->cmp, "!=") == 0) result = cmp != 0;
                else if (strcmp(ins->cmp, ">") == 0) result = cmp > 0;
                else if (strcmp(ins->cmp, ">=") == 0) result = cmp >= 0;
                else if (strcmp(ins->cmp, "<") == 0) result = cmp < 0;
                else if (strcmp(ins->cmp, "<=") == 0) result = cmp <= 0;
                set_reg(&vm, ins->dst, ds_value_bool(result));
                ip++;
                break;
            }
            case OP_MEMBERSHIP: {
                DsValue *needle = &vm.regs[ins->a];
                DsValue *haystack = &vm.regs[ins->b];
                if (haystack->kind != DS_VALUE_ARRAY) { ds_diag_error(diag, ins->span, "runtime right operand of `in` must be an array"); rc = 1; goto done; }
                bool found = false;
                for (size_t i = 0; i < haystack->as.array.len; i++) {
                    DsValue *item = (DsValue *)haystack->as.array.items[i];
                    if (value_exact_equal(needle, item)) { found = true; break; }
                }
                set_reg(&vm, ins->dst, ds_value_bool(found));
                ip++;
                break;
            }
            case OP_REGEX_MATCH: {
                DsValue *text = &vm.regs[ins->a];
                DsValue *lit = &vm.regs[ins->b];
                if (text->kind != DS_VALUE_STRING) { ds_diag_error(diag, ins->span, "internal VM regex invariant failed: accepted `matches` left operand must be a string"); rc = 1; goto done; }
                DsString pattern; int flags = REG_EXTENDED;
                if (!regex_literal_to_pattern(lit, &pattern, &flags)) { ds_diag_error(diag, ins->span, "internal VM regex invariant failed: accepted `matches` right operand must be a regex literal"); rc = 1; goto done; }
                regex_t re;
                int err = regcomp(&re, pattern.data ? pattern.data : "", flags);
                if (err != 0) { ds_diag_error(diag, ins->span, "internal VM regex invariant failed: lowerer accepted an invalid regex pattern"); ds_string_free(&pattern); rc = 1; goto done; }
                int match = regexec(&re, text->as.string.data ? text->as.string.data : "", 0, NULL, 0);
                regfree(&re);
                ds_string_free(&pattern);
                set_reg(&vm, ins->dst, ds_value_bool(match == 0));
                ip++;
                break;
            }
            case OP_INTERPOLATE: {
                DsString rendered;
                if (!interpolate_string(&vm, &p.consts[ins->a].as.string, &rendered, ins->span)) { rc = 1; goto done; }
                set_reg(&vm, ins->dst, ds_value_string_take(&rendered));
                ip++;
                break;
            }
            case OP_INTERP_JOIN: {
                DsString rendered;
                ds_string_init(&rendered);
                for (size_t i = 0; i < ins->arg_count; i++) {
                    DsString piece;
                    ds_value_to_string(&vm.regs[ins->args[i]], &piece);
                    ds_string_append_range(&rendered, piece.data ? piece.data : "", piece.len);
                    ds_string_free(&piece);
                }
                set_reg(&vm, ins->dst, ds_value_string_take(&rendered));
                ip++;
                break;
            }
            case OP_RUN_CAPTURE: {
                DsValue value;
                if (run_capture(&vm, ins, &value) != 0) { rc = 1; goto done; }
                set_reg(&vm, ins->dst, value);
                ip++;
                break;
            }
            case OP_GET_FIELD: {
                DsValue field = ds_value_null();
                if (!vm_command_result_field(&vm, &vm.regs[ins->a], ins->field, ins->span, &field)) { rc = 1; goto done; }
                set_reg(&vm, ins->dst, field);
                ip++;
                break;
            }
            case OP_JUMP:
                ip = (size_t)ins->target;
                break;
            case OP_JUMP_POP:
                for (int i = 0; i < ins->a; i++) vm_pop_scope(&vm);
                ip = (size_t)ins->target;
                break;
            case OP_JUMP_IF_FALSE: {
                bool truth = false;
                ds_value_truthy(&vm.regs[ins->a], &truth);
                ip = truth ? ip + 1 : (size_t)ins->target;
                break;
            }
            case OP_PUSH_SCOPE:
                vm_push_scope(&vm);
                ip++;
                break;
            case OP_POP_SCOPE:
                vm_pop_scope(&vm);
                ip++;
                break;
            case OP_RUN_CMD:
                rc = run_command(&vm, ins);
                if (vm.test_done) goto done;
                if (vm.control_exit_requested || rc != 0) {
                    if (ds_posix_signal_is_runtime_cleanup(vm.interrupted_signal)) {
                        cleanup_signal = ds_handler_signal_from_posix(vm.interrupted_signal);
                    }
                    goto done;
                }
                ip++;
                break;
            case OP_CALL: {
                size_t target_ip = 0;
                if (!call_function(&vm, ins, ip + 1, &target_ip)) { rc = 1; goto done; }
                ip = target_ip;
                break;
            }
            case OP_STDLIB_CALL: {
                DsValue value = ds_value_null();
                if (!ds_vm_stdlib_call(&vm, ins, &value)) { rc = 1; goto done; }
                set_reg(&vm, ins->dst, value);
                ip++;
                break;
            }
            case OP_ARRAY_LITERAL: {
                DsValue array = ds_value_null();
                array.kind = DS_VALUE_ARRAY;
                ds_array_init(&array.as.array);
                for (size_t i = 0; i < ins->arg_count; i++) {
                    DsValue *item = (DsValue *)ds_xcalloc(1, sizeof(DsValue));
                    *item = ds_value_copy(&vm.regs[ins->args[i]]);
                    ds_array_push(&array.as.array, item);
                }
                set_reg(&vm, ins->dst, array);
                ip++;
                break;
            }
            case OP_MAP_LITERAL: {
                DsValue map = ds_value_null();
                map.kind = DS_VALUE_MAP;
                ds_map_init(&map.as.map);
                for (size_t i = 0; i < ins->arg_count; i++) ds_map_set(&map.as.map, ins->words[i], ds_value_copy(&vm.regs[ins->args[i]]));
                set_reg(&vm, ins->dst, map);
                ip++;
                break;
            }
            case OP_GET_INDEX: {
                DsValue *obj = &vm.regs[ins->a];
                DsValue *idx = &vm.regs[ins->b];
                if (obj->kind == DS_VALUE_ARRAY) {
                    if (idx->kind != DS_VALUE_INT) { ds_diag_error(diag, ins->span, "runtime array index must be an int"); rc = 1; goto done; }
                    if (idx->as.integer < 0 || (size_t)idx->as.integer >= obj->as.array.len) { ds_diag_error(diag, ins->span, "array index %lld out of range", (long long)idx->as.integer); rc = 1; goto done; }
                    set_reg(&vm, ins->dst, ds_value_copy((DsValue *)obj->as.array.items[idx->as.integer]));
                } else if (obj->kind == DS_VALUE_MAP) {
                    DsString key;
                    ds_value_to_string(idx, &key);
                    DsStr key_view = {key.data ? key.data : "", key.len};
                    DsValue *found = ds_map_get(&obj->as.map, key_view);
                    if (!found) { ds_diag_error(diag, ins->span, "missing map key `%.*s`", (int)key_view.len, key_view.data); ds_string_free(&key); rc = 1; goto done; }
                    set_reg(&vm, ins->dst, ds_value_copy(found));
                    ds_string_free(&key);
                } else { ds_diag_error(diag, ins->span, "internal VM invariant failed: index receiver should be an array or map after lowering"); rc = 1; goto done; }
                ip++;
                break;
            }
            case OP_PUSH_ARRAY: {
                DsValue *array = lookup_var_ref(&vm, ins->name);
                if (!array) { ds_diag_error(diag, ins->span, "internal VM invariant failed: array push target `%s` should exist after lowering", ins->name); rc = 1; goto done; }
                if (array->kind != DS_VALUE_ARRAY) { ds_diag_error(diag, ins->span, "internal VM invariant failed: array push target should be an array after lowering"); rc = 1; goto done; }
                DsValue *item = (DsValue *)ds_xcalloc(1, sizeof(DsValue));
                *item = ds_value_copy(&vm.regs[ins->a]);
                ds_array_push(&array->as.array, item);
                ip++;
                break;
            }
            case OP_FOR_ARRAY: {
                DsValue *iter = &vm.regs[ins->a];
                if (iter->kind != DS_VALUE_ARRAY) { ds_diag_error(diag, ins->span, "runtime for loop iterable must be an array"); rc = 1; goto done; }
                if (!ins->loop_active) { ins->loop_active = true; ins->loop_index = 0; }
                if (ins->loop_index >= iter->as.array.len) { ins->loop_active = false; ip = (size_t)ins->target; break; }
                vm_push_scope(&vm);
                DsStr key = {ins->name, strlen(ins->name)};
                ds_map_set(&vm.scope->vars, key, ds_value_copy((DsValue *)iter->as.array.items[ins->loop_index]));
                ins->loop_index++;
                ip++;
                break;
            }
            case OP_FOR_RANGE: {
                DsValue *start = &vm.regs[ins->a];
                DsValue *end = &vm.regs[ins->b];
                if (start->kind != DS_VALUE_INT || end->kind != DS_VALUE_INT) { ds_diag_error(diag, ins->span, "runtime range bounds must be ints"); rc = 1; goto done; }
                if (!ins->loop_active) { ins->loop_active = true; ins->loop_current = start->as.integer; }
                if (ins->loop_current > end->as.integer) { ins->loop_active = false; ip = (size_t)ins->target; break; }
                vm_push_scope(&vm);
                DsStr key = {ins->name, strlen(ins->name)};
                ds_map_set(&vm.scope->vars, key, ds_value_int(ins->loop_current));
                ins->loop_current++;
                ip++;
                break;
            }
            case OP_RESET_FOR: {
                if (ins->target >= 0 && (size_t)ins->target < p.instr_len) {
                    Instr *for_ins = &p.instrs[ins->target];
                    if (for_ins->op == OP_FOR_ARRAY || for_ins->op == OP_FOR_RANGE) {
                        for_ins->loop_active = false;
                        for_ins->loop_index = 0;
                        for_ins->loop_current = 0;
                    }
                }
                ip++;
                break;
            }
            case OP_ASSERT: {
                bool truth = false;
                ds_value_truthy(&vm.regs[ins->a], &truth);
                if (!truth) {
                    if (vm.options.test_mode) {
                        const char *test_name = vm.options.test_name.data ? vm.options.test_name.data : "<test>";
                        int test_name_len = (int)vm.options.test_name.len;
                        if (test_name_len <= 0) test_name_len = (int)strlen(test_name);
                        ds_diag_error(diag, ins->span, "test `%.*s`: assertion failed", test_name_len, test_name);
                    } else {
                        ds_diag_error(diag, ins->span, "assertion failed");
                    }
                    rc = 1;
                    goto done;
                }
                ip++;
                break;
            }
            case OP_RETURN_VALUE: {
                size_t return_ip = 0;
                if (vm.return_len == 0) {
                    /*
                     * Lowering only emits OP_RETURN_VALUE inside lowered
                     * function bodies. Reaching this path means the accepted
                     * HIR/bytecode violated the function-return contract.
                     */
                    ds_diag_error(diag, ins->span, "internal VM return invariant failed: return outside active function");
                    rc = 1;
                    goto done;
                }
                int dst = vm.return_dsts[vm.return_len - 1];
                VmScope *caller_scope = vm.return_scopes[vm.return_len - 1];
                DsValue value = ds_value_copy(&vm.regs[ins->a]);
                vm_pop_to_scope(&vm, caller_scope);
                if (!vm_pop_return(&vm, &return_ip)) { ds_value_free(&value); rc = 1; goto done; }
                if (dst >= 0) set_reg(&vm, dst, value);
                else ds_value_free(&value);
                ip = return_ip;
                break;
            }
            case OP_RETURN_FUNC: {
                size_t return_ip = 0;
                VmScope *caller_scope = vm.return_len ? vm.return_scopes[vm.return_len - 1] : NULL;
                vm_pop_to_scope(&vm, caller_scope);
                if (!vm_pop_return(&vm, &return_ip)) { rc = 1; goto done; }
                ip = return_ip;
                break;
            }
            case OP_REGISTER_HANDLER:
                vm_register_handler(&vm, (DsHandlerSignal)ins->a, (size_t)ins->target, ins->b != 0);
                ip++;
                break;
            case OP_END_HANDLER:
                if (handler_mode) goto cleanup_handler_done;
                ip++;
                break;
            case OP_RETURN:
                rc = ins->target;
                goto done;
            case OP_NOP:
                ip++;
                break;
        }
    }

done:
    if (vm.cleanup_running && handler_mode) goto cleanup_handler_done;
    if (!vm.cleanup_running && vm.handler_len > 0) {
        /*
         * Trap/defer/signal parity boundary: VM consumes accepted HIR handlers
         * only. Cleanup order is trap for the active signal, matching defers in
         * LIFO order, then EXIT trap/defers after signal cleanup.
         */
        vm.cleanup_running = true;
        final_rc = rc;
        vm.control_exit_requested = false;
        if (ds_posix_signal_is_runtime_cleanup(vm.interrupted_signal)) {
            cleanup_signal = ds_handler_signal_from_posix(vm.interrupted_signal);
        }
        cleanup_cursor = vm.handler_len;
        cleanup_trap_done = false;
    cleanup_next:
        if (!cleanup_trap_done) {
            cleanup_trap_done = true;
            for (size_t i = 0; i < vm.handler_len; i++) {
                if (vm.handlers[i].signal == cleanup_signal && vm.handlers[i].is_trap) {
                    ip = vm.handlers[i].target;
                    rc = 0;
                    handler_mode = true;
                    goto dispatch_loop;
                }
            }
        }
        while (cleanup_cursor > 0) {
            cleanup_cursor--;
            if (vm.handlers[cleanup_cursor].signal == cleanup_signal && !vm.handlers[cleanup_cursor].is_trap) {
                ip = vm.handlers[cleanup_cursor].target;
                rc = 0;
                handler_mode = true;
                goto dispatch_loop;
            }
        }
        if (cleanup_signal != DS_HANDLER_EXIT) {
            cleanup_signal = DS_HANDLER_EXIT;
            cleanup_cursor = vm.handler_len;
            cleanup_trap_done = false;
            goto cleanup_next;
        }
        rc = final_rc;
        goto cleanup_done;
    cleanup_handler_done:
        handler_mode = false;
        if (vm.control_exit_requested || rc != 0) final_rc = rc;
        vm.control_exit_requested = false;
        goto cleanup_next;
    }
cleanup_done:
    if (int_installed) sigaction(SIGINT, &old_int, NULL);
    if (term_installed) sigaction(SIGTERM, &old_term, NULL);
    for (int i = 0; i < p.next_reg; i++) ds_value_free(&vm.regs[i]);
    free(vm.regs);
    free(vm.return_ips);
    free(vm.return_dsts);
    free(vm.return_scopes);
    free(vm.handlers);
    scope_free_chain(vm.scope);
    program_free(&p);
    return rc;
}
int ds_vm_run_program_args(const DsSource *source, const DsLowerProgram *lowered, int argc, char **argv, DsDiag *diag) {
    DsVmOptions options = {0};
    return ds_vm_run_program_args_options(source, lowered, argc, argv, diag, options);
}

int ds_vm_run_program(const DsSource *source, const DsLowerProgram *lowered, DsDiag *diag) {
    return ds_vm_run_program_args(source, lowered, 0, NULL, diag);
}

int ds_vm_run(const DsSource *source, const DsAst *ast, DsDiag *diag) {
    DsLowerProgram *lowered = ds_lower_program(ast, diag);
    if (!lowered) return 1;
    int rc = ds_vm_run_program(source, lowered, diag);
    ds_lower_program_free(lowered);
    return rc;
}
