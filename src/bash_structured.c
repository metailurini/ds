#include "bash_internal.h"
#include "ds_command_facts.h"

/*
 * Bash-specific structured-value ABI helpers.
 *
 * This file owns only Bash naming/declaration details for structured value
 * sidecars. It does not decide source-language validity or semantic value
 * kinds; those are lowerer/HIR responsibilities.
 */
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
            return bash_is_int_binary_op(expr->as.binary.op)
                ? ds_lower_value_kind_name(DS_LOWER_VALUE_INT)
                : ds_lower_value_kind_name(DS_LOWER_VALUE_BOOL);
        case DS_LOWER_EXPR_UNARY:
            if (ds_str_eq_cstr(expr->as.unary.op, "!")) return ds_lower_value_kind_name(DS_LOWER_VALUE_BOOL);
            if (ds_str_eq_cstr(expr->as.unary.op, "-")) return ds_lower_value_kind_name(DS_LOWER_VALUE_INT);
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
    buf_append_dsstr(out, name);
}

void bash_emit_elem_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_elem_type_");
    buf_append_dsstr(out, name);
}

void bash_emit_map_value_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_value_type_");
    buf_append_dsstr(out, name);
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
            buf_append_dsstr(&e->out, value->as.index.index->as.text);
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

static void bash_emit_type_assignment_for_expr_impl(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent, bool local_decl) {
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

void bash_emit_type_assignment_for_expr(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent, bool local_decl) {
    if (!e->needs_case_types) return;
    bash_emit_type_assignment_for_expr_impl(e, name, value, indent, local_decl);
}

void bash_emit_type_assignment_for_expr_required(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent, bool local_decl) {
    bash_emit_type_assignment_for_expr_impl(e, name, value, indent, local_decl);
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
    if (value->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(value->as.call.name)) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -ga __ds_return_array=()\n");
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -ga __ds_return_elem_type=()\n");
        size_t temp_id = e->temp_counter++;
        emit_indent(&e->out, indent);
        buf_appendf(&e->out, "__ds_mktemp_file __ds_return_iter_%zu 'failed to create stdlib return temp file'\n", temp_id);
        emit_indent(&e->out, indent);
        if (!emit_stdlib_call(e, value, &e->out)) return false;
        buf_appendf(&e->out, " >\"$__ds_return_iter_%zu\"\n", temp_id);
        emit_indent(&e->out, indent);
        buf_append(&e->out, stdlib_array_call_uses_nul_records(value) ?
                   "while IFS= read -r -d '' __ds_line; do __ds_return_array+=(\"$__ds_line\"); __ds_return_elem_type+=(\"string\"); done" :
                   "while IFS= read -r __ds_line; do __ds_return_array+=(\"$__ds_line\"); __ds_return_elem_type+=(\"string\"); done");
        buf_appendf(&e->out, " <\"$__ds_return_iter_%zu\"\n", temp_id);
        emit_indent(&e->out, indent);
        buf_appendf(&e->out, "__ds_temp_remove \"$__ds_return_iter_%zu\"\n", temp_id);
        return true;
    }
    return bash_invariant_fail(e, span, "array return should be literal, named, or forwarded after lowering");
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
    return bash_invariant_fail(e, span, "map return should be literal, named, or forwarded after lowering");
}

/*
 * Row-array Bash ABI. Row arrays are stored as an index array, an element-type
 * sidecar, and one field sidecar per schema field. The sort implementation is
 * intentionally stable insertion sort; v0.37 row arrays are scoped for small
 * in-memory analyzer/reporting datasets rather than large external tables.
 */
static void bash_emit_row_field_suffix(EmitBuf *out, DsStr field) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < field.len; i++) {
        unsigned char c = (unsigned char)field.data[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            buf_append_len(out, (const char *)&field.data[i], 1);
        } else {
            char esc[4] = {'_', hex[c >> 4], hex[c & 0xf], 0};
            buf_append(out, esc);
        }
    }
}

void bash_emit_row_field_array_name(EmitBuf *out, DsStr array_name, DsStr field) {
    buf_append(out, "__ds_row_");
    buf_append_dsstr(out, array_name);
    buf_append(out, "_");
    bash_emit_row_field_suffix(out, field);
}

