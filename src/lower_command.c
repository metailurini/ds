#include "lower_internal.h"
#include "ds_interpolation.h"
#include "ds_command_facts.h"
#include "lower_interp_segments.h"

static DsInterpValueKind interp_kind_from_sym(SymKind kind) {
    switch (kind) {
        case SYM_BOOL: return DS_INTERP_VALUE_BOOL;
        case SYM_INT: return DS_INTERP_VALUE_INT;
        case SYM_STRING: return DS_INTERP_VALUE_STRING;
        case SYM_COMMAND_RESULT: return DS_INTERP_VALUE_COMMAND_RESULT;
        default: return DS_INTERP_VALUE_UNKNOWN;
    }
}

static bool row_schema_field_sym_kind(Lower *lower, const DsLowerRowSchema *schema, DsStr field, DsSpan span, SymKind *kind_out) {
    const DsLowerRowField *row_field = row_schema_find(schema, field);
    if (!row_field) {
        ds_diag_error(lower->diag, span, "unknown row field `%.*s`", (int)field.len, field.data);
        return false;
    }
    *kind_out = sym_kind_from_lower_value_kind(row_field->kind);
    return true;
}

static bool lower_command_interp_resolve_symbol(Lower *lower, DsStr name, DsSpan span,
                                                Symbol **sym_out) {
    Symbol *sym = scope_find(lower->scope, name);
    if (!sym) {
        ds_diag_error(lower->diag, span, "unknown interpolation variable `%.*s`",
                      (int)name.len, name.data);
        return false;
    }
    lower_validate_handler_capture(lower, sym, name, span);
    if (sym_out) *sym_out = sym;
    return true;
}

static bool lower_command_interp_index(Lower *lower, const DsExpr *expr, DsSpan span,
                                       SymKind *value_kind, Symbol **base_sym_out) {
    if (!expr || expr->kind != DS_EXPR_INDEX || !expr->as.index.object ||
        expr->as.index.object->kind != DS_EXPR_IDENT) {
        ds_diag_error(lower->diag, span,
                      "unsupported index interpolation; expected a literal or named index in v0.30.0");
        return false;
    }

    DsStr name = expr->as.index.object->as.text;
    Symbol *sym = NULL;
    if (!lower_command_interp_resolve_symbol(lower, name, span, &sym)) return false;
    if (sym->kind != SYM_ARRAY && sym->kind != SYM_MAP) {
        ds_diag_error(lower->diag, span, "index interpolation requires an array or map value in v0.30.0");
        return false;
    }

    const DsExpr *index = expr->as.index.index;
    bool index_is_int = false;
    bool index_is_string = false;
    bool negative_int = false;
    if (index && index->kind == DS_EXPR_STRING) {
        index_is_string = true;
        if (sym->kind == SYM_MAP) {
            DsStr key = {0};
            if (ds_decode_string_text(index->as.text, &key)) {
                if (key.len == 0) ds_diag_error(lower->diag, span, "empty map keys are deferred in v0.30.0");
                free(key.data);
            }
        }
    } else if (index && index->kind == DS_EXPR_INT) {
        index_is_int = true;
    } else if (index && index->kind == DS_EXPR_UNARY && index->as.unary.op == DS_UNARY_NEGATE &&
               index->as.unary.right && index->as.unary.right->kind == DS_EXPR_INT) {
        index_is_int = true;
        negative_int = true;
    } else if (index && index->kind == DS_EXPR_IDENT) {
        Symbol *idx_sym = scope_find(lower->scope, index->as.text);
        if (!idx_sym) {
            ds_diag_error(lower->diag, span, "unknown interpolation index variable `%.*s`",
                          (int)index->as.text.len, index->as.text.data);
            return false;
        }
        lower_validate_handler_capture(lower, idx_sym, index->as.text, span);
        index_is_int = idx_sym->kind == SYM_INT;
        index_is_string = idx_sym->kind == SYM_STRING;
    } else {
        ds_diag_error(lower->diag, span,
                      "unsupported index interpolation; expected a literal or named index in v0.30.0");
        return false;
    }

    if (sym->kind == SYM_ARRAY) {
        if (!index_is_int) {
            ds_diag_error(lower->diag, span, "array index interpolation requires an int index in v0.30.0");
            return false;
        }
        if (negative_int) {
            ds_diag_error(lower->diag, span, "array index interpolation requires a non-negative index in v0.30.0");
            return false;
        }
    } else if (!index_is_string) {
        ds_diag_error(lower->diag, span, "map index interpolation requires a string key in v0.30.0");
        return false;
    }

    *value_kind = sym->element_kind;
    if (base_sym_out) *base_sym_out = sym;
    return true;
}

