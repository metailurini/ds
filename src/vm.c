#include "ds_command_facts.h"

#include "ds_regex.h"
#include "ds_signal.h"
#include "vm_internal.h"

#include <signal.h>
#include <regex.h>

static bool reg_is_truthy(Vm *vm, int reg) { bool t = false; ds_value_truthy(&vm->regs[reg], &t); return t; }

static DsHandlerSignal resolve_cleanup_signal(Vm *vm) {
    if (ds_posix_signal_is_runtime_cleanup(vm->interrupted_signal)) {
        return ds_handler_signal_from_posix(vm->interrupted_signal);
    }
    return DS_HANDLER_EXIT;
}

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


/*
 * VM field materialization owns runtime values for accepted field-access HIR.
 * Lowering owns source-language field legality and VM/Bash parity gates.
 * Unknown command-result fields here are internal VM invariants after lowering,
 * while missing map keys are runtime/data failures.
 */
bool vm_command_result_field(Vm *vm, const DsValue *value, const char *field, DsSpan span, DsValue *out) {
    if (value->kind == DS_VALUE_MAP) {
        DsStr key = {(char *)field, strlen(field)};
        DsValue *found = ds_map_get((DsMap *)&value->as.map, key);
        if (!found) {
            ds_diag_error(vm->diag, span, "missing map key `%s`", field);
            return false;
        }
        *out = ds_value_copy(found);
        return true;
    }
    if (value->kind != DS_VALUE_COMMAND_RESULT) {
        ds_diag_error(vm->diag, span, "internal VM field invariant failed: field receiver should be a command result or map after lowering");
        return false;
    }

    DsStr field_view = {(char *)field, strlen(field)};
    const DsCommandResultField *desc = ds_command_result_field_lookup(field_view);
    if (desc) {
        switch (desc->id) {
            case DS_COMMAND_RESULT_FIELD_STDOUT:
                ds_string_from_range(&out->as.string, ds_string_data(&value->as.command_result.stdout_text), value->as.command_result.stdout_text.len);
                out->kind = DS_VALUE_STRING;
                return true;
            case DS_COMMAND_RESULT_FIELD_STDERR:
                ds_string_from_range(&out->as.string, ds_string_data(&value->as.command_result.stderr_text), value->as.command_result.stderr_text.len);
                out->kind = DS_VALUE_STRING;
                return true;
            case DS_COMMAND_RESULT_FIELD_STATUS:
            case DS_COMMAND_RESULT_FIELD_CODE:
                *out = ds_value_int(value->as.command_result.code);
                return true;
            case DS_COMMAND_RESULT_FIELD_OK:
                *out = ds_value_bool(value->as.command_result.code == 0);
                return true;
            case DS_COMMAND_RESULT_FIELD_FAILED:
                *out = ds_value_bool(value->as.command_result.code != 0);
                return true;
        }
    }

    ds_diag_error(vm->diag, span, "internal VM field invariant failed: unknown command result field `%s` after lowering", field);
    return false;
}

static bool value_exact_equal(const DsValue *a, const DsValue *b) {
    if (a->kind != b->kind) return false;
    return ds_value_compare(a, b) == 0;
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
    DS_GROW_ARRAY(vm->handlers, vm->handler_len, vm->handler_cap, 8);
    vm->handlers[vm->handler_len++] = (VmHandler){signal, target, is_trap};
}

static const char *op_cmp_name(OpCmp e) {
    static const char *const names[] = {"+", "-", "*", "/", "%", "**", "==", "!=", "<", "<=", ">", ">=", "===", "!=="};
    return (unsigned)e < DS_ARRAY_LEN(names) ? names[e] : "?";
}

