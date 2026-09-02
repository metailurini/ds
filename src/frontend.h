#ifndef DS_FRONTEND_H
#define DS_FRONTEND_H

#include "ds_ast.h"

typedef enum {
#define DS_TOKEN(name) DS_TOK_##name,
#define DS_KEYWORD(name, text) DS_TOK_##name,
#include "token_kinds.def"
#undef DS_KEYWORD
#undef DS_TOKEN
} DsTokenKind;

typedef struct {
    DsTokenKind kind;
    DsStr text;
    DsSpan span;
} DsToken;

typedef struct {
    DsToken *items;
    size_t len;
    size_t cap;
} DsTokenVec;

const char *ds_token_kind_name(DsTokenKind kind);
bool ds_lex(const DsSource *source, DsTokenVec *out, DsDiag *diag);
void ds_tokens_free(DsTokenVec *tokens);
void ds_tokens_print(const DsTokenVec *tokens, FILE *out);

DsAst *ds_parse(const DsTokenVec *tokens, DsDiag *diag);

/* High-level frontend shell: source text -> tokens -> syntax AST. */
bool ds_frontend_parse_source(const DsSource *source, DsTokenVec *tokens,
                              DsAst **ast, DsDiag *diag);

#endif
