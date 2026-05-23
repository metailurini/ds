#ifndef DS_CLI_PROGRAM_H
#define DS_CLI_PROGRAM_H

#include "backend.h"
#include "frontend.h"

typedef struct LoadedUnit LoadedUnit;

typedef struct DsCliProgram {
    DsSource source;
    DsTokenVec tokens;
    DsAst *ast;
    DsLowerProgram *lowered;
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
} DsCliProgram;

bool ds_cli_load_source(const char *path, DsCliProgram *program);
bool ds_cli_load_and_lex(const char *path, DsCliProgram *program);
bool ds_cli_load_parse(const char *path, DsCliProgram *program);
bool ds_cli_load_composed_parse(const char *path, DsCliProgram *program);
bool ds_cli_load_lower(const char *path, DsCliProgram *program);

void ds_cli_program_free(DsCliProgram *program);

#endif
