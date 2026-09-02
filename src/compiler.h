#ifndef DS_COMPILER_H
#define DS_COMPILER_H

#include "program_loader.h"
#include "vm_backend.h"

/*
 * High-level compiler composition shell.
 *
 * Detailed frontend/import, semantic, VM, and Bash implementations remain in
 * their existing modules for now.  This layer makes ownership, phase order,
 * and injectable module boundaries explicit while those internals are migrated
 * incrementally.
 */

typedef enum {
    DS_COMPILE_PHASE_EMPTY,
    DS_COMPILE_PHASE_AST,
    DS_COMPILE_PHASE_HIR
} DsCompilePhase;

typedef struct {
    DsLoadedProgram program;
    DsLowerProgram *hir;
    DsCompilePhase phase;
} DsCompileSession;

typedef bool (*DsCompilerLoadProgramFn)(void *context, const char *path, DsLoadedProgram *program);
typedef void (*DsCompilerFreeLoadedProgramFn)(void *context, DsLoadedProgram *program);
typedef DsLowerProgram *(*DsCompilerAnalyzeFn)(void *context, const DsAst *ast, DsDiag *diag);
typedef void (*DsCompilerFreeHirFn)(void *context, DsLowerProgram *program);
typedef int (*DsCompilerRunVmFn)(void *context, const DsSource *source,
                                 const DsLowerProgram *program, int argc, char **argv,
                                 DsDiag *diag, DsVmOptions options);
typedef int (*DsCompilerRunTestFn)(void *context, const DsSource *source,
                                   const DsLowerProgram *program, const DsLowerTest *test,
                                   DsDiag *diag);
typedef bool (*DsCompilerEmitBashFn)(void *context, const DsSource *source,
                                     const DsLowerProgram *program, const char *output_path,
                                     DsDiag *diag);
typedef void (*DsCompilerDumpBytecodeFn)(void *context, const DsSource *source,
                                         const DsLowerProgram *program, FILE *out);

/*
 * Dependency injection mirrors the architecture boundaries rather than the
 * implementation files. This is intentionally not a service locator or DI
 * framework. Production uses ds_compiler_default(); focused tests or future
 * embedders may replace individual boundary functions explicitly.
 */
typedef struct {
    void *context;
    DsCompilerLoadProgramFn load_tokens;
    DsCompilerLoadProgramFn load_ast;
    DsCompilerLoadProgramFn load_composed_ast;
    DsCompilerFreeLoadedProgramFn free_loaded_program;
} DsCompilerLoaderServices;

typedef struct {
    void *context;
    DsCompilerAnalyzeFn analyze_program;
    DsCompilerFreeHirFn free_hir;
} DsCompilerSemaServices;

typedef struct {
    void *context;
    DsCompilerRunVmFn run_program;
    DsCompilerRunTestFn run_test;
    DsCompilerDumpBytecodeFn dump_bytecode;
} DsCompilerVmServices;

typedef struct {
    void *context;
    DsCompilerEmitBashFn emit_program;
} DsCompilerBashServices;

typedef struct {
    DsCompilerLoaderServices loader;
    DsCompilerSemaServices sema;
    DsCompilerVmServices vm;
    DsCompilerBashServices bash;
} DsCompilerServices;

typedef struct {
    DsCompilerServices services;
} DsCompiler;

DsCompilerServices ds_compiler_default_services(void);
DsCompiler ds_compiler_default(void);
void ds_compiler_init(DsCompiler *compiler, const DsCompilerServices *services);

void ds_compile_session_init(DsCompileSession *session);
void ds_compiler_session_free(DsCompiler *compiler, DsCompileSession *session);

/* Compiler pipeline: source/import composition -> AST -> semantic HIR. */
bool ds_compiler_frontend(DsCompiler *compiler, const char *path, DsCompileSession *session);
bool ds_compiler_sema(DsCompiler *compiler, DsCompileSession *session);
bool ds_compiler_compile(DsCompiler *compiler, const char *path, DsCompileSession *session);

/* Backend entrypoints consume only a prepared HIR session. */
int ds_compiler_run_vm(DsCompiler *compiler, DsCompileSession *session,
                       int argc, char **argv, DsVmOptions options);
int ds_compiler_run_test(DsCompiler *compiler, DsCompileSession *session,
                         const DsLowerTest *test);
bool ds_compiler_emit_bash(DsCompiler *compiler, DsCompileSession *session,
                           const char *output_path);
void ds_compiler_dump_bytecode(DsCompiler *compiler, const DsCompileSession *session,
                               FILE *out);

/* Narrow read access keeps CLI policy out of compilation ownership. */
const DsAst *ds_compile_session_ast(const DsCompileSession *session);
const DsLowerProgram *ds_compile_session_hir(const DsCompileSession *session);
DsDiag *ds_compile_session_diag(DsCompileSession *session);

#endif