static bool lower_command_interp_field(Lower *lower, const DsExpr *expr, DsSpan span,
                                       SymKind *value_kind, bool *indexed) {
    if (!expr || expr->kind != DS_EXPR_FIELD || !expr->as.field.object) return false;
    const DsExpr *object = expr->as.field.object;
    DsStr field = expr->as.field.field;

    if (object->kind == DS_EXPR_IDENT && ds_str_eq_cstr(object->as.text, "env")) {
        if (!lower_validate_env_name(lower, field, span, "v0.27.0")) return false;
        *value_kind = SYM_STRING;
        return true;
    }

    if (object->kind == DS_EXPR_IDENT) {
        Symbol *sym = NULL;
        if (!lower_command_interp_resolve_symbol(lower, object->as.text, span, &sym)) return false;
        if (sym->is_row) return row_schema_field_sym_kind(lower, &sym->row_schema, field, span, value_kind);
        if (sym->kind != SYM_COMMAND_RESULT) {
            ds_diag_error(lower->diag, span,
                          "field interpolation is only supported on command results and rows in v0.37.0");
            return false;
        }
        if (!command_result_field_kind(field, value_kind)) {
            ds_diag_error(lower->diag, span, "unknown command result field `%.*s`",
                          (int)field.len, field.data);
            return false;
        }
        return true;
    }

    if (object->kind == DS_EXPR_INDEX) {
        Symbol *base_sym = NULL;
        SymKind indexed_kind = SYM_UNKNOWN;
        if (!lower_command_interp_index(lower, object, span, &indexed_kind, &base_sym)) return false;
        (void)indexed_kind;
        *indexed = true;
        if (!base_sym || !base_sym->is_row_array) {
            ds_diag_error(lower->diag, span,
                          "indexed field interpolation requires a row-array value in v0.37.0");
            return false;
        }
        return row_schema_field_sym_kind(lower, &base_sym->row_schema, field, span, value_kind);
    }

    ds_diag_error(lower->diag, span,
                  "field interpolation is only supported on command results and rows in v0.37.0");
    return false;
}

static bool lower_command_interp_arithmetic(Lower *lower, const DsExpr *expr, DsSpan span) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_EXPR_INT:
            return true;
        case DS_EXPR_IDENT: {
            Symbol *sym = NULL;
            if (!lower_command_interp_resolve_symbol(lower, expr->as.text, span, &sym)) return false;
            if (sym->kind != SYM_INT) {
                ds_diag_error(lower->diag, span,
                              "arithmetic interpolation operands must be integers in v0.21.0");
                return false;
            }
            return true;
        }
        case DS_EXPR_FIELD: {
            if (!expr->as.field.object || expr->as.field.object->kind != DS_EXPR_IDENT) {
                ds_diag_error(lower->diag, span,
                              "arithmetic interpolation field reads require a row value in v0.37.0");
                return false;
            }
            Symbol *sym = NULL;
            if (!lower_command_interp_resolve_symbol(lower, expr->as.field.object->as.text, span, &sym)) return false;
            if (!sym->is_row) {
                ds_diag_error(lower->diag, span,
                              "arithmetic interpolation field reads require a row value in v0.37.0");
                return false;
            }
            SymKind field_kind = SYM_UNKNOWN;
            if (!row_schema_field_sym_kind(lower, &sym->row_schema, expr->as.field.field, span, &field_kind)) return false;
            if (field_kind != SYM_INT) {
                ds_diag_error(lower->diag, span,
                              "arithmetic interpolation operands must be integers in v0.21.0");
                return false;
            }
            return true;
        }
        case DS_EXPR_UNARY:
            if (expr->as.unary.op != DS_UNARY_NEGATE) return false;
            return lower_command_interp_arithmetic(lower, expr->as.unary.right, span);
        case DS_EXPR_BINARY:
            if (!ds_binary_op_is_arithmetic(expr->as.binary.op)) return false;
            return lower_command_interp_arithmetic(lower, expr->as.binary.left, span) &&
                   lower_command_interp_arithmetic(lower, expr->as.binary.right, span);
        case DS_EXPR_CALL:
            ds_diag_error(lower->diag, span,
                          "function-call interpolation in command words must be bound to a string expression first in v0.21.0");
            return false;
        case DS_EXPR_STRING:
        case DS_EXPR_BOOL:
        case DS_EXPR_REGEX:
        case DS_EXPR_RUN:
        case DS_EXPR_INDEX:
        case DS_EXPR_ARRAY:
        case DS_EXPR_MAP:
        case DS_EXPR_RANGE:
        case DS_EXPR_ERROR:
            return false;
    }
    return false;
}

