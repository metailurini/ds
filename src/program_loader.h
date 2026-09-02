#ifndef DS_PROGRAM_LOADER_H
#define DS_PROGRAM_LOADER_H

#include "loaded_program.h"

/*
 * High-level source/import composition boundary.
 *
 * The current implementation still lives in cli_program.c. This facade keeps
 * compiler orchestration independent from that transitional implementation.
 */
bool ds_program_loader_load_tokens(const char *path, DsLoadedProgram *program);
bool ds_program_loader_load_ast(const char *path, DsLoadedProgram *program);
bool ds_program_loader_load_composed_ast(const char *path, DsLoadedProgram *program);
void ds_program_loader_free(DsLoadedProgram *program);

#endif
