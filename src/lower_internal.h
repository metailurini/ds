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
    bool is_row;
    bool is_row_array;
    DsLowerRowSchema row_schema;
    bool dynamic_scalar;
    int function_depth;
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
    int function_depth;
    int handler_depth;
    int handler_function_depth;
    DsLowerFn *current_function;
    size_t temp_counter;
    Symbol **map_loop_symbols;
    size_t map_loop_len;
    size_t map_loop_cap;
} Lower;

bool lower_str_eq(DsStr a, const char *b);
bool name_eq(DsStr a, const char *b);
bool is_env_name_text(DsStr name);
bool split_member_name(DsStr name, DsStr *ns, DsStr *member);
bool stdlib_return_kind(const DsStdlibHelper *helper, SymKind *kind);
DsLowerValueKind lower_stdlib_return_value_kind(const DsStdlibHelper *helper);
DsStr str_clone(DsStr s);

void scope_init(Scope *scope, Scope *parent);
void scope_free(Scope *scope);
Symbol *scope_find_current(Scope *scope, DsStr name);
Symbol *scope_find(Scope *scope, DsStr name);
bool lower_validate_handler_capture(Lower *lower, const Symbol *sym, DsStr name, DsSpan span);
void scope_define(Lower *lower, Scope *scope, DsStr name, SymKind kind, DsSpan span);
void scope_define_array(Lower *lower, Scope *scope, DsStr name, SymKind kind, SymKind element_kind, DsSpan span);
void scope_define_row(Lower *lower, Scope *scope, DsStr name, DsLowerRowSchema schema, DsSpan span);
void scope_define_row_array(Lower *lower, Scope *scope, DsStr name, DsLowerRowSchema schema, DsSpan span);
void symbol_set_row(Symbol *sym, const DsLowerRowSchema *schema);
void symbol_set_row_array(Symbol *sym, const DsLowerRowSchema *schema);

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
bool lower_expr_produces_command_result(const DsLowerExpr *expr);
bool lower_expr_is_portable_command_result_return(const DsLowerExpr *expr);
DsLowerValueKind lower_value_kind_from_sym(SymKind kind);
SymKind sym_kind_from_lower_value_kind(DsLowerValueKind kind);
void validate_user_call_arg_kinds(Lower *lower, const DsLowerFn *fn, const DsExprVec *args, const SymKind *arg_kinds);
DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);
DsLowerExpr *lower_string_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);
SymKind infer_lower_expr_kind(Lower *lower, const DsLowerExpr *expr);
SymKind infer_array_element_kind(Lower *lower, const DsLowerExpr *expr);
SymKind infer_map_value_kind(Lower *lower, const DsLowerExpr *expr);
void row_schema_init(DsLowerRowSchema *schema);
void row_schema_free(DsLowerRowSchema *schema);
bool row_schema_clone(const DsLowerRowSchema *src, DsLowerRowSchema *dst);
bool row_schema_push(DsLowerRowSchema *schema, DsStr name, DsLowerValueKind kind);
const DsLowerRowField *row_schema_find(const DsLowerRowSchema *schema, DsStr name);
bool row_schema_equal(const DsLowerRowSchema *a, const DsLowerRowSchema *b);
bool lower_expr_row_schema(const DsLowerExpr *expr, const DsLowerRowSchema **schema_out);
bool lower_expr_row_array_schema(const DsLowerExpr *expr, const DsLowerRowSchema **schema_out);
bool lower_expr_is_row(const DsLowerExpr *expr);
bool lower_expr_is_row_array(const DsLowerExpr *expr);
bool lower_map_expr_schema(Lower *lower, const DsLowerExpr *expr, DsLowerRowSchema *schema_out);
/*
 * M3.4 command-word ownership boundary:
 * parser preserves command words as syntax text; lowering owns command-word and
 * interpolation acceptance. VM/Bash should only receive words that passed these
 * validators or were normalized into private temporary bindings.
 */
bool lower_validate_command_word(Lower *lower, DsStr word, DsSpan span);
bool lower_validate_word_interpolation(Lower *lower, DsStr text, DsSpan span);
bool lower_materialize_command_value_call_interpolation(Lower *lower, DsCommand *command, DsLowerStmt *block);
void validate_glob_pattern_arg(Lower *lower, DsStr helper_name, const DsExpr *arg);
bool lower_collection_receiver_is_portable_storage(const DsLowerExpr *expr);
void lower_validate_portable_collection_receiver(Lower *lower, const DsLowerExpr *expr, DsSpan span);
bool lower_collection_index_is_portable(const DsLowerExpr *expr, bool map_index);
void lower_validate_portable_collection_index(Lower *lower, const DsLowerExpr *expr, bool map_index, DsSpan span);
bool lower_collection_element_is_portable(const DsLowerExpr *expr);
bool lower_collection_for_iterable_is_portable(const DsLowerExpr *iterable);
bool lower_collection_map_for_iterable_is_portable(const DsLowerExpr *iterable);
void lower_reject_nonportable_collection_for_iterable(Lower *lower, DsSpan span);
bool lower_decode_string_text(DsStr text, DsStr *out);

bool lower_script_decl(Lower *lower, const DsScriptDecl *decl, DsLowerProgram *program);

DsLowerStmt *stmt_new(DsLowerStmtKind kind, DsSpan span);
DsLowerStmt *lower_call_stmt(Lower *lower, const DsStmt *stmt);
DsLowerStmt *lower_block(Lower *lower, const DsStmt *block, bool child_scope);
DsLowerStmt *lower_stmt(Lower *lower, const DsStmt *stmt);

void collect_function_signature(Lower *lower, const DsStmt *stmt, DsLowerProgram *program);
void collect_top_level_let_signature(Lower *lower, const DsStmt *stmt);
void predeclare_function_return_contracts(Lower *lower, const DsAst *ast);
void infer_function_parameter_kinds(Lower *lower, const DsAst *ast);
bool function_body_reaches(Lower *lower, size_t current_index, size_t target_index, bool *seen, DsSpan *cycle_span);
void reject_recursive_functions(Lower *lower);
void lower_function_body(Lower *lower, DsLowerFn *fn, const DsStmt *stmt);

void lower_expr_free(DsLowerExpr *expr);
void lower_stmt_free(DsLowerStmt *stmt);

#endif
