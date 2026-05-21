#ifndef DS_LOWER_INTERNAL_H
#define DS_LOWER_INTERNAL_H

#include "ds_hir.h"
#include "ds_runtime.h"
#include "ds_stdlib.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SYM_BOOL,
    SYM_INT,
    SYM_STRING,
    SYM_COMMAND_RESULT,
    SYM_ARRAY,
    SYM_MAP,
    SYM_FUNCTION,
    SYM_TOPLEVEL_PREDECLARED,
    SYM_UNKNOWN
} SymKind;

typedef struct {
    char *name;
    SymKind kind;
    SymKind element_kind;
} Symbol;

typedef struct Scope Scope;
struct Scope {
    Scope *parent;
    Symbol *items;
    size_t len;
    size_t cap;
};

typedef struct {
    DsDiag *diag;
    Scope *scope;
    DsLowerProgram *program;
    int loop_depth;
} Lower;

bool lower_str_eq(DsStr a, const char *b);
bool name_eq(DsStr a, const char *b);
bool is_env_name_text(DsStr name);
bool split_member_name(DsStr name, DsStr *ns, DsStr *member);
bool stdlib_return_kind(const DsStdlibHelper *helper, SymKind *kind);
DsStr str_clone(DsStr s);

void scope_init(Scope *scope, Scope *parent);
void scope_free(Scope *scope);
Symbol *scope_find_current(Scope *scope, DsStr name);
Symbol *scope_find(Scope *scope, DsStr name);
void scope_define(Lower *lower, Scope *scope, DsStr name, SymKind kind, DsSpan span);
void scope_define_array(Lower *lower, Scope *scope, DsStr name, SymKind kind, SymKind element_kind, DsSpan span);

DsLowerFn *find_function(DsLowerProgram *program, DsStr name);
int find_function_index(DsLowerProgram *program, DsStr name);

void lower_stmt_vec_push(DsLowerStmtVec *vec, DsLowerStmt *stmt);
void lower_expr_vec_push(DsLowerExprVec *vec, DsLowerExpr *expr);
void lower_fn_param_vec_push(DsLowerFnParamVec *vec, DsLowerFnParam param);
void lower_fn_vec_push(DsLowerFnVec *vec, DsLowerFn fn);
void lower_test_vec_push(DsLowerTestVec *vec, DsLowerTest test);
void lower_decl_vec_push(DsLowerScriptDeclVec *vec, DsLowerScriptDecl decl);
void lower_case_pattern_vec_push(DsLowerCasePatternVec *vec, DsLowerCasePattern pattern);
void lower_case_arm_vec_push(DsLowerCaseArmVec *vec, DsLowerCaseArm arm);

DsLowerExpr *expr_new(DsLowerExprKind kind, DsSpan span);
bool command_result_field_kind(DsStr field, SymKind *kind_out);
DsLowerValueKind lower_value_kind_from_sym(SymKind kind);
DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);
SymKind infer_lower_expr_kind(Lower *lower, const DsLowerExpr *expr);
SymKind infer_array_element_kind(Lower *lower, const DsLowerExpr *expr);
bool validate_cmd_word(Lower *lower, DsStr word, DsSpan span);
bool validate_interpolation(Lower *lower, DsStr text, DsSpan span);
void validate_glob_pattern_arg(Lower *lower, DsStr helper_name, const DsExpr *arg);
bool collection_element_supported_in_bash(const DsLowerExpr *expr);
bool lower_decode_string_text(DsStr text, DsStr *out);

bool lower_script_decl(Lower *lower, const DsScriptDecl *decl, DsLowerProgram *program);

DsLowerStmt *stmt_new(DsLowerStmtKind kind, DsSpan span);
DsLowerStmt *lower_call_stmt(Lower *lower, const DsStmt *stmt);
DsLowerStmt *lower_block(Lower *lower, const DsStmt *block, bool child_scope);
DsLowerStmt *lower_stmt(Lower *lower, const DsStmt *stmt);

void collect_function_signature(Lower *lower, const DsStmt *stmt, DsLowerProgram *program);
void collect_top_level_let_signature(Lower *lower, const DsStmt *stmt);
bool function_body_reaches(Lower *lower, size_t current_index, size_t target_index, bool *seen, DsSpan *cycle_span);
void reject_recursive_functions(Lower *lower);
void lower_function_body(Lower *lower, DsLowerFn *fn, const DsStmt *stmt);

void collect_test(Lower *lower, const DsStmt *stmt, DsLowerProgram *program);

void lower_expr_free(DsLowerExpr *expr);
void lower_stmt_free(DsLowerStmt *stmt);

#endif
