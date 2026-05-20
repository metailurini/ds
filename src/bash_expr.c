#include "bash_internal.h"

#include <stdlib.h>

static bool result_field_is_bool(DsStr field) {
    const DsCommandResultField *desc = ds_command_result_field_lookup(field);
    return desc && desc->kind == DS_COMMAND_RESULT_FIELD_BOOL;
}

static void emit_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_type_");
    buf_append_len(out, name.data, name.len);
}

bool emit_value_expr(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT:
            buf_append(out, "\"$");
            emit_var_name(out, expr->as.text);
            buf_append(out, "\"");
            return true;
        case DS_LOWER_EXPR_STRING:
            return emit_interpolated_string(e, expr, out);
        case DS_LOWER_EXPR_INT:
            buf_append_len(out, expr->as.text.data, expr->as.text.len);
            return true;
        case DS_LOWER_EXPR_BOOL:
            buf_append(out, expr->as.boolean ? "true" : "false");
            return true;
        case DS_LOWER_EXPR_FIELD:
            if (expr->as.field.object->kind != DS_LOWER_EXPR_IDENT) {
                ds_diag_error(e->diag, expr->span, "unsupported command result field receiver for Bash emission");
                return false;
            }
            buf_append(out, "\"$");
            emit_var_name(out, expr->as.field.object->as.text);
            buf_append(out, "_");
            buf_append_len(out, expr->as.field.field.data, expr->as.field.field.len);
            buf_append(out, "\"");
            return true;
        case DS_LOWER_EXPR_INDEX:
            if (expr->as.index.object->kind != DS_LOWER_EXPR_IDENT) {
                ds_diag_error(e->diag, expr->span, "Bash emission only supports indexing named collections in v0.10.0");
                return false;
            }
            if (!expr->as.index.object_is_array && !expr->as.index.object_is_map) {
                ds_diag_error(e->diag, expr->span, "Bash emission needs a known collection kind for indexing in v0.10.0");
                return false;
            }
            buf_append(out, "\"$(");
            buf_append(out, expr->as.index.object_is_map ? "__ds_map_get " : "__ds_array_get ");
            emit_var_name(out, expr->as.index.object->as.text);
            buf_append(out, " ");
            if (expr->as.index.index->kind == DS_LOWER_EXPR_INT) {
                buf_append_len(out, expr->as.index.index->as.text.data, expr->as.index.index->as.text.len);
            } else if (expr->as.index.index->kind == DS_LOWER_EXPR_STRING) {
                char *decoded = NULL; size_t len = 0;
                if (!decode_string_literal(e->diag, expr->as.index.index, &decoded, &len)) return false;
                bash_single_quote(out, decoded, len);
                free(decoded);
            } else if (expr->as.index.index->kind == DS_LOWER_EXPR_IDENT) {
                buf_append(out, "\"$");
                emit_var_name(out, expr->as.index.index->as.text);
                buf_append(out, "\"");
            } else {
                ds_diag_error(e->diag, expr->span, "unsupported Bash collection index expression in v0.10.0");
                return false;
            }
            buf_append(out, ")\"");
            return true;
        case DS_LOWER_EXPR_CALL:
            if (!ds_stdlib_is_name(expr->as.call.name) || stdlib_returns_array(expr->as.call.name)) {
                ds_diag_error(e->diag, expr->span, "unsupported standard-library value expression for Bash emission in v0.11.0");
                return false;
            }
            buf_append(out, "\"$(");
            emit_stdlib_helper_name(out, expr->as.call.name);
            if (!emit_call_args(e, &expr->as.call.args, out)) return false;
            buf_append(out, ")\"");
            return true;
        case DS_LOWER_EXPR_BINARY:
            if (str_eq(expr->as.binary.op, "+") || str_eq(expr->as.binary.op, "-")) {
                buf_append(out, "$(( ");
                if (!emit_condition_operand(e, expr->as.binary.left, out)) return false;
                buf_append(out, str_eq(expr->as.binary.op, "+") ? " + " : " - ");
                if (!emit_condition_operand(e, expr->as.binary.right, out)) return false;
                buf_append(out, " ))");
                return true;
            }
            ds_diag_error(e->diag, expr->span, "unsupported binary value expression for Bash emission in v0.17.0");
            return false;
        default:
            ds_diag_error(e->diag, expr->span, "this expression cannot be emitted as a Bash assignment in v0.2.0");
            return false;
    }
}

bool emit_array_elements(BashEmitter *e, const DsLowerExprVec *elements, EmitBuf *out) {
    buf_append(out, "(");
    for (size_t i = 0; i < elements->len; i++) {
        if (i) buf_append(out, " ");
        if (!emit_value_expr(e, elements->items[i], out)) return false;
    }
    buf_append(out, ")");
    return true;
}

bool emit_map_entries(BashEmitter *e, const DsLowerMapEntryVec *entries, EmitBuf *out) {
    buf_append(out, "(");
    for (size_t i = 0; i < entries->len; i++) {
        if (i) buf_append(out, " ");
        buf_append(out, "[");
        bash_single_quote(out, entries->items[i].key.data, entries->items[i].key.len);
        buf_append(out, "]=");
        if (!emit_value_expr(e, entries->items[i].value, out)) return false;
    }
    buf_append(out, ")");
    return true;
}

