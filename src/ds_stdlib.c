#include "ds_stdlib.h"

static bool str_has_prefix(DsStr a, const char *prefix) {
    size_t len = strlen(prefix);
    return a.len >= len && memcmp(a.data, prefix, len) == 0;
}

static const DsStdlibHelper HELPERS[] = {
    {"file.exists", "__ds_stdlib_file_exists", 1, 1, DS_STDLIB_RETURN_BOOL, false, true, false, false, false, 0, 0},
    {"file.is_file", "__ds_stdlib_file_is_file", 1, 1, DS_STDLIB_RETURN_BOOL, false, true, false, false, false, 0, 0},
    {"file.read", "__ds_stdlib_file_read", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false, 0, 0},
    {"file.write", "__ds_stdlib_file_write", 2, 2, DS_STDLIB_RETURN_STATEMENT_ONLY, true, true, false, false, false, 0, 0},
    {"file.append", "__ds_stdlib_file_append", 2, 2, DS_STDLIB_RETURN_STATEMENT_ONLY, true, true, false, false, false, 0, 0},
    {"dir.exists", "__ds_stdlib_dir_exists", 1, 1, DS_STDLIB_RETURN_BOOL, false, true, false, false, false, 0, 0},
    {"dir.walk", "__ds_stdlib_dir_walk", 1, 1, DS_STDLIB_RETURN_ARRAY, false, false, true, false, false, DS_STDLIB_HELPER_DIR_WALK, 0},
    {"dir.walk!", "__ds_stdlib_dir_walk_required", 1, 1, DS_STDLIB_RETURN_ARRAY, false, false, true, false, false, DS_STDLIB_HELPER_DIR_WALK, 0},
    {"dir.walk_ext", "__ds_stdlib_dir_walk_ext", 2, 2, DS_STDLIB_RETURN_ARRAY, false, false, true, false, false, DS_STDLIB_HELPER_DIR_WALK | DS_STDLIB_HELPER_DIR_WALK_EXT, 0},
    {"dir.walk_ext!", "__ds_stdlib_dir_walk_ext_required", 2, 2, DS_STDLIB_RETURN_ARRAY, false, false, true, false, false, DS_STDLIB_HELPER_DIR_WALK | DS_STDLIB_HELPER_DIR_WALK_EXT, 0},
    {"path.cwd", "__ds_stdlib_path_cwd", 0, 0, DS_STDLIB_RETURN_STRING, false, true, false, false, false, 0, 0},
    {"path.join", "__ds_stdlib_path_join", 1, (size_t)-1, DS_STDLIB_RETURN_STRING, false, true, false, false, false, 0, 0},
    {"path.basename", "__ds_stdlib_path_basename", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false, 0, 0},
    {"path.dirname", "__ds_stdlib_path_dirname", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false, 0, 0},
    {"path.ext", "__ds_stdlib_path_ext", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false, 0, 0},
    {"cmd.exists", "__ds_stdlib_cmd_exists", 1, 1, DS_STDLIB_RETURN_BOOL, false, true, false, false, false, 0, 0},
    {"cmd.require", "__ds_stdlib_cmd_require", 1, 1, DS_STDLIB_RETURN_STATEMENT_ONLY, true, true, false, false, false, 0, 0},
    {"env.get", "__ds_stdlib_env_get", 1, 2, DS_STDLIB_RETURN_STRING, false, true, false, true, false, 0, 0},
    {"env.set", "__ds_stdlib_env_set", 2, 2, DS_STDLIB_RETURN_STATEMENT_ONLY, true, true, false, true, false, 0, 0},
    {"env.unset", "__ds_stdlib_env_unset", 1, 1, DS_STDLIB_RETURN_STATEMENT_ONLY, true, true, false, true, false, 0, 0},
    {"glob", "__ds_stdlib_glob", 1, 1, DS_STDLIB_RETURN_ARRAY, false, true, true, false, true, DS_STDLIB_HELPER_GLOB, 0},
    {"glob!", "__ds_stdlib_glob_required", 1, 1, DS_STDLIB_RETURN_ARRAY, false, true, true, false, true, DS_STDLIB_HELPER_GLOB, 0},
    {"lines", "__ds_stdlib_lines", 1, 1, DS_STDLIB_RETURN_ARRAY, false, true, true, false, false, 0, 0},
    {"regex.match", "__ds_regex_match", 2, 3, DS_STDLIB_RETURN_MAP, false, true, false, false, false, 0, 0},
    {"regex.replace", "__ds_regex_replace", 3, 4, DS_STDLIB_RETURN_STRING, false, true, false, false, false, 0, 0},
    {"string.trim", "__ds_string_trim", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false, 0, DS_BASH_STRING_HELPER_TRIM},
    {"string.upper", "__ds_string_upper", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false, 0, DS_BASH_STRING_HELPER_UPPER},
    {"string.lower", "__ds_string_lower", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false, 0, DS_BASH_STRING_HELPER_LOWER},
    {"string.replace", "__ds_string_replace", 3, 3, DS_STDLIB_RETURN_STRING, false, true, false, false, false, 0, DS_BASH_STRING_HELPER_REPLACE},
    {"string.contains", "__ds_string_contains", 2, 2, DS_STDLIB_RETURN_BOOL, false, true, false, false, false, 0, DS_BASH_STRING_HELPER_CONTAINS},
    {"string.split", "__ds_string_split", 2, 2, DS_STDLIB_RETURN_ARRAY, false, true, true, false, false, 0, DS_BASH_STRING_HELPER_SPLIT},
    {"string.starts_with", "__ds_string_starts_with", 2, 2, DS_STDLIB_RETURN_BOOL, false, true, false, false, false, 0, DS_BASH_STRING_HELPER_STARTS_WITH},
    {"string.ends_with", "__ds_string_ends_with", 2, 2, DS_STDLIB_RETURN_BOOL, false, true, false, false, false, 0, DS_BASH_STRING_HELPER_ENDS_WITH},
    {"string.len", "__ds_string_len", 1, 1, DS_STDLIB_RETURN_INT, false, true, false, false, false, 0, DS_BASH_STRING_HELPER_LEN},
    {"string.index_of", "__ds_string_index_of", 2, 2, DS_STDLIB_RETURN_INT, false, true, false, false, false, 0, DS_BASH_STRING_HELPER_INDEX_OF},
    {"string.last_index_of", "__ds_string_last_index_of", 2, 2, DS_STDLIB_RETURN_INT, false, true, false, false, false, 0, DS_BASH_STRING_HELPER_LAST_INDEX_OF},
    {"string.count", "__ds_string_count", 2, 2, DS_STDLIB_RETURN_INT, false, true, false, false, false, 0, DS_BASH_STRING_HELPER_COUNT},
    {"string.char_at", "__ds_string_char_at", 2, 2, DS_STDLIB_RETURN_STRING, false, false, false, false, false, 0, DS_BASH_STRING_HELPER_CHAR_AT},
    {"string.slice", "__ds_string_slice", 3, 3, DS_STDLIB_RETURN_STRING, false, false, false, false, false, 0, DS_BASH_STRING_HELPER_SLICE},
};

