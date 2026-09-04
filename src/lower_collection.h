#ifndef DS_LOWER_COLLECTION_H
#define DS_LOWER_COLLECTION_H

#include "lower_context.h"

bool lower_collection_receiver_is_portable_storage(const DsLowerExpr *expr);
void lower_validate_portable_collection_receiver(Lower *lower, const DsLowerExpr *expr,
                                                 DsSpan span);
bool lower_collection_index_is_portable(const DsLowerExpr *expr, bool map_index);
void lower_validate_portable_collection_index(Lower *lower, const DsLowerExpr *expr,
                                              bool map_index, DsSpan span);
bool lower_collection_element_is_portable(const DsLowerExpr *expr);
bool lower_collection_row_field_is_portable(const DsLowerExpr *expr);
bool lower_collection_for_iterable_is_portable(const DsLowerExpr *iterable);
bool lower_collection_map_for_iterable_is_portable(const DsLowerExpr *iterable);
void lower_reject_nonportable_collection_for_iterable(Lower *lower, DsSpan span);
bool lower_expr_row_schema(const DsLowerExpr *expr, const DsLowerRowSchema **schema_out);
bool lower_expr_row_array_schema(const DsLowerExpr *expr, const DsLowerRowSchema **schema_out);
bool lower_map_expr_schema(Lower *lower, const DsLowerExpr *expr,
                           DsLowerRowSchema *schema_out);

#endif
