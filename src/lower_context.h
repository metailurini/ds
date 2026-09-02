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

#endif