static bool lower_command_interp_validate_segment(Lower *lower, const LowerInterpSegment *segment,
                                                  DsSpan span) {
    if (!segment || segment->kind != LOWER_INTERP_SEGMENT_EXPR || !segment->expr) return true;
    const DsExpr *expr = segment->expr;
    SymKind value_kind = SYM_UNKNOWN;
    bool indexed = false;
    bool format_allowed = true;
    bool valid = true;

    if (segment->arithmetic_syntax && expr->kind != DS_EXPR_UNARY && expr->kind != DS_EXPR_BINARY) {
        ds_diag_error(lower->diag, span, "invalid arithmetic interpolation in v0.21.0");
        return false;
    }

    switch (expr->kind) {
        case DS_EXPR_IDENT: {
            if (segment->leading_ws || segment->trailing_ws) return false;
            Symbol *sym = NULL;
            valid = lower_command_interp_resolve_symbol(lower, expr->as.text, span, &sym);
            if (valid) value_kind = sym->kind;
            break;
        }
        case DS_EXPR_FIELD:
            if (segment->leading_ws || segment->trailing_ws) return false;
            valid = lower_command_interp_field(lower, expr, span, &value_kind, &indexed);
            break;
        case DS_EXPR_INDEX: {
            if (segment->leading_ws || segment->trailing_ws) return false;
            valid = lower_command_interp_index(lower, expr, span, &value_kind, NULL);
            indexed = true;
            break;
        }
        case DS_EXPR_UNARY:
        case DS_EXPR_BINARY:
            valid = lower_command_interp_arithmetic(lower, expr, span);
            if (!valid && !lower->diag->has_error) {
                ds_diag_error(lower->diag, span, "invalid arithmetic interpolation in v0.21.0");
            }
            value_kind = SYM_INT;
            format_allowed = false;
            break;
        case DS_EXPR_CALL:
            ds_diag_error(lower->diag, span,
                          "function-call interpolation in command words must be bound to a string expression first in v0.21.0");
            return false;
        case DS_EXPR_INT:
        case DS_EXPR_STRING:
        case DS_EXPR_BOOL:
        case DS_EXPR_REGEX:
        case DS_EXPR_RUN:
        case DS_EXPR_ARRAY:
        case DS_EXPR_MAP:
        case DS_EXPR_RANGE:
        case DS_EXPR_ERROR:
            return false;
    }
    if (!valid) return false;

    if (value_kind == SYM_ARRAY || value_kind == SYM_MAP || value_kind == SYM_COMMAND_RESULT) {
        const char *kind_name = value_kind == SYM_ARRAY ? "array" :
                                value_kind == SYM_MAP ? "map" : "command-result";
        ds_diag_error(lower->diag, span,
                      "cannot interpolate %s value in command words; bind a scalar field or indexed element first",
                      kind_name);
        return false;
    }
    if (!segment->has_format) return true;
    if (indexed) {
        ds_diag_error(lower->diag, span,
                      "format specifiers on index interpolation are deferred in v0.30.0; bind the indexed value first");
        return false;
    }
    if (!format_allowed) {
        ds_diag_error(lower->diag, span, "invalid arithmetic interpolation in v0.21.0");
        return false;
    }
    if (!ds_interp_parse_format_spec_for_kind(segment->format, interp_kind_from_sym(value_kind), NULL)) {
        ds_diag_error(lower->diag, span,
                      "unsupported interpolation format specifier `%.*s`; supported: %s",
                      (int)segment->format.len, segment->format.data,
                      ds_interp_supported_format_specs());
        return false;
    }
    return true;
}

