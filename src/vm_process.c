#include "ds_command_facts.h"

#include "vm_internal.h"
#include "ds_interpolation.h"
#include "ds_signal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Trace output
 * ------------------------------------------------------------------------- */

static void print_trace_escaped(FILE *out, const char *data) {
    fputc('"', out);
    const char *text = data ? data : "";
    ds_fprint_escaped(out, text, strlen(text), DS_ESCAPE_BASIC);
    fputc('"', out);
}

/* -------------------------------------------------------------------------
 * Interpolation rendering
 * ------------------------------------------------------------------------- */

static void ascii_transform_string(const DsString *in, DsString *out, DsInterpFormatKind kind) {
    ds_string_init(out);
    size_t a = 0, b = in->len;
    if (kind == DS_INTERP_FORMAT_TRIM) vm_ascii_trim_bounds(in->data, in->len, &a, &b);
    ds_string_append_range(out, in->data ? in->data + a : "", b - a);
    if (kind == DS_INTERP_FORMAT_UPPER || kind == DS_INTERP_FORMAT_LOWER) {
        for (size_t i = 0; i < out->len; i++) {
            if (kind == DS_INTERP_FORMAT_UPPER && out->data[i] >= 'a' && out->data[i] <= 'z') out->data[i] = (char)(out->data[i] - 'a' + 'A');
            if (kind == DS_INTERP_FORMAT_LOWER && out->data[i] >= 'A' && out->data[i] <= 'Z') out->data[i] = (char)(out->data[i] - 'A' + 'a');
        }
    }
}

static void append_padded(DsString *out, const char *data, size_t len, int width, char align) {
    if (width <= (int)len) {
        ds_string_append_range(out, data, len);
        return;
    }
    int pad = width - (int)len;
    int left = 0, right = 0;
    if (align == '<') right = pad;
    else if (align == '>') left = pad;
    else { left = pad / 2; right = pad - left; }
    for (int i = 0; i < left; i++) ds_string_append_char(out, ' ');
    ds_string_append_range(out, data, len);
    for (int i = 0; i < right; i++) ds_string_append_char(out, ' ');
}

static DsInterpValueKind interp_kind_from_value(const DsValue *value) {
    switch (value->kind) {
        case DS_VALUE_BOOL: return DS_INTERP_VALUE_BOOL;
        case DS_VALUE_INT: return DS_INTERP_VALUE_INT;
        case DS_VALUE_STRING: return DS_INTERP_VALUE_STRING;
        case DS_VALUE_COMMAND_RESULT: return DS_INTERP_VALUE_COMMAND_RESULT;
        default: return DS_INTERP_VALUE_UNKNOWN;
    }
}

