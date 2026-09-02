#ifndef DS_LOWER_INTERP_SEGMENTS_H
#define DS_LOWER_INTERP_SEGMENTS_H

#include "ds_ast.h"

typedef enum {
    LOWER_INTERP_SEGMENT_TEXT,
    LOWER_INTERP_SEGMENT_EXPR
} LowerInterpSegmentKind;

typedef struct {
    LowerInterpSegmentKind kind;
    DsStr text;
    DsExpr *expr;
    bool leading_ws;
    bool trailing_ws;
    bool arithmetic_syntax;
    bool has_format;
    DsStr format;
} LowerInterpSegment;

typedef struct {
    LowerInterpSegment *items;
    size_t len;
    size_t cap;
} LowerInterpSegments;

typedef enum {
    LOWER_INTERP_PARSE_OK,
    LOWER_INTERP_PARSE_DECODE_ERROR,
    LOWER_INTERP_PARSE_UNMATCHED_CLOSE,
    LOWER_INTERP_PARSE_UNCLOSED,
    LOWER_INTERP_PARSE_INVALID_EXPR
} LowerInterpParseStatus;

void lower_interp_segments_init(LowerInterpSegments *segments);
void lower_interp_segments_free(LowerInterpSegments *segments);
LowerInterpParseStatus lower_interp_parse_segments(DsStr quoted_text, DsSpan span,
                                                    LowerInterpSegments *segments);

#endif
