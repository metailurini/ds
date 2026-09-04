#ifndef DS_LOWER_SYMBOLS_H
#define DS_LOWER_SYMBOLS_H

#include "ds_hir.h"

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

typedef struct Symbol {
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

typedef struct Lower Lower;

void scope_init(Scope *scope, Scope *parent);
void scope_free(Scope *scope);
Symbol *scope_find_current(Scope *scope, DsStr name);
Symbol *scope_find(Scope *scope, DsStr name);
Symbol *lower_resolve_value_symbol(Lower *lower, DsStr name, DsSpan span,
                                   const char *unknown_kind);
bool lower_validate_handler_capture(Lower *lower, const Symbol *sym, DsStr name, DsSpan span);
void scope_define(Lower *lower, Scope *scope, DsStr name, SymKind kind, DsSpan span);
void scope_define_array(Lower *lower, Scope *scope, DsStr name, SymKind kind,
                        SymKind element_kind, DsSpan span);
void scope_define_row(Lower *lower, Scope *scope, DsStr name,
                      DsLowerRowSchema schema, DsSpan span);
void scope_define_row_array(Lower *lower, Scope *scope, DsStr name,
                            DsLowerRowSchema schema, DsSpan span);
void symbol_set_row(Symbol *sym, const DsLowerRowSchema *schema);
void symbol_set_row_array(Symbol *sym, const DsLowerRowSchema *schema);

DsLowerFn *find_function(DsLowerProgram *program, DsStr name);

bool is_env_name_text(DsStr name);
bool split_member_name(DsStr name, DsStr *ns, DsStr *member);

/* Symbol preparation phases owned by semantic lowering. */
void lower_symbols_predeclare_top_level_bindings(Lower *lower, const DsAst *ast);

#endif