const DsStdlibHelper *ds_stdlib_lookup(DsStr name) {
    for (size_t i = 0; i < DS_ARRAY_LEN(HELPERS); i++) {
        if (ds_str_eq_cstr(name, HELPERS[i].name)) return &HELPERS[i];
    }
    return NULL;
}

bool ds_stdlib_is_namespace(DsStr name) {
    return ds_str_eq_cstr(name, "file") || ds_str_eq_cstr(name, "dir") || ds_str_eq_cstr(name, "path") ||
           ds_str_eq_cstr(name, "cmd") || ds_str_eq_cstr(name, "env") || ds_str_eq_cstr(name, "regex") ||
           ds_str_eq_cstr(name, "string");
}

bool ds_stdlib_is_name(DsStr name) {
    return ds_stdlib_lookup(name) != NULL;
}

bool ds_stdlib_arity_ok(const DsStdlibHelper *helper, size_t argc) {
    return helper && argc >= helper->min_arity &&
           (helper->max_arity == (size_t)-1 || argc <= helper->max_arity);
}

DsStdlibNamespace ds_stdlib_namespace(DsStr name) {
    if (str_has_prefix(name, "file.")) return DS_STDLIB_NAMESPACE_FILE;
    if (str_has_prefix(name, "dir.")) return DS_STDLIB_NAMESPACE_DIR;
    if (str_has_prefix(name, "path.")) return DS_STDLIB_NAMESPACE_PATH;
    if (str_has_prefix(name, "cmd.")) return DS_STDLIB_NAMESPACE_CMD;
    if (str_has_prefix(name, "env.")) return DS_STDLIB_NAMESPACE_ENV;
    if (str_has_prefix(name, "regex.")) return DS_STDLIB_NAMESPACE_REGEX;
    if (str_has_prefix(name, "string.")) return DS_STDLIB_NAMESPACE_STRING;
    if (ds_str_eq_cstr(name, "glob") || ds_str_eq_cstr(name, "glob!") || ds_str_eq_cstr(name, "lines")) {
        return DS_STDLIB_NAMESPACE_TOP_LEVEL;
    }
    return DS_STDLIB_NAMESPACE_UNKNOWN;
}

bool ds_stdlib_is_string_helper(DsStr name) {
    return ds_stdlib_lookup(name) && ds_stdlib_namespace(name) == DS_STDLIB_NAMESPACE_STRING;
}

bool ds_stdlib_is_glob_helper(DsStr name) {
    const DsStdlibHelper *helper = ds_stdlib_lookup(name);
    return helper && (helper->flags & DS_STDLIB_HELPER_GLOB) != 0;
}

