#include "program_loader.h"
#include "cli_program.h"

bool ds_program_loader_load_tokens(const char *path, DsLoadedProgram *program) {
    return ds_cli_load_and_lex(path, program);
}

bool ds_program_loader_load_ast(const char *path, DsLoadedProgram *program) {
    return ds_cli_load_parse(path, program);
}

bool ds_program_loader_load_composed_ast(const char *path, DsLoadedProgram *program) {
    return ds_cli_load_composed_parse(path, program);
}

void ds_program_loader_free(DsLoadedProgram *program) {
    ds_cli_program_free(program);
}
