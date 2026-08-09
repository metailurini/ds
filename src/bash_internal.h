#ifndef DS_BASH_INTERNAL_H
#define DS_BASH_INTERNAL_H

#include "backend.h"
#include "ds_stdlib.h"

typedef DsString EmitBuf;

typedef struct {
    DsStr *items;
    size_t len;
    size_t cap;
} SymbolVec;

typedef struct {
    const DsSource *source;
    DsDiag *diag;
    SymbolVec symbols;
    EmitBuf out;
    int function_depth;
    int handler_depth;
    bool has_cleanup_helpers;
    bool has_signal_handlers;
    size_t temp_counter;
    size_t handler_counter;
    bool needs_case_types;
} BashEmitter;

void buf_append(EmitBuf *buf, const char *text);

const char *emit_buf_data(const EmitBuf *buf);
void buf_append_dsstr(EmitBuf *buf, DsStr value);
void emit_bash_decl_prefix(EmitBuf *out, int function_depth, const char *decl_flags);

bool bash_invariant_fail(BashEmitter *e, DsSpan span, const char *message);
bool symbol_exists(const SymbolVec *symbols, DsStr name);
void free_symbols(SymbolVec *symbols);
void symbols_truncate(SymbolVec *symbols, size_t len);

void emit_indent(EmitBuf *out, int indent);
bool is_safe_identifier(DsStr name);
void emit_var_name(EmitBuf *out, DsStr name);
void emit_fn_name(EmitBuf *out, DsStr name);
void emit_stdlib_helper_name(EmitBuf *out, DsStr name);
bool stdlib_returns_array(DsStr name);
void bash_single_quote(EmitBuf *out, const char *data, size_t len);
void emit_source_loc(EmitBuf *out, const DsSource *fallback, DsSpan span);
bool decode_string_literal(DsDiag *diag, const DsLowerExpr *expr, char **out_data, size_t *out_len);
bool emit_interpolated_string(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out);

const char *bash_lower_expr_static_type_name(const DsLowerExpr *expr);
void bash_emit_type_var_name(EmitBuf *out, DsStr name);
void bash_emit_elem_type_var_name(EmitBuf *out, DsStr name);
void bash_emit_map_value_type_var_name(EmitBuf *out, DsStr name);
void bash_emit_command_result_storage_decl(BashEmitter *e, DsStr name, int indent, bool local_decl);
DsStr bash_command_result_field_storage_name(DsStr field);
bool bash_command_result_field_is_bool(DsStr field);
void bash_emit_command_result_copy_to_return(BashEmitter *e, DsStr source, int indent);
void bash_emit_structured_target_decl(BashEmitter *e, DsStr name, DsLowerValueKind kind, int indent, bool local_decl);
void bash_emit_expr_type_value(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out);
void bash_emit_type_assignment(BashEmitter *e, DsStr name, const char *type, int indent, bool local_decl);
void bash_emit_type_assignment_for_expr(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent, bool local_decl);
void bash_emit_type_assignment_for_expr_required(BashEmitter *e, DsStr name, const DsLowerExpr *value, int indent, bool local_decl);
void bash_emit_return_type(BashEmitter *e, DsLowerValueKind kind, int indent);
bool bash_emit_array_return_payload(BashEmitter *e, const DsLowerExpr *value, DsSpan span, int indent);
bool bash_emit_map_return_payload(BashEmitter *e, const DsLowerExpr *value, DsSpan span, int indent);
void bash_emit_row_field_array_name(EmitBuf *out, DsStr array_name, DsStr field);
void bash_emit_return_row_field_array_name(EmitBuf *out, DsStr field);
const DsLowerMapEntry *bash_row_map_entry(const DsLowerExpr *row, DsStr field);
void bash_emit_row_array_decls(BashEmitter *e, DsStr name, const DsLowerRowSchema *schema, int indent, bool local_decl);
void bash_emit_row_scalar_sidecars_from_map(BashEmitter *e, DsStr name, const DsLowerRowSchema *schema, int indent);
bool bash_emit_row_array_literal(BashEmitter *e, DsStr name, const DsLowerExpr *array, const DsLowerRowSchema *schema, int indent, bool local_decl);
bool bash_emit_row_array_push_literal(BashEmitter *e, DsStr name, const DsLowerRowSchema *schema, const DsLowerExpr *row, int indent);
bool bash_emit_row_from_index(BashEmitter *e, DsStr dest, const DsLowerExpr *index_expr, const DsLowerRowSchema *schema, int indent, bool local_decl);
bool bash_emit_row_array_copy(BashEmitter *e, DsStr dest, DsStr src, const DsLowerRowSchema *schema, int indent, bool local_decl);
bool bash_emit_row_array_expr_into(BashEmitter *e, DsStr dest, const DsLowerExpr *value, const DsLowerRowSchema *schema, int indent, bool local_decl);
bool bash_emit_row_array_sort_call(BashEmitter *e, DsStr dest, const DsLowerExpr *call, const DsLowerRowSchema *schema, int indent, bool local_decl);
bool bash_emit_row_array_return_payload(BashEmitter *e, const DsLowerExpr *value, const DsLowerRowSchema *schema, DsSpan span, int indent);

