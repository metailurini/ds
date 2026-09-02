#ifndef DS_LOWER_INTERNAL_H
#define DS_LOWER_INTERNAL_H

#include "lower_context.h"
#include "lower_kinds.h"
#include "lower_schema.h"
#include "ds_runtime.h"
#include "ds_stdlib.h"

void lower_diag_stdlib_arity_error(Lower *lower, DsSpan span, DsStr name,
                                   size_t min_arity, size_t max_arity, size_t actual);
void lower_diag_unknown_function(Lower *lower, DsSpan span, DsStr name);
void lower_diag_unknown_stdlib_helper(Lower *lower, DsSpan span, DsStr name);
void lower_diag_unknown_string_method(Lower *lower, DsSpan span, DsStr member);
bool is_env_name_text(DsStr name);
bool lower_validate_env_name(Lower *lower, DsStr name, DsSpan span, const char *version);
bool split_member_name(DsStr name, DsStr *ns, DsStr *member);
DsLowerExpr *expr_new(DsLowerExprKind kind, DsSpan span);
bool lower_expr_produces_command_result(const DsLowerExpr *expr);
bool lower_expr_is_portable_command_result_return(const DsLowerExpr *expr);
void validate_user_call_arg_kinds(Lower *lower, const DsLowerFn *fn, const DsExprVec *args, const SymKind *arg_kinds);
DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);
SymKind *lower_args_to_kinds(Lower *lower, const DsExprVec *args, DsLowerExprVec *out);
DsLowerExpr *lower_string_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);
SymKind infer_lower_expr_kind(Lower *lower, const DsLowerExpr *expr);
SymKind infer_array_element_kind(Lower *lower, const DsLowerExpr *expr);
SymKind infer_map_value_kind(Lower *lower, const DsLowerExpr *expr);
bool lower_expr_row_schema(const DsLowerExpr *expr, const DsLowerRowSchema **schema_out);
bool lower_expr_row_array_schema(const DsLowerExpr *expr, const DsLowerRowSchema **schema_out);
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

DsLowerStmt *stmt_new(DsLowerStmtKind kind, DsSpan span);
DsLowerStmt *lower_call_stmt(Lower *lower, const DsStmt *stmt);
DsLowerStmt *lower_block(Lower *lower, const DsStmt *block, bool child_scope);
DsLowerStmt *lower_stmt(Lower *lower, const DsStmt *stmt);

void lower_expr_free(DsLowerExpr *expr);
void lower_stmt_free(DsLowerStmt *stmt);

#endif
