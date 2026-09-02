#include "app.h"

#include "artifact.h"
#include "ds_checker.h"
#include "formatter.h"
#include "inspector.h"

static size_t default_check_warnings(void *context, const DsAst *ast, FILE *out) {
    (void)context;
    return ds_check_warnings_ast(ast, out);
}

static bool default_format_source(void *context, const DsSource *source, const DsAst *ast,
                                  DsString *out, DsDiag *diag) {
    (void)context;
    return ds_formatter_format_source(source, ast, out, diag);
}

static void default_print_tokens(void *context, const DsTokenVec *tokens, FILE *out) {
    (void)context;
    ds_inspector_print_tokens(tokens, out);
}

static void default_print_ast(void *context, const DsAst *ast, FILE *out) {
    (void)context;
    ds_inspector_print_ast(ast, out);
}

static bool default_print_hir(void *context, const DsLowerProgram *program, FILE *out) {
    (void)context;
    return ds_inspector_print_hir(program, out);
}

static bool default_write_artifact(void *context, const char *path,
                                   const char *data, size_t len) {
    (void)context;
    return ds_artifact_write_atomic(path, data, len);
}

static void default_remove_artifact(void *context, const char *path) {
    (void)context;
    ds_artifact_remove(path);
}

DsAppServices ds_app_default_services(void) {
    DsAppServices services = {
        .compiler = ds_compiler_default_services(),
        .tools = {
            .check_warnings = default_check_warnings,
            .format_source = default_format_source,
            .print_tokens = default_print_tokens,
            .print_ast = default_print_ast,
            .print_hir = default_print_hir,
        },
        .artifacts = {
            .write_atomic = default_write_artifact,
            .remove = default_remove_artifact,
        },
    };
    return services;
}

DsApp ds_app_default(void) {
    DsApp app;
    ds_app_init(&app, NULL);
    return app;
}

void ds_app_init(DsApp *app, const DsAppServices *services) {
    if (!app) return;

    DsAppServices defaults = ds_app_default_services();
    ds_compiler_init(&app->compiler, services ? &services->compiler : NULL);
    app->tools = defaults.tools;
    app->artifacts = defaults.artifacts;
    if (!services) return;

    if (services->tools.check_warnings || services->tools.format_source ||
        services->tools.print_tokens || services->tools.print_ast || services->tools.print_hir) {
        app->tools.context = services->tools.context;
        if (services->tools.check_warnings) app->tools.check_warnings = services->tools.check_warnings;
        if (services->tools.format_source) app->tools.format_source = services->tools.format_source;
        if (services->tools.print_tokens) app->tools.print_tokens = services->tools.print_tokens;
        if (services->tools.print_ast) app->tools.print_ast = services->tools.print_ast;
        if (services->tools.print_hir) app->tools.print_hir = services->tools.print_hir;
    }
    if (services->artifacts.write_atomic || services->artifacts.remove) {
        app->artifacts.context = services->artifacts.context;
        if (services->artifacts.write_atomic) app->artifacts.write_atomic = services->artifacts.write_atomic;
        if (services->artifacts.remove) app->artifacts.remove = services->artifacts.remove;
    }
}

int ds_app_run_program(DsApp *app, const char *path, int argc, char **argv,
                       DsVmOptions options) {
    if (!app) return 1;
    DsCompileSession session;
    int rc = ds_compiler_compile(&app->compiler, path, &session)
        ? ds_compiler_run_vm(&app->compiler, &session, argc, argv, options)
        : 1;
    ds_compiler_session_free(&app->compiler, &session);
    return rc;
}

int ds_app_run_tests(DsApp *app, const char *path, FILE *out, FILE *err) {
    if (!app || !out || !err) return 1;
    DsCompileSession session;
    int rc = 1;
    if (!ds_compiler_compile(&app->compiler, path, &session)) goto cleanup;

    const DsLowerProgram *program = ds_compile_session_hir(&session);
    DsDiag *diag = ds_compile_session_diag(&session);
    if (program->tests.len == 0) {
        fprintf(err, "error: no tests found in `%s`\n", path);
        goto cleanup;
    }

    size_t passed = 0;
    size_t failed = 0;
    for (size_t i = 0; i < program->tests.len; i++) {
        const DsLowerTest *test = &program->tests.items[i];
        diag->has_error = false;
        int test_rc = ds_compiler_run_test(&app->compiler, &session, test);
        if (test_rc == 0 && !diag->has_error) {
            fprintf(out, "ok   %.*s\n", (int)test->name.len, test->name.data);
            passed++;
        } else {
            fprintf(out, "fail %.*s\n", (int)test->name.len, test->name.data);
            failed++;
        }
    }
    fprintf(out, "\n%zu tests, %zu passed, %zu failed\n", passed + failed, passed, failed);
    rc = failed == 0 ? 0 : 1;

cleanup:
    ds_compiler_session_free(&app->compiler, &session);
    return rc;
}