void bash_emit_return_row_field_array_name(EmitBuf *out, DsStr field) {
    buf_append(out, "__ds_return_row_");
    bash_emit_row_field_suffix(out, field);
}

bool bash_emit_row_array_decls(BashEmitter *e, DsStr name, const DsLowerRowSchema *schema, int indent, bool local_decl);
bool bash_emit_row_array_literal(BashEmitter *e, DsStr name, const DsLowerExpr *array, const DsLowerRowSchema *schema, int indent, bool local_decl);
bool bash_emit_row_array_expr_into(BashEmitter *e, DsStr dest, const DsLowerExpr *value, const DsLowerRowSchema *schema, int indent, bool local_decl);
bool bash_emit_row_array_sort_call(BashEmitter *e, DsStr dest, const DsLowerExpr *call, const DsLowerRowSchema *schema, int indent, bool local_decl);

bool bash_emit_row_array_return_payload(BashEmitter *e, const DsLowerExpr *value, const DsLowerRowSchema *schema, DsSpan span, int indent) {
    if (!value) return false;
    char temp_buf[64];
    DsStr source = {0};
    if (value->kind == DS_LOWER_EXPR_IDENT) {
        source = value->as.text;
    } else {
        bash_temp_ds_name(temp_buf, sizeof(temp_buf), "return_rows", e->temp_counter++);
        source = (DsStr){temp_buf, strlen(temp_buf)};
        if (value->kind == DS_LOWER_EXPR_ARRAY) {
            if (!bash_emit_row_array_literal(e, source, value, schema, indent, true)) return false;
        } else if (value->kind == DS_LOWER_EXPR_CALL && value->as.call.returns_row_array && ds_str_eq_cstr(value->as.call.name, "rowarray.sort_by")) {
            if (!bash_emit_row_array_sort_call(e, source, value, schema, indent, true)) return false;
        } else if (value->kind == DS_LOWER_EXPR_CALL && value->as.call.returns_row_array && value->as.call.is_user_function) {
            if (!bash_emit_row_array_decls(e, source, schema, indent, true)) return false;
            if (!bash_emit_user_function_value_call_into(e, source, value, indent)) return false;
        } else {
            return bash_invariant_fail(e, span, "unsupported row-array return expression after lowering");
        }
    }
    DsLowerExpr source_expr;
    memset(&source_expr, 0, sizeof(source_expr));
    source_expr.kind = DS_LOWER_EXPR_IDENT;
    source_expr.span = value->span;
    source_expr.as.text = source;
    if (!bash_emit_array_return_payload(e, &source_expr, span, indent)) return false;
    for (size_t i = 0; schema && i < schema->len; i++) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, "declare -ga ");
        bash_emit_return_row_field_array_name(&e->out, schema->items[i].name);
        buf_append(&e->out, "=(\"${");
        bash_emit_row_field_array_name(&e->out, source, schema->items[i].name);
        buf_append(&e->out, "[@]}\")\n");
    }
    return true;
}
const DsLowerMapEntry *bash_row_map_entry(const DsLowerExpr *row, DsStr field) {
    if (!row || row->kind != DS_LOWER_EXPR_MAP) return NULL;
    for (size_t i = 0; i < row->as.map.entries.len; i++) {
        const DsLowerMapEntry *entry = &row->as.map.entries.items[i];
        if (ds_str_eq(entry->key, field)) return entry;
    }
    return NULL;
}

bool bash_emit_row_array_decls(BashEmitter *e, DsStr name, const DsLowerRowSchema *schema, int indent, bool local_decl) {
    emit_indent(&e->out, indent);
    buf_append(&e->out, local_decl ? "local -a " : "declare -a ");
    emit_var_name(&e->out, name);
    buf_append(&e->out, "=()\n");
    emit_indent(&e->out, indent);
    buf_append(&e->out, local_decl ? "local -a " : "declare -a ");
    bash_emit_elem_type_var_name(&e->out, name);
    buf_append(&e->out, "=()\n");
    for (size_t i = 0; schema && i < schema->len; i++) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, local_decl ? "local -a " : "declare -a ");
        bash_emit_row_field_array_name(&e->out, name, schema->items[i].name);
        buf_append(&e->out, "=()\n");
    }
    return true;
}

