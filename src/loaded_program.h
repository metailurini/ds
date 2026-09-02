#ifndef DS_LOADED_PROGRAM_H
#define DS_LOADED_PROGRAM_H

#include "frontend.h"

typedef struct LoadedUnit LoadedUnit;

/*
 * Neutral owned program container used while loading, parsing, composing, and
 * compiling. The legacy CLI loader currently fills this structure.
 */
typedef struct DsLoadedProgram {
    DsSource source;
    DsTokenVec tokens;
    DsAst *ast;
    DsDiag diag;
    LoadedUnit **units;
    size_t units_len;
    size_t units_cap;
    char **loaded_paths;
    size_t loaded_len;
    size_t loaded_cap;
    char **stack;
    size_t stack_len;
    size_t stack_cap;
} DsLoadedProgram;

#endif
