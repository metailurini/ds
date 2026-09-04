#include "lower_expr.h"
#include "lower_free.h"
#include "lower_kinds.h"
#include "lower_interp_segments.h"
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

static void interp_push_text(DsLowerExpr *out, DsStr text, DsSpan span) {
    if (text.len == 0) return;
    DsLowerExpr *part = expr_new(DS_LOWER_EXPR_INTERP_TEXT, span);
    part->as.text = ds_str_clone(text);
    DS_VEC_PUSH(&out->as.interp.parts, part, 8);
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

static void interp_diag_parse_status(Lower *lower, DsSpan span, LowerInterpParseStatus status) {
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
                          "unsupported string interpolation; expected a scalar expression such as `{name}`, `{name.field}`, `{name(args...)}`, or `{name.method(...)}`");
            return;
    }
}

static DsLowerExpr *lower_interpolated_expr(Lower *lower, const DsExpr *expr,
                                             LowerInterpSegments *segments,
                                             SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INTERP, expr->span);
    for (size_t i = 0; i < segments->len; i++) {
        LowerInterpSegment *segment = &segments->items[i];
        if (segment->kind == LOWER_INTERP_SEGMENT_TEXT) {
            interp_push_text(out, segment->text, expr->span);
            continue;
        }
        SymKind inner_kind = SYM_UNKNOWN;
        DsLowerExpr *part = interp_lower_value(lower, segment->expr, expr->span, &inner_kind);
        if (segment->has_format) {
            part = interp_apply_format(lower, part, inner_kind, segment->format, expr->span);
        }
        DS_VEC_PUSH(&out->as.interp.parts, part, 8);
    }
    *kind_out = SYM_STRING;
    return out;
}

DsLowerExpr *lower_string_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    bool has_brace_syntax = memchr(expr->as.text.data, '{', expr->as.text.len) != NULL ||
                            memchr(expr->as.text.data, '}', expr->as.text.len) != NULL;
    if (has_brace_syntax) {
        LowerInterpSegments segments;
        LowerInterpParseStatus status = lower_interp_parse_segments(expr->as.text, expr->span, &segments);
        if (status == LOWER_INTERP_PARSE_OK) {
            DsLowerExpr *out = lower_interpolated_expr(lower, expr, &segments, kind_out);
            lower_interp_segments_free(&segments);
            return out;
        }
        interp_diag_parse_status(lower, expr->span, status);
        lower_interp_segments_free(&segments);
        *kind_out = SYM_STRING;
        return expr_new(DS_LOWER_EXPR_INTERP, expr->span);
    }
    *kind_out = SYM_STRING;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_STRING, expr->span);
    out->as.text = ds_str_clone(expr->as.text);
    return out;
}
