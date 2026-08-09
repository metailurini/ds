#include "ds_command_facts.h"

#include "vm_internal.h"
#include "ds_interpolation.h"
#include "ds_signal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Trace output
 * ------------------------------------------------------------------------- */

static void print_trace_escaped(FILE *out, const char *data) {
    fputc('"', out);
    const char *text = data ? data : "";
    ds_fprint_escaped(out, text, strlen(text), DS_ESCAPE_BASIC);
    fputc('"', out);
}

/* -------------------------------------------------------------------------
 * Interpolation rendering
 * ------------------------------------------------------------------------- */

static void ascii_transform_string(const DsString *in, DsString *out, DsInterpFormatKind kind) {
    ds_string_init(out);
    size_t a = 0, b = in->len;
    if (kind == DS_INTERP_FORMAT_TRIM) vm_ascii_trim_bounds(in->data, in->len, &a, &b);
    ds_string_append_range(out, in->data ? in->data + a : "", b - a);
    if (kind == DS_INTERP_FORMAT_UPPER || kind == DS_INTERP_FORMAT_LOWER) {
        for (size_t i = 0; i < out->len; i++) {
            if (kind == DS_INTERP_FORMAT_UPPER && out->data[i] >= 'a' && out->data[i] <= 'z') out->data[i] = (char)(out->data[i] - 'a' + 'A');
            if (kind == DS_INTERP_FORMAT_LOWER && out->data[i] >= 'A' && out->data[i] <= 'Z') out->data[i] = (char)(out->data[i] - 'A' + 'a');
        }
    }
}

static bool append_padded(DsString *out, const char *data, size_t len, int width, char align) {
    if (width <= (int)len) return ds_string_append_range(out, data, len);
    int pad = width - (int)len;
    int left = 0, right = 0;
    if (align == '<') right = pad;
    else if (align == '>') left = pad;
    else { left = pad / 2; right = pad - left; }
    for (int i = 0; i < left; i++) ds_string_append_char(out, ' ');
    ds_string_append_range(out, data, len);
    for (int i = 0; i < right; i++) ds_string_append_char(out, ' ');
    return true;
}

static DsInterpValueKind interp_kind_from_value(const DsValue *value) {
    switch (value->kind) {
        case DS_VALUE_BOOL: return DS_INTERP_VALUE_BOOL;
        case DS_VALUE_INT: return DS_INTERP_VALUE_INT;
        case DS_VALUE_STRING: return DS_INTERP_VALUE_STRING;
        case DS_VALUE_COMMAND_RESULT: return DS_INTERP_VALUE_COMMAND_RESULT;
        default: return DS_INTERP_VALUE_UNKNOWN;
    }
}

static bool append_formatted_value(Vm *vm, DsValue *value, const char *spec, size_t spec_len, DsString *out, DsSpan span) {
    if (spec_len == 0) {
        DsString rendered; ds_value_to_string(value, &rendered);
        ds_string_append_range(out, ds_string_data(&rendered), rendered.len);
        ds_string_free(&rendered);
        return true;
    }
    DsInterpFormatSpec parsed;
    DsStr spec_text = {(char *)spec, spec_len};
    if (!ds_interp_parse_format_spec_for_kind(spec_text, interp_kind_from_value(value), &parsed)) {
        ds_diag_error(vm->diag, span, "internal VM interpolation invariant failed: unsupported format specifier `%.*s` after lowering", (int)spec_len, spec);
        return false;
    }
    if (parsed.kind == DS_INTERP_FORMAT_UPPER || parsed.kind == DS_INTERP_FORMAT_LOWER || parsed.kind == DS_INTERP_FORMAT_TRIM) {
        DsString rendered;
        ascii_transform_string(&value->as.string, &rendered, parsed.kind);
        ds_string_append_range(out, ds_string_data(&rendered), rendered.len);
        ds_string_free(&rendered);
        return true;
    }
    if (parsed.kind == DS_INTERP_FORMAT_ALIGN_LEFT || parsed.kind == DS_INTERP_FORMAT_ALIGN_RIGHT || parsed.kind == DS_INTERP_FORMAT_ALIGN_CENTER) {
        char align = parsed.kind == DS_INTERP_FORMAT_ALIGN_LEFT ? '<' : parsed.kind == DS_INTERP_FORMAT_ALIGN_RIGHT ? '>' : '^';
        return append_padded(out, ds_string_data(&value->as.string), value->as.string.len, parsed.width, align);
    }
    char buf[64];
    if (parsed.kind == DS_INTERP_FORMAT_INT_DECIMAL) {
        snprintf(buf, sizeof(buf), "%lld", (long long)value->as.integer);
        size_t len = strlen(buf);
        if (parsed.width <= (int)len) return ds_string_append_cstr(out, buf);
        int pad = parsed.width - (int)len;
        if (parsed.zero_pad) {
            if (buf[0] == '-') {
                ds_string_append_char(out, '-');
                for (int i = 0; i < pad; i++) ds_string_append_char(out, '0');
                return ds_string_append_cstr(out, buf + 1);
            }
            for (int i = 0; i < pad; i++) ds_string_append_char(out, '0');
            return ds_string_append_cstr(out, buf);
        }
        for (int i = 0; i < pad; i++) ds_string_append_char(out, ' ');
        return ds_string_append_cstr(out, buf);
    }
    int prec = parsed.precision < 0 ? 6 : parsed.precision;
    DsString tmp;
    ds_string_init(&tmp);
    ds_string_appendf(&tmp, "%lld.", (long long)value->as.integer);
    for (int i = 0; i < prec; i++) ds_string_append_char(&tmp, '0');

    if (parsed.width > (int)tmp.len) {
        append_padded(out, tmp.data, tmp.len, parsed.width, '>');
    } else {
        ds_string_append_range(out, tmp.data, tmp.len);
    }
    ds_string_free(&tmp);
    return true;
}

typedef struct {
    Vm *vm;
    const char *data;
    size_t len;
    size_t pos;
    DsSpan span;
} VmArithmeticParser;

static bool arithmetic_parse_expr(VmArithmeticParser *parser, int64_t *out);

static void arithmetic_skip_ws(VmArithmeticParser *parser) {
    while (parser->pos < parser->len &&
           (parser->data[parser->pos] == ' ' || parser->data[parser->pos] == '\t')) {
        parser->pos++;
    }
}

static bool arithmetic_parse_integer_literal(VmArithmeticParser *parser, int64_t *out) {
    size_t start = parser->pos++;
    while (parser->pos < parser->len && parser->data[parser->pos] >= '0' && parser->data[parser->pos] <= '9') {
        parser->pos++;
    }

    char *text = ds_str_dup_range(parser->data + start, parser->pos - start);
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 10);
    bool ok = errno == 0 && end && *end == '\0';
    free(text);
    if (!ok) {
        ds_diag_error(parser->vm->diag, parser->span, "integer literal is outside the supported int range");
        return false;
    }
    *out = (int64_t)value;
    return true;
}

