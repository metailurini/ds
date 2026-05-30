#define _POSIX_C_SOURCE 200809L

#include "ds_regex.h"

#include <regex.h>
#include <stdlib.h>
#include <string.h>

const char *ds_regex_status_message(DsRegexStatus status) {
    switch (status) {
        case DS_REGEX_OK:
            return "regex pattern is valid";
        case DS_REGEX_ERR_LITERAL_SYNTAX:
            return "invalid regex literal; v0.32.0 supports `/pattern/` and `/pattern/i`";
        case DS_REGEX_ERR_UNSUPPORTED_ESCAPE:
            return "unsupported regex escape in v0.32.0";
        case DS_REGEX_ERR_UNSUPPORTED_GROUP:
            return "lookaround, inline flags, named captures, and non-POSIX regex groups are deferred in v0.32.0";
        case DS_REGEX_ERR_LAZY_QUANTIFIER:
            return "lazy regex quantifiers are deferred in v0.32.0";
        case DS_REGEX_ERR_INVALID_PATTERN:
            return "invalid regex pattern in v0.32.0";
        case DS_REGEX_ERR_TOO_MANY_CAPTURES:
            return "regex patterns with more than nine capture groups are unsupported in v0.32.0";
        case DS_REGEX_ERR_INVALID_FLAGS:
            return "regex flags must be either an empty string or `i` in v0.32.0";
        case DS_REGEX_ERR_INVALID_REPLACEMENT:
            return "regex replacement supports only `$0` through `$9` and `$$` in v0.32.0";
        case DS_REGEX_ERR_UNKNOWN_CAPTURE:
            return "regex replacement references a capture group that cannot exist in v0.32.0";
    }
    return "invalid regex in v0.32.0";
}

bool ds_regex_literal_parts(DsStr lit, DsStr *pattern, bool *insensitive) {
    if (pattern) *pattern = (DsStr){0};
    if (insensitive) *insensitive = false;
    if (lit.len < 3 || lit.data[0] != '/') return false;
    size_t end = 0;
    for (size_t i = 1; i < lit.len; i++) {
        if (lit.data[i] == '\\') {
            i++;
            continue;
        }
        if (lit.data[i] == '/') {
            end = i;
            break;
        }
    }
    if (!end) return false;
    if (pattern) {
        pattern->data = lit.data + 1;
        pattern->len = end - 1;
    }
    if (end + 1 < lit.len) {
        if (end + 2 == lit.len && lit.data[end + 1] == 'i') {
            if (insensitive) *insensitive = true;
        } else {
            return false;
        }
    }
    return true;
}

bool ds_regex_decode_literal_pattern(DsStr pattern, DsString *out) {
    ds_string_init(out);
    for (size_t i = 0; i < pattern.len; i++) {
        char c = pattern.data[i];
        if (c == '\\' && i + 1 < pattern.len && pattern.data[i + 1] == '/') {
            if (!ds_string_append_char(out, '/')) return false;
            i++;
        } else if (!ds_string_append_char(out, c)) {
            return false;
        }
    }
    return true;
}

DsRegexStatus ds_regex_validate_flags(DsStr flags, int *cflags_out) {
    int cflags = REG_EXTENDED;
    if (flags.len == 0) {
        if (cflags_out) *cflags_out = cflags;
        return DS_REGEX_OK;
    }
    if (flags.len == 1 && flags.data[0] == 'i') {
        if (cflags_out) *cflags_out = cflags | REG_ICASE;
        return DS_REGEX_OK;
    }
    return DS_REGEX_ERR_INVALID_FLAGS;
}

static bool is_unsupported_escape(char c) {
    return c == 'p' || c == 'P' || c == 'b' || c == 'd' || c == 'D' ||
           c == 's' || c == 'S' || c == 'w' || c == 'W' ||
           (c >= '1' && c <= '9');
}

DsRegexStatus ds_regex_validate_pattern(DsStr pattern, size_t *capture_count_out) {
    size_t captures = 0;
    bool in_class = false;
    for (size_t i = 0; i < pattern.len; i++) {
        char c = pattern.data[i];
        if (c == '\\') {
            if (i + 1 < pattern.len) {
                if (is_unsupported_escape(pattern.data[i + 1])) return DS_REGEX_ERR_UNSUPPORTED_ESCAPE;
                i++;
            }
            continue;
        }
        if (c == '[') {
            in_class = true;
            continue;
        }
        if (c == ']' && in_class) {
            in_class = false;
            continue;
        }
        if (in_class) continue;
        if (c == '(') {
            if (i + 1 < pattern.len && pattern.data[i + 1] == '?') return DS_REGEX_ERR_UNSUPPORTED_GROUP;
            captures++;
        }
        if ((c == '*' || c == '+' || c == '?' || c == '}') && i + 1 < pattern.len && pattern.data[i + 1] == '?') {
            return DS_REGEX_ERR_LAZY_QUANTIFIER;
        }
    }
    if (capture_count_out) *capture_count_out = captures;
    if (captures > 9) return DS_REGEX_ERR_TOO_MANY_CAPTURES;

    char *tmp = ds_str_dup_range(pattern.data ? pattern.data : "", pattern.len);
    regex_t re;
    int err = regcomp(&re, tmp, REG_EXTENDED);
    free(tmp);
    if (err != 0) return DS_REGEX_ERR_INVALID_PATTERN;
    regfree(&re);
    return DS_REGEX_OK;
}

DsRegexStatus ds_regex_validate_replacement(DsStr replacement, size_t capture_count, bool capture_count_known) {
    for (size_t i = 0; i < replacement.len; i++) {
        if (replacement.data[i] != '$') continue;
        if (i + 1 >= replacement.len) return DS_REGEX_ERR_INVALID_REPLACEMENT;
        char n = replacement.data[++i];
        if (n == '$') continue;
        if (n < '0' || n > '9') return DS_REGEX_ERR_INVALID_REPLACEMENT;
        size_t ref = (size_t)(n - '0');
        if (capture_count_known && ref > capture_count) return DS_REGEX_ERR_UNKNOWN_CAPTURE;
    }
    return DS_REGEX_OK;
}

bool ds_regex_replacement_refs_capture(DsStr replacement, size_t capture_index) {
    if (capture_index > 9) return false;
    char want = (char)('0' + capture_index);
    for (size_t i = 0; i + 1 < replacement.len; i++) {
        if (replacement.data[i] == '$') {
            if (replacement.data[i + 1] == want) return true;
            i++;
        }
    }
    return false;
}