bool ds_stdlib_is_dir_walk_helper(DsStr name) {
    const DsStdlibHelper *helper = ds_stdlib_lookup(name);
    return helper && (helper->flags & DS_STDLIB_HELPER_DIR_WALK) != 0;
}

bool ds_stdlib_is_dir_walk_ext_helper(DsStr name) {
    const DsStdlibHelper *helper = ds_stdlib_lookup(name);
    return helper && (helper->flags & DS_STDLIB_HELPER_DIR_WALK_EXT) != 0;
}

bool ds_stdlib_arg_expects_int(DsStr name, size_t arg_index) {
    if (ds_str_eq_cstr(name, "string.char_at")) return arg_index == 1;
    if (ds_str_eq_cstr(name, "string.slice")) return arg_index == 1 || arg_index == 2;
    return false;
}

bool ds_stdlib_arg_expects_string(DsStr name, size_t arg_index) {
    const DsStdlibHelper *helper = ds_stdlib_lookup(name);
    if (!helper) return false;
    if (ds_stdlib_arg_expects_int(name, arg_index)) return false;
    if (ds_stdlib_is_dir_walk_ext_helper(name) && arg_index == 1) return false;
    if (ds_stdlib_is_dir_walk_helper(name)) return arg_index == 0;
    return helper->string_args_only || ds_stdlib_is_string_helper(name);
}

unsigned ds_stdlib_bash_helper_mask(DsStr name) {
    const DsStdlibHelper *helper = ds_stdlib_lookup(name);
    return helper ? helper->bash_helper_mask : 0;
}

DsStdlibArrayTransport ds_stdlib_array_transport(DsStr name) {
    const DsStdlibHelper *helper = ds_stdlib_lookup(name);
    if (!helper || helper->return_kind != DS_STDLIB_RETURN_ARRAY) {
        return DS_STDLIB_ARRAY_TRANSPORT_NONE;
    }
    if (ds_stdlib_is_dir_walk_helper(name)) return DS_STDLIB_ARRAY_TRANSPORT_NUL_RECORDS;
    return DS_STDLIB_ARRAY_TRANSPORT_NEWLINE_RECORDS;
}

bool ds_stdlib_array_uses_nul_records(DsStr name) {
    return ds_stdlib_array_transport(name) == DS_STDLIB_ARRAY_TRANSPORT_NUL_RECORDS;
}

DsStdlibArrayElementKind ds_stdlib_array_element_kind(DsStr name) {
    const DsStdlibHelper *helper = ds_stdlib_lookup(name);
    if (!helper || helper->return_kind != DS_STDLIB_RETURN_ARRAY) {
        return DS_STDLIB_ARRAY_ELEMENT_UNKNOWN;
    }
    return DS_STDLIB_ARRAY_ELEMENT_STRING;
}

static bool segment_has_recursive_marker(const char *data, size_t len) {
    if (len < 2) return false;
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == '*' && data[i + 1] == '*') return true;
    }
    return false;
}

bool ds_glob_pattern_contains_recursive(DsStr pattern) {
    return segment_has_recursive_marker(pattern.data, pattern.len);
}

DsGlobPatternStatus ds_glob_pattern_validate(DsStr pattern, size_t *recursive_count_out) {
    bool has_recursive = ds_glob_pattern_contains_recursive(pattern);
    size_t recursive_count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= pattern.len; i++) {
        if (i < pattern.len && pattern.data[i] != '/') continue;

        size_t len = i - start;
        const char *seg = pattern.data + start;
        if (len == 2 && memcmp(seg, "**", 2) == 0) {
            recursive_count++;
            if (recursive_count > 1) {
                if (recursive_count_out) *recursive_count_out = recursive_count;
                return DS_GLOB_PATTERN_ERR_MULTIPLE_RECURSIVE_SEGMENTS;
            }
        } else if (segment_has_recursive_marker(seg, len)) {
            if (recursive_count_out) *recursive_count_out = recursive_count;
            return DS_GLOB_PATTERN_ERR_BAD_RECURSIVE_SEGMENT;
        } else if (has_recursive && len == 2 && memcmp(seg, "..", 2) == 0) {
            if (recursive_count_out) *recursive_count_out = recursive_count;
            return DS_GLOB_PATTERN_ERR_PARENT_SEGMENT;
        }

        start = i + 1;
    }

    if (recursive_count_out) *recursive_count_out = recursive_count;
    return DS_GLOB_PATTERN_OK;
}

const char *ds_glob_pattern_status_message(DsGlobPatternStatus status) {
    static const char *const messages[] = {
        "glob pattern is valid",
        "recursive `**` glob patterns must use `**` as a complete path segment in v0.31.0",
        "multiple recursive `**` glob segments are unsupported in v0.31.0",
        "recursive `**` glob patterns with `..` path segments are unsupported in v0.31.0",
    };
    return (unsigned)status < DS_ARRAY_LEN(messages) ? messages[status] : "invalid recursive glob pattern";
}
