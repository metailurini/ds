#ifndef DS_LOWER_EXPR_H
#define DS_LOWER_EXPR_H

#include "lower_context.h"

DsLowerExpr *expr_new(DsLowerExprKind kind, DsSpan span);
bool lower_expr_produces_command_result(const DsLowerExpr *expr);
bool lower_expr_is_portable_command_result_return(const DsLowerExpr *expr);
void validate_user_call_arg_kinds(Lower *lower, const DsLowerFn *fn,
                                  const DsExprVec *args, const SymKind *arg_kinds);
DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);
SymKind *lower_args_to_kinds(Lower *lower, const DsExprVec *args, DsLowerExprVec *out);
DsLowerExpr *lower_string_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);
SymKind infer_lower_expr_kind(Lower *lower, const DsLowerExpr *expr);
SymKind infer_array_element_kind(Lower *lower, const DsLowerExpr *expr);
SymKind infer_map_value_kind(Lower *lower, const DsLowerExpr *expr);
DsStr lower_map_key_decode(const DsMapEntry *entry);
void validate_glob_pattern_arg(Lower *lower, DsStr helper_name, const DsExpr *arg);

#endif
