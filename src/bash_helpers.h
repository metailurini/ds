#ifndef DS_BASH_HELPERS_H
#define DS_BASH_HELPERS_H

const char *ds_bash_command_result_helpers_source(void);
const char *ds_bash_array_helpers_source(void);
const char *ds_bash_map_helpers_source(void);
const char *ds_bash_dynamic_index_helper_source(void);
const char *ds_bash_collection_helpers_source(void);
const char *ds_bash_stdlib_capture_helper_source(void);
const char *ds_bash_stdlib_helpers_source(void);
const char *ds_bash_glob_helpers_source(void);
const char *ds_bash_recursive_glob_helpers_source(void);
const char *ds_bash_regex_helpers_source(void);
const char *ds_bash_regex_match_helpers_source(void);
const char *ds_bash_regex_replace_helpers_source(void);
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

const char *ds_bash_string_helpers_source(unsigned helper_mask);
const char *ds_bash_debug_helpers_source(void);
const char *ds_bash_int_helpers_source(void);
const char *ds_bash_function_value_capture_helpers_source(void);
const char *ds_bash_function_value_materialize_helpers_source(void);

#endif
