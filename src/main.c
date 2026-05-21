#define _XOPEN_SOURCE 700
#include "backend.h"
#include "cli_program.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>



static void usage(FILE *out) {
    fputs("ds v0.21.0\n\n", out);
    fputs("Usage:\n", out);
    fputs("  ds <file.ds> [args...]\n", out);
    fputs("  ds run <file.ds> [args...]\n", out);
    fputs("  ds run [--trace-cmd] [--trace-vm] <file.ds> [args...]\n", out);
    fputs("  ds test <file.ds>\n", out);
    fputs("  ds tokens <file.ds>\n", out);
    fputs("  ds ast <file.ds>\n", out);
    fputs("  ds check <file.ds> [--warnings-as-errors] [--no-warnings]\n", out);
    fputs("  ds fmt <file.ds> [--check] [--write|-w]\n", out);
    fputs("  ds hir <file.ds>\n", out);
    fputs("  ds bytecode <file.ds>\n", out);
    fputs("  ds emit bash <file.ds> -o <file.sh>\n", out);
}

static int usage_error(const char *message) {
    fprintf(stderr, "error: %s\n\n", message);
    usage(stderr);
    return 1;
}

static bool is_direct_script_arg(const char *arg) {
    return strcmp(arg, "run") != 0 && strcmp(arg, "tokens") != 0 &&
           strcmp(arg, "ast") != 0 && strcmp(arg, "check") != 0 && strcmp(arg, "test") != 0 &&
           strcmp(arg, "fmt") != 0 &&
           strcmp(arg, "hir") != 0 && strcmp(arg, "bytecode") != 0 && strcmp(arg, "emit") != 0;
}

static int cli_run_tests(const char *path) {
    DsCliProgram program;
    if (!ds_cli_load_lower(path, &program)) {
        ds_cli_program_free(&program);
        return 1;
    }
    if (program.lowered->tests.len == 0) {
        fprintf(stderr, "error: no tests found in `%s`\n", path);
        ds_cli_program_free(&program);
        return 1;
    }
    size_t passed = 0;
    size_t failed = 0;
    for (size_t i = 0; i < program.lowered->tests.len; i++) {
        DsLowerTest *test = &program.lowered->tests.items[i];
        program.diag.has_error = false;
        int rc = ds_vm_run_test(&program.source, program.lowered, test, &program.diag);
        if (rc == 0 && !program.diag.has_error) {
            fprintf(stdout, "ok   %.*s\n", (int)test->name.len, test->name.data);
            passed++;
        } else {
            fprintf(stdout, "fail %.*s\n", (int)test->name.len, test->name.data);
            failed++;
        }
    }
    fprintf(stdout, "\n%zu tests, %zu passed, %zu failed\n", passed + failed, passed, failed);
    ds_cli_program_free(&program);
    return failed == 0 ? 0 : 1;
}

static bool looks_like_script_path(const char *arg) {
    size_t len = strlen(arg);
    return strstr(arg, "/") != NULL || (len >= 3 && strcmp(arg + len - 3, ".ds") == 0);
}

static bool write_formatted_file(const char *path, const DsString *formatted) {
    struct stat st;
    mode_t mode = 0644;
    if (stat(path, &st) == 0) mode = st.st_mode & 0777;

    size_t path_len = strlen(path);
    char *tmp = (char *)ds_xcalloc(path_len + 32, 1);
    snprintf(tmp, path_len + 32, "%s.tmp.%ld", path, (long)getpid());
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        fprintf(stderr, "error: failed to write `%s`: %s\n", tmp, strerror(errno));
        free(tmp);
        return false;
    }
    bool ok = formatted->len == 0 || fwrite(formatted->data, 1, formatted->len, out) == formatted->len;
    if (fclose(out) != 0) ok = false;
    if (ok && chmod(tmp, mode) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok) {
        fprintf(stderr, "error: failed to write `%s`: %s\n", path, strerror(errno));
        unlink(tmp);
        free(tmp);
        return false;
    }
    free(tmp);
    return true;
}

static int cli_format(int argc, char **argv) {
    bool check = false;
    bool write = false;
    const char *path = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            check = true;
        } else if (strcmp(argv[i], "--write") == 0 || strcmp(argv[i], "-w") == 0) {
            write = true;
        } else if (strncmp(argv[i], "--", 2) == 0) {
            char message[256];
            snprintf(message, sizeof(message), "unknown fmt flag `%s`", argv[i]);
            return usage_error(message);
        } else if (!path) {
            path = argv[i];
        } else {
            return usage_error("expected `ds fmt [--check] [--write|-w] <file.ds>`");
        }
    }
    if (!path) return usage_error("expected `ds fmt [--check] [--write|-w] <file.ds>`");
    if (check && write) return usage_error("`ds fmt --check --write` is invalid");

    DsCliProgram program;
    if (!ds_cli_load_parse(path, &program)) {
        ds_cli_program_free(&program);
        return 1;
    }
    DsString formatted;
    bool ok = ds_format_source(&program.source, program.ast, &formatted, &program.diag);
    if (!ok) {
        ds_cli_program_free(&program);
        return 1;
    }
    int rc = 0;
    if (check) {
        bool differs = formatted.len != program.source.len;
        if (!differs && memcmp(formatted.data, program.source.data, formatted.len) != 0) differs = true;
        if (differs) {
            fprintf(stderr, "%s: needs formatting\n", path);
            rc = 1;
        }
    } else if (write) {
        if (!write_formatted_file(path, &formatted)) rc = 1;
    } else if (formatted.len > 0) {
        fwrite(formatted.data, 1, formatted.len, stdout);
    }
    ds_string_free(&formatted);
    ds_cli_program_free(&program);
    return rc;
}