static bool arithmetic_parse_variable(VmArithmeticParser *parser, int64_t *out) {
    size_t start = parser->pos++;
    while (parser->pos < parser->len && ds_is_ident_continue(parser->data[parser->pos])) {
        parser->pos++;
    }

    char *name = ds_str_dup_range(parser->data + start, parser->pos - start);
    DsValue value;
    if (!lookup_var(parser->vm, name, &value, parser->span)) {
        free(name);
        return false;
    }
    free(name);

    if (parser->pos < parser->len && parser->data[parser->pos] == '.') {
        parser->pos++;
        size_t field_start = parser->pos;
        if (parser->pos < parser->len && ds_is_ident_start(parser->data[parser->pos])) {
            parser->pos++;
            while (parser->pos < parser->len && ds_is_ident_continue(parser->data[parser->pos])) parser->pos++;
        }
        DsStr field = {(char *)parser->data + field_start, parser->pos - field_start};
        if (value.kind != DS_VALUE_MAP) {
            ds_value_free(&value);
            ds_diag_error(parser->vm->diag, parser->span, "arithmetic interpolation field reads require a row value in v0.37.0");
            return false;
        }
        DsValue *found = ds_map_get(&value.as.map, field);
        if (!found) {
            ds_diag_error(parser->vm->diag, parser->span, "missing map key `%.*s`", (int)field.len, field.data);
            ds_value_free(&value);
            return false;
        }
        DsValue field_value = ds_value_copy(found);
        ds_value_free(&value);
        value = field_value;
    }

    if (value.kind != DS_VALUE_INT) {
        ds_value_free(&value);
        ds_diag_error(parser->vm->diag, parser->span, "arithmetic interpolation operands must be integers in v0.21.0");
        return false;
    }
    *out = value.as.integer;
    ds_value_free(&value);
    return true;
}

static bool arithmetic_parse_primary(VmArithmeticParser *parser, int64_t *out) {
    arithmetic_skip_ws(parser);
    if (parser->pos >= parser->len) return false;

    char c = parser->data[parser->pos];
    if (c == '(') {
        parser->pos++;
        if (!arithmetic_parse_expr(parser, out)) return false;
        arithmetic_skip_ws(parser);
        if (parser->pos >= parser->len || parser->data[parser->pos] != ')') return false;
        parser->pos++;
        return true;
    }

    if (c == '-') {
        parser->pos++;
        int64_t value = 0;
        if (!arithmetic_parse_primary(parser, &value)) return false;
        if (value == INT64_MIN) {
            ds_diag_error(parser->vm->diag, parser->span, "integer overflow in unary `-`");
            return false;
        }
        *out = -value;
        return true;
    }

    if (c >= '0' && c <= '9') return arithmetic_parse_integer_literal(parser, out);
    if (ds_is_ident_start(c)) return arithmetic_parse_variable(parser, out);
    return false;
}

static bool arithmetic_parse_power(VmArithmeticParser *parser, int64_t *out) {
    int64_t base = 0;
    if (!arithmetic_parse_primary(parser, &base)) return false;

    arithmetic_skip_ws(parser);
    bool has_exponent = parser->pos + 1 < parser->len &&
                        parser->data[parser->pos] == '*' &&
                        parser->data[parser->pos + 1] == '*';
    if (!has_exponent) {
        *out = base;
        return true;
    }

    parser->pos += 2;
    int64_t exponent = 0;
    if (!arithmetic_parse_power(parser, &exponent)) return false;
    if (exponent < 0) {
        ds_diag_error(parser->vm->diag, parser->span, "negative exponent runtime value is rejected in v0.21.0");
        return false;
    }

    int64_t result = 0;
    if (!vm_i64_pow_checked(base, exponent, &result)) {
        ds_diag_error(parser->vm->diag, parser->span, "integer overflow in operator `**`");
        return false;
    }
    *out = result;
    return true;
}

static bool arithmetic_parse_mul_div(VmArithmeticParser *parser, int64_t *out) {
    int64_t value = 0;
    if (!arithmetic_parse_power(parser, &value)) return false;

    for (;;) {
        arithmetic_skip_ws(parser);
        if (parser->pos >= parser->len) break;

        char op = parser->data[parser->pos];
        if (!(op == '*' || op == '/' || op == '%')) break;
        if (op == '*' && parser->pos + 1 < parser->len && parser->data[parser->pos + 1] == '*') break;
        parser->pos++;

        int64_t rhs = 0;
        if (!arithmetic_parse_power(parser, &rhs)) return false;
        if ((op == '/' || op == '%') && rhs == 0) {
            ds_diag_error(parser->vm->diag, parser->span, "division or modulo by zero");
            return false;
        }
        if ((op == '/' || op == '%') && value == INT64_MIN && rhs == -1) {
            ds_diag_error(parser->vm->diag, parser->span, "integer overflow in operator `%c`", op);
            return false;
        }
        if (op == '*') {
            if (!vm_i64_mul_checked(value, rhs, &value)) {
                ds_diag_error(parser->vm->diag, parser->span, "integer overflow in operator `*`");
                return false;
            }
        } else if (op == '/') {
            value /= rhs;
        } else {
            value %= rhs;
        }
    }
    *out = value;
    return true;
}

static bool arithmetic_parse_expr(VmArithmeticParser *parser, int64_t *out) {
    int64_t value = 0;
    if (!arithmetic_parse_mul_div(parser, &value)) return false;

    for (;;) {
        arithmetic_skip_ws(parser);
        if (parser->pos >= parser->len) break;

        char op = parser->data[parser->pos];
        if (!(op == '+' || op == '-')) break;
        parser->pos++;

        int64_t rhs = 0;
        if (!arithmetic_parse_mul_div(parser, &rhs)) return false;
        if (op == '+') {
            if (!vm_i64_add_checked(value, rhs, &value)) {
                ds_diag_error(parser->vm->diag, parser->span, "integer overflow in operator `+`");
                return false;
            }
        } else if (!vm_i64_sub_checked(value, rhs, &value)) {
            ds_diag_error(parser->vm->diag, parser->span, "integer overflow in operator `-`");
            return false;
        }
    }
    *out = value;
    return true;
}

static bool append_arithmetic_interpolation(Vm *vm, const char *data, size_t len, DsString *out, DsSpan span) {
    VmArithmeticParser parser = {
        .vm = vm,
        .data = data,
        .len = len,
        .pos = 0,
        .span = span,
    };
    int64_t result = 0;
    if (!arithmetic_parse_expr(&parser, &result)) return false;
    arithmetic_skip_ws(&parser);
    if (parser.pos != parser.len) return false;

    return ds_string_appendf(out, "%lld", (long long)result);
}

