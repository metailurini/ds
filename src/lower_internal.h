#ifndef DS_LOWER_INTERNAL_H
#define DS_LOWER_INTERNAL_H

#include "ds_hir.h"
#include "ds_runtime.h"
#include "ds_stdlib.h"

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
    bool saw_scalar_array_value;
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

static inline bool lower_sym_kind_is_scalar(SymKind kind) {
    return kind == SYM_STRING || kind == SYM_INT || kind == SYM_BOOL;
}
static inline bool lower_value_kind_is_scalar(DsLowerValueKind kind) {
    return kind == DS_LOWER_VALUE_STRING || kind == DS_LOWER_VALUE_INT || kind == DS_LOWER_VALUE_BOOL;
}
static inline bool lower_op_is_arithmetic(DsStr op) {
    return ds_str_eq_cstr(op, "+") || ds_str_eq_cstr(op, "-") || ds_str_eq_cstr(op, "*") ||
           ds_str_eq_cstr(op, "/") || ds_str_eq_cstr(op, "%") || ds_str_eq_cstr(op, "**");
}
static inline bool lower_op_is_logical(DsStr op) {
    return ds_str_eq_cstr(op, "&&") || ds_str_eq_cstr(op, "||");
}
static inline bool lower_op_is_comparison(DsStr op) {
    return ds_str_eq_cstr(op, "==") || ds_str_eq_cstr(op, "!=") || ds_str_eq_cstr(op, ">") ||
           ds_str_eq_cstr(op, ">=") || ds_str_eq_cstr(op, "<") || ds_str_eq_cstr(op, "<=");
}
static inline bool lower_op_is_strict_comparison(DsStr op) {
    return lower_op_is_comparison(op) || ds_str_eq_cstr(op, "===") || ds_str_eq_cstr(op, "!==");
}
static inline bool lower_op_is_comparison_like(DsStr op) {
    return lower_op_is_comparison(op) || ds_str_eq_cstr(op, "in") || ds_str_eq_cstr(op, "matches");
}
static inline void lower_diag_stdlib_arity_error(Lower *lower, DsSpan span, DsStr name,
                                                  size_t min_arity, size_t max_arity, size_t actual) {
    if (min_arity == max_arity) {
        ds_diag_error(lower->diag, span, "helper `%.*s` expects %zu arguments but got %zu",
                      (int)name.len, name.data, min_arity, actual);
    } else {
        ds_diag_error(lower->diag, span, "helper `%.*s` expects %zu to %zu arguments but got %zu",
                      (int)name.len, name.data, min_arity, max_arity, actual);
    }
}
static inline void lower_diag_unknown_function(Lower *lower, DsSpan span, DsStr name) {
    ds_diag_error(lower->diag, span, "unknown function `%.*s`", (int)name.len, name.data);
}
static inline void lower_diag_unknown_stdlib_helper(Lower *lower, DsSpan span, DsStr name) {
    ds_diag_error(lower->diag, span, "unknown standard-library helper `%.*s`", (int)name.len, name.data);
}
static inline void lower_diag_unknown_string_method(Lower *lower, DsSpan span, DsStr member) {
    ds_diag_error(lower->diag, span, "unknown string method `%.*s`; supported methods are %s",
                  (int)member.len, member.data, ds_stdlib_string_method_names());
}
static inline DsLowerValueKind lower_fn_param_expected_kind(const DsLowerFnParam *param) {
    if (!param) return DS_LOWER_VALUE_UNKNOWN;
    return param->has_default ? param->default_kind : param->inferred_kind;
}
bool is_env_name_text(DsStr name);
static inline bool lower_validate_env_name(Lower *lower, DsStr name, DsSpan span, const char *version) {
    if (is_env_name_text(name)) return true;
    ds_diag_error(lower->diag, span, "invalid environment variable name `%.*s` in %s",
                  (int)name.len, ds_str_data(name), version);
    return false;
}
bool split_member_name(DsStr name, DsStr *ns, DsStr *member);
bool stdlib_return_kind(const DsStdlibHelper *helper, SymKind *kind);
DsLowerValueKind lower_stdlib_return_value_kind(const DsStdlibHelper *helper);
void scope_init(Scope *scope, Scope *parent);
void scope_free(Scope *scope);
Symbol *scope_find_current(Scope *scope, DsStr name);
Symbol *scope_find(Scope *scope, DsStr name);
Symbol *lower_resolve_value_symbol(Lower *lower, DsStr name, DsSpan span, const char *unknown_kind);
bool lower_validate_handler_capture(Lower *lower, const Symbol *sym, DsStr name, DsSpan span);
void scope_define(Lower *lower, Scope *scope, DsStr name, SymKind kind, DsSpan span);
void scope_define_array(Lower *lower, Scope *scope, DsStr name, SymKind kind, SymKind element_kind, DsSpan span);
void scope_define_row(Lower *lower, Scope *scope, DsStr name, DsLowerRowSchema schema, DsSpan span);
void scope_define_row_array(Lower *lower, Scope *scope, DsStr name, DsLowerRowSchema schema, DsSpan span);
void symbol_set_row(Symbol *sym, const DsLowerRowSchema *schema);
void symbol_set_row_array(Symbol *sym, const DsLowerRowSchema *schema);

DsLowerFn *find_function(DsLowerProgram *program, DsStr name);
int find_function_index(DsLowerProgram *program, DsStr name);

static inline DsLowerExpr *expr_new(DsLowerExprKind kind, DsSpan span) {
    DsLowerExpr *expr = (DsLowerExpr *)ds_xcalloc(1, sizeof(*expr));
    expr->kind = kind;
    expr->span = span;
    return expr;
}
bool command_result_field_kind(DsStr field, SymKind *kind_out);
bool lower_expr_produces_command_result(const DsLowerExpr *expr);
bool lower_expr_is_portable_command_result_return(const DsLowerExpr *expr);
DsLowerValueKind lower_value_kind_from_sym(SymKind kind);
SymKind sym_kind_from_lower_value_kind(DsLowerValueKind kind);
void validate_user_call_arg_kinds(Lower *lower, const DsLowerFn *fn, const DsExprVec *args, const SymKind *arg_kinds);
DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);
SymKind *lower_args_to_kinds(Lower *lower, const DsExprVec *args, DsLowerExprVec *out);
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
bool lower_collection_row_field_is_portable(const DsLowerExpr *expr);
bool lower_collection_for_iterable_is_portable(const DsLowerExpr *iterable);
bool lower_collection_map_for_iterable_is_portable(const DsLowerExpr *iterable);
void lower_reject_nonportable_collection_for_iterable(Lower *lower, DsSpan span);
DsStr lower_map_key_decode(const DsMapEntry *entry);

bool lower_script_decl(Lower *lower, const DsScriptDecl *decl, DsLowerProgram *program);

static inline DsLowerStmt *stmt_new(DsLowerStmtKind kind, DsSpan span) {
    DsLowerStmt *stmt = (DsLowerStmt *)ds_xcalloc(1, sizeof(*stmt));
    stmt->kind = kind;
    stmt->span = span;
    return stmt;
}
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
