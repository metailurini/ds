#include "compiler.h"

#include "bash_backend.h"
#include "program_loader.h"
#include "sema.h"
#include "vm_backend.h"

static bool default_load_tokens(void *context, const char *path, DsLoadedProgram *program) {
    (void)context;
    return ds_program_loader_load_tokens(path, program);
}

static bool default_load_ast(void *context, const char *path, DsLoadedProgram *program) {
    (void)context;
    return ds_program_loader_load_ast(path, program);
}

static bool default_load_composed_ast(void *context, const char *path, DsLoadedProgram *program) {
    (void)context;
    return ds_program_loader_load_composed_ast(path, program);
}

static void default_free_loaded_program(void *context, DsLoadedProgram *program) {
    (void)context;
    ds_program_loader_free(program);
}

static DsLowerProgram *default_analyze_program(void *context, const DsAst *ast, DsDiag *diag) {
    (void)context;
    return ds_sema_analyze_program(ast, diag);
}

static void default_free_hir(void *context, DsLowerProgram *program) {
    (void)context;
    ds_sema_free_program(program);
}

static int default_run_program(void *context, const DsSource *source,
                               const DsLowerProgram *program, int argc, char **argv,
                               DsDiag *diag, DsVmOptions options) {
    (void)context;
    return ds_vm_backend_run_program(source, program, argc, argv, diag, options);
}

static int default_run_test(void *context, const DsSource *source,
                            const DsLowerProgram *program, const DsLowerTest *test,
                            DsDiag *diag) {
    (void)context;
    return ds_vm_backend_run_test(source, program, test, diag);
}

static void default_dump_bytecode(void *context, const DsSource *source,
                                  const DsLowerProgram *program, FILE *out) {
    (void)context;
    ds_vm_backend_dump_bytecode(source, program, out);
}

static bool default_emit_bash(void *context, const DsSource *source,
                              const DsLowerProgram *program, const char *output_path,
                              DsDiag *diag) {
    (void)context;
    return ds_bash_backend_emit_program(source, program, output_path, diag);
}

DsCompilerServices ds_compiler_default_services(void) {
    DsCompilerServices services = {
        .loader = {
            .load_tokens = default_load_tokens,
            .load_ast = default_load_ast,
            .load_composed_ast = default_load_composed_ast,
            .free_loaded_program = default_free_loaded_program,
        },
        .sema = {
            .analyze_program = default_analyze_program,
            .free_hir = default_free_hir,
        },
        .vm = {
            .run_program = default_run_program,
            .run_test = default_run_test,
            .dump_bytecode = default_dump_bytecode,
        },
        .bash = {
            .emit_program = default_emit_bash,
        },
    };
    return services;
}

DsCompiler ds_compiler_default(void) {
    DsCompiler compiler;
    ds_compiler_init(&compiler, NULL);
    return compiler;
}

void ds_compiler_init(DsCompiler *compiler, const DsCompilerServices *services) {
    if (!compiler) return;
    compiler->services = ds_compiler_default_services();
    if (!services) return;
    if (services->loader.load_tokens || services->loader.load_ast ||
        services->loader.load_composed_ast || services->loader.free_loaded_program) {
        compiler->services.loader.context = services->loader.context;
        if (services->loader.load_tokens) compiler->services.loader.load_tokens = services->loader.load_tokens;
        if (services->loader.load_ast) compiler->services.loader.load_ast = services->loader.load_ast;
        if (services->loader.load_composed_ast) {
            compiler->services.loader.load_composed_ast = services->loader.load_composed_ast;
        }
        if (services->loader.free_loaded_program) {
            compiler->services.loader.free_loaded_program = services->loader.free_loaded_program;
        }
    }
    if (services->sema.analyze_program || services->sema.free_hir) {
        compiler->services.sema.context = services->sema.context;
        if (services->sema.analyze_program) {
            compiler->services.sema.analyze_program = services->sema.analyze_program;
        }
        if (services->sema.free_hir) compiler->services.sema.free_hir = services->sema.free_hir;
    }
    if (services->vm.run_program || services->vm.run_test || services->vm.dump_bytecode) {
        compiler->services.vm.context = services->vm.context;
        if (services->vm.run_program) compiler->services.vm.run_program = services->vm.run_program;
        if (services->vm.run_test) compiler->services.vm.run_test = services->vm.run_test;
        if (services->vm.dump_bytecode) compiler->services.vm.dump_bytecode = services->vm.dump_bytecode;
    }
    if (services->bash.emit_program) {
        compiler->services.bash.context = services->bash.context;
        compiler->services.bash.emit_program = services->bash.emit_program;
    }
}

