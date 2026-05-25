#include "bash_internal.h"

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

void bash_emit_command_result_storage_decl(BashEmitter *e, DsStr name, int indent, bool local_decl) {
    emit_indent(&e->out, indent);
    if (local_decl) {
        buf_append(&e->out, "local ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_stdout ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_stderr ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_code ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_status ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_ok ");
        emit_var_name(&e->out, name); buf_append(&e->out, "_failed\n");
        return;
    }
    emit_var_name(&e->out, name); buf_append(&e->out, "_stdout=\"\"; ");
    emit_var_name(&e->out, name); buf_append(&e->out, "_stderr=\"\"; ");
    emit_var_name(&e->out, name); buf_append(&e->out, "_code=0; ");
    emit_var_name(&e->out, name); buf_append(&e->out, "_status=0; ");
    emit_var_name(&e->out, name); buf_append(&e->out, "_ok=false; ");
    emit_var_name(&e->out, name); buf_append(&e->out, "_failed=true\n");
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
