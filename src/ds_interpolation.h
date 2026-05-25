#ifndef DS_INTERPOLATION_H
#define DS_INTERPOLATION_H

#include "ds_common.h"

#include <stdbool.h>

typedef enum {
    DS_INTERP_VALUE_BOOL,
    DS_INTERP_VALUE_INT,
    DS_INTERP_VALUE_STRING,
    DS_INTERP_VALUE_COMMAND_RESULT,
    DS_INTERP_VALUE_UNKNOWN
} DsInterpValueKind;

typedef enum {
    DS_INTERP_FORMAT_UPPER,
    DS_INTERP_FORMAT_LOWER,
    DS_INTERP_FORMAT_TRIM,
    DS_INTERP_FORMAT_ALIGN_LEFT,
    DS_INTERP_FORMAT_ALIGN_RIGHT,
    DS_INTERP_FORMAT_ALIGN_CENTER,
    DS_INTERP_FORMAT_INT_DECIMAL,
    DS_INTERP_FORMAT_INT_FIXED
} DsInterpFormatKind;

typedef struct {
    DsInterpFormatKind kind;
    int width;
    int precision;
    bool zero_pad;
} DsInterpFormatSpec;

/*
 * Shared interpolation-format contract. Lowering owns source acceptance and
 * type checks; VM/Bash use the same parser to render accepted specs and keep
 * any failure there as an internal post-lowering invariant.
 */
bool ds_interp_parse_format_spec(DsStr spec, DsInterpFormatSpec *out);
bool ds_interp_format_spec_supports_kind(const DsInterpFormatSpec *spec, DsInterpValueKind kind);
bool ds_interp_parse_format_spec_for_kind(DsStr spec, DsInterpValueKind kind, DsInterpFormatSpec *out);
const char *ds_interp_supported_format_specs(void);

#endif