static void emit_row_map_field_ref(EmitBuf *out, DsStr row_name, DsStr field) {
    buf_append(out, "${");
    emit_var_name(out, row_name);
    buf_append(out, "[");
    bash_single_quote(out, field.data, field.len);
    buf_append(out, "]}");
}

bool bash_emit_row_scalar_sidecars_from_map(BashEmitter *e, DsStr name, const DsLowerRowSchema *schema, int indent) {
    for (size_t i = 0; schema && i < schema->len; i++) {
        const DsLowerRowField *field = &schema->items[i];
        if (!is_safe_identifier(field->name)) continue;
        emit_indent(&e->out, indent);
        emit_var_name(&e->out, name);
        buf_append(&e->out, "_");
        buf_append_dsstr(&e->out, field->name);
        buf_append(&e->out, "=\"");
        emit_row_map_field_ref(&e->out, name, field->name);
        buf_append(&e->out, "\"\n");
    }
    return true;
}

bool bash_emit_row_array_push_literal(BashEmitter *e, DsStr name, const DsLowerRowSchema *schema, const DsLowerExpr *row, int indent) {
    if (e->function_depth > 0) {
        emit_indent(&e->out, indent);
        buf_append(&e->out, "local -p ");
        bash_emit_elem_type_var_name(&e->out, name);
        buf_append(&e->out, " >/dev/null 2>&1 || local -a ");
        bash_emit_elem_type_var_name(&e->out, name);
        buf_append(&e->out, "=()\n");
        for (size_t i = 0; schema && i < schema->len; i++) {
            emit_indent(&e->out, indent);
            buf_append(&e->out, "local -p ");
            bash_emit_row_field_array_name(&e->out, name, schema->items[i].name);
            buf_append(&e->out, " >/dev/null 2>&1 || local -a ");
            bash_emit_row_field_array_name(&e->out, name, schema->items[i].name);
            buf_append(&e->out, "=()\n");
        }
    }
    emit_indent(&e->out, indent);
    emit_var_name(&e->out, name);
    buf_append(&e->out, "+=(\"${#");
    emit_var_name(&e->out, name);
    buf_append(&e->out, "[@]}\")\n");
    emit_indent(&e->out, indent);
    bash_emit_elem_type_var_name(&e->out, name);
    buf_append(&e->out, "+=(\"map\")\n");
    for (size_t i = 0; schema && i < schema->len; i++) {
        emit_indent(&e->out, indent);
        bash_emit_row_field_array_name(&e->out, name, schema->items[i].name);
        buf_append(&e->out, "+=(");
        if (row && row->kind == DS_LOWER_EXPR_MAP) {
            const DsLowerMapEntry *entry = bash_row_map_entry(row, schema->items[i].name);
            if (!entry) {
            return bash_invariant_fail(e, row ? row->span : (DsSpan){0}, "row literal missing schema field");
            }
            if (!emit_value_expr(e, entry->value, &e->out)) return false;
        } else if (row && row->kind == DS_LOWER_EXPR_IDENT) {
            buf_append(&e->out, "\"");
            emit_row_map_field_ref(&e->out, row->as.text, schema->items[i].name);
            buf_append(&e->out, "\"");
        } else {
            return bash_invariant_fail(e, row ? row->span : (DsSpan){0}, "row-array literal elements should be row literals or named rows after lowering");
        }
        buf_append(&e->out, ")\n");
    }
    return true;
}

bool bash_emit_row_array_literal(BashEmitter *e, DsStr name, const DsLowerExpr *array, const DsLowerRowSchema *schema, int indent, bool local_decl) {
    if (!bash_emit_row_array_decls(e, name, schema, indent, local_decl)) return false;
    for (size_t i = 0; array && i < array->as.array.elements.len; i++) {
        if (!bash_emit_row_array_push_literal(e, name, schema, array->as.array.elements.items[i], indent)) return false;
    }
    return true;
}

static bool emit_row_index_arg(BashEmitter *e, const DsLowerExpr *index, EmitBuf *out) {
    if (index->kind == DS_LOWER_EXPR_INT) {
        buf_append_dsstr(out, index->as.text);
        return true;
    }
    if (index->kind == DS_LOWER_EXPR_IDENT) {
        buf_append(out, "\"$");
        emit_var_name(out, index->as.text);
        buf_append(out, "\"");
        return true;
    }
    return bash_invariant_fail(e, index->span, "row-array index should be a literal or variable after lowering");
}

