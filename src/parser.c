#include "parser_internal.h"

DsToken *parser_peek(Parser *p) { return &p->tokens->items[p->pos]; }
DsToken *parser_previous(Parser *p) { return &p->tokens->items[p->pos - 1]; }
bool parser_at(Parser *p, DsTokenKind kind) { return parser_peek(p)->kind == kind; }
bool parser_next_at(Parser *p, DsTokenKind kind) { return p->pos + 1 < p->tokens->len && p->tokens->items[p->pos + 1].kind == kind; }
bool parser_peek2_at(Parser *p, DsTokenKind kind) { return p->pos + 2 < p->tokens->len && p->tokens->items[p->pos + 2].kind == kind; }
bool parser_at_end(Parser *p) { return parser_at(p, DS_TOK_EOF); }

bool parser_token_text_eq(const DsToken *token, const char *text) {
    return token && ds_str_eq_cstr(token->text, text);
}

bool parser_at_ident_text(Parser *p, const char *text) {
    return parser_at(p, DS_TOK_IDENT) && parser_token_text_eq(parser_peek(p), text);
}

bool parser_next_ident_text(Parser *p, const char *text) {
    return parser_next_at(p, DS_TOK_IDENT) && parser_token_text_eq(&p->tokens->items[p->pos + 1], text);
}

bool parser_at_env_dot(Parser *p) {
    return parser_at_ident_text(p, "env") && parser_next_at(p, DS_TOK_DOT);
}

bool parser_advance_if(Parser *p, DsTokenKind kind) {
    if (!parser_at(p, kind)) return false;
    p->pos++;
    return true;
}

DsToken *parser_advance(Parser *p) {
    if (!parser_at_end(p)) p->pos++;
    return parser_previous(p);
}

DsStr parser_copy_token_text(const DsToken *token) {
    return (DsStr){ds_str_dup_range(token->text.data, token->text.len), token->text.len};
}

DsStr parser_copy_dotted_name(const DsToken *left, const DsToken *right) {
    return ds_str_join_char(left->text, '.', right->text);
}

DsStr parser_copy_bang_name(const DsToken *name) {
    DsStr s;
    s.len = name->text.len + 1;
    s.data = (char *)ds_xcalloc(s.len + 1, 1);
    memcpy(s.data, name->text.data, name->text.len);
    s.data[name->text.len] = '!';
    return s;
}

void parser_skip_newlines(Parser *p) {
    while (parser_advance_if(p, DS_TOK_NEWLINE)) {}
}

bool parser_is_stmt_end(Parser *p) {
    return parser_at(p, DS_TOK_NEWLINE) || parser_at(p, DS_TOK_EOF) || parser_at(p, DS_TOK_RBRACE);
}

void parser_skip_to_stmt_end(Parser *p) {
    while (!parser_is_stmt_end(p)) parser_advance(p);
}

void parser_skip_to_stmt_end_or(Parser *p, DsTokenKind stop) {
    while (!parser_at_end(p) && !parser_at(p, stop) && !parser_is_stmt_end(p)) parser_advance(p);
}

void parser_consume_statement_end(Parser *p) {
    if (parser_at(p, DS_TOK_NEWLINE)) parser_skip_newlines(p);
}

void parser_expect_stmt_end(Parser *p, const char *description) {
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of %s", description);
        parser_skip_to_stmt_end(p);
    }
    parser_consume_statement_end(p);
}

bool parser_expect(Parser *p, DsTokenKind kind, const char *message) {
    if (parser_advance_if(p, kind)) return true;
    ds_diag_error(p->diag, parser_peek(p)->span, "%s", message);
    return false;
}

bool parser_reject_trailing_comma(Parser *p, DsTokenKind closing_kind, const char *message) {
    if (!parser_at(p, closing_kind)) return false;
    ds_diag_error(p->diag, parser_peek(p)->span, "%s", message);
    return true;
}

bool parser_expect_expr(Parser *p, DsSpan span, const char *message) {
    if (!parser_is_stmt_end(p)) return true;
    ds_diag_error(p->diag, span, "%s", message);
    return false;
}

bool parser_is_identifier_like(DsTokenKind kind) {
    return kind == DS_TOK_IDENT || kind == DS_TOK_SCRIPT || kind == DS_TOK_IMPORT || kind == DS_TOK_ARG ||
           kind == DS_TOK_OPTION || kind == DS_TOK_FLAG || kind == DS_TOK_TYPE_STRING ||
           kind == DS_TOK_TYPE_INT || kind == DS_TOK_TYPE_BOOL || kind == DS_TOK_RUN || kind == DS_TOK_FN ||
           kind == DS_TOK_DEFER || kind == DS_TOK_TRAP;
}

bool parser_expect_identifier_like(Parser *p, const char *message) {
    if (parser_is_identifier_like(parser_peek(p)->kind)) {
        parser_advance(p);
        return true;
    }
    ds_diag_error(p->diag, parser_peek(p)->span, "%s", message);
    return false;
}

bool parser_is_redirect_token(DsTokenKind kind) {
    return kind == DS_TOK_REDIRECT_OUT || kind == DS_TOK_REDIRECT_OUT_APPEND ||
           kind == DS_TOK_REDIRECT_ERR || kind == DS_TOK_REDIRECT_ERR_APPEND ||
           kind == DS_TOK_REDIRECT_ALL || kind == DS_TOK_REDIRECT_ALL_APPEND;
}

DsRedirectKind parser_redirect_kind_from_token(DsTokenKind kind) {
    switch (kind) {
        case DS_TOK_REDIRECT_OUT: return DS_REDIRECT_OUT;
        case DS_TOK_REDIRECT_OUT_APPEND: return DS_REDIRECT_OUT_APPEND;
        case DS_TOK_REDIRECT_ERR: return DS_REDIRECT_ERR;
        case DS_TOK_REDIRECT_ERR_APPEND: return DS_REDIRECT_ERR_APPEND;
        case DS_TOK_REDIRECT_ALL: return DS_REDIRECT_ALL;
        case DS_TOK_REDIRECT_ALL_APPEND: return DS_REDIRECT_ALL_APPEND;
        default: return DS_REDIRECT_NONE;
    }
}

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
