#include "lower_interp_segments.h"
#include "ds_runtime.h"
#include "lower_interp_parser.h"

static void literal_clear(DsString *literal) {
    literal->len = 0;
    if (literal->data) literal->data[0] = '\0';
}

static void push_text(LowerInterpSegments *segments, DsString *literal) {
    if (!literal || literal->len == 0) return;
    LowerInterpSegment segment = {0};
    segment.kind = LOWER_INTERP_SEGMENT_TEXT;
    segment.text = (DsStr){ds_str_dup_range(literal->data, literal->len), literal->len};
    DS_VEC_PUSH(segments, segment, 8);
    literal_clear(literal);
}

void lower_interp_segments_init(LowerInterpSegments *segments) {
    if (!segments) return;
    *segments = (LowerInterpSegments){0};
}

void lower_interp_segments_free(LowerInterpSegments *segments) {
    if (!segments) return;
    for (size_t i = 0; i < segments->len; i++) {
        LowerInterpSegment *segment = &segments->items[i];
        free(segment->text.data);
        free(segment->format.data);
        ds_expr_free(segment->expr);
    }
    free(segments->items);
    *segments = (LowerInterpSegments){0};
}

LowerInterpParseStatus lower_interp_parse_segments(DsStr quoted_text, DsSpan span,
                                                    LowerInterpSegments *segments) {
    lower_interp_segments_init(segments);
    DsStr decoded = {0};
    if (!ds_decode_string_text(quoted_text, &decoded)) return LOWER_INTERP_PARSE_DECODE_ERROR;

    DsString literal;
    ds_string_init(&literal);
    LowerInterpParseStatus status = LOWER_INTERP_PARSE_OK;

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
            status = LOWER_INTERP_PARSE_UNMATCHED_CLOSE;
            break;
        }
        if (c != '{') {
            ds_string_append_char(&literal, c);
            i++;
            continue;
        }

        push_text(segments, &literal);
        size_t raw_start = i + 1;
        size_t cursor = raw_start;
        ds_skip_ascii_ws(decoded.data, decoded.len, &cursor);
        bool leading_ws = cursor != raw_start;
        DsExpr *inner = lower_interp_parse_expr(decoded, cursor, span, &cursor);
        size_t raw_end = cursor;
        ds_skip_ascii_ws(decoded.data, decoded.len, &cursor);
        bool trailing_ws = cursor != raw_end;
        if (!inner) {
            status = cursor >= decoded.len ? LOWER_INTERP_PARSE_UNCLOSED
                                           : LOWER_INTERP_PARSE_INVALID_EXPR;
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
            status = LOWER_INTERP_PARSE_UNCLOSED;
            break;
        }
        if (decoded.data[cursor] != '}') {
            ds_expr_free(inner);
            status = LOWER_INTERP_PARSE_INVALID_EXPR;
            break;
        }

        bool arithmetic_syntax = false;
        for (size_t j = raw_start; j < raw_end; j++) {
            char op = decoded.data[j];
            if (op == '+' || op == '-' || op == '*' || op == '/' || op == '%') {
                arithmetic_syntax = true;
                break;
            }
        }

        LowerInterpSegment segment = {0};
        segment.kind = LOWER_INTERP_SEGMENT_EXPR;
        segment.expr = inner;
        segment.leading_ws = leading_ws;
        segment.trailing_ws = trailing_ws;
        segment.arithmetic_syntax = arithmetic_syntax;
        segment.has_format = has_format;
        if (has_format) {
            segment.format.len = cursor - spec_start;
            segment.format.data = ds_str_dup_range(decoded.data + spec_start, segment.format.len);
        }
        DS_VEC_PUSH(segments, segment, 8);
        i = cursor + 1;
    }

    if (status == LOWER_INTERP_PARSE_OK) push_text(segments, &literal);
    ds_string_free(&literal);
    free(decoded.data);
    if (status != LOWER_INTERP_PARSE_OK) lower_interp_segments_free(segments);
    return status;
}
