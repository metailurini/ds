#ifndef DS_INSPECTOR_H
#define DS_INSPECTOR_H

#include "frontend.h"
#include "ds_hir.h"

/* Developer inspection boundary over existing deterministic renderers. */
void ds_inspector_print_tokens(const DsTokenVec *tokens, FILE *out);
void ds_inspector_print_ast(const DsAst *ast, FILE *out);
bool ds_inspector_print_hir(const DsLowerProgram *program, FILE *out);

#endif
