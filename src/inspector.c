#include "inspector.h"

void ds_inspector_print_tokens(const DsTokenVec *tokens, FILE *out) {
    ds_tokens_print(tokens, out);
}

void ds_inspector_print_ast(const DsAst *ast, FILE *out) {
    ds_ast_print(ast, out);
}

bool ds_inspector_print_hir(const DsLowerProgram *program, FILE *out) {
    return ds_hir_dump_program(program, out);
}
