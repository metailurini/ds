#include "bash_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static bool result_field_is_bool(DsStr field) {
    const DsCommandResultField *desc = ds_command_result_field_lookup(field);
    return desc && desc->kind == DS_COMMAND_RESULT_FIELD_BOOL;
}

static DsStr result_field_storage_name(DsStr field) {
    if (field.len == 6 && memcmp(field.data, "status", 6) == 0) {
        return (DsStr){"code", 4};
    }
    return field;
}

static void emit_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_type_");
    buf_append_len(out, name.data, name.len);
}

static void emit_elem_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_elem_type_");
    buf_append_len(out, name.data, name.len);
}

static bool is_int_binary_op(DsStr op) {
    return str_eq(op, "+") || str_eq(op, "-") || str_eq(op, "*") ||
           str_eq(op, "/") || str_eq(op, "%") || str_eq(op, "**");
}

static const char *lower_value_type_name(DsLowerValueKind kind);
static const char *expr_type_name(const DsLowerExpr *expr);

static bool is_user_function_call_expr(const DsLowerExpr *expr) {
    return expr && expr->kind == DS_LOWER_EXPR_CALL && expr->as.call.is_user_function;
}

static void temp_ds_name(char *buf, size_t cap, const char *prefix, size_t id) {
    snprintf(buf, cap, "__%s_%zu", prefix, id);
}

static bool emit_user_call_into_raw_var(BashEmitter *e, const DsLowerExpr *expr, DsStr raw_name, EmitBuf *out) {
    buf_append(out, "__ds_call_value_into ");
    emit_var_name(out, raw_name);
    buf_append(out, " ");
    bash_single_quote(out, lower_value_type_name(expr->as.call.return_kind), strlen(lower_value_type_name(expr->as.call.return_kind)));
    buf_append(out, " ");
    emit_fn_name(out, expr->as.call.name);
    return emit_user_call_args(e, &expr->as.call.args, out);
}

static bool emit_condition_operand_or_raw_temp(BashEmitter *e, const DsLowerExpr *expr, const DsStr *raw_temp, EmitBuf *out) {
    if (raw_temp) {
        buf_append(out, "\"$");
        emit_var_name(out, *raw_temp);
        buf_append(out, "\"");
        return true;
    }
    return emit_condition_operand(e, expr, out);
}

static const char *expr_type_name(const DsLowerExpr *expr) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INTERP:
            return "string";
        case DS_LOWER_EXPR_INT:
            return "int";
        case DS_LOWER_EXPR_BOOL:
            return "bool";
        case DS_LOWER_EXPR_ARRAY:
            return "array";
        case DS_LOWER_EXPR_MAP:
            return "map";
        case DS_LOWER_EXPR_RUN:
            return "command_result";
        case DS_LOWER_EXPR_BINARY:
            return is_int_binary_op(expr->as.binary.op) ? "int" : "bool";
        case DS_LOWER_EXPR_UNARY:
            if (str_eq(expr->as.unary.op, "!")) return "bool";
            if (str_eq(expr->as.unary.op, "-")) return "int";
            return "unknown";
        case DS_LOWER_EXPR_CALL:
            return lower_value_type_name(expr->as.call.return_kind);
        case DS_LOWER_EXPR_FIELD: {
            const DsCommandResultField *desc = ds_command_result_field_lookup(expr->as.field.field);
            if (!desc) return "unknown";
            switch (desc->kind) {
                case DS_COMMAND_RESULT_FIELD_STRING: return "string";
                case DS_COMMAND_RESULT_FIELD_INT: return "int";
                case DS_COMMAND_RESULT_FIELD_BOOL: return "bool";
            }
            return "unknown";
        }
        case DS_LOWER_EXPR_INDEX:
            return lower_value_type_name(expr->as.index.element_kind);
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_REGEX:
        case DS_LOWER_EXPR_RANGE:
        case DS_LOWER_EXPR_ERROR:
            return "unknown";
    }
    return "unknown";
}

static bool emit_user_call_arg_type(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    (void)e;
    if (expr->kind == DS_LOWER_EXPR_IDENT) {
        buf_append(out, " \"${");
        emit_type_var_name(out, expr->as.text);
        buf_append(out, ":-unknown}\"");
        return true;
    }
    buf_append(out, " ");
    const char *type = expr_type_name(expr);
    bash_single_quote(out, type, strlen(type));
    return true;
}

