#ifndef DS_APP_H
#define DS_APP_H

#include "compiler.h"
#include "ds_runtime.h"

typedef enum {
    DS_APP_FORMAT_PRINT,
    DS_APP_FORMAT_CHECK,
    DS_APP_FORMAT_WRITE
} DsAppFormatMode;

typedef enum {
    DS_APP_INSPECT_TOKENS,
    DS_APP_INSPECT_AST,
    DS_APP_INSPECT_HIR,
    DS_APP_INSPECT_BYTECODE
} DsAppInspectKind;

typedef struct {
    bool warnings_as_errors;
    bool no_warnings;
} DsAppCheckOptions;

typedef size_t (*DsAppCheckWarningsFn)(void *context, const DsAst *ast, FILE *out);
typedef bool (*DsAppFormatSourceFn)(void *context, const DsSource *source, const DsAst *ast,
                                    DsString *out, DsDiag *diag);
typedef void (*DsAppPrintTokensFn)(void *context, const DsTokenVec *tokens, FILE *out);
typedef void (*DsAppPrintAstFn)(void *context, const DsAst *ast, FILE *out);
typedef bool (*DsAppPrintHirFn)(void *context, const DsLowerProgram *program, FILE *out);

typedef bool (*DsAppWriteArtifactFn)(void *context, const char *path,
                                     const char *data, size_t len);
typedef void (*DsAppRemoveArtifactFn)(void *context, const char *path);

typedef struct {
    void *context;
    DsAppCheckWarningsFn check_warnings;
    DsAppFormatSourceFn format_source;
    DsAppPrintTokensFn print_tokens;
    DsAppPrintAstFn print_ast;
    DsAppPrintHirFn print_hir;
} DsAppToolServices;

typedef struct {
    void *context;
    DsAppWriteArtifactFn write_atomic;
    DsAppRemoveArtifactFn remove;
} DsAppArtifactServices;

typedef struct {
    DsCompilerServices compiler;
    DsAppToolServices tools;
    DsAppArtifactServices artifacts;
} DsAppServices;

typedef struct {
    DsCompiler compiler;
    DsAppToolServices tools;
    DsAppArtifactServices artifacts;
} DsApp;

/* Application composition: CLI-independent use cases over compiler/tool shells. */
DsAppServices ds_app_default_services(void);
DsApp ds_app_default(void);
void ds_app_init(DsApp *app, const DsAppServices *services);

int ds_app_run_program(DsApp *app, const char *path, int argc, char **argv,
                       DsVmOptions options);
int ds_app_run_tests(DsApp *app, const char *path, FILE *out, FILE *err);
int ds_app_check(DsApp *app, const char *path, DsAppCheckOptions options, FILE *warnings_out);
int ds_app_format(DsApp *app, const char *path, DsAppFormatMode mode, FILE *out, FILE *err);
int ds_app_inspect(DsApp *app, const char *path, DsAppInspectKind kind, FILE *out);
int ds_app_emit_bash(DsApp *app, const char *path, const char *output_path);

#endif
