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
    bool rejects_recursive_glob;
} DsStdlibHelper;

const DsStdlibHelper *ds_stdlib_lookup(DsStr name);
bool ds_stdlib_is_name(DsStr name);
bool ds_stdlib_is_namespace(DsStr name);
bool ds_stdlib_arity_ok(const DsStdlibHelper *helper, size_t argc);

#endif
