#ifndef DS_BASH_BACKEND_H
#define DS_BASH_BACKEND_H

#include "ds_hir.h"

/* Bash backend boundary: accepted HIR -> standalone Bash artifact. */
bool ds_bash_backend_emit_program(const DsSource *source, const DsLowerProgram *program,
                                  const char *output_path, DsDiag *diag);

#endif
