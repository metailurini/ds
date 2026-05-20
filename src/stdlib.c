#include "ds_stdlib.h"

#include <string.h>

static bool str_eq(DsStr a, const char *b) {
    size_t len = strlen(b);
    return a.len == len && memcmp(a.data, b, len) == 0;
}

static const DsStdlibHelper HELPERS[] = {
    {"file.exists", "__ds_stdlib_file_exists", 1, 1, DS_STDLIB_RETURN_BOOL, false, true, false, false, false},
    {"file.is_file", "__ds_stdlib_file_is_file", 1, 1, DS_STDLIB_RETURN_BOOL, false, true, false, false, false},
    {"file.read", "__ds_stdlib_file_read", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false},
    {"file.write", "__ds_stdlib_file_write", 2, 2, DS_STDLIB_RETURN_STATEMENT_ONLY, true, true, false, false, false},
    {"file.append", "__ds_stdlib_file_append", 2, 2, DS_STDLIB_RETURN_STATEMENT_ONLY, true, true, false, false, false},
    {"dir.exists", "__ds_stdlib_dir_exists", 1, 1, DS_STDLIB_RETURN_BOOL, false, true, false, false, false},
    {"path.cwd", "__ds_stdlib_path_cwd", 0, 0, DS_STDLIB_RETURN_STRING, false, true, false, false, false},
    {"path.join", "__ds_stdlib_path_join", 1, (size_t)-1, DS_STDLIB_RETURN_STRING, false, true, false, false, false},
    {"path.basename", "__ds_stdlib_path_basename", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false},
    {"path.dirname", "__ds_stdlib_path_dirname", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false},
    {"path.ext", "__ds_stdlib_path_ext", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false},
    {"cmd.exists", "__ds_stdlib_cmd_exists", 1, 1, DS_STDLIB_RETURN_BOOL, false, true, false, false, false},
    {"cmd.require", "__ds_stdlib_cmd_require", 1, 1, DS_STDLIB_RETURN_STATEMENT_ONLY, true, true, false, false, false},
    {"env.get", "__ds_stdlib_env_get", 1, 2, DS_STDLIB_RETURN_STRING, false, true, false, true, false},
    {"env.set", "__ds_stdlib_env_set", 2, 2, DS_STDLIB_RETURN_STATEMENT_ONLY, true, true, false, true, false},
    {"env.unset", "__ds_stdlib_env_unset", 1, 1, DS_STDLIB_RETURN_STATEMENT_ONLY, true, true, false, true, false},
    {"glob", "__ds_stdlib_glob", 1, 1, DS_STDLIB_RETURN_ARRAY, false, true, true, false, true},
    {"glob!", "__ds_stdlib_glob_required", 1, 1, DS_STDLIB_RETURN_ARRAY, false, true, true, false, true},
    {"lines", "__ds_stdlib_lines", 1, 1, DS_STDLIB_RETURN_ARRAY, false, true, true, false, false},
    {"string.trim", "__ds_string_trim", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false},
    {"string.upper", "__ds_string_upper", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false},
    {"string.lower", "__ds_string_lower", 1, 1, DS_STDLIB_RETURN_STRING, false, true, false, false, false},
    {"string.replace", "__ds_string_replace", 3, 3, DS_STDLIB_RETURN_STRING, false, true, false, false, false},
    {"string.contains", "__ds_string_contains", 2, 2, DS_STDLIB_RETURN_BOOL, false, true, false, false, false},
    {"string.split", "__ds_string_split", 2, 2, DS_STDLIB_RETURN_ARRAY, false, true, true, false, false},
    {"string.starts_with", "__ds_string_starts_with", 2, 2, DS_STDLIB_RETURN_BOOL, false, true, false, false, false},
    {"string.ends_with", "__ds_string_ends_with", 2, 2, DS_STDLIB_RETURN_BOOL, false, true, false, false, false},
};

const DsStdlibHelper *ds_stdlib_lookup(DsStr name) {
    for (size_t i = 0; i < sizeof(HELPERS) / sizeof(HELPERS[0]); i++) {
        if (str_eq(name, HELPERS[i].name)) return &HELPERS[i];
    }
    return NULL;
}

bool ds_stdlib_is_namespace(DsStr name) {
    return str_eq(name, "file") || str_eq(name, "dir") || str_eq(name, "path") ||
           str_eq(name, "cmd") || str_eq(name, "env");
}

bool ds_stdlib_is_name(DsStr name) {
    return ds_stdlib_lookup(name) != NULL;
}

bool ds_stdlib_arity_ok(const DsStdlibHelper *helper, size_t argc) {
    return helper && argc >= helper->min_arity &&
           (helper->max_arity == (size_t)-1 || argc <= helper->max_arity);
}
