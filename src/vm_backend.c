#include "vm_backend.h"
#include "backend.h"

int ds_vm_backend_run_program(const DsSource *source, const DsLowerProgram *program,
                              int argc, char **argv, DsDiag *diag, DsVmOptions options) {
    return ds_vm_run_program_args_options(source, program, argc, argv, diag, options);
}

int ds_vm_backend_run_test(const DsSource *source, const DsLowerProgram *program,
                           const DsLowerTest *test, DsDiag *diag) {
    return ds_vm_run_test(source, program, test, diag);
}

void ds_vm_backend_dump_bytecode(const DsSource *source, const DsLowerProgram *program,
                                 FILE *out) {
    ds_bytecode_dump_program(source, program, out);
}
