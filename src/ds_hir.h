#ifndef DS_HIR_H
#define DS_HIR_H

#include "ds_ast.h"

typedef struct {
    DsScriptDeclKind kind;
    DsScriptType type;
    DsStr name;
    bool has_default;
    DsStr default_text;
    int64_t default_int;
    bool default_bool;
    DsSpan span;
} DsLowerScriptDecl;

typedef struct {
    DsLowerScriptDecl *items;
    size_t len;
    size_t cap;
} DsLowerScriptDeclVec;

typedef enum {
    DS_LOWER_EXPR_IDENT,
    DS_LOWER_EXPR_STRING,
    DS_LOWER_EXPR_INT,
    DS_LOWER_EXPR_BOOL,
    DS_LOWER_EXPR_RUN,
    DS_LOWER_EXPR_FIELD,
    DS_LOWER_EXPR_UNARY,
    DS_LOWER_EXPR_BINARY,
    DS_LOWER_EXPR_CALL,
    DS_LOWER_EXPR_ARRAY,
    DS_LOWER_EXPR_MAP,
    DS_LOWER_EXPR_INDEX,
    DS_LOWER_EXPR_ERROR
} DsLowerExprKind;

typedef struct DsLowerExpr DsLowerExpr;

typedef struct {
    DsLowerExpr **items;
    size_t len;
    size_t cap;
} DsLowerExprVec;

typedef struct {
    DsStr key;
    DsLowerExpr *value;
    DsSpan span;
} DsLowerMapEntry;

typedef struct {
    DsLowerMapEntry *items;
    size_t len;
    size_t cap;
} DsLowerMapEntryVec;

struct DsLowerExpr {
    DsLowerExprKind kind;
    DsSpan span;
    union {
        DsStr text;
        bool boolean;
        DsCommand run;
        struct { DsLowerExpr *object; DsStr field; } field;
        struct { DsStr op; DsLowerExpr *right; } unary;
        struct { DsLowerExpr *left; DsStr op; DsLowerExpr *right; } binary;
        struct { DsStr name; DsLowerExprVec args; } call;
        struct { DsLowerExprVec elements; } array;
        struct { DsLowerMapEntryVec entries; } map;
        struct { DsLowerExpr *object; DsLowerExpr *index; bool object_is_array; bool object_is_map; bool map_key_literal; DsStr map_key; } index;
    } as;
};

typedef enum {
    DS_LOWER_STMT_LET,
    DS_LOWER_STMT_IF,
    DS_LOWER_STMT_BLOCK,
    DS_LOWER_STMT_CMD,
    DS_LOWER_STMT_CALL,
    DS_LOWER_STMT_FOR_ARRAY,
    DS_LOWER_STMT_PUSH,
    DS_LOWER_STMT_ASSERT
} DsLowerStmtKind;

typedef struct DsLowerStmt DsLowerStmt;

typedef struct {
    DsLowerStmt **items;
    size_t len;
    size_t cap;
} DsLowerStmtVec;

typedef struct {
    DsStr name;
    bool has_default;
    DsLowerExpr *default_value;
    DsSpan span;
} DsLowerFnParam;

typedef struct {
    DsLowerFnParam *items;
    size_t len;
    size_t cap;
} DsLowerFnParamVec;

typedef struct {
    DsStr name;
    DsLowerFnParamVec params;
    DsLowerStmt *body;
    size_t required_count;
    DsSpan span;
} DsLowerFn;

typedef struct {
    DsLowerFn *items;
    size_t len;
    size_t cap;
} DsLowerFnVec;

typedef struct {
    DsStr name;
    DsLowerStmt *body;
    DsSpan span;
} DsLowerTest;

typedef struct {
    DsLowerTest *items;
    size_t len;
    size_t cap;
} DsLowerTestVec;

struct DsLowerStmt {
    DsLowerStmtKind kind;
    DsSpan span;
    union {
        struct { DsStr name; DsLowerExpr *value; } let_stmt;
        struct { DsLowerExpr *condition; DsLowerStmt *then_branch; DsLowerStmt *else_branch; } if_stmt;
        struct { DsLowerStmtVec statements; } block_stmt;
        struct { DsStr name; DsLowerExprVec args; } call_stmt;
        struct { DsStr name; DsLowerExpr *iterable; DsLowerStmt *body; } for_stmt;
        struct { DsStr name; DsLowerExpr *value; } push_stmt;
        struct { DsLowerExpr *condition; } assert_stmt;
        DsCommand cmd_stmt;
    } as;
};

typedef struct {
    bool has_script;
    DsLowerScriptDeclVec script_decls;
    DsLowerFnVec functions;
    DsLowerTestVec tests;
    DsLowerStmtVec statements;
    DsSpan span;
} DsLowerProgram;

DsLowerProgram *ds_lower_program(const DsAst *ast, DsDiag *diag);
bool ds_lower_validate(const DsAst *ast, DsDiag *diag);
void ds_lower_program_free(DsLowerProgram *program);
bool ds_hir_dump_program(const DsLowerProgram *program, FILE *out);

#endif
