#include "vm_internal.h"
#include "ds_interpolation.h"

#include <stdio.h>
#include <string.h>

/* VM rendering for interpolation formats already validated and normalized by lowering. */

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

static void append_padded(DsString *out, const char *data, size_t len, int width, char align) {
    if (width <= (int)len) {
        ds_string_append_range(out, data, len);
        return;
    }
    int pad = width - (int)len;
    int left = 0, right = 0;
    if (align == '<') right = pad;
    else if (align == '>') left = pad;
    else { left = pad / 2; right = pad - left; }
    for (int i = 0; i < left; i++) ds_string_append_char(out, ' ');
    ds_string_append_range(out, data, len);
    for (int i = 0; i < right; i++) ds_string_append_char(out, ' ');
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

static bool append_parsed_formatted_value(Vm *vm, DsValue *value,
                                          const DsInterpFormatSpec *parsed,
                                          DsString *out, DsSpan span) {
    if (!ds_interp_format_spec_supports_kind(parsed, interp_kind_from_value(value))) {
        ds_diag_error(vm->diag, span,
                      "internal VM interpolation invariant failed: value kind does not match validated format");
        return false;
    }
    if (parsed->kind == DS_INTERP_FORMAT_UPPER || parsed->kind == DS_INTERP_FORMAT_LOWER ||
        parsed->kind == DS_INTERP_FORMAT_TRIM) {
        DsString rendered;
        ascii_transform_string(&value->as.string, &rendered, parsed->kind);
        ds_string_append_range(out, ds_string_data(&rendered), rendered.len);
        ds_string_free(&rendered);
        return true;
    }
    if (parsed->kind == DS_INTERP_FORMAT_ALIGN_LEFT || parsed->kind == DS_INTERP_FORMAT_ALIGN_RIGHT ||
        parsed->kind == DS_INTERP_FORMAT_ALIGN_CENTER) {
        char align = parsed->kind == DS_INTERP_FORMAT_ALIGN_LEFT ? '<' :
                     parsed->kind == DS_INTERP_FORMAT_ALIGN_RIGHT ? '>' : '^';
        append_padded(out, ds_string_data(&value->as.string), value->as.string.len,
                      parsed->width, align);
        return true;
    }
    char buf[64];
    if (parsed->kind == DS_INTERP_FORMAT_INT_DECIMAL) {
        snprintf(buf, sizeof(buf), "%lld", (long long)value->as.integer);
        size_t len = strlen(buf);
        if (parsed->width <= (int)len) {
            ds_string_append_cstr(out, buf);
            return true;
        }
        int pad = parsed->width - (int)len;
        if (parsed->zero_pad) {
            if (buf[0] == '-') {
                ds_string_append_char(out, '-');
                for (int i = 0; i < pad; i++) ds_string_append_char(out, '0');
                ds_string_append_cstr(out, buf + 1);
                return true;
            }
            for (int i = 0; i < pad; i++) ds_string_append_char(out, '0');
            ds_string_append_cstr(out, buf);
            return true;
        }
        for (int i = 0; i < pad; i++) ds_string_append_char(out, ' ');
        ds_string_append_cstr(out, buf);
        return true;
    }
    int prec = parsed->precision < 0 ? 6 : parsed->precision;
    DsString tmp;
    ds_string_init(&tmp);
    ds_string_appendf(&tmp, "%lld.", (long long)value->as.integer);
    for (int i = 0; i < prec; i++) ds_string_append_char(&tmp, '0');
    if (parsed->width > (int)tmp.len) append_padded(out, tmp.data, tmp.len, parsed->width, '>');
    else ds_string_append_range(out, tmp.data, tmp.len);
    ds_string_free(&tmp);
    return true;
}

bool vm_format_interpolation_value(Vm *vm, DsValue *value,
                                   const DsInterpFormatSpec *spec,
                                   DsString *out, DsSpan span) {
    ds_string_init(out);
    return append_parsed_formatted_value(vm, value, spec, out, span);
}