bool bash_emit_row_from_index(BashEmitter *e, DsStr dest, const DsLowerExpr *index_expr, const DsLowerRowSchema *schema, int indent, bool local_decl) {
    if (!index_expr || index_expr->kind != DS_LOWER_EXPR_INDEX || !index_expr->as.index.returns_row ||
        !index_expr->as.index.object || index_expr->as.index.object->kind != DS_LOWER_EXPR_IDENT) return false;
    DsStr src = index_expr->as.index.object->as.text;
    emit_indent(&e->out, indent);
    buf_append(&e->out, local_decl ? "local -A " : "declare -A ");
    emit_var_name(&e->out, dest);
    buf_append(&e->out, "=()\n");
    emit_indent(&e->out, indent);
    buf_append(&e->out, local_decl ? "local -A " : "declare -A ");
    bash_emit_map_value_type_var_name(&e->out, dest);
    buf_append(&e->out, "=()\n");
    for (size_t i = 0; schema && i < schema->len; i++) {
        const DsLowerRowField *field = &schema->items[i];
        emit_indent(&e->out, indent);
        emit_var_name(&e->out, dest);
        buf_append(&e->out, "[");
        bash_single_quote(&e->out, field->name.data, field->name.len);
        buf_append(&e->out, "]=\"$( __ds_array_get ");
        bash_emit_row_field_array_name(&e->out, src, field->name);
        buf_append(&e->out, " ");
        if (!emit_row_index_arg(e, index_expr->as.index.index, &e->out)) return false;
        buf_append(&e->out, " )\"\n");
        emit_indent(&e->out, indent);
        bash_emit_map_value_type_var_name(&e->out, dest);
        buf_append(&e->out, "[");
        bash_single_quote(&e->out, field->name.data, field->name.len);
        buf_append(&e->out, "]=");
        bash_single_quote(&e->out, ds_lower_value_kind_name(field->kind), strlen(ds_lower_value_kind_name(field->kind)));
        buf_append(&e->out, "\n");
        if (is_safe_identifier(field->name)) {
            emit_indent(&e->out, indent);
            emit_var_name(&e->out, dest);
            buf_append(&e->out, "_");
            buf_append_dsstr(&e->out, field->name);
            buf_append(&e->out, "=\"$( __ds_array_get ");
            bash_emit_row_field_array_name(&e->out, src, field->name);
            buf_append(&e->out, " ");
            if (!emit_row_index_arg(e, index_expr->as.index.index, &e->out)) return false;
            buf_append(&e->out, " )\"\n");
        }
    }
    return true;
}

bool bash_emit_row_array_copy(BashEmitter *e, DsStr dest, DsStr src, const DsLowerRowSchema *schema, int indent, bool local_decl) {
    if (!bash_emit_row_array_decls(e, dest, schema, indent, local_decl)) return false;
    emit_indent(&e->out, indent);
    emit_var_name(&e->out, dest);
    buf_append(&e->out, "=(\"${");
    emit_var_name(&e->out, src);
    buf_append(&e->out, "[@]}\")\n");
    emit_indent(&e->out, indent);
    bash_emit_elem_type_var_name(&e->out, dest);
    buf_append(&e->out, "=(\"${");
    bash_emit_elem_type_var_name(&e->out, src);
    buf_append(&e->out, "[@]}\")\n");
    for (size_t i = 0; schema && i < schema->len; i++) {
        emit_indent(&e->out, indent);
        bash_emit_row_field_array_name(&e->out, dest, schema->items[i].name);
        buf_append(&e->out, "=(\"${");
        bash_emit_row_field_array_name(&e->out, src, schema->items[i].name);
        buf_append(&e->out, "[@]}\")\n");
    }
    return true;
}

