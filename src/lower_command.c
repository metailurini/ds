#include "lower_internal.h"
#include "ds_interpolation.h"
#include "ds_command_facts.h"

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

static bool lower_validate_arithmetic_interpolation_text(Lower *lower, DsStr body, DsSpan span) {
    /*
     * Keep arithmetic interpolation acceptance in lowering. The VM/Bash
     * arithmetic renderers still parse accepted text at runtime/emission time,
     * but malformed source shapes must not be discovered first by a backend.
     */
    bool expect_operand = true;
    int depth = 0;
    for (size_t i = 0; i < body.len;) {
        char c = body.data[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c >= '0' && c <= '9') {
            while (i < body.len && body.data[i] >= '0' && body.data[i] <= '9') i++;
            expect_operand = false;
            continue;
        }
        if (ds_is_ident_start(c)) {
            size_t name_start = i++;
            while (i < body.len && ds_command_name_char(body.data[i])) i++;
            if (i < body.len && body.data[i] == '(') {
                ds_diag_error(lower->diag, span, "function-call interpolation in command words must be bound to a string expression first in v0.21.0");
                return false;
            }
            DsStr name = {body.data + name_start, i - name_start};
            Symbol *sym = scope_find(lower->scope, name);
            if (!sym) {
                ds_diag_error(lower->diag, span, "unknown interpolation variable `%.*s`", (int)name.len, name.data);
                return false;
            }
            lower_validate_handler_capture(lower, sym, name, span);
            SymKind value_kind = sym->kind;
            if (i < body.len && body.data[i] == '.') {
                size_t field_start = ++i;
                if (i < body.len && ds_is_ident_start(body.data[i])) {
                    i++;
                    while (i < body.len && ds_command_name_char(body.data[i])) i++;
                }
                DsStr field = {body.data + field_start, i - field_start};
                if (!sym->is_row) {
                    ds_diag_error(lower->diag, span, "arithmetic interpolation field reads require a row value in v0.37.0");
                    return false;
                }
                if (!row_schema_field_sym_kind(lower, &sym->row_schema, field, span, &value_kind)) return false;
            }
            if (value_kind != SYM_INT) {
                ds_diag_error(lower->diag, span, "arithmetic interpolation operands must be integers in v0.21.0");
                return false;
            }
            expect_operand = false;
            continue;
        }
        if (c == '(') {
            if (!expect_operand) return false;
            depth++;
            i++;
            continue;
        }
        if (c == ')') {
            if (expect_operand || depth <= 0) return false;
            depth--;
            i++;
            expect_operand = false;
            continue;
        }
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
            if (expect_operand) {
                if (c == '-') { i++; continue; }
                return false;
            }
            if (c == '*' && i + 1 < body.len && body.data[i + 1] == '*') i += 2;
            else i++;
            expect_operand = true;
            continue;
        }
        return false;
    }
    return !expect_operand && depth == 0;
}

static bool lower_validate_word_index_interpolation(Lower *lower, DsStr decoded, size_t *j, DsStr name, Symbol *sym, DsSpan span, SymKind *value_kind) {
    if (!sym) {
        ds_diag_error(lower->diag, span, "unknown interpolation variable `%.*s`", (int)name.len, name.data);
        return false;
    }
    if (sym->kind != SYM_ARRAY && sym->kind != SYM_MAP) {
        ds_diag_error(lower->diag, span, "index interpolation requires an array or map value in v0.30.0");
        return false;
    }

    (*j)++;
    ds_skip_ascii_ws(decoded.data, decoded.len, j);
    bool index_is_int = false;
    bool index_is_string = false;
    bool negative_int = false;

    if (*j < decoded.len && decoded.data[*j] == '"') {
        size_t literal_start = *j;
        (*j)++;
        while (*j < decoded.len) {
            if (decoded.data[*j] == '\\' && *j + 1 < decoded.len) { *j += 2; continue; }
            if (decoded.data[*j] == '"') { (*j)++; break; }
            (*j)++;
        }
        index_is_string = true;
        if (sym->kind == SYM_MAP) {
            DsStr literal = {decoded.data + literal_start, *j - literal_start};
            DsStr key = {0};
            if (ds_decode_string_text(literal, &key)) {
                if (key.len == 0) ds_diag_error(lower->diag, span, "empty map keys are deferred in v0.30.0");
                free(key.data);
            }
        }
    } else if (*j < decoded.len && ((decoded.data[*j] == '-' && *j + 1 < decoded.len && decoded.data[*j + 1] >= '0' && decoded.data[*j + 1] <= '9') ||
                                    (decoded.data[*j] >= '0' && decoded.data[*j] <= '9'))) {
        if (decoded.data[*j] == '-') { negative_int = true; (*j)++; }
        while (*j < decoded.len && decoded.data[*j] >= '0' && decoded.data[*j] <= '9') (*j)++;
        index_is_int = true;
    } else if (*j < decoded.len && ds_is_ident_start(decoded.data[*j])) {
        size_t idx_start = *j;
        (*j)++;
        while (*j < decoded.len && ds_is_ident_continue(decoded.data[*j])) (*j)++;
        DsStr idx_name = {decoded.data + idx_start, *j - idx_start};
        Symbol *idx_sym = scope_find(lower->scope, idx_name);
        if (!idx_sym) {
            ds_diag_error(lower->diag, span, "unknown interpolation index variable `%.*s`", (int)idx_name.len, idx_name.data);
            return false;
        }
        lower_validate_handler_capture(lower, idx_sym, idx_name, span);
        index_is_int = idx_sym->kind == SYM_INT;
        index_is_string = idx_sym->kind == SYM_STRING;
    } else {
        ds_diag_error(lower->diag, span, "unsupported index interpolation; expected a literal or named index in v0.30.0");
        return false;
    }

    ds_skip_ascii_ws(decoded.data, decoded.len, j);
    if (*j >= decoded.len || decoded.data[*j] != ']') {
        ds_diag_error(lower->diag, span, "unsupported index interpolation; expected `]` after index expression in v0.30.0");
        return false;
    }
    (*j)++;

    if (sym->kind == SYM_ARRAY) {
        if (!index_is_int) {
            ds_diag_error(lower->diag, span, "array index interpolation requires an int index in v0.30.0");
            return false;
        }
        if (negative_int) {
            ds_diag_error(lower->diag, span, "array index interpolation requires a non-negative index in v0.30.0");
            return false;
        }
    } else if (sym->kind == SYM_MAP && !index_is_string) {
        ds_diag_error(lower->diag, span, "map index interpolation requires a string key in v0.30.0");
        return false;
    }
    *value_kind = sym->element_kind;
    return true;
}

