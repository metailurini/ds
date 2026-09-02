#include "lower_internal.h"
#include "lower_interp_parser.h"

static DsStr quoted_string_from_decoded(const char *data, size_t len) {
    DsString quoted = {0};
    ds_string_append_char(&quoted, '"');
    ds_string_append_escaped(&quoted, data, len);
    ds_string_append_char(&quoted, '"');
    return (DsStr){quoted.data, quoted.len};
}

static void interp_push_literal(DsLowerExpr *out, const char *data, size_t len, DsSpan span) {
    if (len == 0) return;
    DsLowerExpr *part = expr_new(DS_LOWER_EXPR_STRING, span);
    part->as.text = quoted_string_from_decoded(data, len);
    DS_VEC_PUSH(&out->as.interp.parts, part, 8);
}

static DsLowerExpr *lower_interpolated_expr(Lower *lower, const DsExpr *expr, DsStr decoded, SymKind *kind_out) {
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INTERP, expr->span);
    size_t literal_start = 0;
    for (size_t i = 0; i < decoded.len; i++) {
        if (decoded.data[i] == '{' && i + 1 < decoded.len && decoded.data[i + 1] == '{') { i++; continue; }
        if (decoded.data[i] == '}' && i + 1 < decoded.len && decoded.data[i + 1] == '}') { i++; continue; }
        if (decoded.data[i] == '}') {
            ds_diag_error(lower->diag, expr->span, "unmatched `}` in string interpolation; use `}}` for a literal `}`");
            break;
        }
        if (decoded.data[i] != '{') continue;
        size_t j = i + 1;
        ds_skip_ascii_ws(decoded.data, decoded.len, &j);
        DsExpr *inner = lower_interp_parse_expr(decoded, j, expr->span, &j);
        ds_skip_ascii_ws(decoded.data, decoded.len, &j);
        if (inner && j < decoded.len && decoded.data[j] == '}') {
            interp_push_literal(out, decoded.data + literal_start, i - literal_start, expr->span);
            SymKind inner_kind = SYM_UNKNOWN;
            DsLowerExpr *part = lower_expr(lower, inner, &inner_kind);
            if (!lower_sym_kind_is_scalar(inner_kind) && inner_kind != SYM_UNKNOWN) {
                ds_diag_error(lower->diag, expr->span, "interpolation expression must be scalar in v0.21.0");
            }
            DS_VEC_PUSH(&out->as.interp.parts, part, 8);
            ds_expr_free(inner);
            i = j;
            literal_start = j + 1;
            continue;
        }
        ds_expr_free(inner);
        ds_diag_error(lower->diag, expr->span, "unsupported string interpolation; expected a scalar expression such as `{name}`, `{name.field}`, `{name(args...)}`, or `{name.method(...)}`");
        break;
    }
    interp_push_literal(out, decoded.data + literal_start, decoded.len - literal_start, expr->span);
    *kind_out = SYM_STRING;
    return out;
}

DsLowerExpr *lower_string_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    DsStr decoded = {0};
    if (ds_decode_string_text(expr->as.text, &decoded)) {
        if (lower_interp_text_needs_expr_parser(decoded)) {
            DsLowerExpr *out = lower_interpolated_expr(lower, expr, decoded, kind_out);
            free(decoded.data);
            return out;
        }
        free(decoded.data);
    }
    lower_validate_word_interpolation(lower, expr->as.text, expr->span);
    *kind_out = SYM_STRING;
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_STRING, expr->span);
    out->as.text = ds_str_clone(expr->as.text);
    return out;
}
