#include "bash_backend.h"
#include "backend.h"

bool ds_bash_backend_emit_program(const DsSource *source, const DsLowerProgram *program,
                                  const char *output_path, DsDiag *diag) {
    return ds_emit_bash_program(source, program, output_path, diag);
}
