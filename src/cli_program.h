#ifndef DS_CLI_PROGRAM_H
#define DS_CLI_PROGRAM_H

#include "loaded_program.h"

/* Transitional compatibility name while the implementation lives in cli_program.c. */
typedef DsLoadedProgram DsCliProgram;

bool ds_cli_load_source(const char *path, DsCliProgram *program);
bool ds_cli_load_and_lex(const char *path, DsCliProgram *program);
bool ds_cli_load_parse(const char *path, DsCliProgram *program);
bool ds_cli_load_composed_parse(const char *path, DsCliProgram *program);

void ds_cli_program_free(DsCliProgram *program);

#endif
