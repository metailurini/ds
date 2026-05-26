#include "bash_internal.h"
#include "ds_command_result.h"

#include <string.h>

/*
 * Bash-specific structured-value ABI helpers.
 *
 * This file owns only Bash naming/declaration details for structured value
 * sidecars. It does not decide source-language validity or semantic value
 * kinds; those are lowerer/HIR responsibilities.
 */
const char *bash_lower_value_type_name(DsLowerValueKind kind) {
    switch (kind) {
        case DS_LOWER_VALUE_BOOL: return "bool";
        case DS_LOWER_VALUE_INT: return "int";
        case DS_LOWER_VALUE_STRING: return "string";
        case DS_LOWER_VALUE_ARRAY: return "array";
        case DS_LOWER_VALUE_MAP: return "map";
        case DS_LOWER_VALUE_COMMAND_RESULT: return "command_result";
        case DS_LOWER_VALUE_UNKNOWN: return "unknown";
    }
    return "unknown";
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