int ds_app_check(DsApp *app, const char *path, DsAppCheckOptions options, FILE *warnings_out) {
    if (!app || !warnings_out) return 1;
    DsCompileSession session;
    int rc = ds_compiler_compile(&app->compiler, path, &session) ? 0 : 1;
    if (rc == 0 && !options.no_warnings) {
        size_t warnings = app->tools.check_warnings
            ? app->tools.check_warnings(app->tools.context, ds_compile_session_ast(&session), warnings_out)
            : 0;
        if (options.warnings_as_errors && warnings > 0) rc = 1;
    }
    ds_compiler_session_free(&app->compiler, &session);
    return rc;
}

int ds_app_format(DsApp *app, const char *path, DsAppFormatMode mode, FILE *out, FILE *err) {
    if (!app || !path || !out || !err) return 1;

    DsLoadedProgram program = {0};
    int rc = 1;
    if (!app->compiler.services.loader.load_ast ||
        !app->compiler.services.loader.load_ast(app->compiler.services.loader.context, path, &program)) {
        goto cleanup_program;
    }

    DsString formatted;
    if (!app->tools.format_source ||
        !app->tools.format_source(app->tools.context, &program.source, program.ast,
                                  &formatted, &program.diag)) {
        goto cleanup_program;
    }

    rc = 0;
    if (mode == DS_APP_FORMAT_CHECK) {
        bool differs = formatted.len != program.source.len;
        if (!differs && memcmp(formatted.data, program.source.data, formatted.len) != 0) differs = true;
        if (differs) {
            fprintf(err, "%s: needs formatting\n", path);
            rc = 1;
        }
    } else if (mode == DS_APP_FORMAT_WRITE) {
        if (!app->artifacts.write_atomic ||
            !app->artifacts.write_atomic(app->artifacts.context, path, formatted.data, formatted.len)) rc = 1;
    } else if (formatted.len > 0) {
        fwrite(formatted.data, 1, formatted.len, out);
    }
    ds_string_free(&formatted);

cleanup_program:
    if (app->compiler.services.loader.free_loaded_program) {
        app->compiler.services.loader.free_loaded_program(app->compiler.services.loader.context, &program);
    }
    return rc;
}

int ds_app_inspect(DsApp *app, const char *path, DsAppInspectKind kind, FILE *out) {
    if (!app || !path || !out) return 1;

    if (kind == DS_APP_INSPECT_TOKENS || kind == DS_APP_INSPECT_AST) {
        DsLoadedProgram program = {0};
        DsCompilerLoaderServices *loader = &app->compiler.services.loader;
        bool ok = kind == DS_APP_INSPECT_TOKENS
            ? loader->load_tokens && loader->load_tokens(loader->context, path, &program)
            : loader->load_ast && loader->load_ast(loader->context, path, &program);
        int rc = ok ? 0 : 1;
        if (rc == 0 && kind == DS_APP_INSPECT_TOKENS && app->tools.print_tokens) {
            app->tools.print_tokens(app->tools.context, &program.tokens, out);
        }
        if (rc == 0 && kind == DS_APP_INSPECT_AST && app->tools.print_ast) {
            app->tools.print_ast(app->tools.context, program.ast, out);
        }
        if (loader->free_loaded_program) loader->free_loaded_program(loader->context, &program);
        return rc;
    }

    DsCompileSession session;
    int rc = ds_compiler_compile(&app->compiler, path, &session) ? 0 : 1;
    if (rc == 0 && kind == DS_APP_INSPECT_HIR) {
        if (!app->tools.print_hir ||
            !app->tools.print_hir(app->tools.context, ds_compile_session_hir(&session), out)) rc = 1;
    } else if (rc == 0) {
        ds_compiler_dump_bytecode(&app->compiler, &session, out);
    }
    ds_compiler_session_free(&app->compiler, &session);
    return rc;
}

int ds_app_emit_bash(DsApp *app, const char *path, const char *output_path) {
    if (!app || !path || !output_path) return 1;
    DsCompileSession session;
    int rc = ds_compiler_compile(&app->compiler, path, &session) ? 0 : 1;
    if (rc == 0 && !ds_compiler_emit_bash(&app->compiler, &session, output_path)) rc = 1;
    ds_compiler_session_free(&app->compiler, &session);
    if (rc != 0 && app->artifacts.remove) {
        app->artifacts.remove(app->artifacts.context, output_path);
    }
    return rc;
}
