#define _XOPEN_SOURCE 700
#include "ds.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    DsSource source;
    DsTokenVec tokens;
    DsAst *ast;
} LoadedUnit;

typedef struct {
    DsSource source;
    DsTokenVec tokens;
    DsAst *ast;
    DsLowerProgram *lowered;
    DsDiag diag;
    LoadedUnit **units;
    size_t units_len;
    size_t units_cap;
    char **loaded_paths;
    size_t loaded_len;
    size_t loaded_cap;
    char **stack;
    size_t stack_len;
    size_t stack_cap;
} CliProgram;

static void usage(FILE *out) {
    fputs("ds v0.6.0\n\n", out);
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

static void free_string_list(char **items, size_t len) {
    for (size_t i = 0; i < len; i++) free(items[i]);
    free(items);
}

static void cli_program_free(CliProgram *program) {
    ds_lower_program_free(program->lowered);
    ds_ast_free(program->ast);
    ds_tokens_free(&program->tokens);
    if (program->units_len == 0) ds_source_free(&program->source);
    for (size_t i = 0; i < program->units_len; i++) {
        ds_ast_free(program->units[i]->ast);
        ds_tokens_free(&program->units[i]->tokens);
        ds_source_free(&program->units[i]->source);
        free((char *)program->units[i]->source.path);
        free(program->units[i]);
    }
    free(program->units);
    free_string_list(program->loaded_paths, program->loaded_len);
    free_string_list(program->stack, program->stack_len);
}

static void stmt_vec_push_main(DsStmtVec *vec, DsStmt *stmt) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 16;
        vec->items = (DsStmt **)ds_xrealloc(vec->items, vec->cap * sizeof(DsStmt *));
    }
    vec->items[vec->len++] = stmt;
}

static void script_decl_vec_push_main(DsScriptDeclVec *vec, DsScriptDecl decl) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsScriptDecl *)ds_xrealloc(vec->items, vec->cap * sizeof(DsScriptDecl));
    }
    vec->items[vec->len++] = decl;
}

static void units_push(CliProgram *program, LoadedUnit *unit) {
    if (program->units_len == program->units_cap) {
        program->units_cap = program->units_cap ? program->units_cap * 2 : 8;
        program->units = (LoadedUnit **)ds_xrealloc(program->units, program->units_cap * sizeof(LoadedUnit *));
    }
    program->units[program->units_len++] = unit;
}

static void string_push(char ***items, size_t *len, size_t *cap, char *value) {
    if (*len == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *items = (char **)ds_xrealloc(*items, *cap * sizeof(char *));
    }
    (*items)[(*len)++] = value;
}

static bool string_list_contains(char **items, size_t len, const char *value) {
    for (size_t i = 0; i < len; i++) {
        if (strcmp(items[i], value) == 0) return true;
    }
    return false;
}

static char *normalize_existing_path(const char *path) {
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) return ds_str_dup_range(resolved, strlen(resolved));
    return NULL;
}

static char *dir_name_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return ds_str_dup_range(".", 1);
    if (slash == path) return ds_str_dup_range("/", 1);
    return ds_str_dup_range(path, (size_t)(slash - path));
}

static char *join_path(const char *dir, const char *rel) {
    if (rel[0] == '/') return ds_str_dup_range(rel, strlen(rel));
    while (rel[0] == '.' && rel[1] == '/') rel += 2;
    size_t dlen = strlen(dir);
    size_t rlen = strlen(rel);
    bool need_slash = dlen > 0 && dir[dlen - 1] != '/';
    char *out = (char *)ds_xcalloc(dlen + (need_slash ? 1 : 0) + rlen + 1, 1);
    memcpy(out, dir, dlen);
    size_t pos = dlen;
    if (need_slash) out[pos++] = '/';
    memcpy(out + pos, rel, rlen);
    return out;
}

static bool decode_import_path(DsStr literal, char **out) {
    *out = NULL;
    if (literal.len < 2 || literal.data[0] != '"' || literal.data[literal.len - 1] != '"') return false;
    char *buf = (char *)ds_xcalloc(literal.len, 1);
    size_t len = 0;
    for (size_t i = 1; i + 1 < literal.len; i++) {
        char c = literal.data[i];
        if (c == '\\' && i + 1 < literal.len - 1) {
            char escaped = literal.data[++i];
            if (escaped == 'n') c = '\n';
            else if (escaped == 't') c = '\t';
            else if (escaped == '"') c = '"';
            else if (escaped == '\\') c = '\\';
            else c = escaped;
        }
        buf[len++] = c;
    }
    buf[len] = '\0';
    *out = buf;
    return true;
}

static void append_import_stack(CliProgram *program, const char *cycle_path, DsString *out) {
    ds_string_init(out);
    for (size_t i = 0; i < program->stack_len; i++) {
        if (i > 0) ds_string_append_cstr(out, " -> ");
        ds_string_append_cstr(out, program->stack[i]);
    }
    if (program->stack_len > 0) ds_string_append_cstr(out, " -> ");
    ds_string_append_cstr(out, cycle_path);
}

