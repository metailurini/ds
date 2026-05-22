#ifndef DS_BASH_INTERNAL_H
#define DS_BASH_INTERNAL_H

#include "backend.h"
#include "ds_stdlib.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} EmitBuf;

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
    size_t temp_counter;
    size_t handler_counter;
    bool needs_case_types;
} BashEmitter;

void buf_reserve(EmitBuf *buf, size_t need);
void buf_append_len(EmitBuf *buf, const char *data, size_t len);
void buf_append(EmitBuf *buf, const char *text);
void buf_appendf(EmitBuf *buf, const char *fmt, ...);

void symbol_vec_push(SymbolVec *vec, DsStr name);
bool str_eq(DsStr a, const char *b);
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

bool emit_value_expr(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out);
bool emit_array_elements(BashEmitter *e, const DsLowerExprVec *elements, EmitBuf *out);
bool emit_map_entries(BashEmitter *e, const DsLowerMapEntryVec *entries, EmitBuf *out);
bool emit_call_arg_expr(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out);
bool emit_condition_operand(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out);
bool emit_condition(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out);
bool emit_call_args(BashEmitter *e, const DsLowerExprVec *args, EmitBuf *out);
bool emit_function_default(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out);

bool emit_command_word(BashEmitter *e, DsWord command_word, EmitBuf *out);
bool emit_redirect(BashEmitter *e, const DsRedirect *redirect, EmitBuf *out, DsSpan span);
bool emit_trace_redirect_args(BashEmitter *e, const DsRedirect *redirect, EmitBuf *out);
bool emit_capture_words(BashEmitter *e, const DsWordVec *words, EmitBuf *out, DsSpan span);
bool emit_capture_command(BashEmitter *e, const DsCommand *command, EmitBuf *out, DsSpan span);
bool emit_command_pipeline_stages(BashEmitter *e, const DsCommand *command, EmitBuf *out);
bool emit_command_pipeline(BashEmitter *e, const DsCommand *command, EmitBuf *out, DsSpan span);

bool program_has_command(const DsLowerProgram *program);
bool program_uses_run(const DsLowerProgram *program);
bool program_uses_pipeline_run(const DsLowerProgram *program);
bool program_uses_stdlib(const DsLowerProgram *program);
bool program_uses_collection_index(const DsLowerProgram *program);
bool program_uses_map_literal(const DsLowerProgram *program);
bool program_uses_case(const DsLowerProgram *program);
bool program_uses_int_helpers(const DsLowerProgram *program);
bool program_uses_function_value_helpers(const DsLowerProgram *program);
bool program_uses_handlers(const DsLowerProgram *program);

bool emit_block_body(BashEmitter *e, const DsLowerStmt *block, int indent);
bool emit_function(BashEmitter *e, const DsLowerFn *fn);
bool emit_stmt(BashEmitter *e, const DsLowerStmt *stmt, int indent);

#endif