bool emit_value_expr(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out);
bool emit_array_elements(BashEmitter *e, const DsLowerExprVec *elements, EmitBuf *out);
bool emit_map_entries(BashEmitter *e, const DsLowerMapEntryVec *entries, EmitBuf *out);
bool emit_call_arg_expr(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out);
bool emit_condition_operand(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out);
bool emit_condition(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out);
bool emit_call_args(BashEmitter *e, const DsLowerExprVec *args, EmitBuf *out);
bool emit_stdlib_call(BashEmitter *e, const DsLowerExpr *call, EmitBuf *out);
bool stdlib_array_call_uses_nul_records(const DsLowerExpr *call);
bool emit_user_call_args(BashEmitter *e, const DsLowerExprVec *args, EmitBuf *out);

bool bash_is_user_function_call_expr(const DsLowerExpr *expr);
void bash_temp_ds_name(char *buf, size_t cap, const char *prefix, size_t id);
bool bash_command_is_control(const DsCommand *command, const char *name);
bool bash_emit_user_call_into_raw_var(BashEmitter *e, const DsLowerExpr *expr, DsStr raw_name, int indent);
bool bash_emit_user_function_value_call_into(BashEmitter *e, DsStr name, const DsLowerExpr *call, int indent);
bool bash_emit_user_call_statement(BashEmitter *e, DsStr name, const DsLowerExprVec *args, int indent);
bool bash_emit_user_call_capture_return(BashEmitter *e, const DsLowerExpr *call, DsLowerValueKind return_kind, int indent);

bool bash_emit_user_call_to_temp(BashEmitter *e, const DsLowerExpr *expr,
    const char *label, EmitBuf *out, char *temp_buf, size_t buf_size,
    DsStr *temp_out, const DsStr **temp_ptr_out);

void bash_register_symbol(BashEmitter *e, DsStr name);

bool emit_command_word(BashEmitter *e, DsWord command_word, EmitBuf *out);
bool emit_redirect(BashEmitter *e, const DsRedirect *redirect, EmitBuf *out, DsSpan span);
bool emit_trace_redirect_args(BashEmitter *e, const DsRedirect *redirect, EmitBuf *out);
bool emit_capture_words(BashEmitter *e, const DsWordVec *words, EmitBuf *out, DsSpan span);
bool emit_capture_command(BashEmitter *e, const DsCommand *command, EmitBuf *out, DsSpan span);
bool bash_emit_capture_pipeline_assignment(BashEmitter *e, DsStr name, const DsCommand *command, DsSpan span, int indent);
bool emit_command_pipeline_stages(BashEmitter *e, const DsCommand *command, EmitBuf *out);
bool emit_command_pipeline(BashEmitter *e, const DsCommand *command, EmitBuf *out, DsSpan span);

bool program_has_command(const DsLowerProgram *program);
bool program_uses_run(const DsLowerProgram *program);
bool program_uses_pipeline_run(const DsLowerProgram *program);
bool program_uses_stdlib(const DsLowerProgram *program);
bool program_uses_stdlib_capture(const DsLowerProgram *program);
unsigned program_string_helper_mask(const DsLowerProgram *program);
bool program_uses_glob_helpers(const DsLowerProgram *program);
bool program_uses_recursive_glob_helpers(const DsLowerProgram *program);
bool program_uses_regex_base_helpers(const DsLowerProgram *program);
bool program_uses_regex_match_helpers(const DsLowerProgram *program);
bool program_uses_regex_replace_helpers(const DsLowerProgram *program);
bool program_uses_collection_index(const DsLowerProgram *program);
bool program_uses_array_helpers(const DsLowerProgram *program);
bool program_uses_map_helpers(const DsLowerProgram *program);
bool program_uses_map_iteration(const DsLowerProgram *program);
bool program_uses_map_assignment(const DsLowerProgram *program);
bool program_uses_map_literal(const DsLowerProgram *program);
bool program_uses_case(const DsLowerProgram *program);
bool program_uses_membership(const DsLowerProgram *program);
bool program_uses_function_param_types(const DsLowerProgram *program);
bool program_uses_int_helpers(const DsLowerProgram *program);
bool program_uses_function_value_helpers(const DsLowerProgram *program);
bool program_uses_handlers(const DsLowerProgram *program);
bool program_uses_signal_handlers(const DsLowerProgram *program);
bool program_uses_control_commands(const DsLowerProgram *program);

bool emit_block_body(BashEmitter *e, const DsLowerStmt *block, int indent);
bool emit_function(BashEmitter *e, const DsLowerFn *fn);
bool emit_stmt(BashEmitter *e, const DsLowerStmt *stmt, int indent);

#endif