bool bash_emit_row_array_expr_into(BashEmitter *e, DsStr dest, const DsLowerExpr *value, const DsLowerRowSchema *schema, int indent, bool local_decl) {
    if (!value) return false;
    if (value->kind == DS_LOWER_EXPR_IDENT) {
        return bash_emit_row_array_copy(e, dest, value->as.text, schema, indent, local_decl);
    }
    if (value->kind == DS_LOWER_EXPR_ARRAY) {
        return bash_emit_row_array_literal(e, dest, value, schema, indent, local_decl);
    }
    if (value->kind == DS_LOWER_EXPR_CALL && value->as.call.returns_row_array && ds_str_eq_cstr(value->as.call.name, "rowarray.sort_by")) {
        return bash_emit_row_array_sort_call(e, dest, value, schema, indent, local_decl);
    }
    if (value->kind == DS_LOWER_EXPR_CALL && value->as.call.returns_row_array && value->as.call.is_user_function) {
        if (!bash_emit_row_array_decls(e, dest, schema, indent, local_decl)) return false;
        return bash_emit_user_function_value_call_into(e, dest, value, indent);
    }
    return bash_invariant_fail(e, value->span, "unsupported row-array expression after lowering");
}

static DsLowerValueKind row_schema_field_kind(const DsLowerRowSchema *schema, DsStr field) {
    if (!schema) return DS_LOWER_VALUE_UNKNOWN;
    for (size_t i = 0; i < schema->len; i++) {
        if (ds_str_eq(schema->items[i].name, field)) {
            return schema->items[i].kind;
        }
    }
    return DS_LOWER_VALUE_UNKNOWN;
}

