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
    DS_LOWER_EXPR_REGEX,
    DS_LOWER_EXPR_RUN,
    DS_LOWER_EXPR_FIELD,
    DS_LOWER_EXPR_UNARY,
    DS_LOWER_EXPR_BINARY,
    DS_LOWER_EXPR_CALL,
    DS_LOWER_EXPR_INTERP,
    DS_LOWER_EXPR_ARRAY,
    DS_LOWER_EXPR_MAP,
    DS_LOWER_EXPR_INDEX,
    DS_LOWER_EXPR_RANGE,
    DS_LOWER_EXPR_ERROR
} DsLowerExprKind;

typedef struct DsLowerExpr DsLowerExpr;

typedef enum {
    DS_LOWER_VALUE_UNKNOWN,
    DS_LOWER_VALUE_BOOL,
    DS_LOWER_VALUE_INT,
    DS_LOWER_VALUE_STRING,
    DS_LOWER_VALUE_ARRAY,
    DS_LOWER_VALUE_MAP,
    DS_LOWER_VALUE_COMMAND_RESULT
} DsLowerValueKind;

const char *ds_lower_value_kind_name(DsLowerValueKind kind);

typedef struct {
    DsStr name;
    DsLowerValueKind kind;
} DsLowerRowField;

typedef struct {
    DsLowerRowField *items;
    size_t len;
    size_t cap;
} DsLowerRowSchema;

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
        DsStr regex;
        DsCommand run;
        struct { DsLowerExpr *object; DsStr field; DsLowerValueKind field_kind; } field;
        struct { DsStr op; DsLowerExpr *right; } unary;
        struct { DsLowerExpr *left; DsStr op; DsLowerExpr *right; DsLowerValueKind left_kind; DsLowerValueKind right_kind; DsLowerValueKind right_element_kind; } binary;
        struct { DsStr name; DsLowerExprVec args; DsLowerValueKind return_kind; bool is_user_function; bool returns_row; bool returns_row_array; DsLowerRowSchema row_schema; } call;
        struct { DsLowerExprVec parts; } interp;
        struct { DsLowerExprVec elements; bool is_row_array; DsLowerRowSchema row_schema; } array;
        struct { DsLowerMapEntryVec entries; bool is_row; DsLowerRowSchema row_schema; } map;
        struct { DsLowerExpr *object; DsLowerExpr *index; bool object_is_array; bool object_is_map; bool map_key_literal; DsStr map_key; DsLowerValueKind element_kind; bool returns_row; DsLowerRowSchema row_schema; } index;
        struct { DsLowerExpr *start; DsLowerExpr *end; } range;
    } as;
};

typedef enum {
    DS_LOWER_STMT_LET,
    DS_LOWER_STMT_ASSIGN,
    DS_LOWER_STMT_INDEX_ASSIGN,
    DS_LOWER_STMT_IF,
    DS_LOWER_STMT_BLOCK,
    DS_LOWER_STMT_CMD,
    DS_LOWER_STMT_CALL,
    DS_LOWER_STMT_FOR_ARRAY,
    DS_LOWER_STMT_FOR_MAP,
    DS_LOWER_STMT_FOR_RANGE,
    DS_LOWER_STMT_WHILE,
    DS_LOWER_STMT_BREAK,
    DS_LOWER_STMT_CONTINUE,
    DS_LOWER_STMT_CASE,
    DS_LOWER_STMT_PUSH,
    DS_LOWER_STMT_ASSERT
    ,DS_LOWER_STMT_RETURN,
    DS_LOWER_STMT_DEFER,
    DS_LOWER_STMT_TRAP
} DsLowerStmtKind;

typedef struct DsLowerStmt DsLowerStmt;

typedef DsAssignOp DsLowerAssignOp;

const char *ds_lower_assign_op_name(DsLowerAssignOp op);

typedef DsCasePattern DsLowerCasePattern;
typedef DsCasePatternVec DsLowerCasePatternVec;

typedef struct {
    DsLowerCasePatternVec patterns;
    DsLowerStmt *body;
    DsSpan span;
} DsLowerCaseArm;

typedef struct {
    DsLowerCaseArm *items;
    size_t len;
    size_t cap;
} DsLowerCaseArmVec;

typedef struct {
    DsLowerStmt **items;
    size_t len;
    size_t cap;
} DsLowerStmtVec;

typedef struct {
    DsStr name;
    bool has_default;
    DsLowerValueKind default_kind;
    DsLowerValueKind inferred_kind;
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
    DsLowerValueKind return_kind;
    DsLowerValueKind return_element_kind;
    bool returns_row;
    bool returns_row_array;
    DsLowerRowSchema row_schema;
    bool has_return;
    bool all_paths_return;
    bool contains_plain_command;
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
        struct { DsStr name; DsLowerExpr *value; DsLowerValueKind value_kind; DsLowerValueKind element_kind; bool is_row; bool is_row_array; DsLowerRowSchema row_schema; } let_stmt;
        struct { DsStr name; DsLowerAssignOp op; DsLowerExpr *value; } assign_stmt;
        struct {
            DsStr name;
            DsLowerExpr *index;
            DsLowerExpr *value;
            bool target_is_array;
            bool target_is_map;
            DsLowerValueKind value_kind;
        } index_assign_stmt;
        struct { DsLowerExpr *condition; DsLowerStmt *then_branch; DsLowerStmt *else_branch; } if_stmt;
        struct { DsLowerStmtVec statements; bool scoped; } block_stmt;
        struct { DsStr name; DsLowerExprVec args; } call_stmt;
        struct { DsStr name; DsStr value_name; DsLowerExpr *iterable; DsLowerStmt *body; DsLowerValueKind element_kind; bool iterates_row_array; DsLowerRowSchema row_schema; } for_stmt;
        struct { DsLowerExpr *condition; DsLowerStmt *body; } while_stmt;
        struct { DsLowerExpr *selector; DsLowerCaseArmVec arms; } case_stmt;
        struct { DsStr name; DsLowerExpr *value; bool target_is_row_array; DsLowerRowSchema row_schema; } push_stmt;
        struct { DsLowerExpr *condition; } assert_stmt;
        struct { DsLowerExpr *value; DsLowerValueKind return_kind; bool returns_row_array; DsLowerRowSchema row_schema; } return_stmt;
        struct { DsHandlerSignal signal; DsLowerStmt *body; } handler_stmt;
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
DsStr ds_lower_program_script_help(const DsSource *source, const DsLowerProgram *program);

#endif
