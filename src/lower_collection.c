#include "lower_internal.h"

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

static bool lower_interp_is_literal_text(const DsLowerExpr *expr) {
    if (!expr || expr->kind != DS_LOWER_EXPR_INTERP) return false;
    for (size_t i = 0; i < expr->as.interp.parts.len; i++) {
        if (expr->as.interp.parts.items[i]->kind != DS_LOWER_EXPR_INTERP_TEXT) return false;
    }
    return true;
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
        case DS_LOWER_EXPR_INTERP:
            return lower_interp_is_literal_text(expr);
        default:
            return false;
    }
}

bool lower_collection_row_field_is_portable(const DsLowerExpr *expr) {
    if (lower_collection_element_is_portable(expr)) return true;
    switch (expr->kind) {
        case DS_LOWER_EXPR_INTERP:
        case DS_LOWER_EXPR_BINARY:
        case DS_LOWER_EXPR_UNARY:
            return true;
        case DS_LOWER_EXPR_CALL:
            return expr->as.call.return_kind == DS_LOWER_VALUE_STRING ||
                   expr->as.call.return_kind == DS_LOWER_VALUE_INT ||
                   expr->as.call.return_kind == DS_LOWER_VALUE_BOOL;
        default:
            return false;
    }
}

bool lower_collection_for_iterable_is_portable(const DsLowerExpr *iterable) {
    if (!iterable) return false;
    if (iterable->kind == DS_LOWER_EXPR_IDENT) return true;
    if (iterable->kind == DS_LOWER_EXPR_CALL && iterable->as.call.returns_row_array) return true;
    if (iterable->kind == DS_LOWER_EXPR_CALL && iterable->as.call.is_user_function &&
        iterable->as.call.return_kind == DS_LOWER_VALUE_ARRAY) return true;
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
                  "for loop iterable must be a named array, known stdlib array result, or supported array-returning function call for VM/Bash parity; bind temporary arrays to a variable first");
}

bool lower_map_expr_schema(Lower *lower, const DsLowerExpr *expr, DsLowerRowSchema *schema_out) {
    if (!expr || expr->kind != DS_LOWER_EXPR_MAP) return false;
    row_schema_init(schema_out);
    for (size_t i = 0; i < expr->as.map.entries.len; i++) {
        const DsLowerMapEntry *entry = &expr->as.map.entries.items[i];
        SymKind sym = infer_lower_expr_kind(lower, entry->value);
        DsLowerValueKind kind = lower_value_kind_from_sym(sym);
        if (!lower_value_kind_is_scalar(kind)) {
            ds_diag_error(lower->diag, entry->span,
                          "row field `%.*s` must be a scalar string, int, or bool value in v0.37.0",
                          (int)entry->key.len, entry->key.data);
            row_schema_free(schema_out);
            return false;
        }
        if (row_schema_find(schema_out, entry->key)) {
            ds_diag_error(lower->diag, entry->span,
                          "duplicate row field `%.*s`", (int)entry->key.len, entry->key.data);
            row_schema_free(schema_out);
            return false;
        }
        row_schema_push(schema_out, entry->key, kind);
    }
    return true;
}

bool lower_expr_row_schema(const DsLowerExpr *expr, const DsLowerRowSchema **schema_out) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_MAP && expr->as.map.is_row) {
        if (schema_out) *schema_out = &expr->as.map.row_schema;
        return true;
    }
    if (expr->kind == DS_LOWER_EXPR_INDEX && expr->as.index.returns_row) {
        if (schema_out) *schema_out = &expr->as.index.row_schema;
        return true;
    }
    if (expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.returns_row) {
        if (schema_out) *schema_out = &expr->as.call.row_schema;
        return true;
    }
    if (expr->kind == DS_LOWER_EXPR_IDENT) return false;
    return false;
}

bool lower_expr_row_array_schema(const DsLowerExpr *expr, const DsLowerRowSchema **schema_out) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_ARRAY && expr->as.array.is_row_array) {
        if (schema_out) *schema_out = &expr->as.array.row_schema;
        return true;
    }
    if (expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.returns_row_array) {
        if (schema_out) *schema_out = &expr->as.call.row_schema;
        return true;
    }
    return false;
}
