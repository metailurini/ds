#include "bash_internal.h"
#include "ds_command_facts.h"

#include <string.h>

/*
 * Bash-specific structured-value ABI helpers.
 *
 * This file owns only Bash naming/declaration details for structured value
 * sidecars. It does not decide source-language validity or semantic value
 * kinds; those are lowerer/HIR responsibilities.
 */
static bool bash_int_binary_op(DsStr op) {
    return str_eq(op, "+") || str_eq(op, "-") || str_eq(op, "*") ||
           str_eq(op, "/") || str_eq(op, "%") || str_eq(op, "**");
}

const char *bash_lower_expr_static_type_name(const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INTERP:
            return ds_lower_value_kind_name(DS_LOWER_VALUE_STRING);
        case DS_LOWER_EXPR_INT:
            return ds_lower_value_kind_name(DS_LOWER_VALUE_INT);
        case DS_LOWER_EXPR_BOOL:
            return ds_lower_value_kind_name(DS_LOWER_VALUE_BOOL);
        case DS_LOWER_EXPR_ARRAY:
            return ds_lower_value_kind_name(DS_LOWER_VALUE_ARRAY);
        case DS_LOWER_EXPR_MAP:
            return ds_lower_value_kind_name(DS_LOWER_VALUE_MAP);
        case DS_LOWER_EXPR_RUN:
            return ds_lower_value_kind_name(DS_LOWER_VALUE_COMMAND_RESULT);
        case DS_LOWER_EXPR_BINARY:
            return bash_int_binary_op(expr->as.binary.op)
                ? ds_lower_value_kind_name(DS_LOWER_VALUE_INT)
                : ds_lower_value_kind_name(DS_LOWER_VALUE_BOOL);
        case DS_LOWER_EXPR_UNARY:
            if (str_eq(expr->as.unary.op, "!")) return ds_lower_value_kind_name(DS_LOWER_VALUE_BOOL);
            if (str_eq(expr->as.unary.op, "-")) return ds_lower_value_kind_name(DS_LOWER_VALUE_INT);
            return ds_lower_value_kind_name(DS_LOWER_VALUE_UNKNOWN);
        case DS_LOWER_EXPR_CALL:
            return ds_lower_value_kind_name(expr->as.call.return_kind);
        case DS_LOWER_EXPR_FIELD: {
            const DsCommandResultField *desc = ds_command_result_field_lookup(expr->as.field.field);
            return desc ? ds_command_result_field_kind_name(desc->kind) : ds_lower_value_kind_name(DS_LOWER_VALUE_UNKNOWN);
        }
        case DS_LOWER_EXPR_INDEX:
            return ds_lower_value_kind_name(expr->as.index.element_kind);
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_REGEX:
        case DS_LOWER_EXPR_RANGE:
        case DS_LOWER_EXPR_ERROR:
            return ds_lower_value_kind_name(DS_LOWER_VALUE_UNKNOWN);
    }
    return ds_lower_value_kind_name(DS_LOWER_VALUE_UNKNOWN);
}

void bash_emit_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_type_");
    buf_append_len(out, name.data, name.len);
}

void bash_emit_elem_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_elem_type_");
    buf_append_len(out, name.data, name.len);
}

void bash_emit_map_value_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_value_type_");
    buf_append_len(out, name.data, name.len);
}

static const char *command_result_field_default(const DsCommandResultField *field) {
    switch (field->id) {
        case DS_COMMAND_RESULT_FIELD_STDOUT:
        case DS_COMMAND_RESULT_FIELD_STDERR:
            return "\"\"";
        case DS_COMMAND_RESULT_FIELD_STATUS:
        case DS_COMMAND_RESULT_FIELD_CODE:
            return "0";
        case DS_COMMAND_RESULT_FIELD_OK:
            return "false";
        case DS_COMMAND_RESULT_FIELD_FAILED:
            return "true";
    }
    return "\"\"";
}

static void emit_command_result_var(EmitBuf *out, DsStr name, const char *field_name) {
    emit_var_name(out, name);
    buf_append(out, "_");
    buf_append(out, field_name);
}

void bash_emit_command_result_storage_decl(BashEmitter *e, DsStr name, int indent, bool local_decl) {
    emit_indent(&e->out, indent);
    if (local_decl) {
        buf_append(&e->out, "local ");
        for (size_t i = 0; i < ds_command_result_field_count(); i++) {
            const DsCommandResultField *field = ds_command_result_field_at(i);
            if (i > 0) buf_append(&e->out, " ");
            emit_command_result_var(&e->out, name, field->name);
        }
        buf_append(&e->out, "\n");
        return;
    }
    for (size_t i = 0; i < ds_command_result_field_count(); i++) {
        const DsCommandResultField *field = ds_command_result_field_at(i);
        if (i > 0) buf_append(&e->out, "; ");
        emit_command_result_var(&e->out, name, field->name);
        buf_append(&e->out, "=");
        buf_append(&e->out, command_result_field_default(field));
    }
    buf_append(&e->out, "\n");
}