static void lower_command_interp_diag_parse_status(Lower *lower, DsSpan span,
                                                   LowerInterpParseStatus status) {
    switch (status) {
        case LOWER_INTERP_PARSE_OK:
        case LOWER_INTERP_PARSE_DECODE_ERROR:
            return;
        case LOWER_INTERP_PARSE_UNMATCHED_CLOSE:
            ds_diag_error(lower->diag, span,
                          "unmatched `}` in string interpolation; use `}}` for a literal `}`");
            return;
        case LOWER_INTERP_PARSE_UNCLOSED:
            ds_diag_error(lower->diag, span, "unclosed interpolation in string; expected `}`");
            return;
        case LOWER_INTERP_PARSE_INVALID_EXPR:
            ds_diag_error(lower->diag, span,
                          "unsupported string interpolation; expected `{name}`, `{name.field}`, arithmetic, or a supported `:specifier`");
            return;
    }
}

bool lower_validate_word_interpolation(Lower *lower, DsStr text, DsSpan span) {
    LowerInterpSegments segments;
    LowerInterpParseStatus status = lower_interp_parse_segments(text, span, &segments);
    if (status == LOWER_INTERP_PARSE_DECODE_ERROR) return true;
    if (status != LOWER_INTERP_PARSE_OK) {
        lower_command_interp_diag_parse_status(lower, span, status);
        lower_interp_segments_free(&segments);
        return false;
    }
    bool valid = true;
    for (size_t i = 0; i < segments.len; i++) {
        if (segments.items[i].kind != LOWER_INTERP_SEGMENT_EXPR) continue;
        if (!lower_command_interp_validate_segment(lower, &segments.items[i], span)) {
            if (!lower->diag->has_error) {
                ds_diag_error(lower->diag, span,
                              "unsupported string interpolation; expected `{name}`, `{name.field}`, arithmetic, or a supported `:specifier`");
            }
            valid = false;
            break;
        }
    }
    lower_interp_segments_free(&segments);
    return valid;
}


