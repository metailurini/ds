#ifndef DS_AST_H
#define DS_AST_H

#include "ds_command.h"

typedef enum {
    DS_EXPR_IDENT,
    DS_EXPR_STRING,
    DS_EXPR_INT,
    DS_EXPR_BOOL,
    DS_EXPR_RUN,
    DS_EXPR_FIELD,
    DS_EXPR_UNARY,
    DS_EXPR_BINARY,
    DS_EXPR_CALL,
    DS_EXPR_ARRAY,
    DS_EXPR_MAP,
    DS_EXPR_INDEX,
    DS_EXPR_ERROR
} DsExprKind;

typedef struct DsExpr DsExpr;

typedef struct {
    DsExpr **items;
    size_t len;
    size_t cap;
} DsExprVec;

typedef struct {
    DsStr key;
    bool quoted_key;
    DsExpr *value;
    DsSpan span;
} DsMapEntry;

typedef struct {
    DsMapEntry *items;
    size_t len;
    size_t cap;
} DsMapEntryVec;

struct DsExpr {
    DsExprKind kind;
    DsSpan span;
    union {
        DsStr text;
        bool boolean;
        struct { DsWordVec words; } run;
        struct { DsExpr *object; DsStr field; } field;
        struct { DsStr op; DsExpr *right; } unary;
        struct { DsExpr *left; DsStr op; DsExpr *right; } binary;
        struct { DsStr name; DsExprVec args; } call;
        struct { DsExprVec elements; } array;
        struct { DsMapEntryVec entries; } map;
        struct { DsExpr *object; DsExpr *index; } index;
    } as;
};

typedef enum {
    DS_STMT_LET,
    DS_STMT_IF,
    DS_STMT_BLOCK,
    DS_STMT_CMD,
    DS_STMT_IMPORT,
    DS_STMT_FN,
    DS_STMT_CALL,
    DS_STMT_FOR,
    DS_STMT_PUSH,
    DS_STMT_TEST,
    DS_STMT_ASSERT
} DsStmtKind;

typedef struct DsStmt DsStmt;

typedef enum {
    DS_SCRIPT_DECL_ARG,
    DS_SCRIPT_DECL_OPTION,
    DS_SCRIPT_DECL_FLAG
} DsScriptDeclKind;

typedef enum {
    DS_SCRIPT_TYPE_STRING,
    DS_SCRIPT_TYPE_INT,
    DS_SCRIPT_TYPE_BOOL
} DsScriptType;

typedef struct {
    DsScriptDeclKind kind;
    DsScriptType type;
    DsStr name;
    DsExpr *default_value;
    DsSpan span;
} DsScriptDecl;

typedef struct {
    DsScriptDecl *items;
    size_t len;
    size_t cap;
} DsScriptDeclVec;

typedef struct {
    DsScriptDeclVec declarations;
    DsSpan span;
} DsScriptBlock;

typedef struct {
    DsStmt **items;
    size_t len;
    size_t cap;
} DsStmtVec;

typedef struct {
    DsStr name;
    bool has_type;
    DsScriptType type;
    DsExpr *default_value;
    DsSpan span;
} DsFnParam;

typedef struct {
    DsFnParam *items;
    size_t len;
    size_t cap;
} DsFnParamVec;

struct DsStmt {
    DsStmtKind kind;
    DsSpan span;
    union {
        struct { DsStr name; DsExpr *value; } let_stmt;
        struct { DsExpr *condition; DsStmt *then_branch; DsStmt *else_branch; } if_stmt;
        struct { DsStmtVec statements; } block_stmt;
        struct { DsWordVec words; DsRedirect redirect; } cmd_stmt;
        struct { DsStr path; } import_stmt;
        struct { DsStr name; DsFnParamVec params; DsStmt *body; } fn_stmt;
        struct { DsStr name; DsExprVec args; } call_stmt;
        struct { DsStr key_name; DsStr value_name; bool has_value_name; DsExpr *iterable; DsStmt *body; } for_stmt;
        struct { DsStr name; DsExpr *value; } push_stmt;
        struct { DsStr name; DsStmt *body; } test_stmt;
        struct { DsExpr *condition; } assert_stmt;
    } as;
};

typedef struct {
    bool has_script;
    DsScriptBlock script;
    DsStmtVec statements;
    DsSpan span;
} DsAst;

void ds_ast_print(const DsAst *ast, FILE *out);
void ds_ast_free(DsAst *ast);

#endif