static bool append_parsed_formatted_value(Vm *vm, DsValue *value,
                                          const DsInterpFormatSpec *parsed,
                                          DsString *out, DsSpan span) {
    if (!ds_interp_format_spec_supports_kind(parsed, interp_kind_from_value(value))) {
        ds_diag_error(vm->diag, span,
                      "internal VM interpolation invariant failed: value kind does not match validated format");
        return false;
    }
    if (parsed->kind == DS_INTERP_FORMAT_UPPER || parsed->kind == DS_INTERP_FORMAT_LOWER ||
        parsed->kind == DS_INTERP_FORMAT_TRIM) {
        DsString rendered;
        ascii_transform_string(&value->as.string, &rendered, parsed->kind);
        ds_string_append_range(out, ds_string_data(&rendered), rendered.len);
        ds_string_free(&rendered);
        return true;
    }
    if (parsed->kind == DS_INTERP_FORMAT_ALIGN_LEFT || parsed->kind == DS_INTERP_FORMAT_ALIGN_RIGHT ||
        parsed->kind == DS_INTERP_FORMAT_ALIGN_CENTER) {
        char align = parsed->kind == DS_INTERP_FORMAT_ALIGN_LEFT ? '<' :
                     parsed->kind == DS_INTERP_FORMAT_ALIGN_RIGHT ? '>' : '^';
        append_padded(out, ds_string_data(&value->as.string), value->as.string.len,
                      parsed->width, align);
        return true;
    }
    char buf[64];
    if (parsed->kind == DS_INTERP_FORMAT_INT_DECIMAL) {
        snprintf(buf, sizeof(buf), "%lld", (long long)value->as.integer);
        size_t len = strlen(buf);
        if (parsed->width <= (int)len) {
            ds_string_append_cstr(out, buf);
            return true;
        }
        int pad = parsed->width - (int)len;
        if (parsed->zero_pad) {
            if (buf[0] == '-') {
                ds_string_append_char(out, '-');
                for (int i = 0; i < pad; i++) ds_string_append_char(out, '0');
                ds_string_append_cstr(out, buf + 1);
                return true;
            }
            for (int i = 0; i < pad; i++) ds_string_append_char(out, '0');
            ds_string_append_cstr(out, buf);
            return true;
        }
        for (int i = 0; i < pad; i++) ds_string_append_char(out, ' ');
        ds_string_append_cstr(out, buf);
        return true;
    }
    int prec = parsed->precision < 0 ? 6 : parsed->precision;
    DsString tmp;
    ds_string_init(&tmp);
    ds_string_appendf(&tmp, "%lld.", (long long)value->as.integer);
    for (int i = 0; i < prec; i++) ds_string_append_char(&tmp, '0');
    if (parsed->width > (int)tmp.len) append_padded(out, tmp.data, tmp.len, parsed->width, '>');
    else ds_string_append_range(out, tmp.data, tmp.len);
    ds_string_free(&tmp);
    return true;
}