bool lower_validate_command_word(Lower *lower, DsStr word, DsSpan span) {
    if ((word.len == 2 && ((word.data[0] == '*' && word.data[1] == '=') ||
                           (word.data[0] == '/' && word.data[1] == '=') ||
                           (word.data[0] == '%' && word.data[1] == '=')))) {
        ds_diag_error(lower->diag, span, "compound assignment target must be a variable in v0.21.0");
        return false;
    }
    DsCommandWordForm form = ds_command_word_analyze(word);
    if (word.data[0] == '$' && (form.kind == DS_COMMAND_WORD_VARIABLE || form.kind == DS_COMMAND_WORD_FIELD)) {
        DsStr name = form.name;
        Symbol *sym = lower_resolve_value_symbol(lower, name, span, "command variable");
        if (!sym) return false;
        lower_validate_handler_capture(lower, sym, name, span);
        if (word.data[0] == '$' && form.kind == DS_COMMAND_WORD_VARIABLE && name.len + 1 < word.len) {
            char suffix = word.data[name.len + 1];
            if ((sym->kind == SYM_ARRAY || sym->kind == SYM_MAP) && (suffix == '[' || suffix == '.')) {
                ds_diag_error(lower->diag, span, "collection access command arguments are deferred in v0.10.0; bind the indexed value to a variable first");
                return false;
            }
            ds_diag_error(lower->diag, span, "unsupported command variable suffix in v0.10.0");
            return false;
        }
        if (word.data[0] == '$' && form.kind == DS_COMMAND_WORD_FIELD) {
            if (sym->kind == SYM_ARRAY || sym->kind == SYM_MAP) {
                if (sym->kind == SYM_MAP && sym->is_row) {
                    if (!row_schema_find(&sym->row_schema, form.field)) {
                        ds_diag_error(lower->diag, span, "unknown row field `%.*s`", (int)form.field.len, form.field.data);
                        return false;
                    }
                    return true;
                }
                ds_diag_error(lower->diag, span, "collection access command arguments are deferred in v0.10.0; bind the indexed value to a variable first");
                return false;
            }
            if (sym->kind != SYM_COMMAND_RESULT) {
                ds_diag_error(lower->diag, span, "unsupported command variable suffix in v0.10.0");
                return false;
            }
            SymKind field_kind = SYM_UNKNOWN;
            if (!command_result_field_kind(form.field, &field_kind)) {
                DsSpan field_span = span;
                field_span.start.offset = span.start.offset + (int)name.len + 2;
                field_span.start.column = span.start.column + (int)name.len + 2;
                field_span.end.offset = field_span.start.offset + (int)form.field.len;
                field_span.end.column = field_span.start.column + (int)form.field.len;
                ds_diag_error(lower->diag, field_span, "unsupported command result field `%.*s`", (int)form.field.len, form.field.data);
                return false;
            }
        }
        if (sym->kind == SYM_ARRAY || sym->kind == SYM_MAP) {
            ds_diag_error(lower->diag, span, "collection `%.*s` cannot be passed directly as a command argument in v0.10.0; index it first", (int)name.len, name.data);
            return false;
        }
    }
    if (form.kind == DS_COMMAND_WORD_QUOTED) return lower_validate_word_interpolation(lower, word, span);
    if (form.kind == DS_COMMAND_WORD_FIELD && word.data[0] != '$') {
        DsStr name = form.name;
        DsStr field = form.field;
        size_t i = (size_t)(field.data - word.data - 1);
        DsSpan field_span = span;
        field_span.start.offset = span.start.offset + (int)i + 1;
        field_span.start.column = span.start.column + (int)i + 1;
        field_span.end.offset = field_span.start.offset + (int)field.len;
        field_span.end.column = field_span.start.column + (int)field.len;
        if (field.len == 0) {
            ds_diag_error(lower->diag, field_span, "expected field name after `.`");
            return false;
        }
        if (ds_str_eq_cstr(name, "string")) {
            DsStr member = field;
            for (size_t j = 0; j < member.len; j++) {
                if (member.data[j] == '(') {
                    member.len = j;
                    break;
                }
            }
            lower_diag_unknown_string_method(lower, field_span, member);
            return false;
        }
        if (ds_str_eq_cstr(name, "env")) {
            if (!lower_validate_env_name(lower, field, field_span, "v0.27.0")) return false;
            return true;
        }
        SymKind field_kind = SYM_UNKNOWN;
        Symbol *sym = scope_find(lower->scope, name);
        if (!sym) {
            DsSpan name_span = span;
            name_span.end.offset = name_span.start.offset + (int)name.len;
            name_span.end.column = name_span.start.column + (int)name.len;
            ds_diag_error(lower->diag, name_span, "unknown command variable `%.*s`", (int)name.len, name.data);
            return false;
        }
        lower_validate_handler_capture(lower, sym, name, span);
        if (sym->kind != SYM_COMMAND_RESULT) {
            if (sym->kind == SYM_MAP) {
                if (sym->is_row) {
                    if (!row_schema_find(&sym->row_schema, field)) {
                        ds_diag_error(lower->diag, field_span, "unknown row field `%.*s`", (int)field.len, field.data);
                        return false;
                    }
                    return true;
                }
                ds_diag_error(lower->diag, field_span, "map field command arguments are deferred in v0.10.0; bind the field to a variable first");
                return false;
            }
            ds_diag_error(lower->diag, field_span, "field access is only supported on command results and maps in v0.10.0");
            return false;
        }
        if (!command_result_field_kind(field, &field_kind)) {
            ds_diag_error(lower->diag, field_span, "unsupported command result field `%.*s`", (int)field.len, field.data);
            return false;
        }
    }
    return true;
}

