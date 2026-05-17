#include "ds.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    DsSource source;
    DsTokenVec tokens;
    DsAst *ast;
    DsLowerProgram *lowered;
    DsDiag diag;
} CliProgram;

static void usage(FILE *out) {
    fputs("ds v0.5.0\n\n", out);
    fputs("Usage:\n", out);
    fputs("  ds <file.ds> [args...]\n", out);
    fputs("  ds run <file.ds> [args...]\n", out);
    fputs("  ds tokens <file.ds>\n", out);
    fputs("  ds ast <file.ds>\n", out);
    fputs("  ds check <file.ds>\n", out);
    fputs("  ds bytecode <file.ds>\n", out);
    fputs("  ds emit bash <file.ds> -o <file.sh>\n", out);
}

static int usage_error(const char *message) {
    fprintf(stderr, "error: %s\n\n", message);
    usage(stderr);
    return 1;
}

static void cli_program_free(CliProgram *program) {
    ds_lower_program_free(program->lowered);
    ds_ast_free(program->ast);
    ds_tokens_free(&program->tokens);
    ds_source_free(&program->source);
}

static bool cli_load_source(const char *path, CliProgram *program) {
    memset(program, 0, sizeof(*program));
    ds_diag_init(&program->diag, &program->source);
    if (!ds_source_read(path, &program->source, &program->diag)) return false;
    ds_diag_init(&program->diag, &program->source);
    return true;
}

static bool cli_load_and_lex(const char *path, CliProgram *program) {
    if (!cli_load_source(path, program)) return false;
    return ds_lex(&program->source, &program->tokens, &program->diag);
}

static bool cli_load_parse(const char *path, CliProgram *program) {
    if (!cli_load_and_lex(path, program)) return false;
    program->ast = ds_parse(&program->tokens, &program->diag);
    return !program->diag.has_error;
}

static bool cli_load_lower(const char *path, CliProgram *program) {
    if (!cli_load_parse(path, program)) return false;
    program->lowered = ds_lower_program(program->ast, &program->diag);
    return program->lowered != NULL;
}

static bool is_direct_script_arg(const char *arg) {
    return strcmp(arg, "run") != 0 && strcmp(arg, "tokens") != 0 &&
           strcmp(arg, "ast") != 0 && strcmp(arg, "check") != 0 &&
           strcmp(arg, "bytecode") != 0 && strcmp(arg, "emit") != 0;
}

static bool looks_like_script_path(const char *arg) {
    size_t len = strlen(arg);
    return strstr(arg, "/") != NULL || (len >= 3 && strcmp(arg + len - 3, ".ds") == 0);
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
        CliProgram program;
        int rc = cli_load_lower(argv[3], &program) ? 0 : 1;
        if (rc == 0 && !ds_emit_bash_program(&program.source, program.lowered, argv[5], &program.diag)) rc = 1;
        cli_program_free(&program);
        return rc;
    }

    if (argc >= 2 && is_direct_script_arg(argv[1]) &&
        (argc == 2 || access(argv[1], R_OK) == 0 || looks_like_script_path(argv[1]))) {
        CliProgram program;
        int rc = cli_load_lower(argv[1], &program) ? ds_vm_run_program_args(&program.source, program.lowered, argc - 2, argv + 2, &program.diag) : 1;
        cli_program_free(&program);
        return rc;
    }

    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) return usage_error("expected `ds run <file.ds> [args...]`");
        CliProgram program;
        int rc = cli_load_lower(argv[2], &program) ? ds_vm_run_program_args(&program.source, program.lowered, argc - 3, argv + 3, &program.diag) : 1;
        cli_program_free(&program);
        return rc;
    }

    if (argc != 3) {
        return usage_error("expected a command and <file.ds>");
    }

    const char *cmd = argv[1];
    const char *path = argv[2];
    CliProgram program;

    if (strcmp(cmd, "tokens") == 0) {
        int rc = cli_load_and_lex(path, &program) ? 0 : 1;
        if (rc == 0) ds_tokens_print(&program.tokens, stdout);
        cli_program_free(&program);
        return rc;
    }

    if (strcmp(cmd, "ast") == 0) {
        int rc = cli_load_parse(path, &program) ? 0 : 1;
        if (rc == 0) ds_ast_print(program.ast, stdout);
        cli_program_free(&program);
        return rc;
    }

    if (strcmp(cmd, "check") == 0) {
        int rc = cli_load_lower(path, &program) ? 0 : 1;
        cli_program_free(&program);
        return rc;
    }

    if (strcmp(cmd, "bytecode") == 0) {
        int rc = cli_load_lower(path, &program) ? 0 : 1;
        if (rc == 0 && !ds_bytecode_dump_program(&program.source, program.lowered, stdout, &program.diag)) rc = 1;
        cli_program_free(&program);
        return rc;
    }

    fprintf(stderr, "error: unknown command `%s`\n\n", cmd);
    usage(stderr);
    return 1;
}