bool bash_emit_row_array_sort_call(BashEmitter *e, DsStr dest, const DsLowerExpr *call, const DsLowerRowSchema *schema, int indent, bool local_decl) {
    if (!call || call->as.call.args.len < 3) return false;
    char *field_data = NULL, *dir_data = NULL;
    size_t field_len = 0, dir_len = 0;
    if (!decode_string_literal(e->diag, call->as.call.args.items[1], &field_data, &field_len) ||
        !decode_string_literal(e->diag, call->as.call.args.items[2], &dir_data, &dir_len)) return false;
    DsStr field = {field_data, field_len};
    DsLowerValueKind field_kind = row_schema_field_kind(schema, field);
    bool desc = dir_len == 4 && memcmp(dir_data, "desc", 4) == 0;
    size_t id = e->temp_counter++;
    char source_buf[64];
    DsStr src = {0};
    const DsLowerExpr *source_expr = call->as.call.args.items[0];
    if (source_expr->kind == DS_LOWER_EXPR_IDENT) {
        src = source_expr->as.text;
    } else {
        bash_temp_ds_name(source_buf, sizeof(source_buf), "row_sort_src", id);
        src = (DsStr){source_buf, strlen(source_buf)};
        if (!bash_emit_row_array_expr_into(e, src, source_expr, schema, indent, local_decl)) { free(field_data); free(dir_data); return false; }
    }
    if (!bash_emit_row_array_decls(e, dest, schema, indent, local_decl)) { free(field_data); free(dir_data); return false; }
    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "%s -a __ds_sort_%zu=(\"${!", e->function_depth > 0 ? "local" : "declare", id);
    emit_var_name(&e->out, src);
    buf_append(&e->out, "[@]}\")\n");
    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "%s __ds_i_%zu __ds_j_%zu __ds_key_%zu __ds_prev_%zu __ds_left_%zu __ds_right_%zu __ds_lc_set_%zu=0 __ds_lc_old_%zu=\"\"\n", e->function_depth > 0 ? "local" : "declare", id, id, id, id, id, id, id, id);
    if (field_kind == DS_LOWER_VALUE_STRING) {
        emit_indent(&e->out, indent);
        buf_appendf(&e->out, "if [[ ${LC_ALL+x} ]]; then __ds_lc_set_%zu=1; __ds_lc_old_%zu=\"$LC_ALL\"; fi\n", id, id);
        emit_indent(&e->out, indent);
        buf_append(&e->out, "LC_ALL=C\n");
    }
    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "for ((__ds_i_%zu=1; __ds_i_%zu<${#__ds_sort_%zu[@]}; __ds_i_%zu++)); do\n", id, id, id, id);
    emit_indent(&e->out, indent + 1);
    buf_appendf(&e->out, "__ds_key_%zu=\"${__ds_sort_%zu[$__ds_i_%zu]}\"\n", id, id, id);
    emit_indent(&e->out, indent + 1);
    buf_appendf(&e->out, "__ds_j_%zu=$__ds_i_%zu\n", id, id);
    emit_indent(&e->out, indent + 1);
    buf_appendf(&e->out, "while (( __ds_j_%zu > 0 )); do\n", id);
    emit_indent(&e->out, indent + 2);
    buf_appendf(&e->out, "__ds_prev_%zu=\"${__ds_sort_%zu[$((__ds_j_%zu - 1))]}\"\n", id, id, id);
    emit_indent(&e->out, indent + 2);
    buf_appendf(&e->out, "__ds_left_%zu=\"${", id);
    bash_emit_row_field_array_name(&e->out, src, field);
    buf_appendf(&e->out, "[$__ds_prev_%zu]}\"\n", id);
    emit_indent(&e->out, indent + 2);
    buf_appendf(&e->out, "__ds_right_%zu=\"${", id);
    bash_emit_row_field_array_name(&e->out, src, field);
    buf_appendf(&e->out, "[$__ds_key_%zu]}\"\n", id);
    emit_indent(&e->out, indent + 2);
    if (field_kind == DS_LOWER_VALUE_INT) {
        buf_appendf(&e->out, "if (( __ds_left_%zu %s __ds_right_%zu )); then\n", id, desc ? "<" : ">", id);
    } else if (field_kind == DS_LOWER_VALUE_BOOL) {
        buf_appendf(&e->out, "if [[ \"$__ds_left_%zu\" == true ]]; then __ds_left_%zu=1; else __ds_left_%zu=0; fi\n", id, id, id);
        emit_indent(&e->out, indent + 2);
        buf_appendf(&e->out, "if [[ \"$__ds_right_%zu\" == true ]]; then __ds_right_%zu=1; else __ds_right_%zu=0; fi\n", id, id, id);
        emit_indent(&e->out, indent + 2);
        buf_appendf(&e->out, "if (( __ds_left_%zu %s __ds_right_%zu )); then\n", id, desc ? "<" : ">", id);
    } else {
        buf_appendf(&e->out, "if [[ \"$__ds_left_%zu\" %s \"$__ds_right_%zu\" ]]; then\n", id, desc ? "<" : ">", id);
    }
    emit_indent(&e->out, indent + 3);
    buf_appendf(&e->out, "__ds_sort_%zu[$__ds_j_%zu]=\"$__ds_prev_%zu\"\n", id, id, id);
    emit_indent(&e->out, indent + 3);
    buf_appendf(&e->out, "__ds_j_%zu=$((__ds_j_%zu - 1))\n", id, id);
    emit_indent(&e->out, indent + 2);
    buf_append(&e->out, "else break; fi\n");
    emit_indent(&e->out, indent + 1);
    buf_append(&e->out, "done\n");
    emit_indent(&e->out, indent + 1);
    buf_appendf(&e->out, "__ds_sort_%zu[$__ds_j_%zu]=\"$__ds_key_%zu\"\n", id, id, id);
    emit_indent(&e->out, indent);
    buf_append(&e->out, "done\n");
    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "for __ds_idx_%zu in \"${__ds_sort_%zu[@]}\"; do\n", id, id);
    emit_indent(&e->out, indent + 1);
    emit_var_name(&e->out, dest);
    buf_append(&e->out, "+=(\"${#");
    emit_var_name(&e->out, dest);
    buf_append(&e->out, "[@]}\")\n");
    emit_indent(&e->out, indent + 1);
    bash_emit_elem_type_var_name(&e->out, dest);
    buf_append(&e->out, "+=(\"map\")\n");
    for (size_t i = 0; schema && i < schema->len; i++) {
        emit_indent(&e->out, indent + 1);
        bash_emit_row_field_array_name(&e->out, dest, schema->items[i].name);
        buf_append(&e->out, "+=(\"${");
        bash_emit_row_field_array_name(&e->out, src, schema->items[i].name);
        buf_appendf(&e->out, "[$__ds_idx_%zu]}\")\n", id);
    }
    emit_indent(&e->out, indent);
    buf_append(&e->out, "done\n");
    if (field_kind == DS_LOWER_VALUE_STRING) {
        emit_indent(&e->out, indent);
        buf_appendf(&e->out, "if (( __ds_lc_set_%zu )); then LC_ALL=\"$__ds_lc_old_%zu\"; else unset LC_ALL; fi\n", id, id);
    }
    free(field_data);
    free(dir_data);
    return true;
}