bool emit_call_arg_expr(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    if (expr->kind == DS_LOWER_EXPR_IDENT) {
        if (!symbol_exists(&e->symbols, expr->as.text)) {
            ds_diag_error(e->diag, expr->span, "unknown variable `%.*s`", (int)expr->as.text.len, expr->as.text.data);
            return false;
        }
        buf_append(out, "\"$");
        emit_var_name(out, expr->as.text);
        buf_append(out, "\"");
        return true;
    }
    return emit_value_expr(e, expr, out);
}

bool emit_condition_operand(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT:
            if (!symbol_exists(&e->symbols, expr->as.text)) {
                ds_diag_error(e->diag, expr->span, "unknown variable `%.*s`", (int)expr->as.text.len, expr->as.text.data);
                return false;
            }
            buf_append(out, "\"$");
            emit_var_name(out, expr->as.text);
            buf_append(out, "\"");
            return true;
        case DS_LOWER_EXPR_STRING:
            return emit_interpolated_string(e, expr, out);
        case DS_LOWER_EXPR_INT:
            buf_append_len(out, expr->as.text.data, expr->as.text.len);
            return true;
        case DS_LOWER_EXPR_BOOL:
            buf_append(out, expr->as.boolean ? "true" : "false");
            return true;
        case DS_LOWER_EXPR_FIELD:
        case DS_LOWER_EXPR_CALL:
            return emit_value_expr(e, expr, out);
        default:
            ds_diag_error(e->diag, expr->span, "unsupported condition operand for Bash emission");
            return false;
    }
}

bool emit_condition(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    if (expr->kind == DS_LOWER_EXPR_IDENT) {
        if (!symbol_exists(&e->symbols, expr->as.text)) {
            ds_diag_error(e->diag, expr->span, "unknown variable `%.*s`", (int)expr->as.text.len, expr->as.text.data);
            return false;
        }
        if (e->needs_case_types) {
            buf_append(out, "[[ ( \"${");
            emit_type_var_name(out, expr->as.text);
            buf_append(out, ":-unknown}\" == bool && \"$");
            emit_var_name(out, expr->as.text);
            buf_append(out, "\" == true ) || ( \"${");
            emit_type_var_name(out, expr->as.text);
            buf_append(out, ":-unknown}\" == int && \"$");
            emit_var_name(out, expr->as.text);
            buf_append(out, "\" != 0 ) || ( \"${");
            emit_type_var_name(out, expr->as.text);
            buf_append(out, ":-unknown}\" != bool && \"${");
            emit_type_var_name(out, expr->as.text);
            buf_append(out, ":-unknown}\" != int && -n \"$");
            emit_var_name(out, expr->as.text);
            buf_append(out, "\" ) ]]");
            return true;
        }
        buf_append(out, "[[ \"$");
        emit_var_name(out, expr->as.text);
        buf_append(out, "\" == true ]]");
        return true;
    }
    if (expr->kind == DS_LOWER_EXPR_FIELD) {
        if (result_field_is_bool(expr->as.field.field)) {
            buf_append(out, "[[ ");
            emit_value_expr(e, expr, out);
            buf_append(out, " == true ]]");
            return true;
        }
        buf_append(out, "[[ -n ");
        emit_value_expr(e, expr, out);
        buf_append(out, " ]]");
        return true;
    }
    if (expr->kind == DS_LOWER_EXPR_BOOL) {
        buf_append(out, expr->as.boolean ? "true" : "false");
        return true;
    }
    if (expr->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(expr->as.call.name)) {
        buf_append(out, "[[ ");
        if (!emit_value_expr(e, expr, out)) return false;
        buf_append(out, " == true ]]");
        return true;
    }
    if (expr->kind == DS_LOWER_EXPR_UNARY && str_eq(expr->as.unary.op, "!")) {
        buf_append(out, "! ");
        return emit_condition(e, expr->as.unary.right, out);
    }
    if (expr->kind == DS_LOWER_EXPR_BINARY) {
        const char *op = NULL;
        bool negate = false;
        if (str_eq(expr->as.binary.op, "==")) op = "==";
        else if (str_eq(expr->as.binary.op, "!=")) op = "!=";
        else if (str_eq(expr->as.binary.op, ">")) op = ">";
        else if (str_eq(expr->as.binary.op, ">=")) {
            op = "<";
            negate = true;
        } else if (str_eq(expr->as.binary.op, "<")) op = "<";
        else if (str_eq(expr->as.binary.op, "<=")) {
            op = ">";
            negate = true;
        }
        if (!op) {
            ds_diag_error(e->diag, expr->span, "operator `%.*s` cannot be emitted in a Bash condition in v0.2.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
            return false;
        }
        if (negate) buf_append(out, "! ");
        buf_append(out, "[[ ");
        if (!emit_condition_operand(e, expr->as.binary.left, out)) return false;
        buf_appendf(out, " %s ", op);
        if (!emit_condition_operand(e, expr->as.binary.right, out)) return false;
        buf_append(out, " ]]");
        return true;
    }
    ds_diag_error(e->diag, expr->span, "unsupported condition for Bash emission");
    return false;
}

bool emit_call_args(BashEmitter *e, const DsLowerExprVec *args, EmitBuf *out) {
    for (size_t i = 0; i < args->len; i++) {
        buf_append(out, " ");
        if (!emit_call_arg_expr(e, args->items[i], out)) return false;
    }
    return true;
}

bool emit_function_default(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    return emit_value_expr(e, expr, out);
}