static bool interp_parse_int_literal(const char *data, size_t len, size_t *i, int64_t *out) {
    bool neg = false;
    if (*i < len && data[*i] == '-') { neg = true; (*i)++; }
    if (*i >= len || data[*i] < '0' || data[*i] > '9') return false;
    int64_t value = 0;
    while (*i < len && data[*i] >= '0' && data[*i] <= '9') {
        int digit = data[*i] - '0';
        if (value > (INT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        (*i)++;
    }
    *out = neg ? -value : value;
    return true;
}

static bool interp_parse_string_literal(const char *data, size_t len, size_t *i, DsString *out) {
    size_t start = *i;
    if (start >= len || data[start] != '"') return false;
    (*i)++;
    while (*i < len) {
        if (data[*i] == '\\' && *i + 1 < len) { *i += 2; continue; }
        if (data[*i] == '"') { (*i)++; break; }
        (*i)++;
    }
    DsStr literal = {(char *)data + start, *i - start};
    return decode_string_text(literal, out);
}

static bool interp_parse_indexed_value(Vm *vm, DsValue *value, const char *data, size_t len, size_t *j, DsSpan span) {
    (*j)++;
    ds_skip_ascii_ws(data, len, j);
    DsValue indexed = ds_value_null();

    if (value->kind == DS_VALUE_ARRAY) {
        int64_t index = 0;
        bool have_index = false;
        if (*j < len && ((data[*j] == '-' && *j + 1 < len && data[*j + 1] >= '0' && data[*j + 1] <= '9') ||
                         (data[*j] >= '0' && data[*j] <= '9'))) {
            have_index = interp_parse_int_literal(data, len, j, &index);
        } else if (*j < len && ds_is_ident_start(data[*j])) {
            size_t name_start = *j;
            (*j)++;
            while (*j < len && ds_is_ident_continue(data[*j])) (*j)++;
            char *idx_name = ds_str_dup_range(data + name_start, *j - name_start);
            DsValue idx_value;
            if (!lookup_var(vm, idx_name, &idx_value, span)) { free(idx_name); return false; }
            free(idx_name);
            if (idx_value.kind != DS_VALUE_INT) {
                ds_diag_error(vm->diag, span, "runtime array index must be an int");
                ds_value_free(&idx_value);
                return false;
            }
            index = idx_value.as.integer;
            ds_value_free(&idx_value);
            have_index = true;
        }
        if (!have_index) {
            ds_diag_error(vm->diag, span, "internal VM interpolation invariant failed: unsupported array index interpolation shape");
            return false;
        }
        ds_skip_ascii_ws(data, len, j);
        if (*j >= len || data[*j] != ']') {
            ds_diag_error(vm->diag, span, "internal VM interpolation invariant failed: unsupported array index interpolation shape");
            return false;
        }
        (*j)++;
        if (index < 0 || (size_t)index >= value->as.array.len) {
            ds_diag_error(vm->diag, span, "array index %lld out of range", (long long)index);
            return false;
        }
        indexed = ds_value_copy((DsValue *)value->as.array.items[index]);
    } else if (value->kind == DS_VALUE_MAP) {
        DsString key;
        bool have_key = false;
        ds_string_init(&key);
        if (*j < len && data[*j] == '"') {
            have_key = interp_parse_string_literal(data, len, j, &key);
        } else if (*j < len && ds_is_ident_start(data[*j])) {
            size_t name_start = *j;
            (*j)++;
            while (*j < len && ds_is_ident_continue(data[*j])) (*j)++;
            char *idx_name = ds_str_dup_range(data + name_start, *j - name_start);
            DsValue idx_value;
            if (!lookup_var(vm, idx_name, &idx_value, span)) { free(idx_name); ds_string_free(&key); return false; }
            free(idx_name);
            if (idx_value.kind != DS_VALUE_STRING) {
                ds_diag_error(vm->diag, span, "runtime map index must be a string");
                ds_value_free(&idx_value);
                ds_string_free(&key);
                return false;
            }
            ds_string_append_range(&key, ds_string_data(&idx_value.as.string), idx_value.as.string.len);
            ds_value_free(&idx_value);
            have_key = true;
        }
        if (!have_key) {
            ds_diag_error(vm->diag, span, "internal VM interpolation invariant failed: unsupported map index interpolation shape");
            ds_string_free(&key);
            return false;
        }
        ds_skip_ascii_ws(data, len, j);
        if (*j >= len || data[*j] != ']') {
            ds_diag_error(vm->diag, span, "internal VM interpolation invariant failed: unsupported map index interpolation shape");
            ds_string_free(&key);
            return false;
        }
        (*j)++;
        DsStr key_view = {key.data, key.len};
        DsValue *found = ds_map_get(&value->as.map, key_view);
        if (!found) {
            ds_diag_error(vm->diag, span, "missing map key `%.*s`", (int)key_view.len, key_view.data);
            ds_string_free(&key);
            return false;
        }
        indexed = ds_value_copy(found);
        ds_string_free(&key);
    } else {
        ds_diag_error(vm->diag, span, "internal VM interpolation invariant failed: index receiver should be an array or map after lowering");
        return false;
    }

    ds_value_free(value);
    *value = indexed;
    return true;
}

static bool interp_parse_map_field_value(Vm *vm, DsValue *value, const char *data, size_t len, size_t *j, DsSpan span) {
    (*j)++;
    size_t field_start = *j;
    if (*j < len && ds_is_ident_start(data[*j])) {
        (*j)++;
        while (*j < len && ds_is_ident_continue(data[*j])) (*j)++;
    }
    DsStr field = {(char *)data + field_start, *j - field_start};
    if (value->kind != DS_VALUE_MAP) {
        ds_diag_error(vm->diag, span, "internal VM interpolation invariant failed: field receiver should be a row/map after lowering");
        return false;
    }
    DsValue *found = ds_map_get(&value->as.map, field);
    if (!found) {
        ds_diag_error(vm->diag, span, "missing map key `%.*s`", (int)field.len, field.data);
        return false;
    }
    DsValue field_value = ds_value_copy(found);
    ds_value_free(value);
    *value = field_value;
    return true;
}

/* -------------------------------------------------------------------------
 * Command-word and redirect materialization
 * ------------------------------------------------------------------------- */

bool interpolate_string(Vm *vm, const DsString *input, DsString *out, DsSpan span) {
    /*
     * Source-language interpolation acceptance is validated during lowering.
     * VM interpolation errors here are runtime failures (for example arithmetic
     * overflow) or defensive invariant checks for accepted command/string HIR.
     */
    ds_string_init(out);
    for (size_t i = 0; i < input->len; i++) {
        char c = input->data[i];
        if (c == '{') {
            if (i + 1 < input->len && input->data[i + 1] == '{') {
                ds_string_append_char(out, '{');
                i++;
                continue;
            }
            size_t start = i + 1;
            size_t j = start;
            size_t arith_end = start;
            bool maybe_arith = false;
            while (arith_end < input->len && input->data[arith_end] != '}') {
                char ac = input->data[arith_end++];
                if (ac == '+' || ac == '-' || ac == '*' || ac == '/' || ac == '%' || ac == '(' || ac == ')') maybe_arith = true;
            }
            if (!maybe_arith && j < input->len && ds_is_ident_start(input->data[j])) {
                j++;
                while (j < input->len && ds_is_ident_continue(input->data[j])) j++;
                if (j < input->len && (input->data[j] == '}' || input->data[j] == '.' || input->data[j] == ':' || input->data[j] == '[')) {
                    char *name = ds_str_dup_range(input->data + start, j - start);
                    if (strcmp(name, "env") == 0 && input->data[j] == '.') {
                        size_t field_start = ++j;
                        if (j < input->len && ds_is_ident_start(input->data[j])) {
                            j++;
                            while (j < input->len && ds_is_ident_continue(input->data[j])) j++;
                        }
                        char *field = ds_str_dup_range(input->data + field_start, j - field_start);
                        if (j >= input->len || input->data[j] != '}') {
                            ds_diag_error(vm->diag, span, "internal VM interpolation invariant failed: unsupported string interpolation shape");
                            free(field); free(name); ds_string_free(out); return false;
                        }
                        const char *env_value = getenv(field);
                        ds_string_append_cstr(out, env_value ? env_value : "");
                        free(field); free(name);
                        i = j; continue;
                    }
                    DsValue value;
                    if (!lookup_var(vm, name, &value, span)) { free(name); ds_string_free(out); return false; }
                    if (input->data[j] == '[') {
                        if (!interp_parse_indexed_value(vm, &value, input->data, input->len, &j, span)) { free(name); ds_string_free(out); return false; }
                        if (j < input->len && input->data[j] == '.') {
                            if (!interp_parse_map_field_value(vm, &value, input->data, input->len, &j, span)) { free(name); ds_string_free(out); return false; }
                        }
                    } else if (input->data[j] == '.') {
                        size_t field_start = ++j;
                        if (j < input->len && ds_is_ident_start(input->data[j])) {
                            j++;
                            while (j < input->len && ds_is_ident_continue(input->data[j])) j++;
                        }
                        char *field = ds_str_dup_range(input->data + field_start, j - field_start);
                        DsValue field_value = ds_value_null();
                        bool ok = vm_command_result_field(vm, &value, field, span, &field_value);
                        free(field); ds_value_free(&value);
                        if (!ok) { free(name); ds_string_free(out); return false; }
                        value = field_value;
                    }
                    const char *spec = NULL; size_t spec_len = 0;
                    if (j < input->len && input->data[j] == ':') {
                        size_t spec_start = ++j;
                        while (j < input->len && input->data[j] != '}') j++;
                        spec = input->data + spec_start; spec_len = j - spec_start;
                    }
                    if (j >= input->len || input->data[j] != '}') {
                        ds_diag_error(vm->diag, span, "internal VM interpolation invariant failed: unsupported string interpolation shape");
                        ds_value_free(&value); free(name); ds_string_free(out); return false;
                    }
                    bool ok = append_formatted_value(vm, &value, spec, spec_len, out, span);
                    ds_value_free(&value); free(name);
                    if (!ok) { ds_string_free(out); return false; }
                    i = j; continue;
                }
            }
            if (arith_end < input->len && append_arithmetic_interpolation(vm, input->data + start, arith_end - start, out, span)) {
                i = arith_end;
                continue;
            }
            ds_diag_error(vm->diag, span, "internal VM interpolation invariant failed: unsupported string interpolation shape");
            ds_string_free(out); return false;
        }
        if (c == '}') {
            if (i + 1 < input->len && input->data[i + 1] == '}') {
                ds_string_append_char(out, '}');
                i++;
                continue;
            }
            ds_diag_error(vm->diag, span, "internal VM interpolation invariant failed: unmatched literal close brace after lowering");
            ds_string_free(out); return false;
        }
        ds_string_append_char(out, c);
    }
    return true;
}

static bool word_to_arg(Vm *vm, DsStr word, DsSpan span, char **out) {
    DsCommandWordForm form = ds_command_word_analyze(word);
    if (form.kind == DS_COMMAND_WORD_QUOTED) {
        DsString decoded;
        if (!decode_string_text(word, &decoded)) return false;
        DsString rendered;
        bool ok = interpolate_string(vm, &decoded, &rendered, span);
        ds_string_free(&decoded);
        if (!ok) return false;
        *out = ds_str_dup_range(ds_string_data(&rendered), rendered.len);
        ds_string_free(&rendered);
        return true;
    }
    if (form.kind == DS_COMMAND_WORD_VARIABLE) {
        if (word.data[0] == '$' && form.name.len + 1 < word.len) {
            ds_diag_error(vm->diag, span, "internal VM command-word invariant failed: unsupported variable suffix after lowering");
            return false;
        }
        char *name = ds_str_dup_range(form.name.data, form.name.len);
        DsValue value;
        if (!lookup_var(vm, name, &value, span)) {
            free(name);
            return false;
        }
        DsString rendered;
        ds_value_to_string(&value, &rendered);
        *out = ds_str_dup_range(ds_string_data(&rendered), rendered.len);
        ds_string_free(&rendered);
        ds_value_free(&value);
        free(name);
        return true;
    }
    if (form.kind == DS_COMMAND_WORD_FIELD) {
        char *name = ds_str_dup_range(form.name.data, form.name.len);
        char *field = ds_str_dup_range(form.field.data, form.field.len);
        if (strcmp(name, "env") == 0) {
            const char *value = getenv(field);
            *out = ds_str_dup_cstr(value);
            free(name); free(field);
            return true;
        }
        DsValue value;
        if (!lookup_var(vm, name, &value, span)) { free(name); free(field); return false; }
        DsValue field_value = ds_value_null();
        bool ok = false;
        if (value.kind == DS_VALUE_COMMAND_RESULT) {
            ok = vm_command_result_field(vm, &value, field, span, &field_value);
        } else if (value.kind == DS_VALUE_MAP) {
            DsStr key = {field, strlen(field)};
            DsValue *found = ds_map_get(&value.as.map, key);
            if (!found) {
                ds_diag_error(vm->diag, span, "missing map key `%s`", field);
                ok = false;
            } else {
                field_value = ds_value_copy(found);
                ok = true;
            }
        } else {
            ds_diag_error(vm->diag, span, "runtime field command argument requires a command result or row map");
        }
        if (!ok) {
            ds_value_free(&value);
            free(name); free(field);
            return false;
        }
        DsString rendered;
        ds_value_to_string(&field_value, &rendered);
        *out = ds_str_dup_range(ds_string_data(&rendered), rendered.len);
        ds_string_free(&rendered);
        ds_value_free(&field_value);
        ds_value_free(&value);
        free(name); free(field);
        return !vm->diag->has_error;
    }
    *out = ds_str_dup_range(word.data, word.len);
    return true;
}

static bool render_redirect_target(Vm *vm, const DsRedirect *redirect, char **out) {
    DsString decoded;
    if (!decode_string_text(redirect->target, &decoded)) {
        ds_diag_error(vm->diag, redirect->target_span, "invalid redirection target");
        return false;
    }
    DsString rendered;
    bool ok = interpolate_string(vm, &decoded, &rendered, redirect->target_span);
    ds_string_free(&decoded);
    if (!ok) return false;
    *out = ds_str_dup_range(ds_string_data(&rendered), rendered.len);
    ds_string_free(&rendered);
    return true;
}

typedef struct {
    char **items;
    size_t len;
} VmArgv;

typedef struct {
    DsString stdout_text;
    DsString stderr_text;
    int code;
    bool terminated_by_sigpipe;
    bool has_non_sigpipe_failure;
} VmProcessResult;

typedef struct {
    VmArgv argv;
    DsRedirect redirect;
    DsSpan span;
    bool capture;
    int exec_error_fd;
} VmProcessSpec;

static void argv_free(VmArgv *argv) {
    ds_free_cstr_array(argv->items, argv->len);
    *argv = (VmArgv){0};
}

static bool argv_build_range(Vm *vm, Instr *ins, size_t first_word, size_t word_count, VmArgv *argv) {
    *argv = (VmArgv){0};
    if (word_count == 0) return false;
    argv->items = (char **)ds_xcalloc(word_count + 1, sizeof(char *));
    argv->len = word_count;
    for (size_t i = 0; i < word_count; i++) {
        if (!word_to_arg(vm, ins->words[first_word + i], ins->span, &argv->items[i])) {
            argv->len = i;
            argv_free(argv);
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Process specs, result storage, redirects, and built-in control commands
 * ------------------------------------------------------------------------- */

static int process_status_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

static bool process_status_is_signal(int status, int sig) {
    return WIFSIGNALED(status) && WTERMSIG(status) == sig;
}

static void process_result_init(VmProcessResult *result) {
    *result = (VmProcessResult){0};
}

static void process_result_free(VmProcessResult *result) {
    ds_string_free(&result->stdout_text);
    ds_string_free(&result->stderr_text);
    *result = (VmProcessResult){0};
}

static bool open_redirect_target(Vm *vm, const DsRedirect *redirect, int *out_fd) {
    *out_fd = -1;
    char *redirect_path = NULL;
    if (!render_redirect_target(vm, redirect, &redirect_path)) return false;

    int flags = O_CREAT | O_WRONLY;
    if (redirect->kind == DS_REDIRECT_OUT_APPEND || redirect->kind == DS_REDIRECT_ERR_APPEND || redirect->kind == DS_REDIRECT_ALL_APPEND) flags |= O_APPEND;
    else flags |= O_TRUNC;

    int fd = open(redirect_path, flags, 0666);
    if (fd < 0) {
        ds_diag_error(vm->diag, redirect->target_span, "failed to open redirection target `%s`: %s", redirect_path, strerror(errno));
        free(redirect_path);
        return false;
    }
    free(redirect_path);
    *out_fd = fd;
    return true;
}

static bool process_spec_from_instr(Vm *vm, Instr *ins, bool capture, VmProcessSpec *spec) {
    memset(spec, 0, sizeof(*spec));
    spec->span = ins->span;
    spec->redirect = ins->redirect;
    spec->capture = capture;
    return argv_build_range(vm, ins, 0, ins->word_count, &spec->argv);
}

static bool process_spec_from_stage(Vm *vm, Instr *ins, size_t stage_index, bool capture, VmProcessSpec *spec) {
    memset(spec, 0, sizeof(*spec));
    spec->span = ins->span;
    ds_redirect_init(&spec->redirect);
    spec->capture = capture;
    size_t first = 0;
    for (size_t i = 0; i < stage_index; i++) first += ins->stage_word_counts[i];
    return argv_build_range(vm, ins, first, ins->stage_word_counts[stage_index], &spec->argv);
}

static bool parse_exit_code_arg(const char *text, int *out) {
    if (!text || !*text) return false;
    return ds_parse_int_range((DsStr){(char *)text, strlen(text)}, 0, 255, out);
}

static void append_test_helper_message(DsString *out, const VmProcessSpec *spec, size_t first_arg) {
    ds_string_init(out);
    for (size_t i = first_arg; i < spec->argv.len; i++) {
        if (i > first_arg) ds_string_append_char(out, ' ');
        ds_string_append_cstr(out, spec->argv.items[i]);
    }
}

static bool run_control_command(Vm *vm, const VmProcessSpec *spec, int *out_code) {
    *out_code = 0;
    if (spec->capture || spec->argv.len == 0) return false;
    const char *name = spec->argv.items[0];
    if (strcmp(name, "fail") != 0 && strcmp(name, "exit") != 0) return false;

    const bool test_mode = vm->options.test_mode;
    const char *test_name = vm->options.test_name.data ? vm->options.test_name.data : "<test>";
    int test_name_len = (int)vm->options.test_name.len;
    if (test_name_len <= 0) test_name_len = (int)strlen(test_name);

    if (spec->redirect.kind != DS_REDIRECT_NONE) {
        if (test_mode) ds_diag_error(vm->diag, spec->span, "test `%.*s`: `%s` does not support redirection", test_name_len, test_name, name);
        else ds_diag_error(vm->diag, spec->span, "`%s` does not support redirection", name);
        *out_code = 1;
        return true;
    }
    if (strcmp(name, "fail") == 0) {
        DsString message;
        append_test_helper_message(&message, spec, 1);
        if (test_mode) {
            if (message.len > 0) ds_diag_error(vm->diag, spec->span, "test `%.*s`: fail: %.*s", test_name_len, test_name, (int)message.len, message.data);
            else ds_diag_error(vm->diag, spec->span, "test `%.*s`: fail", test_name_len, test_name);
        } else if (message.len > 0) {
            ds_diag_error(vm->diag, spec->span, "%.*s", (int)message.len, message.data);
        } else {
            ds_diag_error(vm->diag, spec->span, "fail");
        }
        ds_string_free(&message);
        *out_code = 1;
        return true;
    }
    if (spec->argv.len != 2) {
        if (test_mode) ds_diag_error(vm->diag, spec->span, "test `%.*s`: `exit` expects exactly one integer code", test_name_len, test_name);
        else ds_diag_error(vm->diag, spec->span, "`exit` expects exactly one integer code");
        *out_code = 1;
        return true;
    }
    int code = 0;
    if (!parse_exit_code_arg(spec->argv.items[1], &code)) {
        if (test_mode) ds_diag_error(vm->diag, spec->span, "test `%.*s`: `exit` code must be an integer from 0 to 255", test_name_len, test_name);
        else ds_diag_error(vm->diag, spec->span, "`exit` code must be an integer from 0 to 255");
        *out_code = 1;
        return true;
    }
    if (test_mode) {
        vm->test_done = true;
        if (code != 0) ds_diag_error(vm->diag, spec->span, "test `%.*s`: exit %d", test_name_len, test_name, code);
    } else {
        vm->control_exit_requested = true;
    }
    *out_code = code;
    return true;
}

static void process_spec_free(VmProcessSpec *spec) {
    argv_free(&spec->argv);
}

static void trace_command_spec(Vm *vm, const VmProcessSpec *spec) {
    if (!vm->options.trace_cmd || spec->argv.len == 0) return;
    fprintf(stderr, "trace: cmd %s:%d:%d:", span_path(vm->source, spec->span), spec->span.start.line, spec->span.start.column);
    for (size_t i = 0; i < spec->argv.len; i++) {
        fputc(' ', stderr);
        print_trace_escaped(stderr, spec->argv.items[i]);
    }
    if (spec->redirect.kind != DS_REDIRECT_NONE) {
        char *redirect_path = NULL;
        const char *op = ds_redirect_shell_op(spec->redirect.kind);
        if (op && render_redirect_target(vm, &spec->redirect, &redirect_path)) {
            fputc(' ', stderr);
            fputs(op, stderr);
            fputc(' ', stderr);
            print_trace_escaped(stderr, redirect_path);
            free(redirect_path);
        } else {
            fputs(" <redirect>", stderr);
        }
    }
    fputc('\n', stderr);
}

/* -------------------------------------------------------------------------
 * Foreground process groups and signal forwarding
 * ------------------------------------------------------------------------- */

static bool fd_set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static bool vm_command_was_interrupted(const Vm *vm, int code) {
    return vm->interrupted_signal != 0 &&
           code == ds_posix_signal_default_status(vm->interrupted_signal);
}

static bool vm_stdout_is_pipe_like(void) {
    struct stat st;
    if (fstat(STDOUT_FILENO, &st) != 0) return false;
    return S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode);
}

static bool vm_command_is_quiet_broken_pipe(const VmProcessSpec *spec, const VmProcessResult *result) {
    /*
     * v0.34.0 closed-stdout DX: when the script itself is piped into a consumer
     * such as `head`, ordinary output commands may conventionally surface as
     * SIGPIPE/141. The VM has the raw wait status, so keep this classification
     * narrow: only an actual SIGPIPE termination on an uncaptured, unredirected
     * command with a pipe-like stdout is quiet. Captured command results still
     * expose the observed 141 status as data.
     */
    return spec && !spec->capture && spec->redirect.kind == DS_REDIRECT_NONE &&
           result && result->code == 128 + SIGPIPE && result->terminated_by_sigpipe &&
           !result->has_non_sigpipe_failure && vm_stdout_is_pipe_like();
}

static bool vm_pipeline_is_quiet_broken_pipe(const Instr *ins, const VmProcessResult *result) {
    return ins && ins->redirect.kind == DS_REDIRECT_NONE &&
           result && result->code == 128 + SIGPIPE && result->terminated_by_sigpipe &&
           !result->has_non_sigpipe_failure && vm_stdout_is_pipe_like();
}

static void vm_forward_signal_to_child_group(Vm *vm, pid_t pgid, int sig) {
    /*
     * Trap/defer/signal parity boundary: foreground command/pipeline signal
     * handling is VM process execution policy, not language validation. The
     * lowerer has already accepted handler declarations; at runtime the VM only
     * records INT/TERM for cleanup classification and forwards the signal to
     * the foreground child process group when one exists.
     */
    if (!ds_posix_signal_is_runtime_cleanup(sig)) return;
    vm_note_interrupted_signal(vm, sig);
    if (pgid > 0) kill(-pgid, sig);
}

static void vm_note_wait_status_signal(Vm *vm, int status) {
    if (WIFSIGNALED(status)) vm_note_interrupted_signal(vm, WTERMSIG(status));
}

static void vm_restore_terminal_pgrp(int tty_fd, pid_t shell_pgid) {
    if (tty_fd >= 0 && shell_pgid > 0) {
        void (*old_ttou)(int) = signal(SIGTTOU, SIG_IGN);
        tcsetpgrp(tty_fd, shell_pgid);
        signal(SIGTTOU, old_ttou);
    }
}

static bool vm_make_foreground_group(pid_t pgid, int *tty_fd, pid_t *shell_pgid) {
    *tty_fd = -1;
    *shell_pgid = -1;
    if (pgid <= 0) return false;
    setpgid(pgid, pgid);
    if (isatty(STDIN_FILENO)) {
        pid_t current = tcgetpgrp(STDIN_FILENO);
        pid_t self = getpgrp();
        if (current == self) {
            void (*old_ttou)(int) = signal(SIGTTOU, SIG_IGN);
            if (tcsetpgrp(STDIN_FILENO, pgid) == 0) {
                *tty_fd = STDIN_FILENO;
                *shell_pgid = self;
            }
            signal(SIGTTOU, old_ttou);
        }
    }
    return true;
}

static bool vm_wait_child(Vm *vm, pid_t pid, pid_t pgid, DsSpan span, const char *kind,
                          const char *command, int *status) {
    while (waitpid(pid, status, 0) < 0) {
        if (errno == EINTR) {
            int sig = vm_take_pending_signal();
            if (sig) vm_forward_signal_to_child_group(vm, pgid, sig);
            continue;
        }
        ds_diag_error(vm->diag, span, "failed waiting for %s `%s`: %s", kind, command, strerror(errno));
        return false;
    }
    vm_note_wait_status_signal(vm, *status);
    return true;
}

static bool vm_wait_foreground_child(Vm *vm, pid_t pid, pid_t pgid, const VmProcessSpec *spec, int *status) {
    /*
     * Direct foreground commands get their own process group when possible.
     * If the ds runner observes INT/TERM while waiting, forward it to that
     * group and let the VM cleanup dispatcher decide the final handler order
     * and conventional status from the shared signal contract.
     */
    int tty_fd = -1;
    pid_t shell_pgid = -1;
    vm_make_foreground_group(pgid, &tty_fd, &shell_pgid);
    if (!vm_wait_child(vm, pid, pgid, spec->span, "command", spec->argv.items[0], status)) {
        vm_restore_terminal_pgrp(tty_fd, shell_pgid);
        return false;
    }
    vm_restore_terminal_pgrp(tty_fd, shell_pgid);
    return true;
}

static bool process_exec_error_pipe(Vm *vm, DsSpan span, const char *command, int pipe_fds[2]) {
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    if (pipe(pipe_fds) != 0 || !fd_set_cloexec(pipe_fds[1])) {
        if (command) ds_diag_error(vm->diag, span, "failed to prepare command `%s`: %s", command, strerror(errno));
        else ds_diag_error(vm->diag, span, "failed to prepare pipeline exec error pipe: %s", strerror(errno));
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        pipe_fds[0] = -1;
        pipe_fds[1] = -1;
        return false;
    }
    return true;
}

static bool process_capture_open(Vm *vm, DsSpan span, const char *kind, FILE **out_fp, FILE **err_fp) {
    *out_fp = tmpfile();
    *err_fp = tmpfile();
    if (*out_fp && *err_fp) return true;
    ds_diag_error(vm->diag, span, "failed to create %s capture temporary files: %s", kind, strerror(errno));
    if (*out_fp) fclose(*out_fp);
    if (*err_fp) fclose(*err_fp);
    *out_fp = NULL;
    *err_fp = NULL;
    return false;
}

static bool process_capture_read(Vm *vm, DsSpan span, const char *kind, FILE *out_fp, FILE *err_fp,
                                 VmProcessResult *result) {
    if (vm_read_stream(out_fp, &result->stdout_text, true, false) == VM_READ_STREAM_OK &&
        vm_read_stream(err_fp, &result->stderr_text, true, false) == VM_READ_STREAM_OK) return true;
    ds_diag_error(vm->diag, span, "failed to read %s capture output", kind);
    return false;
}

static void process_capture_close(FILE **out_fp, FILE **err_fp) {
    if (*out_fp) fclose(*out_fp);
    if (*err_fp) fclose(*err_fp);
    *out_fp = NULL;
    *err_fp = NULL;
}

static void process_child_exec_argv(const VmProcessSpec *spec) {
    execvp(spec->argv.items[0], spec->argv.items);
    int exec_errno = errno;
    if (spec->exec_error_fd >= 0) {
        ssize_t ignored = write(spec->exec_error_fd, &exec_errno, sizeof(exec_errno));
        (void)ignored;
        close(spec->exec_error_fd);
    }
    if (spec->capture) fprintf(stderr, "ds: failed to launch command `%s`: %s\n", spec->argv.items[0], strerror(exec_errno));
    _exit(127);
}

/* -------------------------------------------------------------------------
 * Direct process execution
 * ------------------------------------------------------------------------- */

static void process_child_exec(const VmProcessSpec *spec, int redirect_fd, FILE *out_fp, FILE *err_fp) {
    if (spec->capture) {
        dup2(fileno(out_fp), STDOUT_FILENO);
        dup2(fileno(err_fp), STDERR_FILENO);
    } else if (spec->redirect.kind != DS_REDIRECT_NONE) {
        if (spec->redirect.kind == DS_REDIRECT_OUT || spec->redirect.kind == DS_REDIRECT_OUT_APPEND) dup2(redirect_fd, STDOUT_FILENO);
        else if (spec->redirect.kind == DS_REDIRECT_ERR || spec->redirect.kind == DS_REDIRECT_ERR_APPEND) dup2(redirect_fd, STDERR_FILENO);
        else { dup2(redirect_fd, STDOUT_FILENO); dup2(redirect_fd, STDERR_FILENO); }
    }
    if (redirect_fd >= 0) close(redirect_fd);
    process_child_exec_argv(spec);
}

static bool process_execute(Vm *vm, VmProcessSpec *spec, VmProcessResult *result) {
    process_result_init(result);
    int redirect_fd = -1;
    FILE *out_fp = NULL;
    FILE *err_fp = NULL;
    int exec_error_pipe[2] = {-1, -1};
    bool ok = false;
    spec->exec_error_fd = -1;

    trace_command_spec(vm, spec);

    if (!spec->capture && spec->redirect.kind != DS_REDIRECT_NONE) {
        if (!open_redirect_target(vm, &spec->redirect, &redirect_fd)) goto cleanup;
    }

    if (spec->capture) {
        if (!process_capture_open(vm, spec->span, "command", &out_fp, &err_fp)) goto cleanup;
    }

    if (!process_exec_error_pipe(vm, spec->span, spec->argv.items[0], exec_error_pipe)) goto cleanup;
    spec->exec_error_fd = exec_error_pipe[1];

    pid_t pid = fork();
    if (pid < 0) {
        ds_diag_error(vm->diag, spec->span, "failed to launch command `%s`: %s", spec->argv.items[0], strerror(errno));
        goto cleanup;
    }

    if (pid == 0) {
        setpgid(0, 0);
        close(exec_error_pipe[0]);
        process_child_exec(spec, redirect_fd, out_fp, err_fp);
    }
    setpgid(pid, pid);

    close(exec_error_pipe[1]); exec_error_pipe[1] = -1;
    spec->exec_error_fd = -1;
    if (redirect_fd >= 0) { close(redirect_fd); redirect_fd = -1; }
    int exec_errno = 0;
    ssize_t exec_error_len = read(exec_error_pipe[0], &exec_errno, sizeof(exec_errno));
    close(exec_error_pipe[0]); exec_error_pipe[0] = -1;
    int status = 0;
    if (!vm_wait_foreground_child(vm, pid, pid, spec, &status)) goto cleanup;
    result->code = process_status_code(status);
    result->terminated_by_sigpipe = process_status_is_signal(status, SIGPIPE);

    if (!spec->capture && exec_error_len == (ssize_t)sizeof(exec_errno)) {
        ds_diag_error(vm->diag, spec->span, "failed to launch command `%s`: %s", spec->argv.items[0], strerror(exec_errno));
        ok = true;
        goto cleanup;
    }

    if (spec->capture) {
        if (!process_capture_read(vm, spec->span, "command", out_fp, err_fp, result)) goto cleanup;
    }
    ok = true;

cleanup:
    if (exec_error_pipe[0] >= 0) close(exec_error_pipe[0]);
    if (exec_error_pipe[1] >= 0) close(exec_error_pipe[1]);
    if (redirect_fd >= 0) close(redirect_fd);
    process_capture_close(&out_fp, &err_fp);
    spec->exec_error_fd = -1;
    return ok;
}

/* -------------------------------------------------------------------------
 * Pipeline execution
 * ------------------------------------------------------------------------- */

static void close_pipe_array(int (*pipes)[2], size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (pipes[i][0] >= 0) close(pipes[i][0]);
        if (pipes[i][1] >= 0) close(pipes[i][1]);
        pipes[i][0] = pipes[i][1] = -1;
    }
}

static bool redirect_wants_stdout(DsRedirectKind kind) {
    return kind == DS_REDIRECT_OUT || kind == DS_REDIRECT_OUT_APPEND || kind == DS_REDIRECT_ALL || kind == DS_REDIRECT_ALL_APPEND;
}

static bool redirect_wants_stderr(DsRedirectKind kind) {
    return kind == DS_REDIRECT_ERR || kind == DS_REDIRECT_ERR_APPEND || kind == DS_REDIRECT_ALL || kind == DS_REDIRECT_ALL_APPEND;
}

static void pipeline_child_exec(VmProcessSpec *specs, size_t stage_count, size_t idx, int (*pipes)[2], int redirect_fd, const DsRedirect *pipeline_redirect, FILE *out_fp, FILE *err_fp) {
    VmProcessSpec *spec = &specs[idx];
    if (idx > 0) {
        dup2(pipes[idx - 1][0], STDIN_FILENO);
    } else if (isatty(STDIN_FILENO)) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
    }
    if (idx + 1 < stage_count) {
        dup2(pipes[idx][1], STDOUT_FILENO);
    } else if (spec->capture) {
        dup2(fileno(out_fp), STDOUT_FILENO);
    } else if (redirect_wants_stdout(pipeline_redirect->kind)) {
        dup2(redirect_fd, STDOUT_FILENO);
    }
    if (spec->capture) {
        dup2(fileno(err_fp), STDERR_FILENO);
    } else if (redirect_wants_stderr(pipeline_redirect->kind)) {
        dup2(redirect_fd, STDERR_FILENO);
    }
    close_pipe_array(pipes, stage_count > 0 ? stage_count - 1 : 0);
    if (redirect_fd >= 0) close(redirect_fd);
    process_child_exec_argv(spec);
}

static bool process_execute_pipeline(Vm *vm, Instr *ins, bool capture, VmProcessResult *result) {
    /*
     * Pipeline foreground-signal ownership mirrors direct commands: process
     * code owns process groups, waits, forwarding, and pipefail status; cleanup
     * handler legality and representation remain lowerer/HIR responsibilities.
     */
    process_result_init(result);
    size_t n = ins->stage_count ? ins->stage_count : 1;
    VmProcessSpec *specs = (VmProcessSpec *)ds_xcalloc(n, sizeof(VmProcessSpec));
    pid_t *pids = (pid_t *)ds_xcalloc(n, sizeof(pid_t));
    int *codes = (int *)ds_xcalloc(n, sizeof(int));
    int (*pipes)[2] = (int (*)[2])ds_xcalloc(n > 1 ? n - 1 : 1, sizeof(int[2]));
    int redirect_fd = -1;
    FILE *out_fp = NULL;
    FILE *err_fp = NULL;
    int exec_error_pipe[2] = {-1, -1};
    bool ok = true;

    for (size_t i = 0; i + 1 < n; i++) pipes[i][0] = pipes[i][1] = -1;

    for (size_t i = 0; i < n; i++) {
        if (!process_spec_from_stage(vm, ins, i, capture, &specs[i])) { ok = false; goto cleanup; }
        if (i + 1 == n) specs[i].redirect = ins->redirect;
        else ds_redirect_init(&specs[i].redirect);
        trace_command_spec(vm, &specs[i]);
    }

    if (!capture && ins->redirect.kind != DS_REDIRECT_NONE) {
        if (!open_redirect_target(vm, &ins->redirect, &redirect_fd)) { ok = false; goto cleanup; }
    }
    if (capture) {
        if (!process_capture_open(vm, ins->span, "pipeline", &out_fp, &err_fp)) { ok = false; goto cleanup; }
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if (pipe(pipes[i]) != 0) {
            ds_diag_error(vm->diag, ins->span, "failed to create pipeline pipe: %s", strerror(errno));
            ok = false;
            goto cleanup;
        }
    }
    if (!process_exec_error_pipe(vm, ins->span, NULL, exec_error_pipe)) { ok = false; goto cleanup; }
    for (size_t i = 0; i < n; i++) specs[i].exec_error_fd = exec_error_pipe[1];

    pid_t pgid = -1;
    for (size_t i = 0; i < n; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            ds_diag_error(vm->diag, ins->span, "failed to launch pipeline stage `%s`: %s", specs[i].argv.len ? specs[i].argv.items[0] : "<stage>", strerror(errno));
            ok = false;
            goto cleanup;
        }
        if (pids[i] == 0) {
            if (i == 0) setpgid(0, 0);
            else if (pgid > 0) setpgid(0, pgid);
            close(exec_error_pipe[0]);
            pipeline_child_exec(specs, n, i, pipes, redirect_fd, &ins->redirect, out_fp, err_fp);
        }
        if (i == 0) pgid = pids[i];
        if (pgid > 0) setpgid(pids[i], pgid);
    }

    close(exec_error_pipe[1]); exec_error_pipe[1] = -1;
    close_pipe_array(pipes, n > 1 ? n - 1 : 0);
    if (redirect_fd >= 0) { close(redirect_fd); redirect_fd = -1; }

    int exec_errno = 0;
    ssize_t exec_error_len = read(exec_error_pipe[0], &exec_errno, sizeof(exec_errno));
    close(exec_error_pipe[0]); exec_error_pipe[0] = -1;

    int tty_fd = -1;
    pid_t shell_pgid = -1;
    if (pgid > 0) vm_make_foreground_group(pgid, &tty_fd, &shell_pgid);
    for (size_t i = 0; i < n; i++) {
        int status = 0;
        const char *command = specs[i].argv.len ? specs[i].argv.items[0] : "<stage>";
        if (!vm_wait_child(vm, pids[i], pgid, ins->span, "pipeline stage", command, &status)) {
            vm_restore_terminal_pgrp(tty_fd, shell_pgid);
            ok = false;
            goto cleanup;
        }
        codes[i] = process_status_code(status);
        if (codes[i] != 0) {
            bool stage_sigpipe = process_status_is_signal(status, SIGPIPE);
            if (!stage_sigpipe) result->has_non_sigpipe_failure = true;
            if (i + 1 == n && stage_sigpipe) result->terminated_by_sigpipe = true;
        }
    }
    vm_restore_terminal_pgrp(tty_fd, shell_pgid);
    result->code = ds_command_pipeline_status(codes, n);
    if (!capture && exec_error_len == (ssize_t)sizeof(exec_errno)) {
        ds_diag_error(vm->diag, ins->span, "failed to launch pipeline command: %s", strerror(exec_errno));
    }
    if (capture) {
        if (!process_capture_read(vm, ins->span, "pipeline", out_fp, err_fp, result)) { ok = false; goto cleanup; }
    }

cleanup:
    if (exec_error_pipe[0] >= 0) close(exec_error_pipe[0]);
    if (exec_error_pipe[1] >= 0) close(exec_error_pipe[1]);
    close_pipe_array(pipes, n > 1 ? n - 1 : 0);
    if (redirect_fd >= 0) close(redirect_fd);
    if (!ok) {
        for (size_t i = 0; i < n; i++) if (pids[i] > 0) waitpid(pids[i], NULL, 0);
    }
    process_capture_close(&out_fp, &err_fp);
    for (size_t i = 0; i < n; i++) process_spec_free(&specs[i]);
    free(specs); free(pids); free(codes); free(pipes);
    return ok;
}

int run_command(Vm *vm, Instr *ins) {
    if (ins->word_count == 0) return 0;
    if ((ins->stage_count ? ins->stage_count : 1) > 1) {
        VmProcessResult result;
        bool ok = process_execute_pipeline(vm, ins, false, &result);
        int code = ok ? result.code : 1;
        if (ok && vm_pipeline_is_quiet_broken_pipe(ins, &result)) {
            vm->control_exit_requested = true;
            code = 0;
        }
        if (ok && code != 0 && !vm->diag->has_error && !vm_command_was_interrupted(vm, code)) ds_diag_error(vm->diag, ins->span, "pipeline failed with exit %d", code);
        process_result_free(&result);
        return code;
    }
    VmProcessSpec spec;
    if (!process_spec_from_instr(vm, ins, false, &spec)) return 1;
    int helper_code = 0;
    if (run_control_command(vm, &spec, &helper_code)) {
        process_spec_free(&spec);
        return helper_code;
    }
    VmProcessResult result;
    bool ok = process_execute(vm, &spec, &result);
    int code = ok ? result.code : 1;
    if (ok && vm_command_is_quiet_broken_pipe(&spec, &result)) {
        vm->control_exit_requested = true;
        code = 0;
    }
    if (ok && code != 0 && !vm->diag->has_error && !vm_command_was_interrupted(vm, code)) {
        ds_diag_error(vm->diag, ins->span, "command `%s` failed with exit %d", spec.argv.len > 0 ? spec.argv.items[0] : "<command>", code);
    }
    process_result_free(&result);
    process_spec_free(&spec);
    return code;
}

int run_capture(Vm *vm, Instr *ins, DsValue *out_value) {
    *out_value = ds_value_null();
    if (ins->word_count == 0) return 1;
    if ((ins->stage_count ? ins->stage_count : 1) > 1) {
        VmProcessResult result;
        bool ok = process_execute_pipeline(vm, ins, true, &result);
        if (!ok) { process_result_free(&result); return 1; }
        *out_value = ds_value_command_result_take(&result.stdout_text, &result.stderr_text, result.code);
        return 0;
    }
    VmProcessSpec spec;
    if (!process_spec_from_instr(vm, ins, true, &spec)) return 1;
    VmProcessResult result;
    bool ok = process_execute(vm, &spec, &result);
    process_spec_free(&spec);
    if (!ok) {
        process_result_free(&result);
        return 1;
    }
    *out_value = ds_value_command_result_take(&result.stdout_text, &result.stderr_text, result.code);
    return 0;
}
