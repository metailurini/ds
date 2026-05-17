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

typedef enum {
    DS_LOWER_EXPR_IDENT,
    DS_LOWER_EXPR_STRING,
    DS_LOWER_EXPR_INT,
    DS_LOWER_EXPR_BOOL,
    DS_LOWER_EXPR_UNARY,
    DS_LOWER_EXPR_BINARY,
    DS_LOWER_EXPR_ERROR
} DsLowerExprKind;

typedef struct DsLowerExpr DsLowerExpr;

struct DsLowerExpr {
    DsLowerExprKind kind;
    DsSpan span;
    union {
        DsStr text;
        bool boolean;
        struct { DsStr op; DsLowerExpr *right; } unary;
        struct { DsLowerExpr *left; DsStr op; DsLowerExpr *right; } binary;
    } as;
};

typedef enum {
    DS_LOWER_STMT_LET,
    DS_LOWER_STMT_IF,
    DS_LOWER_STMT_BLOCK,
    DS_LOWER_STMT_CMD
} DsLowerStmtKind;

typedef struct DsLowerStmt DsLowerStmt;

typedef struct {
    DsLowerStmt **items;
    size_t len;
    size_t cap;
} DsLowerStmtVec;

struct DsLowerStmt {
    DsLowerStmtKind kind;
    DsSpan span;
    union {
        struct { DsStr name; DsLowerExpr *value; } let_stmt;
        struct { DsLowerExpr *condition; DsLowerStmt *then_branch; DsLowerStmt *else_branch; } if_stmt;
        struct { DsLowerStmtVec statements; } block_stmt;
        struct { DsWordVec words; } cmd_stmt;
    } as;
};

typedef struct {
    DsLowerStmtVec statements;
    DsSpan span;
} DsLowerProgram;

typedef enum {
    DS_VALUE_NULL,
    DS_VALUE_BOOL,
    DS_VALUE_INT,
    DS_VALUE_STRING
} DsValueKind;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DsString;

typedef struct {
    DsValueKind kind;
    union {
        bool boolean;
        int64_t integer;
        DsString string;
    } as;
} DsValue;

typedef struct {
    void **items;
    size_t len;
    size_t cap;
} DsArray;

typedef struct {
    char **keys;
    DsValue *values;
    size_t len;
    size_t cap;
} DsMap;

typedef enum {
    DS_BYTECODE_MODE_DUMP,
    DS_BYTECODE_MODE_RUN
} DsBytecodeMode;

/* Source loading and diagnostics. */
bool ds_source_read(const char *path, DsSource *out, DsDiag *diag);
void ds_source_free(DsSource *source);

void ds_diag_init(DsDiag *diag, const DsSource *source);
void ds_diag_error(DsDiag *diag, DsSpan span, const char *fmt, ...);

/* Frontend: lexer, parser, and syntax debug output. */
const char *ds_token_kind_name(DsTokenKind kind);
bool ds_lex(const DsSource *source, DsTokenVec *out, DsDiag *diag);
void ds_tokens_free(DsTokenVec *tokens);
void ds_tokens_print(const DsTokenVec *tokens, FILE *out);

DsAst *ds_parse(const DsTokenVec *tokens, DsDiag *diag);
void ds_ast_print(const DsAst *ast, FILE *out);
void ds_ast_free(DsAst *ast);

/* Shared lowered representation consumed by both direct VM and Bash emission. */
DsLowerProgram *ds_lower_program(const DsAst *ast, DsDiag *diag);
bool ds_lower_validate(const DsAst *ast, DsDiag *diag);
void ds_lower_program_free(DsLowerProgram *program);

/* Standalone Bash backend. */
bool ds_emit_bash(const DsSource *source, const DsAst *ast, const char *output_path, DsDiag *diag);
bool ds_emit_bash_program(const DsSource *source, const DsLowerProgram *program, const char *output_path, DsDiag *diag);

/* Runtime primitives and ownership helpers. */
void ds_string_init(DsString *s);
bool ds_string_from_cstr(DsString *s, const char *text);
bool ds_string_from_range(DsString *s, const char *data, size_t len);
bool ds_string_append_range(DsString *s, const char *data, size_t len);
bool ds_string_append_cstr(DsString *s, const char *text);
bool ds_string_append_char(DsString *s, char c);
void ds_string_free(DsString *s);

DsValue ds_value_null(void);
DsValue ds_value_bool(bool value);
DsValue ds_value_int(int64_t value);
DsValue ds_value_string_take(DsString *string);
DsValue ds_value_copy(const DsValue *value);
void ds_value_free(DsValue *value);
bool ds_value_truthy(const DsValue *value, bool *out);
bool ds_value_to_string(const DsValue *value, DsString *out);
int ds_value_compare(const DsValue *left, const DsValue *right);

void ds_array_init(DsArray *array);
bool ds_array_push(DsArray *array, void *item);
void ds_array_clear(DsArray *array);
void ds_array_free(DsArray *array);

void ds_map_init(DsMap *map);
bool ds_map_set(DsMap *map, DsStr key, DsValue value);
DsValue *ds_map_get(DsMap *map, DsStr key);
void ds_map_clear(DsMap *map);
void ds_map_free(DsMap *map);

/* Bytecode and VM backend. */
bool ds_bytecode_dump(const DsSource *source, const DsAst *ast, FILE *out, DsDiag *diag);
bool ds_bytecode_dump_program(const DsSource *source, const DsLowerProgram *program, FILE *out, DsDiag *diag);
int ds_vm_run(const DsSource *source, const DsAst *ast, DsDiag *diag);
int ds_vm_run_program(const DsSource *source, const DsLowerProgram *program, DsDiag *diag);

/* Utility allocation helpers. These abort on allocation failure. */
char *ds_str_dup_range(const char *data, size_t len);
void *ds_xcalloc(size_t count, size_t size);
void *ds_xrealloc(void *ptr, size_t size);

#endif