static bool command_interp_expr_needs_materialization(const DsExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_EXPR_CALL:
        case DS_EXPR_INDEX:
            return true;
        case DS_EXPR_FIELD:
            return command_interp_expr_needs_materialization(expr->as.field.object);
        case DS_EXPR_UNARY:
            return command_interp_expr_needs_materialization(expr->as.unary.right);
        case DS_EXPR_BINARY:
            return command_interp_expr_needs_materialization(expr->as.binary.left) ||
                   command_interp_expr_needs_materialization(expr->as.binary.right);
        case DS_EXPR_ARRAY:
            for (size_t i = 0; i < expr->as.array.elements.len; i++) {
                if (command_interp_expr_needs_materialization(expr->as.array.elements.items[i])) return true;
            }
            return false;
        case DS_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) {
                if (command_interp_expr_needs_materialization(expr->as.map.entries.items[i].value)) return true;
            }
            return false;
        case DS_EXPR_RANGE:
            return command_interp_expr_needs_materialization(expr->as.range.start) ||
                   command_interp_expr_needs_materialization(expr->as.range.end);
        case DS_EXPR_IDENT:
        case DS_EXPR_STRING:
        case DS_EXPR_INT:
        case DS_EXPR_BOOL:
        case DS_EXPR_REGEX:
        case DS_EXPR_RUN:
        case DS_EXPR_ERROR:
            return false;
    }
    return false;
}

static bool command_quoted_word_needs_value_call_materialization(Lower *lower, DsStr word) {
    if (word.len < 2 || word.data[0] != '"' || word.data[word.len - 1] != '"') return false;
    LowerInterpSegments segments;
    LowerInterpParseStatus status = lower_interp_parse_segments(word, (DsSpan){0}, &segments);
    if (status != LOWER_INTERP_PARSE_OK) {
        lower_interp_segments_free(&segments);
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < segments.len && !found; i++) {
        LowerInterpSegment *segment = &segments.items[i];
        if (segment->kind != LOWER_INTERP_SEGMENT_EXPR) continue;
        found = command_interp_expr_needs_materialization(segment->expr);
    }
    lower_interp_segments_free(&segments);
    (void)lower;
    return found;
}

static bool command_word_is_index_field_access(DsStr word) {
    if (word.len == 0 || word.data[0] == '$' || word.data[0] == '"') return false;
    size_t i = 0;
    if (!ds_is_ident_start(word.data[i])) {
        return false;
    }
    i++;
    while (i < word.len && ds_command_name_char(word.data[i])) i++;
    if (i >= word.len || word.data[i] != '[') return false;
    int depth = 1;
    i++;
    while (i < word.len && depth > 0) {
        if (word.data[i] == '[') depth++;
        else if (word.data[i] == ']') depth--;
        i++;
    }
    if (depth != 0 || i >= word.len || word.data[i] != '.') return false;
    i++;
    if (i >= word.len || !ds_is_ident_start(word.data[i])) {
        return false;
    }
    i++;
    while (i < word.len && ds_command_name_char(word.data[i])) i++;
    return i == word.len;
}

static DsStr lower_command_index_field_temp_text(DsStr word) {
    DsString s;
    ds_string_init(&s);
    ds_string_append_char(&s, '"');
    ds_string_append_char(&s, '{');
    ds_string_append_range(&s, word.data, word.len);
    ds_string_append_char(&s, '}');
    ds_string_append_char(&s, '"');
    return (DsStr){s.data, s.len};
}

