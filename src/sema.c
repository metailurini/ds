#include "sema.h"

DsLowerProgram *ds_sema_analyze_program(const DsAst *ast, DsDiag *diag) {
    return ds_lower_program(ast, diag);
}

void ds_sema_free_program(DsLowerProgram *program) {
    ds_lower_program_free(program);
}
