#ifndef DS_LOWER_INTERP_PARSER_H
#define DS_LOWER_INTERP_PARSER_H

#include "ds_ast.h"

/*
 * Parse the expression subset accepted inside `{...}` interpolation. The
 * returned AST is owned by the caller. `end_out` receives the first unconsumed
 * byte offset in `text`.
 */
DsExpr *lower_interp_parse_expr(DsStr text, size_t start, DsSpan span, size_t *end_out);

/* True when decoded string text requires the expression interpolation parser. */
bool lower_interp_text_needs_expr_parser(DsStr decoded);

#endif