static const char *lower_value_type_name(DsLowerValueKind kind) {
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

static bool regex_literal_parts(DsStr lit, DsStr *pattern, bool *insensitive) {
    *insensitive = false;
    if (lit.len < 3 || lit.data[0] != '/') return false;
    size_t end = 0;
    for (size_t i = 1; i < lit.len; i++) {
        if (lit.data[i] == '\\') { i++; continue; }
        if (lit.data[i] == '/') { end = i; break; }
    }
    if (!end) return false;
    pattern->data = lit.data + 1;
    pattern->len = end - 1;
    if (end + 1 < lit.len) {
        if (end + 2 == lit.len && lit.data[end + 1] == 'i') *insensitive = true;
        else return false;
    }
    return true;
}

static bool emit_bash_regex_quoted(EmitBuf *out, DsStr pattern) {
    char *decoded = malloc(pattern.len + 1);
    if (!decoded) return false;
    size_t len = 0;
    for (size_t i = 0; i < pattern.len; i++) {
        char c = pattern.data[i];
        if (c == '\\' && i + 1 < pattern.len && pattern.data[i + 1] == '/') { decoded[len++] = '/'; i++; }
        else decoded[len++] = c;
    }
    bash_single_quote(out, decoded, len);
    free(decoded);
    return true;
}

static void emit_membership_left_type(const DsLowerExpr *left, DsLowerValueKind left_kind, EmitBuf *out) {
    if (left_kind != DS_LOWER_VALUE_UNKNOWN) {
        const char *type = lower_value_type_name(left_kind);
        bash_single_quote(out, type, strlen(type));
    } else if (left->kind == DS_LOWER_EXPR_IDENT) {
        buf_append(out, "\"${"); emit_type_var_name(out, left->as.text); buf_append(out, ":-unknown}\"");
    } else {
        buf_append(out, "'unknown'");
    }
}

static bool emit_membership_compare(BashEmitter *e, const DsLowerExpr *left, DsLowerValueKind left_kind, DsLowerValueKind elem_kind, const DsLowerExpr *elem, EmitBuf *out) {
    (void)e;
    (void)left;
    (void)left_kind;
    buf_append(out, "[[ ");
    buf_append(out, "$__ds_needle_type == ");
    const char *elem_type = lower_value_type_name(elem_kind);
    bash_single_quote(out, elem_type, strlen(elem_type));
    buf_append(out, " && \"$__ds_needle\" == ");
    if (elem) {
        if (!emit_condition_operand(e, elem, out)) return false;
    } else {
        buf_append(out, "\"$__ds_item\"");
    }
    buf_append(out, " ]]");
    return true;
}

static bool emit_membership_condition(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    const DsLowerExpr *left = expr->as.binary.left;
    const DsLowerExpr *right = expr->as.binary.right;
    DsLowerValueKind left_kind = expr->as.binary.left_kind;
    DsLowerValueKind elem_kind = expr->as.binary.right_element_kind;
    char left_temp_buf[64];
    DsStr left_temp = {0};
    const DsStr *left_temp_ptr = NULL;
    buf_append(out, "{ ");
    if (is_user_function_call_expr(left)) {
        temp_ds_name(left_temp_buf, sizeof(left_temp_buf), "in_left", e->temp_counter++);
        left_temp.data = left_temp_buf;
        left_temp.len = strlen(left_temp_buf);
        emit_var_name(out, left_temp);
        buf_append(out, "=\"\"; ");
        if (!emit_user_call_into_raw_var(e, left, left_temp, out)) return false;
        buf_append(out, "; ");
        left_temp_ptr = &left_temp;
    }
    buf_append(out, "__ds_needle=");
    if (left_temp_ptr) {
        buf_append(out, "\"$"); emit_var_name(out, left_temp); buf_append(out, "\"");
    } else if (!emit_value_expr(e, left, out)) return false;
    buf_append(out, "; __ds_needle_type=");
    emit_membership_left_type(left, left_kind, out);
    buf_append(out, "; ");
    if (right->kind == DS_LOWER_EXPR_ARRAY) {
        if (right->as.array.elements.len == 0) { buf_append(out, "false; }"); return true; }
        for (size_t i = 0; i < right->as.array.elements.len; i++) {
            if (i > 0) buf_append(out, " || ");
            DsLowerExpr *elem = right->as.array.elements.items[i];
            DsLowerValueKind literal_kind = DS_LOWER_VALUE_UNKNOWN;
            switch (elem->kind) {
                case DS_LOWER_EXPR_BOOL: literal_kind = DS_LOWER_VALUE_BOOL; break;
                case DS_LOWER_EXPR_INT: literal_kind = DS_LOWER_VALUE_INT; break;
                case DS_LOWER_EXPR_STRING:
                case DS_LOWER_EXPR_INTERP: literal_kind = DS_LOWER_VALUE_STRING; break;
                default: literal_kind = elem_kind; break;
            }
            if (!emit_membership_compare(e, left, left_kind, literal_kind, elem, out)) return false;
        }
        buf_append(out, "; }");
        return true;
    }
    if (right->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(right->as.call.name) && stdlib_returns_array(right->as.call.name)) {
        size_t temp_id = e->temp_counter++;
        buf_appendf(out, "__ds_found=false; __ds_i=0; __ds_iter_%zu=$(mktemp); ", temp_id);
        emit_stdlib_helper_name(out, right->as.call.name);
        if (!emit_call_args(e, &right->as.call.args, out)) return false;
        buf_appendf(out, " >\"$__ds_iter_%zu\"; while IFS= read -r __ds_item; do [[ ", temp_id);
        buf_append(out, "$__ds_needle_type == ");
        const char *elem_type = lower_value_type_name(elem_kind == DS_LOWER_VALUE_UNKNOWN ? DS_LOWER_VALUE_STRING : elem_kind);
        bash_single_quote(out, elem_type, strlen(elem_type));
        buf_appendf(out, " && \"$__ds_needle\" == \"$__ds_item\" ]] && { __ds_found=true; break; }; __ds_i=$((__ds_i + 1)); done <\"$__ds_iter_%zu\"; rm -f \"$__ds_iter_%zu\"; [[ $__ds_found == true ]]; }", temp_id, temp_id);
        return true;
    }
    buf_append(out, "__ds_found=false; __ds_i=0; for __ds_item in ");
    if (right->kind == DS_LOWER_EXPR_IDENT) {
        buf_append(out, "\"${"); emit_var_name(out, right->as.text); buf_append(out, "[@]}\"");
    } else {
        ds_diag_error(e->diag, right->span, "Bash emission supports `in` over named arrays, array literals, and known stdlib string-array results in v0.23.0");
        return false;
    }
    buf_append(out, "; do [[ ");
    buf_append(out, "$__ds_needle_type == ");
    if (elem_kind == DS_LOWER_VALUE_UNKNOWN) {
        buf_append(out, "\"${"); emit_elem_type_var_name(out, right->as.text); buf_append(out, "[$__ds_i]:-unknown}\"");
    } else {
        const char *elem_type = lower_value_type_name(elem_kind);
        bash_single_quote(out, elem_type, strlen(elem_type));
    }
    buf_append(out, " && \"$__ds_needle\" == \"$__ds_item\" ]] && { __ds_found=true; break; }; __ds_i=$((__ds_i + 1)); done; [[ $__ds_found == true ]]; }");
    return true;
}

static bool emit_double_quoted_literal(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    char *decoded = NULL;
    size_t len = 0;
    if (!decode_string_literal(e->diag, expr, &decoded, &len)) return false;
    buf_append(out, "\"");
    for (size_t i = 0; i < len; i++) {
        char c = decoded[i];
        if (c == '"' || c == '\\' || c == '$' || c == '`') buf_append(out, "\\");
        buf_append_len(out, &c, 1);
    }
    buf_append(out, "\"");
    free(decoded);
    return true;
}

static bool emit_interp_expr(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    if (expr->as.interp.parts.len == 0) { buf_append(out, "\"\""); return true; }
    for (size_t i = 0; i < expr->as.interp.parts.len; i++) {
        const DsLowerExpr *part = expr->as.interp.parts.items[i];
        if (part->kind == DS_LOWER_EXPR_STRING) {
            if (!emit_double_quoted_literal(e, part, out)) return false;
        } else if (!emit_value_expr(e, part, out)) return false;
    }
    return true;
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
        case DS_LOWER_EXPR_INTERP:
            return emit_interp_expr(e, expr, out);
        case DS_LOWER_EXPR_INT:
            buf_append_len(out, expr->as.text.data, expr->as.text.len);
            return true;
        case DS_LOWER_EXPR_BOOL:
            buf_append(out, expr->as.boolean ? "true" : "false");
            return true;
        case DS_LOWER_EXPR_FIELD:
            if (expr->as.field.object->kind != DS_LOWER_EXPR_IDENT) {
                /* Lowering rejects temporary field receivers for VM/Bash parity. */
                ds_diag_error(e->diag, expr->span, "internal Bash invariant failed: field receiver should be a named binding after lowering");
                return false;
            }
            {
                DsStr storage_field = result_field_storage_name(expr->as.field.field);
                buf_append(out, "\"$");
                emit_var_name(out, expr->as.field.object->as.text);
                buf_append(out, "_");
                buf_append_len(out, storage_field.data, storage_field.len);
                buf_append(out, "\"");
            }
            return true;
        case DS_LOWER_EXPR_INDEX:
            if (expr->as.index.object->kind != DS_LOWER_EXPR_IDENT) {
                /* Lowering rejects temporary collection receivers for VM/Bash parity. */
                ds_diag_error(e->diag, expr->span, "internal Bash invariant failed: collection index receiver should be a named binding after lowering");
                return false;
            }
            if (!expr->as.index.object_is_array && !expr->as.index.object_is_map) {
                /* Lowering annotates accepted collection indexes with a known collection kind. */
                ds_diag_error(e->diag, expr->span, "internal Bash invariant failed: collection index should have a known collection kind after lowering");
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
                /* Lowering rejects computed indexes that Bash cannot render portably. */
                ds_diag_error(e->diag, expr->span, "internal Bash invariant failed: collection index expression should be literal or named after lowering");
                return false;
            }
            buf_append(out, ")\"");
            return true;
        case DS_LOWER_EXPR_CALL:
            if (expr->as.call.is_user_function) {
                buf_append(out, "\"$(__ds_call_value ");
                bash_single_quote(out, lower_value_type_name(expr->as.call.return_kind), strlen(lower_value_type_name(expr->as.call.return_kind)));
                buf_append(out, " ");
                emit_fn_name(out, expr->as.call.name);
                if (!emit_user_call_args(e, &expr->as.call.args, out)) return false;
                buf_append(out, ")\"");
                return true;
            }
            if (!ds_stdlib_is_name(expr->as.call.name) || stdlib_returns_array(expr->as.call.name)) {
                ds_diag_error(e->diag, expr->span, "internal Bash invariant failed: value call should be a scalar stdlib or user-function call after lowering");
                return false;
            }
            buf_append(out, "\"$(");
            emit_stdlib_helper_name(out, expr->as.call.name);
            if (!emit_call_args(e, &expr->as.call.args, out)) return false;
            buf_append(out, ")\"");
            return true;
        case DS_LOWER_EXPR_BINARY:
            if (is_int_binary_op(expr->as.binary.op)) {
                buf_append(out, "\"$(__ds_int_bin ");
                bash_single_quote(out, expr->as.binary.op.data, expr->as.binary.op.len);
                buf_append(out, " ");
                if (!emit_value_expr(e, expr->as.binary.left, out)) return false;
                buf_append(out, " ");
                if (!emit_value_expr(e, expr->as.binary.right, out)) return false;
                buf_append(out, ")\"");
                return true;
            }
            if (str_eq(expr->as.binary.op, "in") || str_eq(expr->as.binary.op, "matches") ||
                str_eq(expr->as.binary.op, "&&") || str_eq(expr->as.binary.op, "||")) {
                buf_append(out, "$(if ");
                if (!emit_condition(e, expr, out)) return false;
                buf_append(out, "; then printf true; else printf false; fi)");
                return true;
            }
            ds_diag_error(e->diag, expr->span, "internal Bash invariant failed: binary value expression should be supported or rejected by lowering");
            return false;
        case DS_LOWER_EXPR_UNARY:
            if (str_eq(expr->as.unary.op, "-")) {
                buf_append(out, "\"$(__ds_int_neg ");
                if (!emit_value_expr(e, expr->as.unary.right, out)) return false;
                buf_append(out, ")\"");
                return true;
            }
            ds_diag_error(e->diag, expr->span, "internal Bash invariant failed: unary value expression should be supported or rejected by lowering");
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
        case DS_LOWER_EXPR_REGEX:
            ds_diag_error(e->diag, expr->span,
                          "internal Bash regex invariant failed: regex literal reached condition emission outside `matches`");
            return false;
        case DS_LOWER_EXPR_FIELD:
        case DS_LOWER_EXPR_CALL:
        case DS_LOWER_EXPR_INDEX:
        case DS_LOWER_EXPR_BINARY:
        case DS_LOWER_EXPR_UNARY:
        case DS_LOWER_EXPR_INTERP:
            return emit_value_expr(e, expr, out);
        default:
            ds_diag_error(e->diag, expr->span, "internal Bash invariant failed: condition operand should be supported or rejected by lowering");
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
    if (expr->kind == DS_LOWER_EXPR_CALL) {
        DsLowerValueKind kind = expr->as.call.return_kind;
        if (expr->as.call.is_user_function) {
            char temp_buf[64];
            temp_ds_name(temp_buf, sizeof(temp_buf), "cond", e->temp_counter++);
            DsStr temp = {temp_buf, strlen(temp_buf)};
            buf_append(out, "{ ");
            emit_var_name(out, temp);
            buf_append(out, "=\"\"; ");
            if (!emit_user_call_into_raw_var(e, expr, temp, out)) return false;
            if (kind == DS_LOWER_VALUE_BOOL) {
                buf_append(out, "; [[ \"$");
                emit_var_name(out, temp);
                buf_append(out, "\" == true ]]; }");
                return true;
            }
            if (kind == DS_LOWER_VALUE_INT) {
                buf_append(out, "; [[ \"$");
                emit_var_name(out, temp);
                buf_append(out, "\" != 0 ]]; }");
                return true;
            }
            if (kind == DS_LOWER_VALUE_STRING) {
                buf_append(out, "; [[ -n \"$");
                emit_var_name(out, temp);
                buf_append(out, "\" ]]; }");
                return true;
            }
            buf_append(out, "; false; }");
            return true;
        }
        if (kind == DS_LOWER_VALUE_BOOL) {
            buf_append(out, "[[ ");
            if (!emit_value_expr(e, expr, out)) return false;
            buf_append(out, " == true ]]");
            return true;
        }
        if (kind == DS_LOWER_VALUE_INT) {
            buf_append(out, "[[ ");
            if (!emit_value_expr(e, expr, out)) return false;
            buf_append(out, " != 0 ]]");
            return true;
        }
        if (kind == DS_LOWER_VALUE_STRING) {
            buf_append(out, "[[ -n ");
            if (!emit_value_expr(e, expr, out)) return false;
            buf_append(out, " ]]");
            return true;
        }
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
            if (str_eq(expr->as.binary.op, "&&") || str_eq(expr->as.binary.op, "||")) {
                buf_append(out, "{ ");
                if (!emit_condition(e, expr->as.binary.left, out)) return false;
                buf_append(out, str_eq(expr->as.binary.op, "&&") ? "; } && { " : "; } || { ");
                if (!emit_condition(e, expr->as.binary.right, out)) return false;
                buf_append(out, "; }");
                return true;
            }
            if (str_eq(expr->as.binary.op, "in")) return emit_membership_condition(e, expr, out);
            if (str_eq(expr->as.binary.op, "matches")) {
                DsStr pattern = {0}; bool insensitive = false;
                if (expr->as.binary.right->kind != DS_LOWER_EXPR_REGEX || !regex_literal_parts(expr->as.binary.right->as.regex, &pattern, &insensitive)) {
                    ds_diag_error(e->diag, expr->span,
                                  "internal Bash regex invariant failed: accepted `matches` HIR must carry a validated regex literal");
                    return false;
                }
                char left_temp_buf[64];
                DsStr left_temp = {0};
                const DsStr *left_temp_ptr = NULL;
                if (insensitive) buf_append(out, "{ ");
                else buf_append(out, "{ ");
                if (is_user_function_call_expr(expr->as.binary.left)) {
                    temp_ds_name(left_temp_buf, sizeof(left_temp_buf), "match_left", e->temp_counter++);
                    left_temp.data = left_temp_buf;
                    left_temp.len = strlen(left_temp_buf);
                    emit_var_name(out, left_temp);
                    buf_append(out, "=\"\"; ");
                    if (!emit_user_call_into_raw_var(e, expr->as.binary.left, left_temp, out)) return false;
                    buf_append(out, "; ");
                    left_temp_ptr = &left_temp;
                }
                buf_append(out, "__ds_regex=");
                if (!emit_bash_regex_quoted(out, pattern)) return false;
                if (insensitive) buf_append(out, "; __ds_old_nocasematch=$(shopt -p nocasematch || true); shopt -s nocasematch; [[ ");
                else buf_append(out, "; [[ ");
                if (!emit_condition_operand_or_raw_temp(e, expr->as.binary.left, left_temp_ptr, out)) return false;
                buf_append(out, " =~ $__ds_regex");
                if (insensitive) buf_append(out, " ]]; __ds_match_rc=$?; eval \"$__ds_old_nocasematch\"; [[ $__ds_match_rc -eq 0 ]]; }");
                else buf_append(out, " ]]; }");
                return true;
            }
            ds_diag_error(e->diag, expr->span, "operator `%.*s` cannot be emitted in a Bash condition in v0.2.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
            return false;
        }
        if (negate) buf_append(out, "! ");
        if (is_user_function_call_expr(expr->as.binary.left) || is_user_function_call_expr(expr->as.binary.right)) {
            char left_temp_buf[64];
            char right_temp_buf[64];
            DsStr left_temp = {0};
            DsStr right_temp = {0};
            const DsStr *left_temp_ptr = NULL;
            const DsStr *right_temp_ptr = NULL;
            buf_append(out, "{ ");
            if (is_user_function_call_expr(expr->as.binary.left)) {
                temp_ds_name(left_temp_buf, sizeof(left_temp_buf), "cmp_left", e->temp_counter++);
                left_temp.data = left_temp_buf;
                left_temp.len = strlen(left_temp_buf);
                emit_var_name(out, left_temp);
                buf_append(out, "=\"\"; ");
                if (!emit_user_call_into_raw_var(e, expr->as.binary.left, left_temp, out)) return false;
                buf_append(out, "; ");
                left_temp_ptr = &left_temp;
            }
            if (is_user_function_call_expr(expr->as.binary.right)) {
                temp_ds_name(right_temp_buf, sizeof(right_temp_buf), "cmp_right", e->temp_counter++);
                right_temp.data = right_temp_buf;
                right_temp.len = strlen(right_temp_buf);
                emit_var_name(out, right_temp);
                buf_append(out, "=\"\"; ");
                if (!emit_user_call_into_raw_var(e, expr->as.binary.right, right_temp, out)) return false;
                buf_append(out, "; ");
                right_temp_ptr = &right_temp;
            }
            buf_append(out, "[[ ");
            if (!emit_condition_operand_or_raw_temp(e, expr->as.binary.left, left_temp_ptr, out)) return false;
            buf_appendf(out, " %s ", op);
            if (!emit_condition_operand_or_raw_temp(e, expr->as.binary.right, right_temp_ptr, out)) return false;
            buf_append(out, " ]]; }");
            return true;
        }
        buf_append(out, "[[ ");
        if (!emit_condition_operand(e, expr->as.binary.left, out)) return false;
        buf_appendf(out, " %s ", op);
        if (!emit_condition_operand(e, expr->as.binary.right, out)) return false;
        buf_append(out, " ]]");
        return true;
    }
    ds_diag_error(e->diag, expr->span, "internal Bash invariant failed: condition should be supported or rejected by lowering");
    return false;
}

bool emit_call_args(BashEmitter *e, const DsLowerExprVec *args, EmitBuf *out) {
    for (size_t i = 0; i < args->len; i++) {
        buf_append(out, " ");
        if (!emit_call_arg_expr(e, args->items[i], out)) return false;
    }
    return true;
}

bool emit_user_call_args(BashEmitter *e, const DsLowerExprVec *args, EmitBuf *out) {
    for (size_t i = 0; i < args->len; i++) {
        buf_append(out, " ");
        if (!emit_call_arg_expr(e, args->items[i], out)) return false;
        if (!emit_user_call_arg_type(e, args->items[i], out)) return false;
    }
    return true;
}

bool emit_function_default(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    return emit_value_expr(e, expr, out);
}
