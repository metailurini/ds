#include "bash_internal.h"

#include <string.h>

static bool is_int_binary_op(DsStr op) {
    return str_eq(op, "+") || str_eq(op, "-") || str_eq(op, "*") ||
           str_eq(op, "/") || str_eq(op, "%") || str_eq(op, "**");
}

static const char *return_expr_type_name(const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INTERP: return "string";
        case DS_LOWER_EXPR_INT: return "int";
        case DS_LOWER_EXPR_BOOL: return "bool";
        case DS_LOWER_EXPR_ARRAY: return "array";
        case DS_LOWER_EXPR_MAP: return "map";
        case DS_LOWER_EXPR_RUN: return "command_result";
        case DS_LOWER_EXPR_BINARY: return is_int_binary_op(expr->as.binary.op) ? "int" : "bool";
        case DS_LOWER_EXPR_UNARY:
            if (str_eq(expr->as.unary.op, "!")) return "bool";
            if (str_eq(expr->as.unary.op, "-")) return "int";
            return "unknown";
        case DS_LOWER_EXPR_CALL: return bash_lower_value_type_name(expr->as.call.return_kind);
        case DS_LOWER_EXPR_INDEX: return bash_lower_value_type_name(expr->as.index.element_kind);
        default: return "unknown";
    }
}

static void emit_array_element_type_entries(BashEmitter *e, const DsLowerExprVec *elements, int indent, const char *target_name) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "declare -ga ");
    buf_append(&e->out, target_name);
    buf_append(&e->out, "=(");
    for (size_t i = 0; i < elements->len; i++) {
        if (i) buf_append(&e->out, " ");
        const char *type = return_expr_type_name(elements->items[i]);
        bash_single_quote(&e->out, type, strlen(type));
    }
    buf_append(&e->out, ")\n");
}

static void emit_map_value_type_entries(BashEmitter *e, const DsLowerMapEntryVec *entries, int indent, const char *target_name) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "declare -gA ");
    buf_append(&e->out, target_name);
    buf_append(&e->out, "=(");
    for (size_t i = 0; i < entries->len; i++) {
        if (i) buf_append(&e->out, " ");
        buf_append(&e->out, "[");
        bash_single_quote(&e->out, entries->items[i].key.data, entries->items[i].key.len);
        buf_append(&e->out, "]=");
        const char *type = return_expr_type_name(entries->items[i].value);
        bash_single_quote(&e->out, type, strlen(type));
    }
    buf_append(&e->out, ")\n");
}

static void emit_return_type(BashEmitter *e, DsLowerValueKind kind, int indent) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "__ds_return_type=");
    const char *return_type = bash_lower_value_type_name(kind);
    bash_single_quote(&e->out, return_type, strlen(return_type));
    buf_append(&e->out, "\n");
}

static bool emit_array_return(BashEmitter *e, const DsLowerExpr *value, DsSpan span, int indent) {
    if (value->kind == DS_LOWER_EXPR_ARRAY) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -ga __ds_return_array=");
        if (!emit_array_elements(e, &value->as.array.elements, &e->out)) return false;
        buf_append(&e->out, "\n");
        emit_array_element_type_entries(e, &value->as.array.elements, indent, "__ds_return_elem_type");
        return true;
    }
    if (value->kind == DS_LOWER_EXPR_IDENT) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -ga __ds_return_array=(\"${");
        emit_var_name(&e->out, value->as.text);
        buf_append(&e->out, "[@]}\")\n");
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -ga __ds_return_elem_type=(\"${");
        bash_emit_elem_type_var_name(&e->out, value->as.text);
        buf_append(&e->out, "[@]}\")\n");
        return true;
    }
    ds_diag_error(e->diag, span, "internal Bash invariant failed: array return should be literal, named, or forwarded after lowering");
    return false;
}

static bool emit_map_return(BashEmitter *e, const DsLowerExpr *value, DsSpan span, int indent) {
    if (value->kind == DS_LOWER_EXPR_MAP) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -gA __ds_return_map=");
        if (!emit_map_entries(e, &value->as.map.entries, &e->out)) return false;
        buf_append(&e->out, "\n");
        emit_map_value_type_entries(e, &value->as.map.entries, indent, "__ds_return_value_type");
        return true;
    }
    if (value->kind == DS_LOWER_EXPR_IDENT) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -gA __ds_return_map=()\n");
        emit_indent(&e->out, indent);
        buf_append(&e->out, "for __ds_key in \"${!");
        emit_var_name(&e->out, value->as.text);
        buf_append(&e->out, "[@]}\"; do __ds_return_map[\"$__ds_key\"]=\"${");
        emit_var_name(&e->out, value->as.text);
        buf_append(&e->out, "[$__ds_key]}\"; done\n");
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -gA __ds_return_value_type=()\n");
        emit_indent(&e->out, indent);
        buf_append(&e->out, "for __ds_key in \"${!");
        bash_emit_map_value_type_var_name(&e->out, value->as.text);
        buf_append(&e->out, "[@]}\"; do __ds_return_value_type[\"$__ds_key\"]=\"${");
        bash_emit_map_value_type_var_name(&e->out, value->as.text);
        buf_append(&e->out, "[$__ds_key]}\"; done\n");
        return true;
    }
    ds_diag_error(e->diag, span, "internal Bash invariant failed: map return should be literal, named, or forwarded after lowering");
    return false;
}

static bool emit_command_result_return(BashEmitter *e, const DsLowerExpr *value, DsSpan span, int indent) {
    if (value->kind == DS_LOWER_EXPR_RUN) {
        if (ds_command_is_pipeline(&value->as.run)) {
            DsStr ret_name = {"return", strlen("return")};
            return bash_emit_capture_pipeline_assignment(e, ret_name, &value->as.run, value->span, indent);
        }
        emit_indent(&e->out, indent);
        buf_append(&e->out, "__ds_capture __ds_return");
        if (!emit_capture_command(e, &value->as.run, &e->out, value->span)) return false;
        buf_append(&e->out, "\n");
        return true;
    }
    if (value->kind == DS_LOWER_EXPR_IDENT) {
        bash_emit_command_result_copy_to_return(e, value->as.text, indent);
        return true;
    }
    ds_diag_error(e->diag, span, "internal Bash invariant failed: command-result return should be run, named, or forwarded after lowering");
    return false;
}

bool bash_emit_return_stmt(BashEmitter *e, const DsLowerStmt *stmt, int indent) {
    const DsLowerExpr *value = stmt->as.return_stmt.value;
    DsLowerValueKind kind = stmt->as.return_stmt.return_kind;

    emit_return_type(e, kind, indent);
    emit_indent(&e->out, indent);

    if (value->kind == DS_LOWER_EXPR_CALL && value->as.call.is_user_function) {
        if (!bash_emit_user_call_capture_return(e, value, kind, indent)) return false;
        emit_return_type(e, kind, indent);
    } else if (kind == DS_LOWER_VALUE_ARRAY) {
        if (!emit_array_return(e, value, stmt->span, indent)) return false;
    } else if (kind == DS_LOWER_VALUE_MAP) {
        if (!emit_map_return(e, value, stmt->span, indent)) return false;
    } else if (kind == DS_LOWER_VALUE_COMMAND_RESULT) {
        if (!emit_command_result_return(e, value, stmt->span, indent)) return false;
    } else {
        buf_append(&e->out, "__ds_return_value=");
        if (!emit_value_expr(e, value, &e->out)) return false;
        buf_append(&e->out, " || return $?\n");
    }

    emit_indent(&e->out, indent);
    buf_append(&e->out, "return 0\n\n");
    return true;
}
