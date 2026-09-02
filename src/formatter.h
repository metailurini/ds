#ifndef DS_FORMATTER_H
#define DS_FORMATTER_H

#include "ds_ast.h"
#include "ds_runtime.h"

/* Formatting boundary: parsed source/AST -> canonical source text. */
bool ds_formatter_format_source(const DsSource *source, const DsAst *ast,
                                DsString *out, DsDiag *diag);

#endif