static bool check_div_zero_and_overflow(DsDiag *diag, DsSpan span,
    int64_t left, int64_t right, const char *op_name) {
    if (right == 0) {
        ds_diag_error(diag, span, "division or modulo by zero");
        return false;
    }
    if (left == INT64_MIN && right == -1) {
        ds_diag_error(diag, span, "integer overflow in operator `%s`", op_name);
        return false;
    }
    return true;
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
    if (!vm.scope) {
        ds_diag_error(diag, ds_span_zero(source), "failed to initialize runtime scope");
        program_free(&p);
        return 1;
    }
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
                    if (!ds_map_set(&vm.scope->vars, key, ds_value_copy(&vm.regs[ins->a]))) {
                        ds_diag_error(diag, ins->span, "failed to store variable `%s`", ins->name);
                        rc = 1;
                        goto done;
                    }
                }
                ip++;
                break;
            }
            case OP_SET_ENV: {
                DsString rendered;
                ds_value_to_string(&vm.regs[ins->a], &rendered);
                if (setenv(ins->name, ds_string_data(&rendered), 1) != 0) {
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
                bool truth = reg_is_truthy(&vm, ins->a);
                set_reg(&vm, ins->dst, ds_value_bool(!truth));
                ip++;
                break;
            }
            case OP_BINARY: {
                DsValue *left = &vm.regs[ins->a];
                DsValue *right = &vm.regs[ins->b];
                switch (ins->cmp_enum) {
                    case OP_CMP_ADD:
                        if (left->kind == DS_VALUE_INT && right->kind == DS_VALUE_INT) {
                            int64_t out = 0;
                            if (!vm_i64_add_checked(left->as.integer, right->as.integer, &out)) {
                                ds_diag_error(diag, ins->span, "integer overflow in operator `+`");
                                rc = 1; goto done;
                            }
                            set_reg(&vm, ins->dst, ds_value_int(out));
                        } else if (left->kind == DS_VALUE_STRING && right->kind == DS_VALUE_STRING) {
                            DsString joined;
                            ds_string_init(&joined);
                            ds_string_append_range(&joined, ds_string_data(&left->as.string), left->as.string.len);
                            ds_string_append_range(&joined, ds_string_data(&right->as.string), right->as.string.len);
                            set_reg(&vm, ins->dst, ds_value_string_take(&joined));
                        } else {
                            ds_diag_error(diag, ins->span, "runtime operator `+` supports int+int or string+string");
                            rc = 1; goto done;
                        }
                        break;
                    case OP_CMP_SUB:
                        if (left->kind != DS_VALUE_INT || right->kind != DS_VALUE_INT) {
                            ds_diag_error(diag, ins->span, "runtime operator `-` requires integer operands");
                            rc = 1; goto done;
                        }
                        {
                            int64_t out = 0;
                            if (!vm_i64_sub_checked(left->as.integer, right->as.integer, &out)) {
                                ds_diag_error(diag, ins->span, "integer overflow in operator `-`");
                                rc = 1; goto done;
                            }
                            set_reg(&vm, ins->dst, ds_value_int(out));
                        }
                        break;
                    case OP_CMP_MUL:
                        if (left->kind != DS_VALUE_INT || right->kind != DS_VALUE_INT) {
                            ds_diag_error(diag, ins->span, "runtime arithmetic operator `*` requires integer operands");
                            rc = 1; goto done;
                        }
                        {
                            int64_t out = 0;
                            if (!vm_i64_mul_checked(left->as.integer, right->as.integer, &out)) {
                                ds_diag_error(diag, ins->span, "integer overflow in operator `*`");
                                rc = 1; goto done;
                            }
                            set_reg(&vm, ins->dst, ds_value_int(out));
                        }
                        break;
                    case OP_CMP_DIV:
                        if (left->kind != DS_VALUE_INT || right->kind != DS_VALUE_INT) {
                            ds_diag_error(diag, ins->span, "runtime arithmetic operator `/` requires integer operands");
                            rc = 1; goto done;
                        }
                        if (!check_div_zero_and_overflow(diag, ins->span, left->as.integer, right->as.integer, "/"))
                        { rc = 1; goto done; }
                        set_reg(&vm, ins->dst, ds_value_int(left->as.integer / right->as.integer));
                        break;
                    case OP_CMP_MOD:
                        if (left->kind != DS_VALUE_INT || right->kind != DS_VALUE_INT) {
                            ds_diag_error(diag, ins->span, "runtime arithmetic operator `%%` requires integer operands");
                            rc = 1; goto done;
                        }
                        if (!check_div_zero_and_overflow(diag, ins->span, left->as.integer, right->as.integer, "%"))
                        { rc = 1; goto done; }
                        set_reg(&vm, ins->dst, ds_value_int(left->as.integer % right->as.integer));
                        break;
                    case OP_CMP_POW:
                        if (left->kind != DS_VALUE_INT || right->kind != DS_VALUE_INT) {
                            ds_diag_error(diag, ins->span, "runtime arithmetic operator `**` requires integer operands");
                            rc = 1; goto done;
                        }
                        if (right->as.integer < 0) {
                            ds_diag_error(diag, ins->span, "negative exponent runtime value is rejected in v0.21.0");
                            rc = 1; goto done;
                        }
                        {
                            int64_t out = 0;
                            if (!vm_i64_pow_checked(left->as.integer, right->as.integer, &out)) {
                                ds_diag_error(diag, ins->span, "integer overflow in operator `**`");
                                rc = 1; goto done;
                            }
                            set_reg(&vm, ins->dst, ds_value_int(out));
                        }
                        break;
                    default:
                        ds_diag_error(diag, ins->span, "internal VM invariant failed: unknown binary operator `%s` after lowering", op_cmp_name(ins->cmp_enum));
                        rc = 1; goto done;
                }
                ip++;
                break;
            }
            case OP_COMPARE: {
                bool same_kind = vm.regs[ins->a].kind == vm.regs[ins->b].kind;
                int cmp = ds_value_compare(&vm.regs[ins->a], &vm.regs[ins->b]);
                bool result = false;
                switch (ins->cmp_enum) {
                    case OP_CMP_EQ_EQ_EQ: result = same_kind && cmp == 0; break;
                    case OP_CMP_NE_EQ:    result = !same_kind || cmp != 0; break;
                    case OP_CMP_EQ_EQ:    result = cmp == 0; break;
                    case OP_CMP_NE:       result = cmp != 0; break;
                    case OP_CMP_GT:       result = cmp > 0; break;
                    case OP_CMP_GE:       result = cmp >= 0; break;
                    case OP_CMP_LT:       result = cmp < 0; break;
                    case OP_CMP_LE:       result = cmp <= 0; break;
                    default: break;
                }
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
                DsValue *pattern_value = &vm.regs[ins->b];
                if (text->kind != DS_VALUE_STRING) { ds_diag_error(diag, ins->span, "internal VM regex invariant failed: accepted `matches` left operand must be a string"); rc = 1; goto done; }
                if (pattern_value->kind != DS_VALUE_STRING) { ds_diag_error(diag, ins->span, "runtime right operand of `matches` must be a regex pattern string"); rc = 1; goto done; }
                DsStr pattern = {pattern_value->as.string.data, pattern_value->as.string.len};
                DsRegexStatus status = ds_regex_validate_pattern(pattern, NULL);
                if (status != DS_REGEX_OK) { ds_diag_error(diag, ins->span, "%s", ds_regex_status_message(status)); rc = 1; goto done; }
                int flags = REG_EXTENDED | (ins->regex_case_insensitive ? REG_ICASE : 0);
                regex_t re;
                char *tmp = ds_str_dup_range(ds_str_data(pattern), pattern.len);
                int err = regcomp(&re, tmp, flags);
                free(tmp);
                if (err != 0) { ds_diag_error(diag, ins->span, "invalid regex pattern in v0.32.0"); rc = 1; goto done; }
                int match = regexec(&re, ds_string_data(&text->as.string), 0, NULL, 0);
                regfree(&re);
                if (match != 0 && match != REG_NOMATCH) { ds_diag_error(diag, ins->span, "failed to evaluate regex in v0.32.0"); rc = 1; goto done; }
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
                    ds_string_append_range(&rendered, ds_string_data(&piece), piece.len);
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
                bool truth = reg_is_truthy(&vm, ins->a);
                ip = truth ? ip + 1 : (size_t)ins->target;
                break;
            }
            case OP_PUSH_SCOPE:
                if (!vm_push_scope(&vm)) { ds_diag_error(diag, ins->span, "failed to initialize runtime scope"); rc = 1; goto done; }
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
                    cleanup_signal = resolve_cleanup_signal(&vm);
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
                DsValue array = ds_value_array();
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
                DsValue map;
                if (!ds_value_map_init(&map)) { ds_diag_error(diag, ins->span, "failed to initialize map"); rc = 1; goto done; }
                for (size_t i = 0; i < ins->arg_count; i++) {
                    if (!ds_map_set(&map.as.map, ins->words[i], ds_value_copy(&vm.regs[ins->args[i]]))) {
                        ds_value_free(&map);
                        ds_diag_error(diag, ins->span, "failed to initialize map entry");
                        rc = 1;
                        goto done;
                    }
                }
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
                    DsStr key_view = {key.data, key.len};
                    DsValue *found = ds_map_get(&obj->as.map, key_view);
                    if (!found) { ds_diag_error(diag, ins->span, "missing map key `%.*s`", (int)key_view.len, key_view.data); ds_string_free(&key); rc = 1; goto done; }
                    set_reg(&vm, ins->dst, ds_value_copy(found));
                    ds_string_free(&key);
                } else { ds_diag_error(diag, ins->span, "internal VM invariant failed: index receiver should be an array or map after lowering"); rc = 1; goto done; }
                ip++;
                break;
            }
            case OP_SET_INDEX: {
                DsValue *obj = lookup_var_ref(&vm, ins->name);
                DsValue *idx = &vm.regs[ins->a];
                DsValue *value = &vm.regs[ins->b];
                if (!obj) { ds_diag_error(diag, ins->span, "internal VM invariant failed: index assignment target `%s` should exist after lowering", ins->name); rc = 1; goto done; }
                if (obj->kind == DS_VALUE_ARRAY) {
                    if (idx->kind != DS_VALUE_INT) { ds_diag_error(diag, ins->span, "runtime array index must be an int"); rc = 1; goto done; }
                    if (idx->as.integer < 0 || (size_t)idx->as.integer >= obj->as.array.len) { ds_diag_error(diag, ins->span, "array index %lld out of range", (long long)idx->as.integer); rc = 1; goto done; }
                    DsValue *slot = (DsValue *)obj->as.array.items[idx->as.integer];
                    ds_value_free(slot);
                    *slot = ds_value_copy(value);
                } else if (obj->kind == DS_VALUE_MAP) {
                    if (idx->kind != DS_VALUE_STRING) { ds_diag_error(diag, ins->span, "runtime map key must be a string"); rc = 1; goto done; }
                    DsStr key = {idx->as.string.data, idx->as.string.len};
                    if (key.len == 0) { ds_diag_error(diag, ins->span, "map key must be non-empty"); rc = 1; goto done; }
                    if (!ds_map_set(&obj->as.map, key, ds_value_copy(value))) { ds_diag_error(diag, ins->span, "failed to set map key `%.*s`", (int)key.len, key.data); rc = 1; goto done; }
                } else { ds_diag_error(diag, ins->span, "internal VM invariant failed: index assignment target should be an array or map after lowering"); rc = 1; goto done; }
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
                if (!vm_push_scope(&vm)) { ds_diag_error(diag, ins->span, "failed to initialize loop scope"); rc = 1; goto done; }
                DsStr key = {ins->name, strlen(ins->name)};
                if (!ds_map_set(&vm.scope->vars, key, ds_value_copy((DsValue *)iter->as.array.items[ins->loop_index]))) { ds_diag_error(diag, ins->span, "failed to bind loop variable"); rc = 1; goto done; }
                ins->loop_index++;
                ip++;
                break;
            }
            case OP_FOR_MAP: {
                DsValue *iter = &vm.regs[ins->a];
                if (iter->kind != DS_VALUE_MAP) { ds_diag_error(diag, ins->span, "runtime map loop iterable must be a map"); rc = 1; goto done; }
                if (!ins->loop_active) {
                    ins->loop_active = true;
                    ins->loop_index = 0;
                    ds_map_sorted_keys_free(ins->loop_keys, ins->loop_key_count);
                    ins->loop_keys = NULL;
                    ins->loop_key_count = 0;
                    if (!ds_map_sorted_keys(&iter->as.map, &ins->loop_keys, &ins->loop_key_count)) {
                        ds_diag_error(diag, ins->span, "failed to prepare sorted map keys");
                        rc = 1; goto done;
                    }
                }
                if (ins->loop_index >= ins->loop_key_count) {
                    ds_map_sorted_keys_free(ins->loop_keys, ins->loop_key_count);
                    ins->loop_keys = NULL;
                    ins->loop_key_count = 0;
                    ins->loop_active = false;
                    ip = (size_t)ins->target;
                    break;
                }
                DsStr map_key = ins->loop_keys[ins->loop_index];
                DsValue *found = ds_map_get(&iter->as.map, map_key);
                if (!found) { ds_diag_error(diag, ins->span, "runtime map loop key disappeared during iteration"); rc = 1; goto done; }
                if (!vm_push_scope(&vm)) { ds_diag_error(diag, ins->span, "failed to initialize loop scope"); rc = 1; goto done; }
                DsString key_text;
                ds_string_from_range(&key_text, ds_str_data(map_key), map_key.len);
                DsStr key_var = {ins->name, strlen(ins->name)};
                DsStr value_var = {ins->value_name, strlen(ins->value_name)};
                if (!ds_map_set(&vm.scope->vars, key_var, ds_value_string_take(&key_text)) ||
                    !ds_map_set(&vm.scope->vars, value_var, ds_value_copy(found))) {
                    ds_diag_error(diag, ins->span, "failed to bind map loop variables"); rc = 1; goto done;
                }
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
                if (!vm_push_scope(&vm)) { ds_diag_error(diag, ins->span, "failed to initialize loop scope"); rc = 1; goto done; }
                DsStr key = {ins->name, strlen(ins->name)};
                if (!ds_map_set(&vm.scope->vars, key, ds_value_int(ins->loop_current))) { ds_diag_error(diag, ins->span, "failed to bind loop variable"); rc = 1; goto done; }
                ins->loop_current++;
                ip++;
                break;
            }
            case OP_RESET_FOR: {
                if (ins->target >= 0 && (size_t)ins->target < p.instr_len) {
                    Instr *for_ins = &p.instrs[ins->target];
                    if (for_ins->op == OP_FOR_ARRAY || for_ins->op == OP_FOR_MAP || for_ins->op == OP_FOR_RANGE) {
                        for_ins->loop_active = false;
                        for_ins->loop_index = 0;
                        for_ins->loop_current = 0;
                        if (for_ins->op == OP_FOR_MAP) {
                            ds_map_sorted_keys_free(for_ins->loop_keys, for_ins->loop_key_count);
                            for_ins->loop_keys = NULL;
                            for_ins->loop_key_count = 0;
                        }
                    }
                }
                ip++;
                break;
            }
            case OP_ASSERT: {
                bool truth = reg_is_truthy(&vm, ins->a);
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
                VmReturnFrame frame = {0};
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
                DsValue value = ds_value_copy(&vm.regs[ins->a]);
                if (!vm_pop_return(&vm, &frame)) { ds_value_free(&value); rc = 1; goto done; }
                vm_pop_to_scope(&vm, frame.scope);
                if (frame.dst >= 0) set_reg(&vm, frame.dst, value);
                else ds_value_free(&value);
                ip = frame.ip;
                break;
            }
            case OP_RETURN_FUNC: {
                VmReturnFrame frame = {0};
                if (!vm_pop_return(&vm, &frame)) { rc = 1; goto done; }
                vm_pop_to_scope(&vm, frame.scope);
                ip = frame.ip;
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
        cleanup_signal = resolve_cleanup_signal(&vm);
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
    free(vm.returns);
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

int ds_vm_run_test(const DsSource *source, const DsLowerProgram *lowered, const DsLowerTest *test, DsDiag *diag) {
    DsLowerProgram view;
    memset(&view, 0, sizeof(view));
    view.functions = lowered->functions;
    view.span = test->span;
    view.statements.len = 1;
    view.statements.cap = 1;
    view.statements.items = (DsLowerStmt **)&test->body;
    DsVmOptions options = {0};
    options.test_mode = true;
    options.test_name = test->name;
    return ds_vm_run_program_args_options(source, &view, 0, NULL, diag, options);
}

int ds_vm_run(const DsSource *source, const DsAst *ast, DsDiag *diag) {
    DsLowerProgram *lowered = ds_lower_program(ast, diag);
    if (!lowered) return 1;
    int rc = ds_vm_run_program(source, lowered, diag);
    ds_lower_program_free(lowered);
    return rc;
}
