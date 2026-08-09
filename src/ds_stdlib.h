#ifndef DS_STDLIB_H
#define DS_STDLIB_H

#include "ds_common.h"

typedef enum {
    DS_STDLIB_RETURN_BOOL,
    DS_STDLIB_RETURN_INT,
    DS_STDLIB_RETURN_STRING,
    DS_STDLIB_RETURN_ARRAY,
    DS_STDLIB_RETURN_MAP,
    DS_STDLIB_RETURN_COMMAND_RESULT,
    DS_STDLIB_RETURN_STATEMENT_ONLY
} DsStdlibReturnKind;

typedef enum {
    DS_STDLIB_NAMESPACE_TOP_LEVEL,
    DS_STDLIB_NAMESPACE_FILE,
    DS_STDLIB_NAMESPACE_DIR,
    DS_STDLIB_NAMESPACE_PATH,
    DS_STDLIB_NAMESPACE_CMD,
    DS_STDLIB_NAMESPACE_ENV,
    DS_STDLIB_NAMESPACE_REGEX,
    DS_STDLIB_NAMESPACE_STRING,
    DS_STDLIB_NAMESPACE_UNKNOWN
} DsStdlibNamespace;

typedef enum {
    DS_STDLIB_ARRAY_TRANSPORT_NONE,
    DS_STDLIB_ARRAY_TRANSPORT_NEWLINE_RECORDS,
    DS_STDLIB_ARRAY_TRANSPORT_NUL_RECORDS
} DsStdlibArrayTransport;

typedef enum {
    DS_STDLIB_ARRAY_ELEMENT_UNKNOWN,
    DS_STDLIB_ARRAY_ELEMENT_STRING
} DsStdlibArrayElementKind;

enum {
    DS_BASH_STRING_HELPER_TRIM = 1u << 0,
    DS_BASH_STRING_HELPER_UPPER = 1u << 1,
    DS_BASH_STRING_HELPER_LOWER = 1u << 2,
    DS_BASH_STRING_HELPER_REPLACE = 1u << 3,
    DS_BASH_STRING_HELPER_CONTAINS = 1u << 4,
    DS_BASH_STRING_HELPER_SPLIT = 1u << 5,
    DS_BASH_STRING_HELPER_STARTS_WITH = 1u << 6,
    DS_BASH_STRING_HELPER_ENDS_WITH = 1u << 7,
    DS_BASH_STRING_HELPER_LEN = 1u << 8,
    DS_BASH_STRING_HELPER_INDEX_OF = 1u << 9,
    DS_BASH_STRING_HELPER_LAST_INDEX_OF = 1u << 10,
    DS_BASH_STRING_HELPER_COUNT = 1u << 11,
    DS_BASH_STRING_HELPER_CHAR_AT = 1u << 12,
    DS_BASH_STRING_HELPER_SLICE = 1u << 13,
    DS_BASH_STRING_HELPER_FORMAT_CENTER = 1u << 14,
};

enum {
    DS_STDLIB_HELPER_GLOB = 1u << 0,
    DS_STDLIB_HELPER_DIR_WALK = 1u << 1,
    DS_STDLIB_HELPER_DIR_WALK_EXT = 1u << 2,
};

typedef struct {
    const char *name;
    const char *bash_name;
    size_t min_arity;
    size_t max_arity;
    DsStdlibReturnKind return_kind;
    bool statement_only;
    bool string_args_only;
    bool iterable;
    bool validates_env_name;
    bool validates_glob_pattern;
    unsigned flags;
    unsigned bash_helper_mask;
} DsStdlibHelper;

typedef enum {
    DS_GLOB_PATTERN_OK,
    DS_GLOB_PATTERN_ERR_BAD_RECURSIVE_SEGMENT,
    DS_GLOB_PATTERN_ERR_MULTIPLE_RECURSIVE_SEGMENTS,
    DS_GLOB_PATTERN_ERR_PARENT_SEGMENT
} DsGlobPatternStatus;

const DsStdlibHelper *ds_stdlib_lookup(DsStr name);
bool ds_stdlib_is_name(DsStr name);
bool ds_stdlib_is_namespace(DsStr name);
bool ds_stdlib_arity_ok(const DsStdlibHelper *helper, size_t argc);
DsStdlibNamespace ds_stdlib_namespace(DsStr name);
const char *ds_stdlib_string_method_names(void);
bool ds_stdlib_is_string_helper(DsStr name);
bool ds_stdlib_is_glob_helper(DsStr name);
bool ds_stdlib_is_dir_walk_helper(DsStr name);
bool ds_stdlib_is_dir_walk_ext_helper(DsStr name);
bool ds_stdlib_arg_expects_int(DsStr name, size_t arg_index);
bool ds_stdlib_arg_expects_string(DsStr name, size_t arg_index);
unsigned ds_stdlib_bash_helper_mask(DsStr name);
DsStdlibArrayTransport ds_stdlib_array_transport(DsStr name);
DsStdlibArrayElementKind ds_stdlib_array_element_kind(DsStr name);
bool ds_glob_pattern_contains_recursive(DsStr pattern);
DsGlobPatternStatus ds_glob_pattern_validate(DsStr pattern, size_t *recursive_count_out);
const char *ds_glob_pattern_status_message(DsGlobPatternStatus status);

#endif
