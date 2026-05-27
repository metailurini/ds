#ifndef DS_CHECKER_H
#define DS_CHECKER_H

#include "ds_ast.h"

size_t ds_check_warnings_ast(const DsAst *ast, FILE *out);

#endif