void ds_compile_session_init(DsCompileSession *session) {
    if (!session) return;
    memset(session, 0, sizeof(*session));
    session->phase = DS_COMPILE_PHASE_EMPTY;
}

void ds_compiler_session_free(DsCompiler *compiler, DsCompileSession *session) {
    if (!compiler || !session) return;
    if (session->hir && compiler->services.sema.free_hir) {
        compiler->services.sema.free_hir(compiler->services.sema.context, session->hir);
        session->hir = NULL;
    }
    if (compiler->services.loader.free_loaded_program) {
        compiler->services.loader.free_loaded_program(compiler->services.loader.context, &session->program);
    }
    ds_compile_session_init(session);
}

bool ds_compiler_frontend(DsCompiler *compiler, const char *path, DsCompileSession *session) {
    if (!compiler || !path || !session || !compiler->services.loader.load_composed_ast) return false;
    ds_compile_session_init(session);
    if (!compiler->services.loader.load_composed_ast(compiler->services.loader.context, path, &session->program)) return false;
    session->phase = DS_COMPILE_PHASE_AST;
    return true;
}

bool ds_compiler_sema(DsCompiler *compiler, DsCompileSession *session) {
    if (!compiler || !session || session->phase != DS_COMPILE_PHASE_AST ||
        !compiler->services.sema.analyze_program) return false;

    ds_diag_init(&session->program.diag, &session->program.source);
    session->hir = compiler->services.sema.analyze_program(compiler->services.sema.context, session->program.ast, &session->program.diag);
    if (!session->hir) return false;

    session->phase = DS_COMPILE_PHASE_HIR;
    return true;
}

bool ds_compiler_compile(DsCompiler *compiler, const char *path, DsCompileSession *session) {
    if (!ds_compiler_frontend(compiler, path, session)) return false;
    return ds_compiler_sema(compiler, session);
}

static bool session_has_hir(const DsCompileSession *session) {
    return session && session->phase == DS_COMPILE_PHASE_HIR && session->hir;
}

int ds_compiler_run_vm(DsCompiler *compiler, DsCompileSession *session,
                       int argc, char **argv, DsVmOptions options) {
    if (!compiler || !session_has_hir(session) || !compiler->services.vm.run_program) return 1;
    return compiler->services.vm.run_program(compiler->services.vm.context, &session->program.source,
                                             session->hir, argc, argv,
                                             &session->program.diag, options);
}

int ds_compiler_run_test(DsCompiler *compiler, DsCompileSession *session,
                         const DsLowerTest *test) {
    if (!compiler || !session_has_hir(session) || !test || !compiler->services.vm.run_test) return 1;
    return compiler->services.vm.run_test(compiler->services.vm.context, &session->program.source,
                                          session->hir, test, &session->program.diag);
}

bool ds_compiler_emit_bash(DsCompiler *compiler, DsCompileSession *session,
                           const char *output_path) {
    if (!compiler || !session_has_hir(session) || !output_path || !compiler->services.bash.emit_program) return false;
    return compiler->services.bash.emit_program(compiler->services.bash.context, &session->program.source,
                                           session->hir, output_path,
                                           &session->program.diag);
}

void ds_compiler_dump_bytecode(DsCompiler *compiler, const DsCompileSession *session,
                               FILE *out) {
    if (!compiler || !session_has_hir(session) || !out || !compiler->services.vm.dump_bytecode) return;
    compiler->services.vm.dump_bytecode(compiler->services.vm.context, &session->program.source,
                                         session->hir, out);
}

const DsAst *ds_compile_session_ast(const DsCompileSession *session) {
    return session ? session->program.ast : NULL;
}

const DsLowerProgram *ds_compile_session_hir(const DsCompileSession *session) {
    return session ? session->hir : NULL;
}

DsDiag *ds_compile_session_diag(DsCompileSession *session) {
    return session ? &session->program.diag : NULL;
}
