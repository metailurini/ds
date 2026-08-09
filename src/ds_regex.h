#ifndef DS_REGEX_H
#define DS_REGEX_H

#include "ds_common.h"
#include "ds_runtime.h"

typedef enum {
    DS_REGEX_OK,
    DS_REGEX_ERR_LITERAL_SYNTAX,
    DS_REGEX_ERR_UNSUPPORTED_ESCAPE,
    DS_REGEX_ERR_UNSUPPORTED_GROUP,
    DS_REGEX_ERR_LAZY_QUANTIFIER,
    DS_REGEX_ERR_INVALID_PATTERN,
    DS_REGEX_ERR_TOO_MANY_CAPTURES,
    DS_REGEX_ERR_INVALID_FLAGS,
    DS_REGEX_ERR_INVALID_REPLACEMENT,
    DS_REGEX_ERR_UNKNOWN_CAPTURE
} DsRegexStatus;

const char *ds_regex_status_message(DsRegexStatus status);
bool ds_regex_literal_parts(DsStr lit, DsStr *pattern, bool *insensitive);
void ds_regex_decode_literal_pattern(DsStr pattern, DsString *out);
DsRegexStatus ds_regex_validate_flags(DsStr flags, int *cflags_out);
DsRegexStatus ds_regex_validate_pattern(DsStr pattern, size_t *capture_count_out);
DsRegexStatus ds_regex_validate_replacement(DsStr replacement, size_t capture_count, bool capture_count_known);

#endif
