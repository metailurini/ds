#ifndef DS_VM_BACKEND_H
#define DS_VM_BACKEND_H

#include "ds_hir.h"
#include "vm_options.h"

/* VM backend boundary: accepted HIR -> bytecode execution or inspection. */
int ds_vm_backend_run_program(const DsSource *source, const DsLowerProgram *program,
                              int argc, char **argv, DsDiag *diag, DsVmOptions options);
int ds_vm_backend_run_test(const DsSource *source, const DsLowerProgram *program,
                           const DsLowerTest *test, DsDiag *diag);
void ds_vm_backend_dump_bytecode(const DsSource *source, const DsLowerProgram *program,
                                 FILE *out);

#endif