bool vm_format_interpolation_value(Vm *vm, DsValue *value,
                                   const DsInterpFormatSpec *spec,
                                   DsString *out, DsSpan span) {
    ds_string_init(out);
    return append_parsed_formatted_value(vm, value, spec, out, span);
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

typedef struct {
    char **items;
    size_t len;
} VmArgv;

typedef struct {
    DsString stdout_text;
    DsString stderr_text;
    int code;
    bool terminated_by_sigpipe;
    bool has_non_sigpipe_failure;
} VmProcessResult;

typedef struct {
    VmArgv argv;
    DsRedirect redirect;
    char *redirect_path;
    DsSpan span;
    bool capture;
    int exec_error_fd;
} VmProcessSpec;

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

/* -------------------------------------------------------------------------
 * Process specs, result storage, redirects, and built-in control commands
 * ------------------------------------------------------------------------- */

static int process_status_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static bool process_status_is_signal(int status, int sig) {
    return WIFSIGNALED(status) && WTERMSIG(status) == sig;
}

static void process_result_init(VmProcessResult *result) {
    *result = (VmProcessResult){0};
}

static void process_result_free(VmProcessResult *result) {
    ds_string_free(&result->stdout_text);
    ds_string_free(&result->stderr_text);
    *result = (VmProcessResult){0};
}

static bool open_redirect_target(Vm *vm, const DsRedirect *redirect,
                                 const char *redirect_path, int *out_fd) {
    *out_fd = -1;
    if (!redirect_path) {
        ds_diag_error(vm->diag, redirect->target_span,
                      "internal VM redirect invariant failed: missing lowered redirect value");
        return false;
    }
    int flags = O_CREAT | O_WRONLY;
    if (redirect->kind == DS_REDIRECT_OUT_APPEND || redirect->kind == DS_REDIRECT_ERR_APPEND || redirect->kind == DS_REDIRECT_ALL_APPEND) flags |= O_APPEND;
    else flags |= O_TRUNC;

    int fd = open(redirect_path, flags, 0666);
    if (fd < 0) {
        ds_diag_error(vm->diag, redirect->target_span, "failed to open redirection target `%s`: %s", redirect_path, strerror(errno));
        return false;
    }
    *out_fd = fd;
    return true;
}

static bool process_spec_from_instr(Vm *vm, Instr *ins, bool capture, VmProcessSpec *spec) {
    memset(spec, 0, sizeof(*spec));
    spec->span = ins->span;
    spec->redirect = ins->redirect;
    spec->capture = capture;
    if (!argv_build_range(vm, ins, 0, ins->word_count, &spec->argv)) return false;
    if (spec->redirect.kind != DS_REDIRECT_NONE) {
        if (ins->redirect_reg >= 0) {
            if (!value_reg_to_cstr(vm, ins->redirect_reg, spec->redirect.target_span, &spec->redirect_path)) {
                argv_free(&spec->argv);
                return false;
            }
        } else {
            spec->redirect_path = ds_str_dup_range(ins->redirect_literal.data, ins->redirect_literal.len);
        }
    }
    return true;
}

static bool process_spec_from_stage(Vm *vm, Instr *ins, size_t stage_index, bool capture, VmProcessSpec *spec) {
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

static bool run_control_command(Vm *vm, const VmProcessSpec *spec, int *out_code) {
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

static void process_spec_free(VmProcessSpec *spec) {
    argv_free(&spec->argv);
    free(spec->redirect_path);
    spec->redirect_path = NULL;
}

static void trace_command_spec(Vm *vm, const VmProcessSpec *spec) {
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

/* -------------------------------------------------------------------------
 * Foreground process groups and signal forwarding
 * ------------------------------------------------------------------------- */

static bool fd_set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static bool vm_command_was_interrupted(const Vm *vm, int code) {
    return vm->interrupted_signal != 0 &&
           code == ds_posix_signal_default_status(vm->interrupted_signal);
}

static bool vm_stdout_is_pipe_like(void) {
    struct stat st;
    if (fstat(STDOUT_FILENO, &st) != 0) return false;
    return S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode);
}

static bool vm_command_is_quiet_broken_pipe(const VmProcessSpec *spec, const VmProcessResult *result) {
    /*
     * v0.34.0 closed-stdout DX: when the script itself is piped into a consumer
     * such as `head`, ordinary output commands may conventionally surface as
     * SIGPIPE/141. The VM has the raw wait status, so keep this classification
     * narrow: only an actual SIGPIPE termination on an uncaptured, unredirected
     * command with a pipe-like stdout is quiet. Captured command results still
     * expose the observed 141 status as data.
     */
    return spec && !spec->capture && spec->redirect.kind == DS_REDIRECT_NONE &&
           result && result->code == 128 + SIGPIPE && result->terminated_by_sigpipe &&
           !result->has_non_sigpipe_failure && vm_stdout_is_pipe_like();
}

static bool vm_pipeline_is_quiet_broken_pipe(const Instr *ins, const VmProcessResult *result) {
    return ins && ins->redirect.kind == DS_REDIRECT_NONE &&
           result && result->code == 128 + SIGPIPE && result->terminated_by_sigpipe &&
           !result->has_non_sigpipe_failure && vm_stdout_is_pipe_like();
}

static void vm_forward_signal_to_child_group(Vm *vm, pid_t pgid, int sig) {
    /*
     * Trap/defer/signal parity boundary: foreground command/pipeline signal
     * handling is VM process execution policy, not language validation. The
     * lowerer has already accepted handler declarations; at runtime the VM only
     * records INT/TERM for cleanup classification and forwards the signal to
     * the foreground child process group when one exists.
     */
    if (!ds_posix_signal_is_runtime_cleanup(sig)) return;
    vm_note_interrupted_signal(vm, sig);
    if (pgid > 0) kill(-pgid, sig);
}

static void vm_note_wait_status_signal(Vm *vm, int status) {
    if (WIFSIGNALED(status)) vm_note_interrupted_signal(vm, WTERMSIG(status));
}

static void vm_restore_terminal_pgrp(int tty_fd, pid_t shell_pgid) {
    if (tty_fd >= 0 && shell_pgid > 0) {
        void (*old_ttou)(int) = signal(SIGTTOU, SIG_IGN);
        tcsetpgrp(tty_fd, shell_pgid);
        signal(SIGTTOU, old_ttou);
    }
}

static bool vm_make_foreground_group(pid_t pgid, int *tty_fd, pid_t *shell_pgid) {
    *tty_fd = -1;
    *shell_pgid = -1;
    if (pgid <= 0) return false;
    setpgid(pgid, pgid);
    if (isatty(STDIN_FILENO)) {
        pid_t current = tcgetpgrp(STDIN_FILENO);
        pid_t self = getpgrp();
        if (current == self) {
            void (*old_ttou)(int) = signal(SIGTTOU, SIG_IGN);
            if (tcsetpgrp(STDIN_FILENO, pgid) == 0) {
                *tty_fd = STDIN_FILENO;
                *shell_pgid = self;
            }
            signal(SIGTTOU, old_ttou);
        }
    }
    return true;
}

static bool vm_wait_child(Vm *vm, pid_t pid, pid_t pgid, DsSpan span, const char *kind,
                          const char *command, int *status) {
    while (waitpid(pid, status, 0) < 0) {
        if (errno == EINTR) {
            int sig = vm_take_pending_signal();
            if (sig) vm_forward_signal_to_child_group(vm, pgid, sig);
            continue;
        }
        ds_diag_error(vm->diag, span, "failed waiting for %s `%s`: %s", kind, command, strerror(errno));
        return false;
    }
    vm_note_wait_status_signal(vm, *status);
    return true;
}

static bool vm_wait_foreground_child(Vm *vm, pid_t pid, pid_t pgid, const VmProcessSpec *spec, int *status) {
    /*
     * Direct foreground commands get their own process group when possible.
     * If the ds runner observes INT/TERM while waiting, forward it to that
     * group and let the VM cleanup dispatcher decide the final handler order
     * and conventional status from the shared signal contract.
     */
    int tty_fd = -1;
    pid_t shell_pgid = -1;
    vm_make_foreground_group(pgid, &tty_fd, &shell_pgid);
    if (!vm_wait_child(vm, pid, pgid, spec->span, "command", spec->argv.items[0], status)) {
        vm_restore_terminal_pgrp(tty_fd, shell_pgid);
        return false;
    }
    vm_restore_terminal_pgrp(tty_fd, shell_pgid);
    return true;
}

static bool process_exec_error_pipe(Vm *vm, DsSpan span, const char *command, int pipe_fds[2]) {
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    if (pipe(pipe_fds) != 0 || !fd_set_cloexec(pipe_fds[1])) {
        if (command) ds_diag_error(vm->diag, span, "failed to prepare command `%s`: %s", command, strerror(errno));
        else ds_diag_error(vm->diag, span, "failed to prepare pipeline exec error pipe: %s", strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        pipe_fds[0] = -1;
        pipe_fds[1] = -1;
        return false;
    }
    return true;
}

static bool process_capture_open(Vm *vm, DsSpan span, const char *kind, FILE **out_fp, FILE **err_fp) {
    *out_fp = tmpfile();
    *err_fp = tmpfile();
    if (*out_fp && *err_fp) return true;
    ds_diag_error(vm->diag, span, "failed to create %s capture temporary files: %s", kind, strerror(errno));
    if (*out_fp) fclose(*out_fp);
    if (*err_fp) fclose(*err_fp);
    *out_fp = NULL;
    *err_fp = NULL;
    return false;
}

static bool process_capture_read(Vm *vm, DsSpan span, const char *kind, FILE *out_fp, FILE *err_fp,
                                 VmProcessResult *result) {
    if (vm_read_stream(out_fp, &result->stdout_text, true, false) == VM_READ_STREAM_OK &&
        vm_read_stream(err_fp, &result->stderr_text, true, false) == VM_READ_STREAM_OK) return true;
    ds_diag_error(vm->diag, span, "failed to read %s capture output", kind);
    return false;
}

static void process_capture_close(FILE **out_fp, FILE **err_fp) {
    if (*out_fp) fclose(*out_fp);
    if (*err_fp) fclose(*err_fp);
    *out_fp = NULL;
    *err_fp = NULL;
}

static void process_child_exec_argv(const VmProcessSpec *spec) {
    execvp(spec->argv.items[0], spec->argv.items);
    int exec_errno = errno;
    if (spec->exec_error_fd >= 0) {
        ssize_t ignored = write(spec->exec_error_fd, &exec_errno, sizeof(exec_errno));
        (void)ignored;
        close(spec->exec_error_fd);
    }
    if (spec->capture) fprintf(stderr, "ds: failed to launch command `%s`: %s\n", spec->argv.items[0], strerror(exec_errno));
    _exit(127);
}

/* -------------------------------------------------------------------------
 * Direct process execution
 * ------------------------------------------------------------------------- */

static void process_child_exec(const VmProcessSpec *spec, int redirect_fd, FILE *out_fp, FILE *err_fp) {
    if (spec->capture) {
        dup2(fileno(out_fp), STDOUT_FILENO);
        dup2(fileno(err_fp), STDERR_FILENO);
    } else if (spec->redirect.kind != DS_REDIRECT_NONE) {
        if (spec->redirect.kind == DS_REDIRECT_OUT || spec->redirect.kind == DS_REDIRECT_OUT_APPEND) dup2(redirect_fd, STDOUT_FILENO);
        else if (spec->redirect.kind == DS_REDIRECT_ERR || spec->redirect.kind == DS_REDIRECT_ERR_APPEND) dup2(redirect_fd, STDERR_FILENO);
        else { dup2(redirect_fd, STDOUT_FILENO); dup2(redirect_fd, STDERR_FILENO); }
    }
    if (redirect_fd >= 0) close(redirect_fd);
    process_child_exec_argv(spec);
}

static bool process_execute(Vm *vm, VmProcessSpec *spec, VmProcessResult *result) {
    process_result_init(result);
    int redirect_fd = -1;
    FILE *out_fp = NULL;
    FILE *err_fp = NULL;
    int exec_error_pipe[2] = {-1, -1};
    bool ok = false;
    spec->exec_error_fd = -1;

    trace_command_spec(vm, spec);

    if (!spec->capture && spec->redirect.kind != DS_REDIRECT_NONE) {
        if (!open_redirect_target(vm, &spec->redirect, spec->redirect_path, &redirect_fd)) goto cleanup;
    }

    if (spec->capture) {
        if (!process_capture_open(vm, spec->span, "command", &out_fp, &err_fp)) goto cleanup;
    }

    if (!process_exec_error_pipe(vm, spec->span, spec->argv.items[0], exec_error_pipe)) goto cleanup;
    spec->exec_error_fd = exec_error_pipe[1];

    pid_t pid = fork();
    if (pid < 0) {
        ds_diag_error(vm->diag, spec->span, "failed to launch command `%s`: %s", spec->argv.items[0], strerror(errno));
        goto cleanup;
    }

    if (pid == 0) {
        setpgid(0, 0);
        close(exec_error_pipe[0]);
        process_child_exec(spec, redirect_fd, out_fp, err_fp);
    }
    setpgid(pid, pid);

    close(exec_error_pipe[1]); exec_error_pipe[1] = -1;
    spec->exec_error_fd = -1;
    if (redirect_fd >= 0) { close(redirect_fd); redirect_fd = -1; }
    int exec_errno = 0;
    ssize_t exec_error_len = read(exec_error_pipe[0], &exec_errno, sizeof(exec_errno));
    close(exec_error_pipe[0]); exec_error_pipe[0] = -1;
    int status = 0;
    if (!vm_wait_foreground_child(vm, pid, pid, spec, &status)) goto cleanup;
    result->code = process_status_code(status);
    result->terminated_by_sigpipe = process_status_is_signal(status, SIGPIPE);

    if (!spec->capture && exec_error_len == (ssize_t)sizeof(exec_errno)) {
        ds_diag_error(vm->diag, spec->span, "failed to launch command `%s`: %s", spec->argv.items[0], strerror(exec_errno));
        ok = true;
        goto cleanup;
    }

    if (spec->capture) {
        if (!process_capture_read(vm, spec->span, "command", out_fp, err_fp, result)) goto cleanup;
    }
    ok = true;

cleanup:
    if (exec_error_pipe[0] >= 0) close(exec_error_pipe[0]);
    if (exec_error_pipe[1] >= 0) close(exec_error_pipe[1]);
    if (redirect_fd >= 0) close(redirect_fd);
    process_capture_close(&out_fp, &err_fp);
    spec->exec_error_fd = -1;
    return ok;
}

/* -------------------------------------------------------------------------
 * Pipeline execution
 * ------------------------------------------------------------------------- */

static void close_pipe_array(int (*pipes)[2], size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (pipes[i][0] >= 0) close(pipes[i][0]);
        if (pipes[i][1] >= 0) close(pipes[i][1]);
        pipes[i][0] = pipes[i][1] = -1;
    }
}

static bool redirect_wants_stdout(DsRedirectKind kind) {
    return kind == DS_REDIRECT_OUT || kind == DS_REDIRECT_OUT_APPEND || kind == DS_REDIRECT_ALL || kind == DS_REDIRECT_ALL_APPEND;
}

static bool redirect_wants_stderr(DsRedirectKind kind) {
    return kind == DS_REDIRECT_ERR || kind == DS_REDIRECT_ERR_APPEND || kind == DS_REDIRECT_ALL || kind == DS_REDIRECT_ALL_APPEND;
}

static void pipeline_child_exec(VmProcessSpec *specs, size_t stage_count, size_t idx, int (*pipes)[2], int redirect_fd, const DsRedirect *pipeline_redirect, FILE *out_fp, FILE *err_fp) {
    VmProcessSpec *spec = &specs[idx];
    if (idx > 0) {
        dup2(pipes[idx - 1][0], STDIN_FILENO);
    } else if (isatty(STDIN_FILENO)) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
    }
    if (idx + 1 < stage_count) {
        dup2(pipes[idx][1], STDOUT_FILENO);
    } else if (spec->capture) {
        dup2(fileno(out_fp), STDOUT_FILENO);
    } else if (redirect_wants_stdout(pipeline_redirect->kind)) {
        dup2(redirect_fd, STDOUT_FILENO);
    }
    if (spec->capture) {
        dup2(fileno(err_fp), STDERR_FILENO);
    } else if (redirect_wants_stderr(pipeline_redirect->kind)) {
        dup2(redirect_fd, STDERR_FILENO);
    }
    close_pipe_array(pipes, stage_count > 0 ? stage_count - 1 : 0);
    if (redirect_fd >= 0) close(redirect_fd);
    process_child_exec_argv(spec);
}

static bool process_execute_pipeline(Vm *vm, Instr *ins, bool capture, VmProcessResult *result) {
    /*
     * Pipeline foreground-signal ownership mirrors direct commands: process
     * code owns process groups, waits, forwarding, and pipefail status; cleanup
     * handler legality and representation remain lowerer/HIR responsibilities.
     */
    process_result_init(result);
    size_t n = ins->stage_count ? ins->stage_count : 1;
    VmProcessSpec *specs = (VmProcessSpec *)ds_xcalloc(n, sizeof(VmProcessSpec));
    pid_t *pids = (pid_t *)ds_xcalloc(n, sizeof(pid_t));
    int *codes = (int *)ds_xcalloc(n, sizeof(int));
    int (*pipes)[2] = (int (*)[2])ds_xcalloc(n > 1 ? n - 1 : 1, sizeof(int[2]));
    int redirect_fd = -1;
    char *pipeline_redirect_path = NULL;
    FILE *out_fp = NULL;
    FILE *err_fp = NULL;
    int exec_error_pipe[2] = {-1, -1};
    bool ok = true;

    for (size_t i = 0; i + 1 < n; i++) pipes[i][0] = pipes[i][1] = -1;
    if (ins->redirect.kind != DS_REDIRECT_NONE) {
        if (ins->redirect_reg >= 0) {
            if (!value_reg_to_cstr(vm, ins->redirect_reg, ins->redirect.target_span, &pipeline_redirect_path)) {
                ok = false;
                goto cleanup;
            }
        } else {
            pipeline_redirect_path = ds_str_dup_range(ins->redirect_literal.data, ins->redirect_literal.len);
        }
    }

    for (size_t i = 0; i < n; i++) {
        if (!process_spec_from_stage(vm, ins, i, capture, &specs[i])) { ok = false; goto cleanup; }
        if (i + 1 == n) {
            specs[i].redirect = ins->redirect;
            if (pipeline_redirect_path) specs[i].redirect_path = ds_str_dup_cstr(pipeline_redirect_path);
        } else {
            ds_redirect_init(&specs[i].redirect);
        }
        trace_command_spec(vm, &specs[i]);
    }

    if (!capture && ins->redirect.kind != DS_REDIRECT_NONE) {
        if (!open_redirect_target(vm, &ins->redirect, pipeline_redirect_path, &redirect_fd)) { ok = false; goto cleanup; }
    }
    if (capture) {
        if (!process_capture_open(vm, ins->span, "pipeline", &out_fp, &err_fp)) { ok = false; goto cleanup; }
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if (pipe(pipes[i]) != 0) {
            ds_diag_error(vm->diag, ins->span, "failed to create pipeline pipe: %s", strerror(errno));
            ok = false;
            goto cleanup;
        }
    }
    if (!process_exec_error_pipe(vm, ins->span, NULL, exec_error_pipe)) { ok = false; goto cleanup; }
    for (size_t i = 0; i < n; i++) specs[i].exec_error_fd = exec_error_pipe[1];

    pid_t pgid = -1;
    for (size_t i = 0; i < n; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            ds_diag_error(vm->diag, ins->span, "failed to launch pipeline stage `%s`: %s", specs[i].argv.len ? specs[i].argv.items[0] : "<stage>", strerror(errno));
            ok = false;
            goto cleanup;
        }
        if (pids[i] == 0) {
            if (i == 0) setpgid(0, 0);
            else if (pgid > 0) setpgid(0, pgid);
            close(exec_error_pipe[0]);
            pipeline_child_exec(specs, n, i, pipes, redirect_fd, &ins->redirect, out_fp, err_fp);
        }
        if (i == 0) pgid = pids[i];
        if (pgid > 0) setpgid(pids[i], pgid);
    }

    close(exec_error_pipe[1]); exec_error_pipe[1] = -1;
    close_pipe_array(pipes, n > 1 ? n - 1 : 0);
    if (redirect_fd >= 0) { close(redirect_fd); redirect_fd = -1; }

    int exec_errno = 0;
    ssize_t exec_error_len = read(exec_error_pipe[0], &exec_errno, sizeof(exec_errno));
    close(exec_error_pipe[0]); exec_error_pipe[0] = -1;

    int tty_fd = -1;
    pid_t shell_pgid = -1;
    if (pgid > 0) vm_make_foreground_group(pgid, &tty_fd, &shell_pgid);
    for (size_t i = 0; i < n; i++) {
        int status = 0;
        const char *command = specs[i].argv.len ? specs[i].argv.items[0] : "<stage>";
        if (!vm_wait_child(vm, pids[i], pgid, ins->span, "pipeline stage", command, &status)) {
            vm_restore_terminal_pgrp(tty_fd, shell_pgid);
            ok = false;
            goto cleanup;
        }
        codes[i] = process_status_code(status);
        if (codes[i] != 0) {
            bool stage_sigpipe = process_status_is_signal(status, SIGPIPE);
            if (!stage_sigpipe) result->has_non_sigpipe_failure = true;
            if (i + 1 == n && stage_sigpipe) result->terminated_by_sigpipe = true;
        }
    }
    vm_restore_terminal_pgrp(tty_fd, shell_pgid);
    result->code = ds_command_pipeline_status(codes, n);
    if (!capture && exec_error_len == (ssize_t)sizeof(exec_errno)) {
        ds_diag_error(vm->diag, ins->span, "failed to launch pipeline command: %s", strerror(exec_errno));
    }
    if (capture) {
        if (!process_capture_read(vm, ins->span, "pipeline", out_fp, err_fp, result)) { ok = false; goto cleanup; }
    }

cleanup:
    if (exec_error_pipe[0] >= 0) close(exec_error_pipe[0]);
    if (exec_error_pipe[1] >= 0) close(exec_error_pipe[1]);
    close_pipe_array(pipes, n > 1 ? n - 1 : 0);
    if (redirect_fd >= 0) close(redirect_fd);
    if (!ok) {
        for (size_t i = 0; i < n; i++) if (pids[i] > 0) waitpid(pids[i], NULL, 0);
    }
    process_capture_close(&out_fp, &err_fp);
    for (size_t i = 0; i < n; i++) process_spec_free(&specs[i]);
    free(pipeline_redirect_path);
    free(specs); free(pids); free(codes); free(pipes);
    return ok;
}

int run_command(Vm *vm, Instr *ins) {
    if (ins->word_count == 0) return 0;
    if ((ins->stage_count ? ins->stage_count : 1) > 1) {
        VmProcessResult result;
        bool ok = process_execute_pipeline(vm, ins, false, &result);
        int code = ok ? result.code : 1;
        if (ok && vm_pipeline_is_quiet_broken_pipe(ins, &result)) {
            vm->control_exit_requested = true;
            code = 0;
        }
        if (ok && code != 0 && !vm->diag->has_error && !vm_command_was_interrupted(vm, code)) ds_diag_error(vm->diag, ins->span, "pipeline failed with exit %d", code);
        process_result_free(&result);
        return code;
    }
    VmProcessSpec spec;
    if (!process_spec_from_instr(vm, ins, false, &spec)) return 1;
    int helper_code = 0;
    if (run_control_command(vm, &spec, &helper_code)) {
        process_spec_free(&spec);
        return helper_code;
    }
    VmProcessResult result;
    bool ok = process_execute(vm, &spec, &result);
    int code = ok ? result.code : 1;
    if (ok && vm_command_is_quiet_broken_pipe(&spec, &result)) {
        vm->control_exit_requested = true;
        code = 0;
    }
    if (ok && code != 0 && !vm->diag->has_error && !vm_command_was_interrupted(vm, code)) {
        ds_diag_error(vm->diag, ins->span, "command `%s` failed with exit %d", spec.argv.len > 0 ? spec.argv.items[0] : "<command>", code);
    }
    process_result_free(&result);
    process_spec_free(&spec);
    return code;
}

int run_capture(Vm *vm, Instr *ins, DsValue *out_value) {
    *out_value = ds_value_null();
    if (ins->word_count == 0) return 1;
    if ((ins->stage_count ? ins->stage_count : 1) > 1) {
        VmProcessResult result;
        bool ok = process_execute_pipeline(vm, ins, true, &result);
        if (!ok) { process_result_free(&result); return 1; }
        *out_value = ds_value_command_result_take(&result.stdout_text, &result.stderr_text, result.code);
        return 0;
    }
    VmProcessSpec spec;
    if (!process_spec_from_instr(vm, ins, true, &spec)) return 1;
    VmProcessResult result;
    bool ok = process_execute(vm, &spec, &result);
    process_spec_free(&spec);
    if (!ok) {
        process_result_free(&result);
        return 1;
    }
    *out_value = ds_value_command_result_take(&result.stdout_text, &result.stderr_text, result.code);
    return 0;
}
