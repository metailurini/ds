#include "lower_internal.h"

#include <stdbool.h>

/*
 * Collection portability policy lives in lowering because it decides which
 * VM-capable collection shapes are also representable by standalone Bash.
 * VM/Bash consume accepted HIR and may keep runtime/invariant diagnostics, but
 * they must not expand or rediscover these source-language acceptance rules.
 */

bool lower_collection_receiver_is_portable_storage(const DsLowerExpr *expr) {
    return expr && expr->kind == DS_LOWER_EXPR_IDENT;
}

void lower_validate_portable_collection_receiver(Lower *lower, const DsLowerExpr *expr, DsSpan span) {
    if (lower_collection_receiver_is_portable_storage(expr)) return;
    ds_diag_error(lower->diag, span,
                  "collection and command-result field/index access requires a named binding for VM/Bash parity; bind the value to a variable first");
}

bool lower_collection_index_is_portable(const DsLowerExpr *expr, bool map_index) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_IDENT) return true;
    return map_index ? expr->kind == DS_LOWER_EXPR_STRING : expr->kind == DS_LOWER_EXPR_INT;
}

void lower_validate_portable_collection_index(Lower *lower, const DsLowerExpr *expr, bool map_index, DsSpan span) {
    if (lower_collection_index_is_portable(expr, map_index)) return;
    ds_diag_error(lower->diag, span,
                  "collection index expression must be a literal or variable for VM/Bash parity; bind the computed index to a variable first");
}

bool lower_collection_element_is_portable(const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INT:
        case DS_LOWER_EXPR_BOOL:
        case DS_LOWER_EXPR_FIELD:
        case DS_LOWER_EXPR_INDEX:
            return true;
        default:
            return false;
    }
}

bool lower_collection_for_iterable_is_portable(const DsLowerExpr *iterable) {
    if (!iterable) return false;
    if (iterable->kind == DS_LOWER_EXPR_IDENT) return true;
    if (iterable->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(iterable->as.call.name)) {
        const DsStdlibHelper *helper = ds_stdlib_lookup(iterable->as.call.name);
        return helper && helper->return_kind == DS_STDLIB_RETURN_ARRAY;
    }
    return false;
}

bool lower_collection_map_for_iterable_is_portable(const DsLowerExpr *iterable) {
    if (!iterable) return false;
    if (iterable->kind == DS_LOWER_EXPR_IDENT) return true;
    return iterable->kind == DS_LOWER_EXPR_CALL && iterable->as.call.is_user_function &&
           iterable->as.call.return_kind == DS_LOWER_VALUE_MAP;
}

void lower_reject_nonportable_collection_for_iterable(Lower *lower, DsSpan span) {
    ds_diag_error(lower->diag, span,
                  "for loop iterable must be a named array or known stdlib array result for VM/Bash parity in v0.10.0; bind temporary arrays to a variable first");
}