bool lower_validate_word_interpolation(Lower *lower, DsStr text, DsSpan span) {
    DsStr decoded = {0};
    if (!ds_decode_string_text(text, &decoded)) return true;
    for (size_t i = 0; i < decoded.len; i++) {
        char c = decoded.data[i];
        if (c == '{' && i + 1 < decoded.len && decoded.data[i + 1] == '{') { i++; continue; }
        if (c == '}' && i + 1 < decoded.len && decoded.data[i + 1] == '}') { i++; continue; }
        if (c == '}') {
            ds_diag_error(lower->diag, span, "unmatched `}` in string interpolation; use `}}` for a literal `}`");
            free(decoded.data);
            return false;
        }
        if (c != '{') continue;
        size_t start = i + 1;
        size_t j = start;
        if (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || decoded.data[j] == '_')) {
            j++;
            while (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || (decoded.data[j] >= '0' && decoded.data[j] <= '9') || decoded.data[j] == '_')) j++;
            DsStr name = {decoded.data + start, j - start};
            if (j < decoded.len && decoded.data[j] == '(') {
                ds_diag_error(lower->diag, span, "function-call interpolation in command words must be bound to a string expression first in v0.21.0");
                free(decoded.data);
                return false;
            }
            if (j >= decoded.len) {
                ds_diag_error(lower->diag, span, "unclosed interpolation in string; expected `}`");
                free(decoded.data);
                return false;
            }
            if (!memchr(decoded.data + j, '}', decoded.len - j)) {
                ds_diag_error(lower->diag, span, "unclosed interpolation in string; expected `}`");
                free(decoded.data);
                return false;
            }
            if (ds_str_eq_cstr(name, "env") && j < decoded.len && decoded.data[j] == '.') {
                size_t field_start = ++j;
                if (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || decoded.data[j] == '_')) {
                    j++;
                    while (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || (decoded.data[j] >= '0' && decoded.data[j] <= '9') || decoded.data[j] == '_')) j++;
                }
                DsStr field = {decoded.data + field_start, j - field_start};
                if (!lower_validate_env_name(lower, field, span, "v0.27.0")) {
                    free(decoded.data);
                    return false;
                }
                if (j < decoded.len && decoded.data[j] == '}') { i = j; continue; }
            }
            Symbol *sym = scope_find(lower->scope, name);
            if (!sym) {
                ds_diag_error(lower->diag, span, "unknown interpolation variable `%.*s`", (int)name.len, name.data);
                free(decoded.data);
                return false;
            }
            lower_validate_handler_capture(lower, sym, name, span);
            SymKind value_kind = sym->kind;
            bool indexed_interp = false;
            if (j < decoded.len && decoded.data[j] == '[') {
                if (!lower_validate_word_index_interpolation(lower, decoded, &j, name, sym, span, &value_kind)) {
                    free(decoded.data);
                    return false;
                }
                indexed_interp = true;
                if (j < decoded.len && decoded.data[j] == '.') {
                    size_t field_start = ++j;
                    if (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || decoded.data[j] == '_')) {
                        j++;
                        while (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || (decoded.data[j] >= '0' && decoded.data[j] <= '9') || decoded.data[j] == '_')) j++;
                    }
                    DsStr field = {decoded.data + field_start, j - field_start};
                    if (!sym->is_row_array) {
                        ds_diag_error(lower->diag, span, "indexed field interpolation requires a row-array value in v0.37.0");
                        free(decoded.data);
                        return false;
                    }
                    if (!row_schema_field_sym_kind(lower, &sym->row_schema, field, span, &value_kind)) {
                        free(decoded.data);
                        return false;
                    }
                }
            } else if (j < decoded.len && decoded.data[j] == '.') {
                size_t field_start = ++j;
                if (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || decoded.data[j] == '_')) {
                    j++;
                    while (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') || (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') || (decoded.data[j] >= '0' && decoded.data[j] <= '9') || decoded.data[j] == '_')) j++;
                }
                DsStr field = {decoded.data + field_start, j - field_start};
                if (sym->is_row) {
                    if (!row_schema_field_sym_kind(lower, &sym->row_schema, field, span, &value_kind)) {
                        free(decoded.data);
                        return false;
                    }
                } else if (sym->kind != SYM_COMMAND_RESULT) {
                    ds_diag_error(lower->diag, span, "field interpolation is only supported on command results and rows in v0.37.0");
                    free(decoded.data);
                    return false;
                } else if (!command_result_field_kind(field, &value_kind)) {
                    ds_diag_error(lower->diag, span, "unknown command result field `%.*s`", (int)field.len, field.data);
                    free(decoded.data);
                    return false;
                }
            }
            if (value_kind == SYM_ARRAY || value_kind == SYM_MAP || value_kind == SYM_COMMAND_RESULT) {
                const char *kind_name = value_kind == SYM_ARRAY ? "array" : value_kind == SYM_MAP ? "map" : "command-result";
                ds_diag_error(lower->diag, span, "cannot interpolate %s value in command words; bind a scalar field or indexed element first", kind_name);
                free(decoded.data);
                return false;
            }
            if (j < decoded.len && decoded.data[j] == ':') {
                if (indexed_interp) {
                    ds_diag_error(lower->diag, span, "format specifiers on index interpolation are deferred in v0.30.0; bind the indexed value first");
                    free(decoded.data);
                    return false;
                }
                size_t spec_start = ++j;
                while (j < decoded.len && decoded.data[j] != '}') j++;
                if (j >= decoded.len) break;
                DsStr spec = {decoded.data + spec_start, j - spec_start};
                if (!ds_interp_parse_format_spec_for_kind(spec, interp_kind_from_sym(value_kind), NULL)) {
                    ds_diag_error(lower->diag, span, "unsupported interpolation format specifier `%.*s`; supported: %s", (int)spec.len, spec.data, ds_interp_supported_format_specs());
                    free(decoded.data);
                    return false;
                }
            }
            if (j < decoded.len && decoded.data[j] == '}') { i = j; continue; }
        }
        size_t k = start;
        bool maybe_arith = false;
        while (k < decoded.len && decoded.data[k] != '}') {
            char ac = decoded.data[k++];
            if (ac == '+' || ac == '-' || ac == '*' || ac == '/' || ac == '%' || ac == '(' || ac == ')') maybe_arith = true;
        }
        if (k >= decoded.len) {
            ds_diag_error(lower->diag, span, "unclosed interpolation in string; expected `}`");
            free(decoded.data);
            return false;
        }
        if (maybe_arith && k < decoded.len && decoded.data[k] == '}') {
            DsStr body = {decoded.data + start, k - start};
            if (!lower_validate_arithmetic_interpolation_text(lower, body, span)) {
                if (!lower->diag->has_error) ds_diag_error(lower->diag, span, "invalid arithmetic interpolation in v0.21.0");
                free(decoded.data);
                return false;
            }
            i = k;
            continue;
        }
        ds_diag_error(lower->diag, span, "unsupported string interpolation; expected `{name}`, `{name.field}`, arithmetic, or a supported `:specifier`");
        free(decoded.data);
        return false;
    }
    free(decoded.data);
    return true;
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

static bool command_quoted_word_needs_value_call_materialization(Lower *lower, DsStr word) {
    if (word.len < 2 || word.data[0] != '"' || word.data[word.len - 1] != '"') return false;
    DsStr decoded = {0};
    if (!ds_decode_string_text(word, &decoded)) return false;
    bool found = ds_command_word_contains_direct_call_interpolation(decoded);
    if (!found) {
        for (size_t i = 0; i < decoded.len; i++) {
            if (decoded.data[i] != '{') continue;
            if (i + 1 < decoded.len && decoded.data[i + 1] == '{') { i++; continue; }
            size_t j = i + 1;
            while (j < decoded.len && decoded.data[j] != '}') {
                if (decoded.data[j] == '[') { found = true; break; }
                j++;
            }
            if (found) break;
            i = j;
        }
    }
    free(decoded.data);
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
