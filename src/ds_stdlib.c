#include "ds_stdlib.h"

static const DsStdlibHelper HELPERS[] = {
#define DS_STDLIB_HELPER(id, name, bash_name, min_arity, max_arity, return_kind, statement_only, string_args_only, iterable, validates_env_name, validates_glob_pattern, flags, bash_helper_mask) \
    {DS_STDLIB_ID_##id, name, bash_name, min_arity, max_arity, return_kind, statement_only, string_args_only, iterable, validates_env_name, validates_glob_pattern, flags, bash_helper_mask},
#include "stdlib_helpers.def"
#undef DS_STDLIB_HELPER
};

typedef struct {
    const char *name;
    DsStdlibNamespace value;
} DsStdlibNamespaceEntry;

static const DsStdlibNamespaceEntry NAMESPACES[] = {
    {"file", DS_STDLIB_NAMESPACE_FILE},
    {"dir", DS_STDLIB_NAMESPACE_DIR},
    {"path", DS_STDLIB_NAMESPACE_PATH},
    {"cmd", DS_STDLIB_NAMESPACE_CMD},
    {"env", DS_STDLIB_NAMESPACE_ENV},
    {"regex", DS_STDLIB_NAMESPACE_REGEX},
    {"string", DS_STDLIB_NAMESPACE_STRING},
};

static bool name_has_namespace(DsStr name, const char *namespace_name) {
    size_t len = strlen(namespace_name);
    return name.len > len && name.data[len] == '.' && memcmp(name.data, namespace_name, len) == 0;
}

const DsStdlibHelper *ds_stdlib_lookup(DsStr name) {
    for (size_t i = 0; i < DS_ARRAY_LEN(HELPERS); i++) {
        if (ds_str_eq_cstr(name, HELPERS[i].name)) return &HELPERS[i];
    }
    return NULL;
}

bool ds_stdlib_is_namespace(DsStr name) {
    for (size_t i = 0; i < DS_ARRAY_LEN(NAMESPACES); i++) {
        if (ds_str_eq_cstr(name, NAMESPACES[i].name)) return true;
    }
    return false;
}

bool ds_stdlib_is_name(DsStr name) {
    return ds_stdlib_lookup(name) != NULL;
}

bool ds_stdlib_arity_ok(const DsStdlibHelper *helper, size_t argc) {
    return helper && argc >= helper->min_arity &&
           (helper->max_arity == (size_t)-1 || argc <= helper->max_arity);
}

DsStdlibNamespace ds_stdlib_namespace(DsStr name) {
    for (size_t i = 0; i < DS_ARRAY_LEN(NAMESPACES); i++) {
        if (name_has_namespace(name, NAMESPACES[i].name)) return NAMESPACES[i].value;
    }
    if (ds_str_eq_cstr(name, "glob") || ds_str_eq_cstr(name, "glob!") || ds_str_eq_cstr(name, "lines")) {
        return DS_STDLIB_NAMESPACE_TOP_LEVEL;
    }
    return DS_STDLIB_NAMESPACE_UNKNOWN;
}

const char *ds_stdlib_string_method_names(void) {
    static char names[256];
    if (names[0]) return names;
    size_t len = 0;
    for (size_t i = 0; i < DS_ARRAY_LEN(HELPERS); i++) {
        const char *name = HELPERS[i].name;
        if (strncmp(name, "string.", 7) != 0) continue;
        int written = snprintf(names + len, sizeof(names) - len, "%s%s", len ? ", " : "", name + 7);
        if (written < 0 || (size_t)written >= sizeof(names) - len) break;
        len += (size_t)written;
    }
    return names;
}

bool ds_stdlib_is_string_helper(DsStr name) {
    const DsStdlibHelper *helper = ds_stdlib_lookup(name);
    return helper && helper->id >= DS_STDLIB_ID_STRING_TRIM && helper->id <= DS_STDLIB_ID_STRING_SLICE;
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
    const DsStdlibHelper *helper = ds_stdlib_lookup(name);
    if (!helper) return false;
    if (helper->id == DS_STDLIB_ID_STRING_CHAR_AT) return arg_index == 1;
    return helper->id == DS_STDLIB_ID_STRING_SLICE && (arg_index == 1 || arg_index == 2);
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
