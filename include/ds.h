#ifndef DS_H
#define DS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    char *data;
    size_t len;
} DsStr;

typedef struct {
    const char *path;
    char *data;
    size_t len;
} DsSource;

typedef struct {
    size_t offset;
    int line;
    int column;
} DsLoc;

typedef struct {
    DsLoc start;
    DsLoc end;
} DsSpan;

typedef struct {
    const DsSource *source;
    bool has_error;
} DsDiag;

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
    DS_TOK_TRUE,
    DS_TOK_FALSE,
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
    DS_TOK_LBRACE,
    DS_TOK_RBRACE,
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

typedef enum {
    DS_EXPR_IDENT,
    DS_EXPR_STRING,
    DS_EXPR_INT,
    DS_EXPR_BOOL,
    DS_EXPR_UNARY,
    DS_EXPR_BINARY,
    DS_EXPR_ERROR
} DsExprKind;

typedef struct DsExpr DsExpr;

struct DsExpr {
    DsExprKind kind;
    DsSpan span;
    union {
        DsStr text;
        bool boolean;
        struct { DsStr op; DsExpr *right; } unary;
        struct { DsExpr *left; DsStr op; DsExpr *right; } binary;
    } as;
};

typedef enum {
    DS_STMT_LET,
    DS_STMT_IF,
    DS_STMT_BLOCK,
    DS_STMT_CMD
} DsStmtKind;

typedef struct DsStmt DsStmt;

typedef struct {
    DsStmt **items;
    size_t len;
    size_t cap;
} DsStmtVec;

typedef struct {
    DsStr *items;
    size_t len;
    size_t cap;
} DsWordVec;

struct DsStmt {
    DsStmtKind kind;
    DsSpan span;
    union {
        struct { DsStr name; DsExpr *value; } let_stmt;
        struct { DsExpr *condition; DsStmt *then_branch; DsStmt *else_branch; } if_stmt;
        struct { DsStmtVec statements; } block_stmt;
        struct { DsWordVec words; } cmd_stmt;
    } as;
};

typedef struct {
    DsStmtVec statements;
    DsSpan span;
} DsAst;

bool ds_source_read(const char *path, DsSource *out, DsDiag *diag);
void ds_source_free(DsSource *source);

void ds_diag_init(DsDiag *diag, const DsSource *source);
void ds_diag_error(DsDiag *diag, DsSpan span, const char *fmt, ...);

const char *ds_token_kind_name(DsTokenKind kind);
bool ds_lex(const DsSource *source, DsTokenVec *out, DsDiag *diag);
void ds_tokens_free(DsTokenVec *tokens);
void ds_tokens_print(const DsTokenVec *tokens, FILE *out);

DsAst *ds_parse(const DsTokenVec *tokens, DsDiag *diag);
void ds_ast_print(const DsAst *ast, FILE *out);
void ds_ast_free(DsAst *ast);

bool ds_emit_bash(const DsSource *source, const DsAst *ast, const char *output_path, DsDiag *diag);

char *ds_str_dup_range(const char *data, size_t len);
void *ds_xcalloc(size_t count, size_t size);
void *ds_xrealloc(void *ptr, size_t size);

#endif