DsStr bash_command_result_field_storage_name(DsStr field) {
    const DsCommandResultField *desc = ds_command_result_field_lookup(field);
    if (!desc) return field;
    return (DsStr){(char *)desc->storage_name, strlen(desc->storage_name)};
}

bool bash_command_result_field_is_bool(DsStr field) {
    const DsCommandResultField *desc = ds_command_result_field_lookup(field);
    return desc && desc->kind == DS_COMMAND_RESULT_FIELD_BOOL;
}

void bash_emit_command_result_copy_to_return(BashEmitter *e, DsStr source, int indent) {
    for (size_t i = 0; i < ds_command_result_field_count(); i++) {
        const DsCommandResultField *field = ds_command_result_field_at(i);
        emit_indent(&e->out, indent);
        buf_append(&e->out, "printf -v __ds_return_");
        buf_append(&e->out, field->name);
        buf_append(&e->out, " '%s' \"$");
        emit_command_result_var(&e->out, source, field->storage_name);
        buf_append(&e->out, "\"\n");
    }
}

bool bash_emit_structured_target_decl(BashEmitter *e, DsStr name, DsLowerValueKind kind, int indent, bool local_decl) {
    switch (kind) {
        case DS_LOWER_VALUE_ARRAY:
            emit_indent(&e->out, indent);
            buf_append(&e->out, local_decl ? "local -a " : "declare -a ");
            emit_var_name(&e->out, name);
            buf_append(&e->out, "=()\n");
            emit_indent(&e->out, indent);
            buf_append(&e->out, local_decl ? "local -a " : "declare -a ");
            bash_emit_elem_type_var_name(&e->out, name);
            buf_append(&e->out, "=()\n");
            return true;
        case DS_LOWER_VALUE_MAP:
            emit_indent(&e->out, indent);
            buf_append(&e->out, local_decl ? "local -A " : "declare -A ");
            emit_var_name(&e->out, name);
            buf_append(&e->out, "=()\n");
            emit_indent(&e->out, indent);
            buf_append(&e->out, local_decl ? "local -A " : "declare -A ");
            bash_emit_map_value_type_var_name(&e->out, name);
            buf_append(&e->out, "=()\n");
            return true;
        case DS_LOWER_VALUE_COMMAND_RESULT:
            bash_emit_command_result_storage_decl(e, name, indent, local_decl);
            return true;
        default:
            emit_indent(&e->out, indent);
            if (local_decl) buf_append(&e->out, "local ");
            emit_var_name(&e->out, name);
            buf_append(&e->out, "=\"\"\n");
            return true;
    }
}

void bash_emit_expr_type_value(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    (void)e;
    if (expr->kind == DS_LOWER_EXPR_IDENT) {
        buf_append(out, "\"${");
        bash_emit_type_var_name(out, expr->as.text);
        buf_append(out, ":-unknown}\"");
        return;
    }
    const char *type = bash_lower_expr_static_type_name(expr);
    bash_single_quote(out, type, strlen(type));
}

void bash_emit_type_assignment(BashEmitter *e, DsStr name, const char *type, int indent, bool local_decl) {
    if (!e->needs_case_types) return;
    emit_indent(&e->out, indent);
    if (local_decl) buf_append(&e->out, "local ");
    bash_emit_type_var_name(&e->out, name);
    buf_append(&e->out, "=");
    bash_single_quote(&e->out, type, strlen(type));
    buf_append(&e->out, "\n");
}

static void emit_expr_array_element_types(BashEmitter *e, const DsLowerExprVec *elements, int indent, const char *decl, DsStr target_name) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, decl);
    bash_emit_elem_type_var_name(&e->out, target_name);
    buf_append(&e->out, "=(");
    for (size_t i = 0; i < elements->len; i++) {
        if (i) buf_append(&e->out, " ");
        const char *type = bash_lower_expr_static_type_name(elements->items[i]);
        bash_single_quote(&e->out, type, strlen(type));
    }
    buf_append(&e->out, ")\n");
}

static void emit_expr_map_value_types(BashEmitter *e, const DsLowerMapEntryVec *entries, int indent, const char *decl, DsStr target_name) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, decl);
    bash_emit_map_value_type_var_name(&e->out, target_name);
    buf_append(&e->out, "=(");
    for (size_t i = 0; i < entries->len; i++) {
        if (i) buf_append(&e->out, " ");
        buf_append(&e->out, "[");
        bash_single_quote(&e->out, entries->items[i].key.data, entries->items[i].key.len);
        buf_append(&e->out, "]=");
        const char *type = bash_lower_expr_static_type_name(entries->items[i].value);
        bash_single_quote(&e->out, type, strlen(type));
    }
    buf_append(&e->out, ")\n");
}

