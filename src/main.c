#include "ds.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *out) {
    fputs("ds v0.3.0\n\n", out);
    fputs("Usage:\n", out);
    fputs("  ds <file.ds>\n", out);
    fputs("  ds run <file.ds>\n", out);
    fputs("  ds tokens <file.ds>\n", out);
    fputs("  ds ast <file.ds>\n", out);
    fputs("  ds check <file.ds>\n", out);
    fputs("  ds bytecode <file.ds>\n", out);
    fputs("  ds emit bash <file.ds> -o <file.sh>\n", out);
}

static int load_and_lex(const char *path, DsSource *source, DsTokenVec *tokens, DsDiag *diag) {
    ds_diag_init(diag, source);
    if (!ds_source_read(path, source, diag)) return 1;
    ds_diag_init(diag, source);
    if (!ds_lex(source, tokens, diag)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 1 : 0;
    }

    if (argc >= 2 && strcmp(argv[1], "emit") == 0) {
        if (argc != 6 || strcmp(argv[2], "bash") != 0 || strcmp(argv[4], "-o") != 0) {
            fprintf(stderr, "error: expected `ds emit bash <file.ds> -o <file.sh>`\n\n");
            usage(stderr);
            return 1;
        }

        const char *path = argv[3];
        const char *output_path = argv[5];
        DsSource source = {0};
        DsTokenVec tokens = {0};
        DsDiag diag;

        if (load_and_lex(path, &source, &tokens, &diag) != 0) {
            ds_tokens_free(&tokens);
            ds_source_free(&source);
            return 1;
        }

        DsAst *ast = ds_parse(&tokens, &diag);
        int rc = diag.has_error ? 1 : 0;
        if (rc == 0 && !ds_emit_bash(&source, ast, output_path, &diag)) rc = 1;

        ds_ast_free(ast);
        ds_tokens_free(&tokens);
        ds_source_free(&source);
        return rc;
    }

    if (argc == 2 &&
        strcmp(argv[1], "run") != 0 && strcmp(argv[1], "tokens") != 0 &&
        strcmp(argv[1], "ast") != 0 && strcmp(argv[1], "check") != 0 &&
        strcmp(argv[1], "bytecode") != 0) {
        const char *path = argv[1];
        DsSource source = {0};
        DsTokenVec tokens = {0};
        DsDiag diag;

        if (load_and_lex(path, &source, &tokens, &diag) != 0) {
            ds_tokens_free(&tokens);
            ds_source_free(&source);
            return 1;
        }

        DsAst *ast = ds_parse(&tokens, &diag);
        int rc = diag.has_error ? 1 : ds_vm_run(&source, ast, &diag);
        ds_ast_free(ast);
        ds_tokens_free(&tokens);
        ds_source_free(&source);
        return rc;
    }

    if (argc != 3) {
        usage(stderr);
        return 1;
    }

    const char *cmd = argv[1];
    const char *path = argv[2];
    DsSource source = {0};
    DsTokenVec tokens = {0};
    DsDiag diag;

    if (load_and_lex(path, &source, &tokens, &diag) != 0) {
        ds_tokens_free(&tokens);
        ds_source_free(&source);
        return 1;
    }

    if (strcmp(cmd, "tokens") == 0) {
        ds_tokens_print(&tokens, stdout);
        ds_tokens_free(&tokens);
        ds_source_free(&source);
        return 0;
    }

    if (strcmp(cmd, "ast") == 0 || strcmp(cmd, "check") == 0 || strcmp(cmd, "bytecode") == 0 || strcmp(cmd, "run") == 0) {
        DsAst *ast = ds_parse(&tokens, &diag);
        int rc = diag.has_error ? 1 : 0;
        if (strcmp(cmd, "ast") == 0 && rc == 0) {
            ds_ast_print(ast, stdout);
        }
        if (strcmp(cmd, "check") == 0 && rc == 0 && !ds_lower_validate(ast, &diag)) {
            rc = 1;
        }
        if (strcmp(cmd, "bytecode") == 0 && rc == 0 && !ds_bytecode_dump(&source, ast, stdout, &diag)) {
            rc = 1;
        }
        if (strcmp(cmd, "run") == 0 && rc == 0) {
            rc = ds_vm_run(&source, ast, &diag);
        }
        ds_ast_free(ast);
        ds_tokens_free(&tokens);
        ds_source_free(&source);
        return rc;
    }

    fprintf(stderr, "error: unknown command `%s`\n\n", cmd);
    usage(stderr);
    ds_tokens_free(&tokens);
    ds_source_free(&source);
    return 1;
}
