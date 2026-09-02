#ifndef DS_SEMA_H
#define DS_SEMA_H

#include "ds_hir.h"

/* Semantic acceptance boundary: composed AST -> backend-neutral HIR. */
DsLowerProgram *ds_sema_analyze_program(const DsAst *ast, DsDiag *diag);
void ds_sema_free_program(DsLowerProgram *program);

#endif
