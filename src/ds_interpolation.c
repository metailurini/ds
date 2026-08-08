#include "ds_interpolation.h"

static bool parse_format_limit(DsStr spec, size_t start, size_t end, int *value) {
    if (start >= end) return false;
    return ds_parse_int_range((DsStr){spec.data + start, end - start}, 1, 1024, value);
}

bool ds_interp_parse_format_spec(DsStr spec, DsInterpFormatSpec *out) {
    if (!out || spec.len == 0) return false;
    memset(out, 0, sizeof(*out));
    out->precision = -1;

    if (ds_str_eq_cstr(spec, "upper")) {
        out->kind = DS_INTERP_FORMAT_UPPER;
        return true;
    }
    if (ds_str_eq_cstr(spec, "lower")) {
        out->kind = DS_INTERP_FORMAT_LOWER;
        return true;
    }
    if (ds_str_eq_cstr(spec, "trim")) {
        out->kind = DS_INTERP_FORMAT_TRIM;
        return true;
    }
    if (spec.data[0] == '<' || spec.data[0] == '>' || spec.data[0] == '^') {
        if (!parse_format_limit(spec, 1, spec.len, &out->width)) return false;
        out->kind = spec.data[0] == '<' ? DS_INTERP_FORMAT_ALIGN_LEFT :
                    spec.data[0] == '>' ? DS_INTERP_FORMAT_ALIGN_RIGHT :
                                           DS_INTERP_FORMAT_ALIGN_CENTER;
        return true;
    }

    size_t i = 0;
    bool zero = false;
    if (i < spec.len && spec.data[i] == '0') {
        zero = true;
        i++;
    }
    size_t digits_start = i;
    while (i < spec.len && spec.data[i] >= '0' && spec.data[i] <= '9') i++;
    if (i < spec.len && spec.data[i] == 'd') {
        if (i + 1 != spec.len || !parse_format_limit(spec, digits_start, i, &out->width)) return false;
        out->kind = DS_INTERP_FORMAT_INT_DECIMAL;
        out->zero_pad = zero;
        return true;
    }
    if (zero) return false;
    if (i < spec.len && spec.data[i] == '.') {
        if (i > digits_start && !parse_format_limit(spec, digits_start, i, &out->width)) return false;
        size_t prec_start = ++i;
        while (i < spec.len && spec.data[i] >= '0' && spec.data[i] <= '9') i++;
        if (i >= spec.len || spec.data[i] != 'f' || i + 1 != spec.len) return false;
        if (!parse_format_limit(spec, prec_start, i, &out->precision)) return false;
        out->kind = DS_INTERP_FORMAT_INT_FIXED;
        out->zero_pad = zero;
        return true;
    }
    return false;
}

bool ds_interp_format_spec_supports_kind(const DsInterpFormatSpec *spec, DsInterpValueKind kind) {
    if (!spec) return false;
    switch (spec->kind) {
        case DS_INTERP_FORMAT_UPPER:
        case DS_INTERP_FORMAT_LOWER:
        case DS_INTERP_FORMAT_TRIM:
        case DS_INTERP_FORMAT_ALIGN_LEFT:
        case DS_INTERP_FORMAT_ALIGN_RIGHT:
        case DS_INTERP_FORMAT_ALIGN_CENTER:
            return kind == DS_INTERP_VALUE_STRING;
        case DS_INTERP_FORMAT_INT_DECIMAL:
        case DS_INTERP_FORMAT_INT_FIXED:
            return kind == DS_INTERP_VALUE_INT;
    }
    return false;
}

bool ds_interp_parse_format_spec_for_kind(DsStr spec, DsInterpValueKind kind, DsInterpFormatSpec *out) {
    DsInterpFormatSpec parsed;
    if (!ds_interp_parse_format_spec(spec, &parsed)) return false;
    if (!ds_interp_format_spec_supports_kind(&parsed, kind)) return false;
    if (out) *out = parsed;
    return true;
}

const char *ds_interp_supported_format_specs(void) {
    return "upper, lower, trim, <N, >N, ^N, Nd, 0Nd, .Pf, N.Pf";
}
