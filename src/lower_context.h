#ifndef DS_LOWER_CONTEXT_H
#define DS_LOWER_CONTEXT_H

#include "lower_symbols.h"

struct Lower {
    DsDiag *diag;
    Scope root_scope;
    Scope *scope;
    DsLowerProgram *program;
    int loop_depth;
    int function_depth;
    int handler_depth;
    int handler_function_depth;
    DsLowerFn *current_function;
    size_t temp_counter;
    Symbol **map_loop_symbols;
    size_t map_loop_len;
    size_t map_loop_cap;
};

void lower_context_init(Lower *lower, DsDiag *diag, DsLowerProgram *program);
void lower_context_free(Lower *lower);

void lower_diag_stdlib_arity_error(Lower *lower, DsSpan span, DsStr name,
                                   size_t min_arity, size_t max_arity, size_t actual);
void lower_diag_unknown_function(Lower *lower, DsSpan span, DsStr name);
void lower_diag_unknown_stdlib_helper(Lower *lower, DsSpan span, DsStr name);
void lower_diag_unknown_string_method(Lower *lower, DsSpan span, DsStr member);
bool lower_validate_env_name(Lower *lower, DsStr name, DsSpan span, const char *version);

#endif