static int cli_check(int argc, char **argv) {
    bool warnings_as_errors = false;
    bool no_warnings = false;
    const char *path = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--warnings-as-errors") == 0) {
            warnings_as_errors = true;
        } else if (strcmp(argv[i], "--no-warnings") == 0) {
            no_warnings = true;
        } else if (strncmp(argv[i], "--", 2) == 0) {
            char message[256];
            snprintf(message, sizeof(message), "unknown check flag `%s`", argv[i]);
            return usage_error(message);
        } else if (!path) {
            path = argv[i];
        } else {
            return usage_error("expected a command and <file.ds>");
        }
    }
    if (!path) return usage_error("expected `ds check [--warnings-as-errors] [--no-warnings] <file.ds>`");
    if (warnings_as_errors && no_warnings) return usage_error("`ds check --warnings-as-errors --no-warnings` is invalid");

    DsCliProgram program;
    int rc = ds_cli_load_lower(path, &program) ? 0 : 1;
    if (rc == 0 && !no_warnings) {
        size_t warnings = ds_check_warnings_ast(program.ast, stderr);
        if (warnings_as_errors && warnings > 0) rc = 1;
    }
    ds_cli_program_free(&program);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 1 : 0;
    }

    if (strcmp(argv[1], "emit") == 0) {
        if (argc != 6 || strcmp(argv[2], "bash") != 0 || strcmp(argv[4], "-o") != 0) {
            return usage_error("expected `ds emit bash <file.ds> -o <file.sh>`");
        }
        DsCliProgram program;
        int rc = ds_cli_load_lower(argv[3], &program) ? 0 : 1;
        if (rc == 0 && !ds_emit_bash_program(&program.source, program.lowered, argv[5], &program.diag)) rc = 1;
        ds_cli_program_free(&program);
        if (rc != 0) unlink(argv[5]);
        return rc;
    }

    if (argc >= 2 && is_direct_script_arg(argv[1]) &&
        (access(argv[1], R_OK) == 0 || looks_like_script_path(argv[1]))) {
        DsCliProgram program;
        int rc = ds_cli_load_lower(argv[1], &program) ? ds_vm_run_program_args(&program.source, program.lowered, argc - 2, argv + 2, &program.diag) : 1;
        ds_cli_program_free(&program);
        return rc;
    }

    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) return usage_error("expected `ds run [--trace-cmd] [--trace-vm] <file.ds> [args...]`");
        DsVmOptions options = {0};
        int path_index = 2;
        while (path_index < argc && strncmp(argv[path_index], "--", 2) == 0) {
            if (strcmp(argv[path_index], "--trace-cmd") == 0) options.trace_cmd = true;
            else if (strcmp(argv[path_index], "--trace-vm") == 0) options.trace_vm = true;
            else {
                char message[256];
                snprintf(message, sizeof(message), "unknown run flag `%s`", argv[path_index]);
                return usage_error(message);
            }
            path_index++;
        }
        if (path_index >= argc) return usage_error("expected script path after `ds run` flags");
        DsCliProgram program;
        int rc = ds_cli_load_lower(argv[path_index], &program) ? ds_vm_run_program_args_options(&program.source, program.lowered, argc - path_index - 1, argv + path_index + 1, &program.diag, options) : 1;
        ds_cli_program_free(&program);
        return rc;
    }

    if (strcmp(argv[1], "test") == 0) {
        if (argc != 3) return usage_error("expected `ds test <file.ds>`");
        return cli_run_tests(argv[2]);
    }

    if (strcmp(argv[1], "fmt") == 0) {
        return cli_format(argc, argv);
    }

    if (strcmp(argv[1], "check") == 0) {
        if (argc == 2) return usage_error("expected a command and <file.ds>");
        return cli_check(argc, argv);
    }

    if (argc != 3) {
        return usage_error("expected a command and <file.ds>");
    }

    const char *cmd = argv[1];
    const char *path = argv[2];
    DsCliProgram program;

    if (strcmp(cmd, "tokens") == 0) {
        int rc = ds_cli_load_and_lex(path, &program) ? 0 : 1;
        if (rc == 0) ds_tokens_print(&program.tokens, stdout);
        ds_cli_program_free(&program);
        return rc;
    }

    if (strcmp(cmd, "ast") == 0) {
        int rc = ds_cli_load_parse(path, &program) ? 0 : 1;
        if (rc == 0) ds_ast_print(program.ast, stdout);
        ds_cli_program_free(&program);
        return rc;
    }

    if (strcmp(cmd, "hir") == 0) {
        int rc = ds_cli_load_lower(path, &program) ? 0 : 1;
        if (rc == 0 && !ds_hir_dump_program(program.lowered, stdout)) rc = 1;
        ds_cli_program_free(&program);
        return rc;
    }

    if (strcmp(cmd, "bytecode") == 0) {
        int rc = ds_cli_load_lower(path, &program) ? 0 : 1;
        if (rc == 0 && !ds_bytecode_dump_program(&program.source, program.lowered, stdout, &program.diag)) rc = 1;
        ds_cli_program_free(&program);
        return rc;
    }

    fprintf(stderr, "error: unknown command `%s`\n\n", cmd);
    usage(stderr);
    return 1;
}
