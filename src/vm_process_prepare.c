#include "vm_process_internal.h"

#include <stdio.h>
#include <string.h>

/* Prepare accepted command bytecode for process execution without interpreting source syntax. */

static void print_trace_escaped(FILE *out, const char *data) {
    fputc('"', out);
    const char *text = data ? data : "";
    ds_fprint_escaped(out, text, strlen(text), DS_ESCAPE_BASIC);
    fputc('"', out);
}
static bool value_reg_to_cstr(Vm *vm, int reg, DsSpan span, char **out) {
    if (reg < 0) {
        ds_diag_error(vm->diag, span, "internal VM command-value invariant failed: missing value register");
        return false;
    }
    DsString rendered;
    ds_value_to_string(&vm->regs[reg], &rendered);
    *out = ds_str_dup_range(ds_string_data(&rendered), rendered.len);
    ds_string_free(&rendered);
    return true;
}
static bool word_to_arg(Vm *vm, DsStr literal_word, int value_reg, DsSpan span, char **out) {
    if (value_reg >= 0) return value_reg_to_cstr(vm, value_reg, span, out);
    *out = ds_str_dup_range(literal_word.data, literal_word.len);
    return true;
}
static void argv_free(VmArgv *argv) {
    ds_free_cstr_array(argv->items, argv->len);
    *argv = (VmArgv){0};
}
static bool argv_build_range(Vm *vm, Instr *ins, size_t first_word, size_t word_count, VmArgv *argv) {
    *argv = (VmArgv){0};
    if (word_count == 0) return false;
    argv->items = (char **)ds_xcalloc(word_count + 1, sizeof(char *));
    argv->len = word_count;
    for (size_t i = 0; i < word_count; i++) {
        if (!word_to_arg(vm, ins->word_literals[first_word + i], ins->args[first_word + i],
                         ins->span, &argv->items[i])) {
            argv->len = i;
            argv_free(argv);
            return false;
        }
    }
    return true;
}
bool vm_process_redirect_path_from_instr(Vm *vm, Instr *ins, char **out) {
    *out = NULL;
    if (ins->redirect.kind == DS_REDIRECT_NONE) return true;
    if (ins->redirect_reg >= 0) {
        return value_reg_to_cstr(vm, ins->redirect_reg, ins->redirect.target_span, out);
    }
    *out = ds_str_dup_range(ins->redirect_literal.data, ins->redirect_literal.len);
    return true;
}
bool vm_process_spec_from_instr(Vm *vm, Instr *ins, bool capture, VmProcessSpec *spec) {
    memset(spec, 0, sizeof(*spec));
    spec->span = ins->span;
    spec->redirect = ins->redirect;
    spec->capture = capture;
    if (!argv_build_range(vm, ins, 0, ins->word_count, &spec->argv)) return false;
    if (!vm_process_redirect_path_from_instr(vm, ins, &spec->redirect_path)) {
        argv_free(&spec->argv);
        return false;
    }
    return true;
}
bool vm_process_spec_from_stage(Vm *vm, Instr *ins, size_t stage_index, bool capture, VmProcessSpec *spec) {
    memset(spec, 0, sizeof(*spec));
    spec->span = ins->span;
    ds_redirect_init(&spec->redirect);
    spec->capture = capture;
    size_t first = 0;
    for (size_t i = 0; i < stage_index; i++) first += ins->stage_word_counts[i];
    return argv_build_range(vm, ins, first, ins->stage_word_counts[stage_index], &spec->argv);
}
static bool parse_exit_code_arg(const char *text, int *out) {
    if (!text || !*text) return false;
    return ds_parse_int_range((DsStr){(char *)text, strlen(text)}, 0, 255, out);
}
static void append_test_helper_message(DsString *out, const VmProcessSpec *spec, size_t first_arg) {
    ds_string_init(out);
    for (size_t i = first_arg; i < spec->argv.len; i++) {
        if (i > first_arg) ds_string_append_char(out, ' ');
        ds_string_append_cstr(out, spec->argv.items[i]);
    }
}
bool vm_process_run_control_command(Vm *vm, const VmProcessSpec *spec, int *out_code) {
    *out_code = 0;
    if (spec->capture || spec->argv.len == 0) return false;
    const char *name = spec->argv.items[0];
    if (strcmp(name, "fail") != 0 && strcmp(name, "exit") != 0) return false;

    const bool test_mode = vm->options.test_mode;
    const char *test_name = vm->options.test_name.data ? vm->options.test_name.data : "<test>";
    int test_name_len = (int)vm->options.test_name.len;
    if (test_name_len <= 0) test_name_len = (int)strlen(test_name);

    if (spec->redirect.kind != DS_REDIRECT_NONE) {
        if (test_mode) ds_diag_error(vm->diag, spec->span, "test `%.*s`: `%s` does not support redirection", test_name_len, test_name, name);
        else ds_diag_error(vm->diag, spec->span, "`%s` does not support redirection", name);
        *out_code = 1;
        return true;
    }
    if (strcmp(name, "fail") == 0) {
        DsString message;
        append_test_helper_message(&message, spec, 1);
        if (test_mode) {
            if (message.len > 0) ds_diag_error(vm->diag, spec->span, "test `%.*s`: fail: %.*s", test_name_len, test_name, (int)message.len, message.data);
            else ds_diag_error(vm->diag, spec->span, "test `%.*s`: fail", test_name_len, test_name);
        } else if (message.len > 0) {
            ds_diag_error(vm->diag, spec->span, "%.*s", (int)message.len, message.data);
        } else {
            ds_diag_error(vm->diag, spec->span, "fail");
        }
        ds_string_free(&message);
        *out_code = 1;
        return true;
    }
    if (spec->argv.len != 2) {
        if (test_mode) ds_diag_error(vm->diag, spec->span, "test `%.*s`: `exit` expects exactly one integer code", test_name_len, test_name);
        else ds_diag_error(vm->diag, spec->span, "`exit` expects exactly one integer code");
        *out_code = 1;
        return true;
    }
    int code = 0;
    if (!parse_exit_code_arg(spec->argv.items[1], &code)) {
        if (test_mode) ds_diag_error(vm->diag, spec->span, "test `%.*s`: `exit` code must be an integer from 0 to 255", test_name_len, test_name);
        else ds_diag_error(vm->diag, spec->span, "`exit` code must be an integer from 0 to 255");
        *out_code = 1;
        return true;
    }
    if (test_mode) {
        vm->test_done = true;
        if (code != 0) ds_diag_error(vm->diag, spec->span, "test `%.*s`: exit %d", test_name_len, test_name, code);
    } else {
        vm->control_exit_requested = true;
    }
    *out_code = code;
    return true;
}
void vm_process_spec_free(VmProcessSpec *spec) {
    argv_free(&spec->argv);
    free(spec->redirect_path);
    spec->redirect_path = NULL;
}
void vm_process_trace_spec(Vm *vm, const VmProcessSpec *spec) {
    if (!vm->options.trace_cmd || spec->argv.len == 0) return;
    fprintf(stderr, "trace: cmd %s:%d:%d:", span_path(vm->source, spec->span), spec->span.start.line, spec->span.start.column);
    for (size_t i = 0; i < spec->argv.len; i++) {
        fputc(' ', stderr);
        print_trace_escaped(stderr, spec->argv.items[i]);
    }
    if (spec->redirect.kind != DS_REDIRECT_NONE) {
        const char *op = ds_redirect_shell_op(spec->redirect.kind);
        if (op && spec->redirect_path) {
            fputc(' ', stderr);
            fputs(op, stderr);
            fputc(' ', stderr);
            print_trace_escaped(stderr, spec->redirect_path);
        } else {
            fputs(" <redirect>", stderr);
        }
    }
    fputc('\n', stderr);
}
