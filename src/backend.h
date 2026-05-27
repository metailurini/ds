#ifndef DS_BACKEND_H
#define DS_BACKEND_H

#include "ds_ast.h"
#include "ds_hir.h"
#include "ds_runtime.h"

typedef enum {
    DS_BYTECODE_MODE_DUMP,
    DS_BYTECODE_MODE_RUN
} DsBytecodeMode;

typedef struct {
    bool trace_cmd;
    bool trace_vm;
    bool test_mode;
    DsStr test_name;
} DsVmOptions;

bool ds_format_source(const DsSource *source, const DsAst *ast, DsString *out, DsDiag *diag);

bool ds_emit_bash(const DsSource *source, const DsAst *ast, const char *output_path, DsDiag *diag);
bool ds_emit_bash_program(const DsSource *source, const DsLowerProgram *program, const char *output_path, DsDiag *diag);

bool ds_bytecode_dump(const DsSource *source, const DsAst *ast, FILE *out, DsDiag *diag);
bool ds_bytecode_dump_program(const DsSource *source, const DsLowerProgram *program, FILE *out, DsDiag *diag);
int ds_vm_run(const DsSource *source, const DsAst *ast, DsDiag *diag);
int ds_vm_run_program(const DsSource *source, const DsLowerProgram *program, DsDiag *diag);
int ds_vm_run_program_args(const DsSource *source, const DsLowerProgram *program, int argc, char **argv, DsDiag *diag);
int ds_vm_run_program_args_options(const DsSource *source, const DsLowerProgram *program, int argc, char **argv, DsDiag *diag, DsVmOptions options);
int ds_vm_run_test(const DsSource *source, const DsLowerProgram *program, const DsLowerTest *test, DsDiag *diag);

#endif
