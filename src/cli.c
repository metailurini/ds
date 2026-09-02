#include "cli.h"

#include "ds_common.h"

#include <stdarg.h>
#include <unistd.h>



static void usage(FILE *out) {
    fputs("ds v0.38.0\n\n", out);
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

static int usage_error(const char *fmt, ...) {
    va_list args;
    fputs("error: ", stderr);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputs("\n\n", stderr);
    usage(stderr);
    return 1;
}

typedef struct {
    const char *name;
    bool *value;
} CliBoolFlag;

static int parse_flagged_path(int argc, char **argv, int start, const char *command,
                              const CliBoolFlag *flags, size_t flag_count, bool stop_at_path,
                              const char *usage_message, const char *extra_arg_message,
                              int *path_index) {
    *path_index = -1;
    for (int i = start; i < argc; i++) {
        bool matched = false;
        for (size_t j = 0; j < flag_count; j++) {
            if (strcmp(argv[i], flags[j].name) == 0) {
                *flags[j].value = true;
                matched = true;
                break;
            }
        }
        if (matched) continue;
        if (strncmp(argv[i], "--", 2) == 0) return usage_error("unknown %s flag `%s`", command, argv[i]);
        if (*path_index >= 0) return usage_error("%s", extra_arg_message ? extra_arg_message : usage_message);
        *path_index = i;
        if (stop_at_path) break;
    }
    if (*path_index < 0) return usage_error("%s", usage_message);
    return 0;
}

static bool is_direct_script_arg(const char *arg) {
    return strcmp(arg, "run") != 0 && strcmp(arg, "tokens") != 0 &&
           strcmp(arg, "ast") != 0 && strcmp(arg, "check") != 0 && strcmp(arg, "test") != 0 &&
           strcmp(arg, "fmt") != 0 &&
           strcmp(arg, "hir") != 0 && strcmp(arg, "bytecode") != 0 && strcmp(arg, "emit") != 0;
}

static int cli_run_program(DsApp *app, const char *path, int argc, char **argv, DsVmOptions options) {
    return ds_app_run_program(app, path, argc, argv, options);
}

static int cli_run_tests(DsApp *app, const char *path) {
    return ds_app_run_tests(app, path, stdout, stderr);
}

static int cli_format(DsApp *app, int argc, char **argv) {
    bool check = false;
    bool write = false;
    CliBoolFlag flags[] = {{"--check", &check}, {"--write", &write}, {"-w", &write}};
    int path_index;
    if (parse_flagged_path(argc, argv, 2, "fmt", flags, 3, false,
                           "expected `ds fmt [--check] [--write|-w] <file.ds>`", NULL, &path_index)) return 1;
    if (check && write) return usage_error("`ds fmt --check --write` is invalid");

    DsAppFormatMode mode = check ? DS_APP_FORMAT_CHECK
                                 : write ? DS_APP_FORMAT_WRITE : DS_APP_FORMAT_PRINT;
    return ds_app_format(app, argv[path_index], mode, stdout, stderr);
}

static int cli_inspect(DsApp *app, const char *cmd, const char *path) {
    DsAppInspectKind kind = DS_APP_INSPECT_BYTECODE;
    if (strcmp(cmd, "tokens") == 0) kind = DS_APP_INSPECT_TOKENS;
    else if (strcmp(cmd, "ast") == 0) kind = DS_APP_INSPECT_AST;
    else if (strcmp(cmd, "hir") == 0) kind = DS_APP_INSPECT_HIR;

    return ds_app_inspect(app, path, kind, stdout);
}

static int cli_check(DsApp *app, int argc, char **argv) {
    bool warnings_as_errors = false;
    bool no_warnings = false;
    CliBoolFlag flags[] = {{"--warnings-as-errors", &warnings_as_errors}, {"--no-warnings", &no_warnings}};
    int path_index;
    if (parse_flagged_path(argc, argv, 2, "check", flags, 2, false,
                           "expected `ds check [--warnings-as-errors] [--no-warnings] <file.ds>`",
                           "expected a command and <file.ds>", &path_index)) return 1;
    if (warnings_as_errors && no_warnings) {
        return usage_error("`ds check --warnings-as-errors --no-warnings` is invalid");
    }

    DsAppCheckOptions options = {
        .warnings_as_errors = warnings_as_errors,
        .no_warnings = no_warnings,
    };
    return ds_app_check(app, argv[path_index], options, stderr);
}

int ds_cli_run(DsApp *app, int argc, char **argv) {
    if (!app) return 1;
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 1 : 0;
    }

    if (strcmp(argv[1], "emit") == 0) {
        if (argc != 6 || strcmp(argv[2], "bash") != 0 || strcmp(argv[4], "-o") != 0) {
            return usage_error("expected `ds emit bash <file.ds> -o <file.sh>`");
        }
        return ds_app_emit_bash(app, argv[3], argv[5]);
    }

    if (argc >= 2 && is_direct_script_arg(argv[1]) &&
        (access(argv[1], R_OK) == 0 || ds_path_looks_like_script(argv[1]))) {
        return cli_run_program(app, argv[1], argc - 2, argv + 2, (DsVmOptions){0});
    }

    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) return usage_error("expected `ds run [--trace-cmd] [--trace-vm] <file.ds> [args...]`");
        DsVmOptions options = {0};
        CliBoolFlag flags[] = {{"--trace-cmd", &options.trace_cmd}, {"--trace-vm", &options.trace_vm}};
        int path_index;
        if (parse_flagged_path(argc, argv, 2, "run", flags, 2, true,
                               "expected script path after `ds run` flags", NULL, &path_index)) return 1;
        return cli_run_program(app, argv[path_index], argc - path_index - 1, argv + path_index + 1, options);
    }

    if (strcmp(argv[1], "test") == 0) {
        if (argc != 3) return usage_error("expected `ds test <file.ds>`");
        return cli_run_tests(app, argv[2]);
    }

    if (strcmp(argv[1], "fmt") == 0) {
        return cli_format(app, argc, argv);
    }

    if (strcmp(argv[1], "check") == 0) {
        if (argc == 2) return usage_error("expected a command and <file.ds>");
        return cli_check(app, argc, argv);
    }

    if (argc != 3) {
        return usage_error("expected a command and <file.ds>");
    }

    const char *cmd = argv[1];
    const char *path = argv[2];
    if (strcmp(cmd, "tokens") == 0 || strcmp(cmd, "ast") == 0 ||
        strcmp(cmd, "hir") == 0 || strcmp(cmd, "bytecode") == 0) {
        return cli_inspect(app, cmd, path);
    }

    return usage_error("unknown command `%s`", cmd);
}
