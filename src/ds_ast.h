#ifndef DS_AST_H
#define DS_AST_H

#include "ds_command.h"

typedef enum {
#include "generated/ast_expr_kinds.inc"
} DsExprKind;

typedef struct DsExpr DsExpr;

void ds_expr_free(DsExpr *expr);

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

typedef enum {
    DS_UNARY_NOT,
    DS_UNARY_NEGATE
} DsUnaryOp;

typedef enum {
    DS_BINARY_ADD,
    DS_BINARY_SUB,
    DS_BINARY_MUL,
    DS_BINARY_DIV,
    DS_BINARY_MOD,
    DS_BINARY_POW,
    DS_BINARY_AND,
    DS_BINARY_OR,
    DS_BINARY_EQ,
    DS_BINARY_NE,
    DS_BINARY_GT,
    DS_BINARY_GE,
    DS_BINARY_LT,
    DS_BINARY_LE,
    DS_BINARY_IN,
    DS_BINARY_MATCHES,
    DS_BINARY_INVALID
} DsBinaryOp;

struct DsExpr {
    DsExprKind kind;
    DsSpan span;
    union {
#include "generated/ast_expr_union.inc"
    } as;
};

const char *ds_unary_op_name(DsUnaryOp op);
const char *ds_binary_op_name(DsBinaryOp op);
DsBinaryOp ds_binary_op_from_text(DsStr text);
bool ds_binary_op_is_arithmetic(DsBinaryOp op);
bool ds_binary_op_is_logical(DsBinaryOp op);
bool ds_binary_op_is_comparison(DsBinaryOp op);
bool ds_binary_op_is_comparison_like(DsBinaryOp op);
DsExpr *ds_expr_new(DsExprKind kind, DsSpan span);

typedef enum {
#include "generated/ast_stmt_kinds.inc"
} DsStmtKind;

typedef enum {
    DS_HANDLER_EXIT,
    DS_HANDLER_INT,
    DS_HANDLER_TERM,
    DS_HANDLER_INVALID
} DsHandlerSignal;

typedef struct DsStmt DsStmt;

typedef enum {
    DS_ASSIGN_SET,
    DS_ASSIGN_ADD,
    DS_ASSIGN_SUB,
    DS_ASSIGN_MUL,
    DS_ASSIGN_DIV,
    DS_ASSIGN_MOD
} DsAssignOp;

const char *ds_assign_op_name(DsAssignOp op);
const char *ds_assign_binary_op(DsAssignOp op);

typedef enum {
    DS_CASE_PATTERN_STRING,
    DS_CASE_PATTERN_INT,
    DS_CASE_PATTERN_BOOL,
    DS_CASE_PATTERN_DEFAULT
} DsCasePatternKind;

typedef struct {
    DsCasePatternKind kind;
    DsStr text;
    bool boolean;
    DsSpan span;
} DsCasePattern;

typedef struct {
    DsCasePattern *items;
    size_t len;
    size_t cap;
} DsCasePatternVec;

void ds_case_pattern_fprint(FILE *out, const DsCasePattern *pattern);
void ds_case_pattern_vec_free(DsCasePatternVec *patterns);

typedef struct {
    DsCasePatternVec patterns;
    DsStmt *body;
    DsSpan span;
} DsCaseArm;

typedef struct {
    DsCaseArm *items;
    size_t len;
    size_t cap;
} DsCaseArmVec;

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

const char *ds_script_type_name(DsScriptType type);

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
#include "generated/ast_stmt_union.inc"
    } as;
};

DsStmt *ds_stmt_new(DsStmtKind kind, DsSpan span);

typedef struct {
    bool has_script;
    DsScriptBlock script;
    DsStmtVec statements;
    DsSpan span;
} DsAst;

void ds_ast_print(const DsAst *ast, FILE *out);
void ds_ast_free(DsAst *ast);

#endif