static DsStr lower_make_temp_name(Lower *lower, const char *prefix) {
    char buf[96];
    do {
        snprintf(buf, sizeof(buf), "__ds_%s_%zu", prefix, lower->temp_counter++);
        DsStr candidate = {buf, strlen(buf)};
        if (!scope_find(lower->scope, candidate)) break;
    } while (true);
    size_t len = strlen(buf);
    return (DsStr){ds_str_dup_range(buf, len), len};
}

static DsLowerStmt *lower_command_interpolation_temp_string_let(Lower *lower, DsStr name, DsStr quoted_text, DsSpan span) {
    DsExpr fake;
    memset(&fake, 0, sizeof(fake));
    fake.kind = DS_EXPR_STRING;
    fake.span = span;
    fake.as.text = quoted_text;
    SymKind kind = SYM_UNKNOWN;
    DsLowerStmt *let = stmt_new(DS_LOWER_STMT_LET, span);
    let->as.let_stmt.name = ds_str_clone(name);
    let->as.let_stmt.value = lower_expr(lower, &fake, &kind);
    let->as.let_stmt.value_kind = lower_value_kind_from_sym(kind);
    let->as.let_stmt.element_kind = DS_LOWER_VALUE_UNKNOWN;
    if (kind != SYM_STRING && kind != SYM_UNKNOWN) {
        ds_diag_error(lower->diag, span, "function call in command interpolation must return a scalar string-renderable value in v0.27.0");
    }
    scope_define(lower, lower->scope, name, SYM_STRING, span);
    return let;
}

static DsStr lower_command_temp_word(DsStr name) {
    DsString s;
    ds_string_init(&s);
    ds_string_append_char(&s, '$');
    ds_string_append_range(&s, name.data, name.len);
    return (DsStr){s.data, s.len};
}

static DsStr lower_redirect_temp_target(DsStr name) {
    DsString s;
    ds_string_init(&s);
    ds_string_append_char(&s, '"');
    ds_string_append_char(&s, '{');
    ds_string_append_range(&s, name.data, name.len);
    ds_string_append_char(&s, '}');
    ds_string_append_char(&s, '"');
    return (DsStr){s.data, s.len};
}

bool lower_materialize_command_value_call_interpolation(Lower *lower, DsCommand *command, DsLowerStmt *block) {
    /*
     * M3.4 command-word contract: direct scalar value-call interpolation in a
     * quoted command word is not handed to VM/Bash as a backend-specific
     * command substitution problem. Lowering evaluates the interpolation as a
     * normal string expression into a private temporary, then rewrites the
     * command word to an ordinary `$temp` argument. Unsupported return kinds are
     * diagnosed while lowering the temporary string expression.
     */
    bool changed = false;
    for (size_t s = 0; s < command->stages.len; s++) {
        for (size_t i = 0; i < command->stages.items[s].words.len; i++) {
            DsWord *word = &command->stages.items[s].words.items[i];
            bool materialize_index_field = command_word_is_index_field_access(word->text);
            if (!materialize_index_field && !command_quoted_word_needs_value_call_materialization(lower, word->text)) continue;
            DsStr tmp = lower_make_temp_name(lower, "cmd_interp");
            DsStr temp_text = materialize_index_field ? lower_command_index_field_temp_text(word->text) : word->text;
            DS_VEC_PUSH(&block->as.block_stmt.statements, lower_command_interpolation_temp_string_let(lower, tmp, temp_text, word->span), 16);
            if (materialize_index_field) free(temp_text.data);
            free(word->text.data);
            word->text = lower_command_temp_word(tmp);
            free(tmp.data);
            changed = true;
        }
    }
    if (command->redirect.kind != DS_REDIRECT_NONE && command_quoted_word_needs_value_call_materialization(lower, command->redirect.target)) {
        DsStr tmp = lower_make_temp_name(lower, "redir_interp");
        DS_VEC_PUSH(&block->as.block_stmt.statements, lower_command_interpolation_temp_string_let(lower, tmp, command->redirect.target, command->redirect.target_span), 16);
        free(command->redirect.target.data);
        command->redirect.target = lower_redirect_temp_target(tmp);
        free(tmp.data);
        changed = true;
    }
    return changed;
}