static void move_script_block(DsAst *dest, DsAst *src, bool is_root, DsDiag *diag) {
    if (!src->has_script) return;
    if (!is_root) {
        ds_diag_error(diag, src->script.span, "imported files cannot declare `script` blocks in v0.6.0");
        return;
    }
    if (dest->has_script) {
        ds_diag_error(diag, src->script.span, "duplicate `script` block");
        return;
    }
    dest->has_script = true;
    dest->script.span = src->script.span;
    for (size_t i = 0; i < src->script.declarations.len; i++) script_decl_vec_push_main(&dest->script.declarations, src->script.declarations.items[i]);
    free(src->script.declarations.items);
    src->script.declarations.items = NULL;
    src->script.declarations.len = 0;
    src->script.declarations.cap = 0;
    src->has_script = false;
}

static bool load_composed_file(CliProgram *program, const char *path, DsSpan import_span, bool is_root, DsAst *composed);

static bool process_ast_statements(CliProgram *program, LoadedUnit *unit, bool is_root, DsAst *composed) {
    move_script_block(composed, unit->ast, is_root, &program->diag);
    for (size_t i = 0; i < unit->ast->statements.len; i++) {
        DsStmt *stmt = unit->ast->statements.items[i];
        if (stmt->kind == DS_STMT_IMPORT) {
            char *import_rel = NULL;
            if (!decode_import_path(stmt->as.import_stmt.path, &import_rel)) {
                ds_diag_error(&program->diag, stmt->span, "invalid import path");
                continue;
            }
            char *dir = dir_name_dup(unit->source.path ? unit->source.path : ".");
            char *joined = join_path(dir, import_rel);
            bool loaded = load_composed_file(program, joined, stmt->span, false, composed);
            if (!loaded && !program->diag.has_error) {
                ds_diag_error(&program->diag, stmt->span, "failed to load imported file `%s`", joined);
            }
            free(joined);
            free(dir);
            free(import_rel);
            /* keep import stmt owned by source AST; it is not part of composed program */
        } else {
            stmt_vec_push_main(&composed->statements, stmt);
            unit->ast->statements.items[i] = NULL;
        }
    }
    unit->ast->statements.len = 0;
    return !program->diag.has_error;
}

static bool load_composed_file(CliProgram *program, const char *path, DsSpan import_span, bool is_root, DsAst *composed) {
    char *normalized = normalize_existing_path(path);
    if (!normalized) {
        if (is_root) {
            ds_source_read(path, &program->source, &program->diag);
        } else {
            ds_diag_error(&program->diag, import_span, "failed to open imported file `%s`: %s", path, strerror(errno));
        }
        return false;
    }

    for (size_t i = 0; i < program->stack_len; i++) {
        if (strcmp(program->stack[i], normalized) == 0) {
            DsString stack;
            append_import_stack(program, normalized, &stack);
            ds_diag_error(&program->diag, import_span, "import cycle detected: %s", stack.data ? stack.data : normalized);
            ds_string_free(&stack);
            free(normalized);
            return false;
        }
    }

    if (string_list_contains(program->loaded_paths, program->loaded_len, normalized)) {
        free(normalized);
        return true;
    }

    string_push(&program->loaded_paths, &program->loaded_len, &program->loaded_cap, ds_str_dup_range(normalized, strlen(normalized)));
    string_push(&program->stack, &program->stack_len, &program->stack_cap, ds_str_dup_range(normalized, strlen(normalized)));

    LoadedUnit *unit = (LoadedUnit *)ds_xcalloc(1, sizeof(LoadedUnit));
    char *owned_path = ds_str_dup_range(path, strlen(path));
    ds_diag_init(&program->diag, &unit->source);
    if (!ds_source_read(owned_path, &unit->source, &program->diag)) {
        if (!is_root) ds_diag_error(&program->diag, import_span, "failed to read imported file `%s`", path);
        free(owned_path);
        free(unit);
        free(program->stack[--program->stack_len]);
        free(normalized);
        return false;
    }
    unit->source.path = owned_path;
    ds_diag_init(&program->diag, &unit->source);
    bool ok = ds_lex(&unit->source, &unit->tokens, &program->diag);
    if (ok) {
        unit->ast = ds_parse(&unit->tokens, &program->diag);
        ok = !program->diag.has_error;
    }
    if (ok && is_root) {
        composed->span = unit->ast->span;
        program->source = unit->source;
    }
    if (ok) process_ast_statements(program, unit, is_root, composed);
    units_push(program, unit);

    free(program->stack[--program->stack_len]);
    free(normalized);
    return !program->diag.has_error;
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

static bool cli_load_composed_parse(const char *path, CliProgram *program) {
    memset(program, 0, sizeof(*program));
    ds_diag_init(&program->diag, NULL);
    DsAst *composed = (DsAst *)ds_xcalloc(1, sizeof(DsAst));
    bool ok = load_composed_file(program, path, (DsSpan){{0, 1, 1}, {0, 1, 1}, NULL}, true, composed);
    if (!ok || program->diag.has_error) {
        ds_ast_free(composed);
        return false;
    }
    program->ast = composed;
    return true;
}

static bool cli_load_lower(const char *path, CliProgram *program) {
    if (!cli_load_composed_parse(path, program)) return false;
    ds_diag_init(&program->diag, &program->source);
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
        (access(argv[1], R_OK) == 0 || looks_like_script_path(argv[1]))) {
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
