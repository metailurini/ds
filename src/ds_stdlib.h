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
bool ds_glob_pattern_contains_recursive(DsStr pattern);
DsGlobPatternStatus ds_glob_pattern_validate(DsStr pattern, size_t *recursive_count_out);
const char *ds_glob_pattern_status_message(DsGlobPatternStatus status);

#endif
