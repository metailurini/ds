#ifndef DS_BASH_HELPERS_H
#define DS_BASH_HELPERS_H

#include "ds_stdlib.h"

const char *ds_bash_command_result_helpers_source(void);
const char *ds_bash_array_helpers_source(void);
const char *ds_bash_map_helpers_source(void);
const char *ds_bash_dynamic_index_helper_source(void);
const char *ds_bash_collection_helpers_source(void);
const char *ds_bash_stdlib_capture_helper_source(void);
const char *ds_bash_stdlib_helpers_source(void);
const char *ds_bash_temp_helpers_source(void);
const char *ds_bash_glob_helpers_source(void);
const char *ds_bash_recursive_glob_helpers_source(void);
const char *ds_bash_regex_helpers_source(void);
const char *ds_bash_regex_match_helpers_source(void);
const char *ds_bash_regex_replace_helpers_source(void);

const char *ds_bash_string_helpers_source(unsigned helper_mask);
const char *ds_bash_debug_helpers_source(void);
const char *ds_bash_int_helpers_source(void);
const char *ds_bash_function_value_capture_helpers_source(void);
const char *ds_bash_function_value_materialize_helpers_source(void);

#endif
