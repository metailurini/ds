#include "lower_internal.h"
#include "lower_interp_parser.h"
#include "ds_interpolation.h"

static DsInterpValueKind interp_value_kind_from_sym(SymKind kind) {
    switch (kind) {
        case SYM_BOOL: return DS_INTERP_VALUE_BOOL;
        case SYM_INT: return DS_INTERP_VALUE_INT;
        case SYM_STRING: return DS_INTERP_VALUE_STRING;
        case SYM_COMMAND_RESULT: return DS_INTERP_VALUE_COMMAND_RESULT;
        case SYM_ARRAY:
        case SYM_MAP:
        case SYM_FUNCTION:
        case SYM_TOPLEVEL_PREDECLARED:
        case SYM_UNKNOWN:
            return DS_INTERP_VALUE_UNKNOWN;
    }
    return DS_INTERP_VALUE_UNKNOWN;
}

static void interp_literal_clear(DsString *literal) {
    literal->len = 0;
    if (literal->data) literal->data[0] = '\0';
}

static void interp_push_text(DsLowerExpr *out, DsString *literal, DsSpan span) {
    if (!literal || literal->len == 0) return;
    DsLowerExpr *part = expr_new(DS_LOWER_EXPR_INTERP_TEXT, span);
    part->as.text = (DsStr){ds_str_dup_range(literal->data, literal->len), literal->len};
    DS_VEC_PUSH(&out->as.interp.parts, part, 8);
    interp_literal_clear(literal);
}

static DsLowerExpr *interp_lower_value(Lower *lower, const DsExpr *inner, DsSpan span,
                                       SymKind *kind_out) {
    SymKind inner_kind = SYM_UNKNOWN;
    DsLowerExpr *value = lower_expr(lower, inner, &inner_kind);
    if (!lower_sym_kind_is_scalar(inner_kind) && inner_kind != SYM_UNKNOWN) {
        ds_diag_error(lower->diag, span, "interpolation expression must be scalar in v0.21.0");
    }
    if (kind_out) *kind_out = inner_kind;
    return value;
}

static DsLowerExpr *interp_apply_format(Lower *lower, DsLowerExpr *value, SymKind value_kind,
                                        DsStr spec_text, DsSpan span) {
    DsInterpFormatSpec spec;
    if (!ds_interp_parse_format_spec_for_kind(spec_text, interp_value_kind_from_sym(value_kind), &spec)) {
        ds_diag_error(lower->diag, span,
                      "unsupported interpolation format specifier `%.*s`; supported: %s",
                      (int)spec_text.len, spec_text.data, ds_interp_supported_format_specs());
        return value;
    }
    DsLowerExpr *formatted = expr_new(DS_LOWER_EXPR_INTERP_FORMAT, span);
    formatted->as.interp_format.value = value;
    formatted->as.interp_format.spec = spec;
    return formatted;
}

static DsLowerExpr *lower_interpolated_expr(Lower *lower, const DsExpr *expr,
                                             DsStr decoded, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INTERP, expr->span);
    DsString literal;
    ds_string_init(&literal);

    for (size_t i = 0; i < decoded.len;) {
        char c = decoded.data[i];
        if (c == '{' && i + 1 < decoded.len && decoded.data[i + 1] == '{') {
            ds_string_append_char(&literal, '{');
            i += 2;
            continue;
        }
        if (c == '}' && i + 1 < decoded.len && decoded.data[i + 1] == '}') {
            ds_string_append_char(&literal, '}');
            i += 2;
            continue;
        }
        if (c == '}') {
            ds_diag_error(lower->diag, expr->span,
                          "unmatched `}` in string interpolation; use `}}` for a literal `}`");
            break;
        }
        if (c != '{') {
            ds_string_append_char(&literal, c);
            i++;
            continue;
        }

        interp_push_text(out, &literal, expr->span);
        size_t cursor = i + 1;
        ds_skip_ascii_ws(decoded.data, decoded.len, &cursor);
        DsExpr *inner = lower_interp_parse_expr(decoded, cursor, expr->span, &cursor);
        ds_skip_ascii_ws(decoded.data, decoded.len, &cursor);
        if (!inner) {
            ds_diag_error(lower->diag, expr->span,
                          "unsupported string interpolation; expected a scalar expression such as `{name}`, `{name.field}`, `{name(args...)}`, or `{name.method(...)}`");
            break;
        }

        bool has_format = cursor < decoded.len && decoded.data[cursor] == ':';
        size_t spec_start = 0;
        if (has_format) {
            spec_start = ++cursor;
            while (cursor < decoded.len && decoded.data[cursor] != '}') cursor++;
        }
        if (cursor >= decoded.len) {
            ds_expr_free(inner);
            ds_diag_error(lower->diag, expr->span,
                          "unclosed interpolation in string; expected `}`");
            break;
        }
        if (decoded.data[cursor] != '}') {
            ds_expr_free(inner);
            ds_diag_error(lower->diag, expr->span,
                          "unsupported string interpolation; expected a scalar expression such as `{name}`, `{name.field}`, `{name(args...)}`, or `{name.method(...)}`");
            break;
        }

        SymKind inner_kind = SYM_UNKNOWN;
        DsLowerExpr *part = interp_lower_value(lower, inner, expr->span, &inner_kind);
        ds_expr_free(inner);
        if (has_format) {
            DsStr spec_text = {decoded.data + spec_start, cursor - spec_start};
            part = interp_apply_format(lower, part, inner_kind, spec_text, expr->span);
        }

        DS_VEC_PUSH(&out->as.interp.parts, part, 8);
        i = cursor + 1;
    }

    interp_push_text(out, &literal, expr->span);
    ds_string_free(&literal);
    *kind_out = SYM_STRING;
    return out;
}

DsLowerExpr *lower_string_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    DsStr decoded = {0};
    if (ds_decode_string_text(expr->as.text, &decoded)) {
        bool has_brace_syntax = memchr(decoded.data, '{', decoded.len) != NULL ||
                                memchr(decoded.data, '}', decoded.len) != NULL;
        if (has_brace_syntax) {
            DsLowerExpr *out = lower_interpolated_expr(lower, expr, decoded, kind_out);
            free(decoded.data);
            return out;
        }
        free(decoded.data);
    }
    *kind_out = SYM_STRING;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_STRING, expr->span);
    out->as.text = ds_str_clone(expr->as.text);
    return out;
}
