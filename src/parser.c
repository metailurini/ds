#include "parser_internal.h"

DsAst *ds_parse(const DsTokenVec *tokens, DsDiag *diag) {
    Parser p = {tokens, 0, diag, 0, 0};
    DsAst *ast = (DsAst *)ds_xcalloc(1, sizeof(DsAst));
    bool after_executable = false;
    if (tokens->len > 0) ast->span.start = tokens->items[0].span.start;
    parser_skip_newlines(&p);
    while (!parser_at_end(&p)) {
        if (parser_advance_if(&p, DS_TOK_SCRIPT)) {
            parse_script_block(&p, ast);
            parser_skip_newlines(&p);
            continue;
        }
        if (parser_advance_if(&p, DS_TOK_IMPORT)) {
            DsStmt *stmt = parse_import_stmt(&p, true, after_executable);
            if (stmt) DS_VEC_PUSH(&ast->statements, stmt, 16);
            parser_skip_newlines(&p);
            continue;
        }
        if (parser_advance_if(&p, DS_TOK_FN)) {
            DsStmt *stmt = parse_fn(&p, true);
            if (stmt) DS_VEC_PUSH(&ast->statements, stmt, 16);
            parser_skip_newlines(&p);
            continue;
        }
        if (parser_at(&p, DS_TOK_TEST) && parser_next_at(&p, DS_TOK_STRING)) {
            parser_advance(&p);
            DsStmt *stmt = parse_test(&p, true);
            if (stmt) DS_VEC_PUSH(&ast->statements, stmt, 16);
            after_executable = true;
            parser_skip_newlines(&p);
            continue;
        }
        DsStmt *stmt = parse_stmt(&p);
        if (stmt) DS_VEC_PUSH(&ast->statements, stmt, 16);
        after_executable = true;
        parser_skip_newlines(&p);
    }
    if (tokens->len > 0) ast->span.end = tokens->items[tokens->len - 1].span.end;
    return ast;
}
