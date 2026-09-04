#ifndef DS_HIR_H
#define DS_HIR_H

#include "ds_ast.h"
#include "ds_interpolation.h"

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
#include "generated/hir_expr_kinds.inc"
} DsLowerExprKind;

typedef struct DsLowerExpr DsLowerExpr;

typedef enum {
    DS_LOWER_COMMAND_WORD_LITERAL,
    DS_LOWER_COMMAND_WORD_VALUE
} DsLowerCommandWordKind;

typedef struct {
    DsLowerCommandWordKind kind;
    DsStr source_text;
    DsStr literal_text;
    DsLowerExpr *value;
    DsSpan span;
} DsLowerCommandWord;

typedef struct {
    DsLowerCommandWord *items;
    size_t len;
    size_t cap;
} DsLowerCommandWordVec;

typedef struct {
    DsLowerCommandWordVec words;
    DsSpan span;
} DsLowerCommandStage;

typedef struct {
    DsLowerCommandStage *items;
    size_t len;
    size_t cap;
} DsLowerCommandStageVec;

typedef struct {
    DsRedirectKind kind;
    DsStr source_target;
    DsStr literal_target;
    DsLowerExpr *target;
    DsSpan op_span;
    DsSpan target_span;
} DsLowerRedirect;

typedef struct {
    DsCommandKind kind;
    DsLowerCommandStageVec stages;
    DsLowerRedirect redirect;
    DsSpan span;
} DsLowerCommand;

bool ds_lower_command_is_pipeline(const DsLowerCommand *command);

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
#include "generated/hir_expr_union.inc"
    } as;
};

typedef enum {
#include "generated/hir_stmt_kinds.inc"
} DsLowerStmtKind;

typedef struct DsLowerStmt DsLowerStmt;

typedef DsAssignOp DsLowerAssignOp;

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
#include "generated/hir_stmt_union.inc"
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
void ds_lower_program_free(DsLowerProgram *program);
bool ds_hir_dump_program(const DsLowerProgram *program, FILE *out);
DsStr ds_lower_program_script_help(const DsSource *source, const DsLowerProgram *program);

#endif
