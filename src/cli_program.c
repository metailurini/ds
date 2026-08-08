#include "cli_program.h"

#include <errno.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct LoadedUnit {
    DsSource source;
    DsTokenVec tokens;
    DsAst *ast;
};

void ds_cli_program_free(DsCliProgram *program) {
    if (!program) return;
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
    ds_free_cstr_array(program->loaded_paths, program->loaded_len);
    ds_free_cstr_array(program->stack, program->stack_len);
}

static void import_stack_push(DsCliProgram *program, const char *path) {
    DS_GROW_ARRAY(program->stack, program->stack_len, program->stack_cap, 8);
    program->stack[program->stack_len++] = ds_str_dup_cstr(path);
}

static void import_stack_pop(DsCliProgram *program) {
    if (program->stack_len == 0) return;
    free(program->stack[--program->stack_len]);
}

static bool string_list_contains(char **items, size_t len, const char *value) {
    for (size_t i = 0; i < len; i++) {
        if (strcmp(items[i], value) == 0) return true;
    }
    return false;
}

static char *normalize_existing_path(const char *path) {
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) return ds_str_dup_cstr(resolved);
    return NULL;
}

static char *join_path(const char *dir, const char *rel) {
    if (rel[0] == '/') return ds_str_dup_cstr(rel);
    while (rel[0] == '.' && rel[1] == '/') rel += 2;
    return ds_path_join(dir, rel);
}

static void append_import_stack(DsCliProgram *program, const char *cycle_path, DsString *out) {
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
    for (size_t i = 0; i < src->script.declarations.len; i++) {
        DS_VEC_PUSH(&dest->script.declarations, src->script.declarations.items[i], 8);
    }
    free(src->script.declarations.items);
    src->script.declarations = (DsScriptDeclVec){0};
    src->has_script = false;
}

static bool load_composed_file(DsCliProgram *program, const char *path, DsSpan import_span, bool is_root, DsAst *composed);

static bool process_ast_statements(DsCliProgram *program, LoadedUnit *unit, bool is_root, DsAst *composed) {
    move_script_block(composed, unit->ast, is_root, &program->diag);
    for (size_t i = 0; i < unit->ast->statements.len; i++) {
        DsStmt *stmt = unit->ast->statements.items[i];
        if (stmt->kind == DS_STMT_IMPORT) {
            DsStr import_rel = {0};
            if (!ds_decode_string_literal(stmt->as.import_stmt.path, &import_rel)) {
                ds_diag_error(&program->diag, stmt->span, "invalid import path");
                continue;
            }
            char *dir = ds_path_dirname_dup(unit->source.path ? unit->source.path : ".");
            char *joined = join_path(dir, import_rel.data);
            bool loaded = load_composed_file(program, joined, stmt->span, false, composed);
            if (!loaded && !program->diag.has_error) {
                ds_diag_error(&program->diag, stmt->span, "failed to load imported file `%s`", joined);
            }
            free(joined);
            free(dir);
            free(import_rel.data);
        } else {
            DS_VEC_PUSH(&composed->statements, stmt, 16);
            unit->ast->statements.items[i] = NULL;
        }
    }
    return !program->diag.has_error;
}

static bool load_composed_file(DsCliProgram *program, const char *path, DsSpan import_span, bool is_root, DsAst *composed) {
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

    DS_GROW_ARRAY(program->loaded_paths, program->loaded_len, program->loaded_cap, 8);
    program->loaded_paths[program->loaded_len++] = ds_str_dup_cstr(normalized);
    import_stack_push(program, normalized);

    LoadedUnit *unit = (LoadedUnit *)ds_xcalloc(1, sizeof(LoadedUnit));
    char *owned_path = ds_str_dup_cstr(path);
    ds_diag_init(&program->diag, &unit->source);
    if (!ds_source_read(owned_path, &unit->source, &program->diag)) {
        if (!is_root) ds_diag_error(&program->diag, import_span, "failed to read imported file `%s`", path);
        free(owned_path);
        free(unit);
        import_stack_pop(program);
        free(normalized);
        return false;
    }
    unit->source.path = owned_path;
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
    DS_GROW_ARRAY(program->units, program->units_len, program->units_cap, 8);
    program->units[program->units_len++] = unit;

    import_stack_pop(program);
    free(normalized);
    return !program->diag.has_error;
}

bool ds_cli_load_source(const char *path, DsCliProgram *program) {
    memset(program, 0, sizeof(*program));
    ds_diag_init(&program->diag, &program->source);
    if (!ds_source_read(path, &program->source, &program->diag)) return false;
    return true;
}

bool ds_cli_load_and_lex(const char *path, DsCliProgram *program) {
    if (!ds_cli_load_source(path, program)) return false;
    return ds_lex(&program->source, &program->tokens, &program->diag);
}

bool ds_cli_load_parse(const char *path, DsCliProgram *program) {
    if (!ds_cli_load_and_lex(path, program)) return false;
    program->ast = ds_parse(&program->tokens, &program->diag);
    return !program->diag.has_error;
}

bool ds_cli_load_composed_parse(const char *path, DsCliProgram *program) {
    memset(program, 0, sizeof(*program));
    ds_diag_init(&program->diag, NULL);
    DsAst *composed = (DsAst *)ds_xcalloc(1, sizeof(DsAst));
    bool ok = load_composed_file(program, path, ds_span_zero(NULL), true, composed);
    if (!ok || program->diag.has_error) {
        ds_ast_free(composed);
        return false;
    }
    program->ast = composed;
    return true;
}

bool ds_cli_load_lower(const char *path, DsCliProgram *program) {
    if (!ds_cli_load_composed_parse(path, program)) return false;
    ds_diag_init(&program->diag, &program->source);
    program->lowered = ds_lower_program(program->ast, &program->diag);
    return program->lowered != NULL;
}
