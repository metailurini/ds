#ifndef DS_BACKEND_H
#define DS_BACKEND_H

#include "ds_ast.h"
#include "ds_hir.h"
#include "ds_runtime.h"
#include "vm_options.h"

typedef enum {
    DS_BYTECODE_MODE_DUMP,
    DS_BYTECODE_MODE_RUN
} DsBytecodeMode;

bool ds_format_source(const DsSource *source, const DsAst *ast, DsString *out, DsDiag *diag);

bool ds_emit_bash_program(const DsSource *source, const DsLowerProgram *program, const char *output_path, DsDiag *diag);

void ds_bytecode_dump_program(const DsSource *source, const DsLowerProgram *program, FILE *out);
int ds_vm_run_program(const DsSource *source, const DsLowerProgram *program, DsDiag *diag);
int ds_vm_run_program_args_options(const DsSource *source, const DsLowerProgram *program, int argc, char **argv, DsDiag *diag, DsVmOptions options);
int ds_vm_run_test(const DsSource *source, const DsLowerProgram *program, const DsLowerTest *test, DsDiag *diag);

#endif
