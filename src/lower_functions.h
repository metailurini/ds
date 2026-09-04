#ifndef DS_LOWER_FUNCTIONS_H
#define DS_LOWER_FUNCTIONS_H

#include "lower_context.h"

/*
 * Function-analysis phase boundary inside semantic lowering.
 *
 * lower.c owns phase order. Detailed signature collection, inference, return
 * discovery, body lowering, and call-graph traversal remain private to the
 * corresponding implementation units.
 */
void lower_functions_collect_signatures(Lower *lower, const DsAst *ast);
void lower_functions_infer_parameter_kinds(Lower *lower, const DsAst *ast);
void lower_functions_predeclare_return_contracts(Lower *lower, const DsAst *ast);
void lower_functions_lower_bodies(Lower *lower, const DsAst *ast);
void lower_functions_validate_call_graph(Lower *lower);

#endif