static void emit_index_type_assignment(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent, bool local_decl) {
    if (value->kind != DS_LOWER_EXPR_INDEX || value->as.index.object->kind != DS_LOWER_EXPR_IDENT) return;
    emit_indent(&e->out, indent);
    if (local_decl) buf_append(&e->out, "local ");
    bash_emit_type_var_name(&e->out, name);
    buf_append(&e->out, "=");
    if (value->as.index.object_is_array) {
        buf_append(&e->out, "\"${");
        bash_emit_elem_type_var_name(&e->out, value->as.index.object->as.text);
        buf_append(&e->out, "[");
        if (value->as.index.index->kind == DS_LOWER_EXPR_INT) {
            buf_append_len(&e->out, value->as.index.index->as.text.data, value->as.index.index->as.text.len);
        } else if (value->as.index.index->kind == DS_LOWER_EXPR_IDENT) {
            buf_append(&e->out, "$");
            emit_var_name(&e->out, value->as.index.index->as.text);
        } else {
            buf_append(&e->out, "0");
        }
        buf_append(&e->out, "]:-unknown}\"\n");
    } else if (value->as.index.object_is_map) {
        buf_append(&e->out, "\"${");
        bash_emit_map_value_type_var_name(&e->out, value->as.index.object->as.text);
        buf_append(&e->out, "[");
        if (value->as.index.map_key_literal) {
            bash_single_quote(&e->out, value->as.index.map_key.data, value->as.index.map_key.len);
        } else if (value->as.index.index->kind == DS_LOWER_EXPR_IDENT) {
            buf_append(&e->out, "$");
            emit_var_name(&e->out, value->as.index.index->as.text);
        }
        buf_append(&e->out, "]:-unknown}\"\n");
    }
}

void bash_emit_type_assignment_for_expr(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent, bool local_decl) {
    if (!e->needs_case_types) return;
    emit_indent(&e->out, indent);
    if (local_decl) buf_append(&e->out, "local ");
    bash_emit_type_var_name(&e->out, name);
    buf_append(&e->out, "=");
    bash_emit_expr_type_value(e, value, &e->out);
    buf_append(&e->out, "\n");

    if (value->kind == DS_LOWER_EXPR_ARRAY) {
        emit_expr_array_element_types(e, &value->as.array.elements, indent, local_decl ? "local -a " : "declare -a ", name);
    } else if (value->kind == DS_LOWER_EXPR_MAP) {
        emit_expr_map_value_types(e, &value->as.map.entries, indent, local_decl ? "local -A " : "declare -A ", name);
    } else {
        emit_index_type_assignment(e, name, value, indent, local_decl);
    }
}

void bash_emit_collection_element_type_value(BashEmitter *e, const DsLowerExpr *value, EmitBuf *out) {
    bash_emit_expr_type_value(e, value, out);
}

void bash_emit_return_type(BashEmitter *e, DsLowerValueKind kind, int indent) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "__ds_return_type=");
    const char *return_type = ds_lower_value_kind_name(kind);
    bash_single_quote(&e->out, return_type, strlen(return_type));
    buf_append(&e->out, "\n");
}

static void emit_return_array_element_types(BashEmitter *e, const DsLowerExprVec *elements, int indent, const char *target_name) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "declare -ga ");
    buf_append(&e->out, target_name);
    buf_append(&e->out, "=(");
    for (size_t i = 0; i < elements->len; i++) {
        if (i) buf_append(&e->out, " ");
        const char *type = bash_lower_expr_static_type_name(elements->items[i]);
        bash_single_quote(&e->out, type, strlen(type));
    }
    buf_append(&e->out, ")\n");
}

static void emit_return_map_value_types(BashEmitter *e, const DsLowerMapEntryVec *entries, int indent, const char *target_name) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, "declare -gA ");
    buf_append(&e->out, target_name);
    buf_append(&e->out, "=(");
    for (size_t i = 0; i < entries->len; i++) {
        if (i) buf_append(&e->out, " ");
        buf_append(&e->out, "[");
        bash_single_quote(&e->out, entries->items[i].key.data, entries->items[i].key.len);
        buf_append(&e->out, "]=");
        const char *type = bash_lower_expr_static_type_name(entries->items[i].value);
        bash_single_quote(&e->out, type, strlen(type));
    }
    buf_append(&e->out, ")\n");
}

bool bash_emit_array_return_payload(BashEmitter *e, const DsLowerExpr *value, DsSpan span, int indent) {
    if (value->kind == DS_LOWER_EXPR_ARRAY) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -ga __ds_return_array=");
        if (!emit_array_elements(e, &value->as.array.elements, &e->out)) return false;
        buf_append(&e->out, "\n");
        emit_return_array_element_types(e, &value->as.array.elements, indent, "__ds_return_elem_type");
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

bool bash_emit_map_return_payload(BashEmitter *e, const DsLowerExpr *value, DsSpan span, int indent) {
    if (value->kind == DS_LOWER_EXPR_MAP) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -gA __ds_return_map=");
        if (!emit_map_entries(e, &value->as.map.entries, &e->out)) return false;
        buf_append(&e->out, "\n");
        emit_return_map_value_types(e, &value->as.map.entries, indent, "__ds_return_value_type");
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
