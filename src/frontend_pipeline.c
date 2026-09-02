#include "frontend.h"

bool ds_frontend_parse_source(const DsSource *source, DsTokenVec *tokens,
                              DsAst **ast, DsDiag *diag) {
    if (!source || !tokens || !ast || !diag) return false;
    if (!ds_lex(source, tokens, diag)) return false;
    *ast = ds_parse(tokens, diag);
    return !diag->has_error;
}
