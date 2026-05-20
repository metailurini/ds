#ifndef DS_FRONTEND_H
#define DS_FRONTEND_H

#include "ds_ast.h"

typedef enum {
    DS_TOK_EOF,
    DS_TOK_NEWLINE,
    DS_TOK_IDENT,
    DS_TOK_INT,
    DS_TOK_STRING,
    DS_TOK_DOLLAR_IDENT,
    DS_TOK_LET,
    DS_TOK_IF,
    DS_TOK_ELSE,
    DS_TOK_SCRIPT,
    DS_TOK_IMPORT,
    DS_TOK_ARG,
    DS_TOK_OPTION,
    DS_TOK_FLAG,
    DS_TOK_RUN,
    DS_TOK_TYPE_STRING,
    DS_TOK_TYPE_INT,
    DS_TOK_TYPE_BOOL,
    DS_TOK_TRUE,
    DS_TOK_FALSE,
    DS_TOK_FN,
    DS_TOK_FOR,
    DS_TOK_IN,
    DS_TOK_WHILE,
    DS_TOK_BREAK,
    DS_TOK_CONTINUE,
    DS_TOK_CASE,
    DS_TOK_PIPE,
    DS_TOK_TEST,
    DS_TOK_ASSERT,
    DS_TOK_COLON,
    DS_TOK_COMMA,
    DS_TOK_EQUAL,
    DS_TOK_EQUAL_EQUAL,
    DS_TOK_BANG,
    DS_TOK_BANG_EQUAL,
    DS_TOK_GREATER,
    DS_TOK_GREATER_EQUAL,
    DS_TOK_LESS,
    DS_TOK_LESS_EQUAL,
    DS_TOK_PLUS,
    DS_TOK_MINUS,
    DS_TOK_STAR,
    DS_TOK_SLASH,
    DS_TOK_DOT,
    DS_TOK_REDIRECT_OUT,
    DS_TOK_REDIRECT_OUT_APPEND,
    DS_TOK_REDIRECT_ERR,
    DS_TOK_REDIRECT_ERR_APPEND,
    DS_TOK_REDIRECT_ALL,
    DS_TOK_REDIRECT_ALL_APPEND,
    DS_TOK_LBRACE,
    DS_TOK_RBRACE,
    DS_TOK_LBRACKET,
    DS_TOK_RBRACKET,
    DS_TOK_LPAREN,
    DS_TOK_RPAREN,
    DS_TOK_UNKNOWN
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

#endif
