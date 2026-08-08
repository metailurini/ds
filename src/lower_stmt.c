#include "lower_internal.h"

static bool lower_validate_handler_signal(Lower *lower, const DsStmt *stmt) {
    if (stmt->as.handler_stmt.signal != DS_HANDLER_INVALID) return true;
    /*
     * Trap/defer/signal ownership: the parser preserves the quoted signal
     * token as syntax, while lowering owns the language-level supported-signal
     * set shared by VM and emitted Bash.
     */
    const char *form = stmt->kind == DS_STMT_TRAP ? "trap" : "defer on:";
    DsStr text = stmt->as.handler_stmt.signal_text;
    ds_diag_error(lower->diag, stmt->span,
                  "unsupported %s signal `%.*s`; supported signals are EXIT, INT, and TERM",
                  form, (int)text.len, ds_str_data(text));
    return false;
}

static bool expr_is_env_namespace(const DsExpr *expr) {
    return expr && expr->kind == DS_EXPR_IDENT && lower_str_eq(expr->as.text, "env");
}

static bool expr_is_env_value_access(const DsExpr *expr) {
    return expr && expr->kind == DS_EXPR_FIELD && expr_is_env_namespace(expr->as.field.object);
}

DsLowerStmt *lower_call_stmt(Lower *lower, const DsStmt *stmt) {
    DsLowerStmt *out = stmt_new(DS_LOWER_STMT_CALL, stmt->span);
    out->as.call_stmt.name = str_clone(stmt->as.call_stmt.name);
    const DsStdlibHelper *stdlib_helper = ds_stdlib_lookup(stmt->as.call_stmt.name);
    bool stdlib = stdlib_helper != NULL;
    DsLowerFn *fn = stdlib ? NULL : find_function(lower->program, stmt->as.call_stmt.name);
    if (stdlib_helper) {
        SymKind ret = SYM_UNKNOWN;
        if (!stdlib_return_kind(stdlib_helper, &ret)) {
            lower_diag_unknown_stdlib_helper(lower, stmt->span, stmt->as.call_stmt.name);
        } else if (!ds_stdlib_arity_ok(stdlib_helper, stmt->as.call_stmt.args.len)) {
            lower_diag_stdlib_arity_error(lower, stmt->span, stmt->as.call_stmt.name,
                                          stdlib_helper->min_arity, stdlib_helper->max_arity,
                                          stmt->as.call_stmt.args.len);
        } else if (!stdlib_helper->statement_only) {
            ds_diag_error(lower->diag, stmt->span, "helper `%.*s` returns a value in v0.11.0; assign it with `let` or use it in an expression", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
        }
    } else if (!fn) {
        DsStr ns = {0}, member = {0};
        bool has_member = split_member_name(stmt->as.call_stmt.name, &ns, &member);
        if (has_member && ds_str_eq_cstr(ns, "string")) {
            lower_diag_unknown_string_method(lower, stmt->span, member);
        } else if (has_member && ds_stdlib_is_namespace(ns)) lower_diag_unknown_stdlib_helper(lower, stmt->span, stmt->as.call_stmt.name);
        else if (has_member) ds_diag_error(lower->diag, stmt->span, "only `push` collection method is supported in v0.10.0");
        else lower_diag_unknown_function(lower, stmt->span, stmt->as.call_stmt.name);
    } else if (stmt->as.call_stmt.args.len < fn->required_count || stmt->as.call_stmt.args.len > fn->params.len) {
        if (fn->required_count == fn->params.len) {
            ds_diag_error(lower->diag, stmt->span, "function `%.*s` expects %zu arguments but got %zu",
                          (int)fn->name.len, fn->name.data, fn->params.len, stmt->as.call_stmt.args.len);
        } else {
            ds_diag_error(lower->diag, stmt->span, "function `%.*s` expects %zu to %zu arguments but got %zu",
                          (int)fn->name.len, fn->name.data, fn->required_count, fn->params.len, stmt->as.call_stmt.args.len);
        }
    }
    SymKind *arg_kinds = stmt->as.call_stmt.args.len ? (SymKind *)ds_xcalloc(stmt->as.call_stmt.args.len, sizeof(SymKind)) : NULL;
    for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) {
        SymKind arg_kind = SYM_UNKNOWN;
        lower_expr_vec_push(&out->as.call_stmt.args, lower_expr(lower, stmt->as.call_stmt.args.items[i], &arg_kind));
        arg_kinds[i] = arg_kind;
        if (stdlib && arg_kind != SYM_STRING && arg_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, stmt->as.call_stmt.args.items[i]->span, "standard-library helper `%.*s` expects string arguments in v0.11.0", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
        } else if (arg_kind == SYM_ARRAY || arg_kind == SYM_MAP) {
            ds_diag_error(lower->diag, stmt->as.call_stmt.args.items[i]->span,
                          "passing collection values to functions is deferred in v0.10.0; index or bind scalar values instead");
        }
    }
    if (stdlib_helper && stdlib_helper->validates_env_name && stmt->as.call_stmt.args.len > 0 && stmt->as.call_stmt.args.items[0]->kind == DS_EXPR_STRING) {
        DsStr decoded = {0};
        if (ds_decode_string_text(stmt->as.call_stmt.args.items[0]->as.text, &decoded)) {
            lower_validate_env_name(lower, decoded, stmt->as.call_stmt.args.items[0]->span, "v0.11.0");
            free(decoded.data);
        }
    }
    if (fn) validate_user_call_arg_kinds(lower, fn, &stmt->as.call_stmt.args, arg_kinds);
    free(arg_kinds);
    if (stdlib && stmt->as.call_stmt.args.len > 0) validate_glob_pattern_arg(lower, stmt->as.call_stmt.name, stmt->as.call_stmt.args.items[0]);
    return out;
}

DsLowerStmt *lower_block(Lower *lower, const DsStmt *block, bool child_scope) {
    DsLowerStmt *out = stmt_new(DS_LOWER_STMT_BLOCK, block->span);
    out->as.block_stmt.scoped = child_scope;
    Scope *saved = lower->scope;
    Scope *local = NULL;
    if (child_scope) {
        local = (Scope *)ds_xcalloc(1, sizeof(Scope));
        scope_init(local, saved);
        lower->scope = local;
    }
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) {
        lower_stmt_vec_push(&out->as.block_stmt.statements, lower_stmt(lower, block->as.block_stmt.statements.items[i]));
    }
    if (child_scope) {
        lower->scope = saved;
        scope_free(local);
        free(local);
    }
    return out;
}

static void maybe_update_symbol(Symbol *sym, SymKind kind) {
    if (!sym) return;
    if (kind != SYM_UNKNOWN && sym->kind != SYM_TOPLEVEL_PREDECLARED) sym->kind = kind;
}

static void lower_temp_scope_begin(Lower *lower, Scope *scope, Scope **saved) {
    *saved = lower->scope;
    scope_init(scope, *saved);
    lower->scope = scope;
}

static void lower_temp_scope_end(Lower *lower, Scope *scope, Scope *saved) {
    lower->scope = saved;
    scope_free(scope);
}

typedef struct {
    DsStmtKind outer_kind;
} RunMaterializeCtx;

static DsLowerValueKind lower_infer_collection_element_kind(Lower *lower, const DsLowerExpr *expr, SymKind kind);
static void lower_apply_let_schema(Lower *lower, DsLowerStmt *out, DsStr name, SymKind kind, DsSpan span);

static DsLowerStmt *lower_materialize_run_in_stmt(Lower *lower, const DsCommand *command, DsSpan span, Scope *temp_scope, Scope **saved_scope, DsCommand *command_copy) {
    ds_command_clone(command_copy, command);
    DsLowerStmt *block = stmt_new(DS_LOWER_STMT_BLOCK, span);
    block->as.block_stmt.scoped = false;
    memset(temp_scope, 0, sizeof(*temp_scope));
    *saved_scope = NULL;
    lower_temp_scope_begin(lower, temp_scope, saved_scope);
    if (lower_materialize_command_value_call_interpolation(lower, command_copy, block)) return block;
    lower_temp_scope_end(lower, temp_scope, *saved_scope);
    lower_stmt_free(block);
    return NULL;
}

static DsExpr lower_run_copy_expr(const DsExpr *source, DsCommand command) {
    DsExpr expr;
    memset(&expr, 0, sizeof(expr));
    expr.kind = DS_EXPR_RUN;
    expr.span = source->span;
    expr.as.run = command;
    return expr;
}

static DsLowerStmt *lower_let_with_value(Lower *lower, const DsStmt *stmt, const DsExpr *value) {
    SymKind kind = SYM_UNKNOWN;
    DsLowerStmt *out = stmt_new(DS_LOWER_STMT_LET, stmt->span);
    out->as.let_stmt.name = str_clone(stmt->as.let_stmt.name);
    out->as.let_stmt.value = lower_expr(lower, value, &kind);
    out->as.let_stmt.value_kind = lower_value_kind_from_sym(kind);
    out->as.let_stmt.element_kind = lower_infer_collection_element_kind(lower, out->as.let_stmt.value, kind);
    lower_apply_let_schema(lower, out, stmt->as.let_stmt.name, kind, stmt->span);
    return out;
}

static bool pattern_equal(const DsLowerCasePattern *a, const DsLowerCasePattern *b) {
    if (a->kind != b->kind) return false;
    if (a->kind == DS_CASE_PATTERN_BOOL) return a->boolean == b->boolean;
    if (a->kind == DS_CASE_PATTERN_DEFAULT) return true;
    return ds_str_eq(a->text, b->text);
}

static void validate_case_pattern_duplicate(Lower *lower, const DsLowerCaseArmVec *arms, const DsLowerCasePattern *pattern) {
    for (size_t i = 0; i < arms->len; i++) {
        const DsLowerCaseArm *arm = &arms->items[i];
        for (size_t j = 0; j < arm->patterns.len; j++) {
            if (pattern_equal(&arm->patterns.items[j], pattern)) {
                ds_diag_error(lower->diag, pattern->span, "duplicate case pattern in v0.17.0");
                return;
            }
        }
    }
}

static void validate_case_pattern_duplicate_in_arm(Lower *lower, const DsLowerCasePatternVec *patterns, const DsLowerCasePattern *pattern) {
    for (size_t i = 0; i < patterns->len; i++) {
        if (pattern_equal(&patterns->items[i], pattern)) {
            ds_diag_error(lower->diag, pattern->span, "duplicate case pattern in v0.17.0");
            return;
        }
    }
}

static bool lower_return_value_has_portable_backend_representation(DsLowerValueKind return_kind, const DsLowerExpr *value) {
    if (!value) return false;
    if (value->kind == DS_LOWER_EXPR_CALL && value->as.call.is_user_function) return true;
    switch (return_kind) {
        case DS_LOWER_VALUE_ARRAY:
            return value->kind == DS_LOWER_EXPR_ARRAY || value->kind == DS_LOWER_EXPR_IDENT ||
                   (value->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(value->as.call.name));
        case DS_LOWER_VALUE_MAP:
            return value->kind == DS_LOWER_EXPR_MAP || value->kind == DS_LOWER_EXPR_IDENT ||
                   (value->kind == DS_LOWER_EXPR_CALL && lower_str_eq(value->as.call.name, "regex.match"));
        case DS_LOWER_VALUE_COMMAND_RESULT:
            return lower_expr_is_portable_command_result_return(value);
        case DS_LOWER_VALUE_STRING:
        case DS_LOWER_VALUE_INT:
        case DS_LOWER_VALUE_BOOL:
        case DS_LOWER_VALUE_UNKNOWN:
            return true;
    }
    return false;
}

static void lower_validate_return_backend_representation(Lower *lower, const DsLowerStmt *stmt) {
    if (!stmt || stmt->kind != DS_LOWER_STMT_RETURN) return;
    /*
     * Structured function returns are part of the VM/Bash ABI. The VM can
     * carry arbitrary temporary structured values, but standalone Bash only has
     * portable return payloads for literals, named values, run captures, and
     * forwarded user-function calls. Keep that acceptance rule in lowering so
     * backend emitters do not become semantic validators.
     */
    if (!lower_return_value_has_portable_backend_representation(stmt->as.return_stmt.return_kind, stmt->as.return_stmt.value)) {
        ds_diag_error(lower->diag, stmt->as.return_stmt.value ? stmt->as.return_stmt.value->span : stmt->span,
                      "structured function returns require a literal, named value, run capture, or forwarded user-function call for VM/Bash parity in v0.26.0; bind unsupported temporary values first");
    }
}

static void lower_validate_function_return_contract(Lower *lower, const DsStmt *src, DsLowerStmt *out, SymKind value_kind) {
    out->as.return_stmt.return_kind = lower_value_kind_from_sym(value_kind);
    lower_validate_return_backend_representation(lower, out);

    if (!lower->current_function) return;

    DsLowerValueKind ret = lower_value_kind_from_sym(value_kind);
    bool is_array = ret == DS_LOWER_VALUE_ARRAY;
    bool is_map = ret == DS_LOWER_VALUE_MAP;

    if (ret == DS_LOWER_VALUE_UNKNOWN) {
        ds_diag_error(lower->diag, src->span, "function return value kind must be known in v0.21.0");
    } else if (!lower->current_function->has_return) {
        lower->current_function->has_return = true;
        lower->current_function->return_kind = ret;
        if (is_array || is_map) {
            lower->current_function->return_element_kind = lower_value_kind_from_sym(
                is_array ? infer_array_element_kind(lower, out->as.return_stmt.value) :
                           infer_map_value_kind(lower, out->as.return_stmt.value));
            const DsLowerRowSchema *schema = NULL;
            if (out->as.return_stmt.value && out->as.return_stmt.value->kind == DS_LOWER_EXPR_IDENT) {
                Symbol *sym = scope_find(lower->scope, out->as.return_stmt.value->as.text);
                if (sym && (is_array ? sym->is_row_array : sym->is_row)) schema = &sym->row_schema;
            }
            if (!schema) {
                if (is_array) lower_expr_row_array_schema(out->as.return_stmt.value, &schema);
                else lower_expr_row_schema(out->as.return_stmt.value, &schema);
            }
            if (schema) {
                if (is_array) {
                    out->as.return_stmt.returns_row_array = true;
                    row_schema_clone(schema, &out->as.return_stmt.row_schema);
                    lower->current_function->returns_row_array = true;
                } else {
                    lower->current_function->returns_row = true;
                }
                row_schema_clone(schema, &lower->current_function->row_schema);
            }
        }
    } else if (lower->current_function->return_kind != ret) {
        ds_diag_error(lower->diag, src->span, "all return statements in a function must have the same value kind in v0.21.0");
    } else if (is_array || is_map) {
        DsLowerValueKind element = lower_value_kind_from_sym(
            is_array ? infer_array_element_kind(lower, out->as.return_stmt.value) :
                       infer_map_value_kind(lower, out->as.return_stmt.value));
        if (lower->current_function->return_element_kind == DS_LOWER_VALUE_UNKNOWN) lower->current_function->return_element_kind = element;
        else if (element != DS_LOWER_VALUE_UNKNOWN && lower->current_function->return_element_kind != element) lower->current_function->return_element_kind = DS_LOWER_VALUE_UNKNOWN;
        const DsLowerRowSchema *schema = NULL;
        if (out->as.return_stmt.value && out->as.return_stmt.value->kind == DS_LOWER_EXPR_IDENT) {
            Symbol *sym = scope_find(lower->scope, out->as.return_stmt.value->as.text);
            if (sym && (is_array ? sym->is_row_array : sym->is_row)) schema = &sym->row_schema;
        }
        if (!schema) {
            if (is_array) lower_expr_row_array_schema(out->as.return_stmt.value, &schema);
            else lower_expr_row_schema(out->as.return_stmt.value, &schema);
        }
        bool *fn_returns_row = is_array ? &lower->current_function->returns_row_array : &lower->current_function->returns_row;
        if (!*fn_returns_row && schema) {
            if (is_array) {
                out->as.return_stmt.returns_row_array = true;
                row_schema_clone(schema, &out->as.return_stmt.row_schema);
                lower->current_function->returns_row_array = true;
            } else {
                lower->current_function->returns_row = true;
            }
            row_schema_clone(schema, &lower->current_function->row_schema);
        } else if (is_array && *fn_returns_row && schema && !row_schema_equal(&lower->current_function->row_schema, schema)) {
            ds_diag_error(lower->diag, src->span, "all row-array returns in a function must have the same field names and scalar kinds in v0.37.0");
        } else if (!is_array && *fn_returns_row && schema && !row_schema_equal(&lower->current_function->row_schema, schema)) {
            ds_diag_error(lower->diag, src->span, "all row returns in a function must have the same field names and scalar kinds in v0.37.0");
        } else if (is_array && schema) {
            out->as.return_stmt.returns_row_array = true;
            row_schema_clone(schema, &out->as.return_stmt.row_schema);
        }
    }
}

static DsLowerStmt *lower_return_with_value(Lower *lower, const DsStmt *stmt, const DsExpr *value) {
    DsLowerStmt *out = stmt_new(DS_LOWER_STMT_RETURN, stmt->span);
    if (lower->handler_depth > 0) {
        ds_diag_error(lower->diag, stmt->span, "`return` from a cleanup handler is not supported in v0.22.0; move the return into a function called by the handler or use `exit`");
    }
    if (lower->function_depth <= 0 || !lower->current_function) {
        ds_diag_error(lower->diag, stmt->span, "`return` is only allowed inside a function");
    }
    SymKind value_kind = SYM_UNKNOWN;
    out->as.return_stmt.value = lower_expr(lower, value, &value_kind);
    lower_validate_function_return_contract(lower, stmt, out, value_kind);
    return out;
}

static DsLowerCasePattern lower_case_pattern(const DsCasePattern *pattern) {
    DsLowerCasePattern out;
    memset(&out, 0, sizeof(out));
    out.span = pattern->span;
    out.boolean = pattern->boolean;
    switch (pattern->kind) {
        case DS_CASE_PATTERN_STRING: out.kind = DS_CASE_PATTERN_STRING; out.text = str_clone(pattern->text); break;
        case DS_CASE_PATTERN_INT: out.kind = DS_CASE_PATTERN_INT; out.text = str_clone(pattern->text); break;
        case DS_CASE_PATTERN_BOOL: out.kind = DS_CASE_PATTERN_BOOL; break;
        case DS_CASE_PATTERN_DEFAULT: out.kind = DS_CASE_PATTERN_DEFAULT; break;
    }
    return out;
}

static bool lower_push_map_loop_symbol(Lower *lower, Symbol *symbol) {
    if (!symbol) return false;
    DS_GROW_ARRAY(lower->map_loop_symbols, lower->map_loop_len, lower->map_loop_cap, 4);
    lower->map_loop_symbols[lower->map_loop_len++] = symbol;
    return true;
}

static void lower_pop_map_loop_symbol(Lower *lower) {
    if (lower->map_loop_len == 0) return;
    lower->map_loop_len--;
    lower->map_loop_symbols[lower->map_loop_len] = NULL;
}

static bool lower_in_map_loop_for_symbol(const Lower *lower, const Symbol *symbol) {
    if (!symbol) return false;
    for (size_t i = 0; i < lower->map_loop_len; i++) {
        if (lower->map_loop_symbols[i] == symbol) return true;
    }
    return false;
}

static bool lowered_expr_has_dynamic_scalar_value(Lower *lower, const DsLowerExpr *expr) {
    if (!expr) return false;
    if (expr->kind == DS_LOWER_EXPR_IDENT) {
        Symbol *sym = scope_find(lower->scope, expr->as.text);
        return sym && sym->dynamic_scalar;
    }
    if (expr->kind == DS_LOWER_EXPR_INDEX) {
        return expr->as.index.object_is_array || expr->as.index.object_is_map;
    }
    return false;
}

static void lower_update_collection_element_kind(Symbol *sym, SymKind value_kind) {
    if (!sym || value_kind == SYM_UNKNOWN) return;
    if (sym->element_kind == SYM_UNKNOWN) sym->element_kind = value_kind;
    else if (sym->element_kind != value_kind) sym->element_kind = SYM_UNKNOWN;
}

static DsLowerValueKind lower_infer_collection_element_kind(Lower *lower, const DsLowerExpr *expr, SymKind kind) {
    if (kind == SYM_ARRAY) return lower_value_kind_from_sym(infer_array_element_kind(lower, expr));
    if (kind == SYM_MAP) return lower_value_kind_from_sym(infer_map_value_kind(lower, expr));
    return DS_LOWER_VALUE_UNKNOWN;
}

static void lower_apply_let_schema(Lower *lower, DsLowerStmt *out, DsStr name, SymKind kind, DsSpan span) {
    const DsLowerRowSchema *schema = NULL;
    bool is_collection = (kind == SYM_ARRAY || kind == SYM_MAP);
    if (out->as.let_stmt.value && out->as.let_stmt.value->kind == DS_LOWER_EXPR_IDENT) {
        Symbol *src = scope_find(lower->scope, out->as.let_stmt.value->as.text);
        if (src && ((kind == SYM_ARRAY && src->is_row_array) || (kind == SYM_MAP && src->is_row))) schema = &src->row_schema;
    }
    if (!schema && kind == SYM_ARRAY) lower_expr_row_array_schema(out->as.let_stmt.value, &schema);
    else if (!schema && kind == SYM_MAP) lower_expr_row_schema(out->as.let_stmt.value, &schema);
    if (is_collection && schema) {
        bool is_array = kind == SYM_ARRAY;
        if (is_array) out->as.let_stmt.is_row_array = true;
        else out->as.let_stmt.is_row = true;
        row_schema_clone(schema, &out->as.let_stmt.row_schema);
        if (is_array) scope_define_row_array(lower, lower->scope, name, out->as.let_stmt.row_schema, span);
        else scope_define_row(lower, lower->scope, name, out->as.let_stmt.row_schema, span);
    } else {
        SymKind element_kind = is_collection ? (kind == SYM_ARRAY ? infer_array_element_kind(lower, out->as.let_stmt.value) :
                                                                   infer_map_value_kind(lower, out->as.let_stmt.value)) : SYM_UNKNOWN;
        scope_define_array(lower, lower->scope, name, kind, element_kind, span);
        Symbol *sym = scope_find_current(lower->scope, name);
        if (sym && kind == SYM_ARRAY && out->as.let_stmt.value && out->as.let_stmt.value->kind == DS_LOWER_EXPR_ARRAY &&
            out->as.let_stmt.value->as.array.elements.len > 0 && !out->as.let_stmt.value->as.array.is_row_array) {
            sym->saw_scalar_array_value = true;
        }
    }
}

static bool expr_is_negative_int_literal(const DsExpr *expr) {
    return expr && expr->kind == DS_EXPR_UNARY && lower_str_eq(expr->as.unary.op, "-") &&
           expr->as.unary.right && expr->as.unary.right->kind == DS_EXPR_INT;
}

static DsLowerStmt *lower_index_assign_stmt(Lower *lower, const DsStmt *stmt) {
    DsLowerStmt *out = stmt_new(DS_LOWER_STMT_INDEX_ASSIGN, stmt->span);
    const DsExpr *target = stmt->as.index_assign_stmt.target;
    const DsExpr *object = NULL;
    const DsExpr *index_expr = NULL;

    if (stmt->as.index_assign_stmt.op != DS_ASSIGN_SET) {
        ds_diag_error(lower->diag, stmt->span,
                      "compound index assignment is unsupported in v0.30.0; use `target[index] = value`");
    }

    if (!target || target->kind != DS_EXPR_INDEX) {
        if (target && target->kind == DS_EXPR_FIELD) {
            ds_diag_error(lower->diag, target->span,
                          "field-style map assignment is deferred in v0.30.0; use bracket syntax like `map[key] = value`");
        } else {
            ds_diag_error(lower->diag, stmt->span,
                          "index assignment target must be a named array or map element in v0.30.0");
        }
        SymKind tmp_kind = SYM_UNKNOWN;
        out->as.index_assign_stmt.index = expr_new(DS_LOWER_EXPR_ERROR, stmt->span);
        out->as.index_assign_stmt.value = lower_expr(lower, stmt->as.index_assign_stmt.value, &tmp_kind);
        return out;
    }

    object = target->as.index.object;
    index_expr = target->as.index.index;
    if (!object || object->kind != DS_EXPR_IDENT) {
        if (object && object->kind == DS_EXPR_FIELD && object->as.field.object &&
            object->as.field.object->kind == DS_EXPR_IDENT &&
            lower_str_eq(object->as.field.object->as.text, "env")) {
            ds_diag_error(lower->diag, object->span,
                          "environment values are scalar strings, not mutable arrays in v0.30.0");
        } else if (object && object->kind == DS_EXPR_FIELD && object->as.field.object &&
                   object->as.field.object->kind == DS_EXPR_IDENT) {
            Symbol *field_receiver = scope_find(lower->scope, object->as.field.object->as.text);
            if (field_receiver && field_receiver->kind == SYM_COMMAND_RESULT) {
                ds_diag_error(lower->diag, object->span,
                              "command-result fields are not mutable collection targets in v0.30.0");
            } else {
                ds_diag_error(lower->diag, object->span,
                              "nested or field-based index assignment targets are deferred in v0.30.0; assign only to named flat arrays or maps");
            }
        } else if (object && object->kind == DS_EXPR_FIELD) {
            ds_diag_error(lower->diag, object->span,
                          "nested or field-based index assignment targets are deferred in v0.30.0; assign only to named flat arrays or maps");
        } else if (object && object->kind == DS_EXPR_CALL) {
            ds_diag_error(lower->diag, object->span,
                          "function-result index assignment is deferred in v0.30.0; bind the collection to a variable first");
        } else {
            ds_diag_error(lower->diag, target->span,
                          "index assignment target must be a named flat array or map in v0.30.0");
        }
        SymKind idx_kind = SYM_UNKNOWN, value_kind = SYM_UNKNOWN;
        out->as.index_assign_stmt.index = lower_expr(lower, index_expr, &idx_kind);
        out->as.index_assign_stmt.value = lower_expr(lower, stmt->as.index_assign_stmt.value, &value_kind);
        out->as.index_assign_stmt.value_kind = lower_value_kind_from_sym(value_kind);
        return out;
    }

    out->as.index_assign_stmt.name = str_clone(object->as.text);
    Symbol *sym = scope_find(lower->scope, object->as.text);
    if (!sym) {
        ds_diag_error(lower->diag, object->span,
                      "index assignment target `%.*s` is not defined; use `let` to declare a collection", (int)object->as.text.len, object->as.text.data);
    } else if (sym->kind == SYM_ARRAY) {
        out->as.index_assign_stmt.target_is_array = true;
    } else if (sym->kind == SYM_MAP) {
        out->as.index_assign_stmt.target_is_map = true;
        if (lower_in_map_loop_for_symbol(lower, sym)) {
            ds_diag_error(lower->diag, object->span,
                          "mutating the map currently being iterated is unsupported in v0.30.0; mutate a different map or collect changes first");
        }
    } else if (sym->kind == SYM_UNKNOWN || sym->kind == SYM_TOPLEVEL_PREDECLARED) {
        ds_diag_error(lower->diag, object->span,
                      "index assignment target kind must be a known array or map in v0.30.0");
    } else {
        ds_diag_error(lower->diag, object->span,
                      "index assignment target `%.*s` must be a named array or map in v0.30.0", (int)object->as.text.len, object->as.text.data);
    }
    if (sym) lower_validate_handler_capture(lower, sym, object->as.text, object->span);

    SymKind idx_kind = SYM_UNKNOWN;
    out->as.index_assign_stmt.index = lower_expr(lower, index_expr, &idx_kind);
    if (out->as.index_assign_stmt.target_is_array) {
        lower_validate_portable_collection_index(lower, out->as.index_assign_stmt.index, false, index_expr ? index_expr->span : target->span);
        if (idx_kind != SYM_INT && idx_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, index_expr ? index_expr->span : target->span, "array index assignment requires an int index in v0.30.0");
        }
        if (expr_is_negative_int_literal(index_expr)) {
            ds_diag_error(lower->diag, index_expr->span, "array index assignment requires a non-negative index in v0.30.0");
        }
    } else if (out->as.index_assign_stmt.target_is_map) {
        lower_validate_portable_collection_index(lower, out->as.index_assign_stmt.index, true, index_expr ? index_expr->span : target->span);
        if (index_expr && index_expr->kind == DS_EXPR_STRING) {
            DsStr decoded = {0};
            if (ds_decode_string_text(index_expr->as.text, &decoded)) {
                if (decoded.len == 0) ds_diag_error(lower->diag, index_expr->span, "empty map keys are deferred in v0.30.0");
                free(decoded.data);
            }
        } else if (idx_kind != SYM_STRING && idx_kind != SYM_UNKNOWN) {
            ds_diag_error(lower->diag, index_expr ? index_expr->span : target->span, "map index assignment requires a string key in v0.30.0");
        }
    }

    SymKind value_kind = SYM_UNKNOWN;
    out->as.index_assign_stmt.value = lower_expr(lower, stmt->as.index_assign_stmt.value, &value_kind);
    bool dynamic_scalar_rhs = value_kind == SYM_UNKNOWN && lowered_expr_has_dynamic_scalar_value(lower, out->as.index_assign_stmt.value);
    if (value_kind == SYM_ARRAY || value_kind == SYM_MAP || value_kind == SYM_COMMAND_RESULT) {
        ds_diag_error(lower->diag, stmt->as.index_assign_stmt.value->span,
                      "index assignment value must be a flat scalar in v0.30.0; nested collections and command results are deferred");
    } else if (value_kind == SYM_UNKNOWN && !dynamic_scalar_rhs) {
        ds_diag_error(lower->diag, stmt->as.index_assign_stmt.value->span,
                      "index assignment value kind must be a known scalar in v0.30.0; bind unsupported values first");
    }
    out->as.index_assign_stmt.value_kind = lower_value_kind_from_sym(value_kind);
    if (sym && (out->as.index_assign_stmt.target_is_array || out->as.index_assign_stmt.target_is_map)) {
        if (dynamic_scalar_rhs) sym->element_kind = SYM_UNKNOWN;
        else lower_update_collection_element_kind(sym, value_kind);
        if (out->as.index_assign_stmt.target_is_map && sym->is_row) {
            sym->is_row = false;
            row_schema_free(&sym->row_schema);
            row_schema_init(&sym->row_schema);
        }
    }
    return out;
}

DsLowerStmt *lower_stmt(Lower *lower, const DsStmt *stmt) {
    switch (stmt->kind) {
        case DS_STMT_LET: {
            if (stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == DS_EXPR_RUN) {
                DsCommand command_copy;
                Scope temp_scope;
                Scope *saved_scope = NULL;
                DsLowerStmt *block = lower_materialize_run_in_stmt(lower, &stmt->as.let_stmt.value->as.run, stmt->span, &temp_scope, &saved_scope, &command_copy);
                if (block) {
                    DsExpr fake = lower_run_copy_expr(stmt->as.let_stmt.value, command_copy);
                    DsLowerStmt *out = lower_let_with_value(lower, stmt, &fake);
                    lower_temp_scope_end(lower, &temp_scope, saved_scope);
                    lower_stmt_vec_push(&block->as.block_stmt.statements, out);
                    ds_command_free(&command_copy);
                    return block;
                }
                ds_command_free(&command_copy);
            }
            return lower_let_with_value(lower, stmt, stmt->as.let_stmt.value);
        }
        case DS_STMT_ASSIGN: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_ASSIGN, stmt->span);
            out->as.assign_stmt.name = str_clone(stmt->as.assign_stmt.name);
            out->as.assign_stmt.op = stmt->as.assign_stmt.op;
            bool env_assign = stmt->as.assign_stmt.name.len > 4 && ds_str_has_prefix_cstr(stmt->as.assign_stmt.name, "env.");
            if (env_assign) {
                DsStr env_name = {stmt->as.assign_stmt.name.data + 4, stmt->as.assign_stmt.name.len - 4};
                lower_validate_env_name(lower, env_name, stmt->span, "v0.27.0");
                if (stmt->as.assign_stmt.op != DS_ASSIGN_SET) {
                    ds_diag_error(lower->diag, stmt->span, "environment assignment supports only `=` in v0.27.0");
                }
                SymKind value_kind = SYM_UNKNOWN;
                out->as.assign_stmt.value = lower_expr(lower, stmt->as.assign_stmt.value, &value_kind);
                if (!lower_sym_kind_is_scalar(value_kind) && value_kind != SYM_UNKNOWN) {
                    ds_diag_error(lower->diag, stmt->as.assign_stmt.value->span, "environment variable assignment requires a scalar value in v0.27.0");
                }
                return out;
            }
            Symbol *sym = scope_find(lower->scope, stmt->as.assign_stmt.name);
            if (!sym) {
                ds_diag_error(lower->diag, stmt->span, "assignment target `%.*s` is not defined; use `let` to declare variables", (int)stmt->as.assign_stmt.name.len, stmt->as.assign_stmt.name.data);
            } else if (sym->kind == SYM_ARRAY || sym->kind == SYM_MAP) {
                ds_diag_error(lower->diag, stmt->span, "collection reassignment is deferred in v0.17.0");
            } else {
                lower_validate_handler_capture(lower, sym, stmt->as.assign_stmt.name, stmt->span);
            }
            SymKind value_kind = SYM_UNKNOWN;
            out->as.assign_stmt.value = lower_expr(lower, stmt->as.assign_stmt.value, &value_kind);
            if (value_kind == SYM_ARRAY || value_kind == SYM_MAP) {
                ds_diag_error(lower->diag, stmt->as.assign_stmt.value->span, "collection reassignment is deferred in v0.17.0");
            }
            if (stmt->as.assign_stmt.op != DS_ASSIGN_SET) {
                if (sym && sym->kind != SYM_INT && sym->kind != SYM_UNKNOWN && sym->kind != SYM_TOPLEVEL_PREDECLARED) {
                    ds_diag_error(lower->diag, stmt->span, "compound arithmetic assignment supports only integer variables in v0.21.0");
                }
                if (value_kind != SYM_INT && value_kind != SYM_UNKNOWN) {
                    ds_diag_error(lower->diag, stmt->as.assign_stmt.value->span, "compound arithmetic assignment supports only integer values in v0.21.0");
                }
            }
            if (stmt->as.assign_stmt.op == DS_ASSIGN_SET) maybe_update_symbol(sym, value_kind);
            else maybe_update_symbol(sym, SYM_INT);
            return out;
        }
        case DS_STMT_INDEX_ASSIGN:
            return lower_index_assign_stmt(lower, stmt);
        case DS_STMT_CMD: {
            DsCommand command_copy;
            Scope temp_scope;
            Scope *saved_scope = NULL;
            DsLowerStmt *block = lower_materialize_run_in_stmt(lower, &stmt->as.cmd_stmt, stmt->span, &temp_scope, &saved_scope, &command_copy);
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_CMD, stmt->span);
            ds_command_clone(&out->as.cmd_stmt, &command_copy);
            for (size_t s = 0; s < command_copy.stages.len; s++) {
                if (command_copy.stages.items[s].words.len == 0) ds_diag_error(lower->diag, command_copy.stages.items[s].span, "empty pipeline stage");
                for (size_t i = 0; i < command_copy.stages.items[s].words.len; i++) {
                    lower_validate_command_word(lower, command_copy.stages.items[s].words.items[i].text, command_copy.stages.items[s].words.items[i].span);
                }
            }
            if (command_copy.redirect.kind != DS_REDIRECT_NONE) {
                if (command_copy.redirect.target.len == 0) {
                    ds_diag_error(lower->diag, command_copy.redirect.op_span, "expected redirection target");
                } else {
                    lower_validate_word_interpolation(lower, command_copy.redirect.target, command_copy.redirect.target_span);
                }
            }
            ds_command_free(&command_copy);
            lower_temp_scope_end(lower, &temp_scope, saved_scope);
            if (block) {
                lower_stmt_vec_push(&block->as.block_stmt.statements, out);
                return block;
            }
            lower_stmt_free(block);
            return out;
        }
        case DS_STMT_CALL:
            return lower_call_stmt(lower, stmt);
        case DS_STMT_IF: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_IF, stmt->span);
            SymKind cond_kind = SYM_UNKNOWN;
            out->as.if_stmt.condition = lower_expr(lower, stmt->as.if_stmt.condition, &cond_kind);
            if (cond_kind != SYM_BOOL && cond_kind != SYM_UNKNOWN) {
                ds_diag_error(lower->diag, stmt->as.if_stmt.condition->span, "condition expression must be bool");
            }
            out->as.if_stmt.then_branch = lower_block(lower, stmt->as.if_stmt.then_branch, true);
            if (stmt->as.if_stmt.else_branch) out->as.if_stmt.else_branch = lower_block(lower, stmt->as.if_stmt.else_branch, true);
            return out;
        }
        case DS_STMT_FOR: {
            DsLowerStmtKind loop_kind = stmt->as.for_stmt.has_value_name ? DS_LOWER_STMT_FOR_MAP :
                                        (stmt->as.for_stmt.iterable && stmt->as.for_stmt.iterable->kind == DS_EXPR_RANGE ? DS_LOWER_STMT_FOR_RANGE : DS_LOWER_STMT_FOR_ARRAY);
            DsLowerStmt *out = stmt_new(loop_kind, stmt->span);
            out->as.for_stmt.name = str_clone(stmt->as.for_stmt.key_name);
            if (stmt->as.for_stmt.has_value_name) out->as.for_stmt.value_name = str_clone(stmt->as.for_stmt.value_name);
            if (stmt->as.for_stmt.has_value_name && ds_str_eq(stmt->as.for_stmt.key_name, stmt->as.for_stmt.value_name)) {
                ds_diag_error(lower->diag, stmt->span, "map loop key and value variables must be different in v0.29.0");
            }
            SymKind element_kind = SYM_UNKNOWN;
            if (out->kind == DS_LOWER_STMT_FOR_RANGE) {
                const DsExpr *range = stmt->as.for_stmt.iterable;
                DsLowerExpr *lowered_range = expr_new(DS_LOWER_EXPR_RANGE, range->span);
                SymKind start_kind = SYM_UNKNOWN, end_kind = SYM_UNKNOWN;
                lowered_range->as.range.start = lower_expr(lower, range->as.range.start, &start_kind);
                lowered_range->as.range.end = lower_expr(lower, range->as.range.end, &end_kind);
                if (start_kind != SYM_INT && start_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, range->as.range.start->span, "range start must be an int in v0.23.0");
                if (end_kind != SYM_INT && end_kind != SYM_UNKNOWN) ds_diag_error(lower->diag, range->as.range.end->span, "range end must be an int in v0.23.0");
                out->as.for_stmt.iterable = lowered_range;
                element_kind = SYM_INT;
            } else if (out->kind == DS_LOWER_STMT_FOR_MAP) {
                SymKind iterable_kind = SYM_UNKNOWN;
                bool env_namespace_iterable = expr_is_env_namespace(stmt->as.for_stmt.iterable);
                bool env_value_iterable = expr_is_env_value_access(stmt->as.for_stmt.iterable);
                if (env_namespace_iterable) {
                    out->as.for_stmt.iterable = expr_new(DS_LOWER_EXPR_IDENT, stmt->as.for_stmt.iterable->span);
                    out->as.for_stmt.iterable->as.text = str_clone(stmt->as.for_stmt.iterable->as.text);
                } else {
                    out->as.for_stmt.iterable = lower_expr(lower, stmt->as.for_stmt.iterable, &iterable_kind);
                }
                if (env_namespace_iterable) {
                    ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span,
                                  "environment iteration is unsupported in v0.29.0; `env` is a namespace, not a map");
                } else if (env_value_iterable) {
                    ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span,
                                  "environment iteration is unsupported in v0.29.0; `env.NAME` is a string value, not a map");
                } else if (iterable_kind == SYM_ARRAY) {
                    ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span, "two-name map loops require a map iterable in v0.29.0; arrays use `for item in array`");
                } else if (iterable_kind == SYM_COMMAND_RESULT) {
                    ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span, "command-result values are not maps; use `for key, value in map` only with flat maps in v0.29.0");
                } else if (iterable_kind == SYM_UNKNOWN) {
                    ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span,
                                  "map loop iterable kind must be known in v0.29.0; use a named map or supported flat map-returning function call");
                } else if (iterable_kind != SYM_MAP && iterable_kind != SYM_UNKNOWN) {
                    ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span, "two-name map loops require a map iterable in v0.29.0");
                }
                if (iterable_kind == SYM_MAP && !lower_collection_map_for_iterable_is_portable(out->as.for_stmt.iterable)) {
                    ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span,
                                  "map loop iterable must be a named map or supported map-returning function call for VM/Bash parity in v0.29.0; bind temporary maps to a variable first");
                }
                element_kind = infer_map_value_kind(lower, out->as.for_stmt.iterable);
            } else {
                SymKind iterable_kind = SYM_UNKNOWN;
                out->as.for_stmt.iterable = lower_expr(lower, stmt->as.for_stmt.iterable, &iterable_kind);
                if (!stmt->as.for_stmt.has_value_name && iterable_kind != SYM_ARRAY && iterable_kind != SYM_UNKNOWN) {
                    if (iterable_kind == SYM_MAP) ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span, "one-name loop over a map is not supported in v0.29.0; use `for key, value in map`");
                    else ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span, "for loop iterable must be an array in v0.10.0");
                }
                if (iterable_kind == SYM_ARRAY && !lower_collection_for_iterable_is_portable(out->as.for_stmt.iterable)) {
                    lower_reject_nonportable_collection_for_iterable(lower, stmt->as.for_stmt.iterable->span);
                }
                element_kind = infer_array_element_kind(lower, out->as.for_stmt.iterable);
            }
            out->as.for_stmt.element_kind = lower_value_kind_from_sym(element_kind);
            if (out->kind == DS_LOWER_STMT_FOR_ARRAY && element_kind == SYM_MAP) {
                const DsLowerRowSchema *schema = NULL;
                if (out->as.for_stmt.iterable && out->as.for_stmt.iterable->kind == DS_LOWER_EXPR_IDENT) {
                    Symbol *iter_sym = scope_find(lower->scope, out->as.for_stmt.iterable->as.text);
                    if (iter_sym && iter_sym->is_row_array) schema = &iter_sym->row_schema;
                }
                if (!schema) lower_expr_row_array_schema(out->as.for_stmt.iterable, &schema);
                if (schema) {
                    out->as.for_stmt.iterates_row_array = true;
                    row_schema_clone(schema, &out->as.for_stmt.row_schema);
                } else {
                    ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span,
                                  "row-array iteration requires a named row array or known row-array result in v0.37.0");
                }
            }
            Symbol *map_loop_symbol = NULL;
            if (out->kind == DS_LOWER_STMT_FOR_MAP && stmt->as.for_stmt.iterable && stmt->as.for_stmt.iterable->kind == DS_EXPR_IDENT) {
                map_loop_symbol = scope_find(lower->scope, stmt->as.for_stmt.iterable->as.text);
            }
            Scope local;
            scope_init(&local, lower->scope);
            Scope *saved = lower->scope;
            int saved_depth = lower->loop_depth;
            lower->loop_depth++;
            lower->scope = &local;
            if (out->kind == DS_LOWER_STMT_FOR_MAP) {
                scope_define(lower, &local, stmt->as.for_stmt.key_name, SYM_STRING, stmt->span);
                scope_define(lower, &local, stmt->as.for_stmt.value_name, element_kind, stmt->span);
                if (element_kind == SYM_UNKNOWN) {
                    Symbol *value_sym = scope_find_current(&local, stmt->as.for_stmt.value_name);
                    if (value_sym) value_sym->dynamic_scalar = true;
                }
            } else {
                scope_define(lower, &local, stmt->as.for_stmt.key_name, element_kind, stmt->span);
                if (out->as.for_stmt.iterates_row_array) {
                    Symbol *row_sym = scope_find_current(&local, stmt->as.for_stmt.key_name);
                    if (row_sym) symbol_set_row(row_sym, &out->as.for_stmt.row_schema);
                }
            }
            bool pushed_map_loop_symbol = false;
            if (out->kind == DS_LOWER_STMT_FOR_MAP && map_loop_symbol && map_loop_symbol->kind == SYM_MAP) {
                lower_push_map_loop_symbol(lower, map_loop_symbol);
                pushed_map_loop_symbol = true;
            }
            out->as.for_stmt.body = lower_block(lower, stmt->as.for_stmt.body, false);
            if (pushed_map_loop_symbol) lower_pop_map_loop_symbol(lower);
            lower->scope = saved;
            lower->loop_depth = saved_depth;
            scope_free(&local);
            return out;
        }
        case DS_STMT_WHILE: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_WHILE, stmt->span);
            SymKind cond_kind = SYM_UNKNOWN;
            out->as.while_stmt.condition = lower_expr(lower, stmt->as.while_stmt.condition, &cond_kind);
            int saved_depth = lower->loop_depth;
            lower->loop_depth++;
            out->as.while_stmt.body = lower_block(lower, stmt->as.while_stmt.body, true);
            lower->loop_depth = saved_depth;
            return out;
        }
        case DS_STMT_BREAK:
        case DS_STMT_CONTINUE: {
            DsLowerStmtKind lowered;
            const char *name;
            if (stmt->kind == DS_STMT_BREAK) { lowered = DS_LOWER_STMT_BREAK; name = "break"; }
            else { lowered = DS_LOWER_STMT_CONTINUE; name = "continue"; }
            if (lower->loop_depth <= 0) ds_diag_error(lower->diag, stmt->span, "`%s` is only allowed inside a loop", name);
            return stmt_new(lowered, stmt->span);
        }
        case DS_STMT_CASE: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_CASE, stmt->span);
            SymKind selector_kind = SYM_UNKNOWN;
            out->as.case_stmt.selector = lower_expr(lower, stmt->as.case_stmt.selector, &selector_kind);
            bool selector_is_dynamic_scalar = selector_kind == SYM_UNKNOWN &&
                                              lowered_expr_has_dynamic_scalar_value(lower, out->as.case_stmt.selector);
            if (selector_kind == SYM_ARRAY || selector_kind == SYM_MAP || selector_kind == SYM_COMMAND_RESULT ||
                (selector_kind == SYM_UNKNOWN && !selector_is_dynamic_scalar)) {
                ds_diag_error(lower->diag, stmt->as.case_stmt.selector->span, "case selectors must have a known scalar string, int, or bool kind in v0.17.0");
            }
            if (stmt->as.case_stmt.arms.len == 0) ds_diag_error(lower->diag, stmt->span, "case statements require at least one arm");
            bool seen_default = false;
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                const DsCaseArm *arm = &stmt->as.case_stmt.arms.items[i];
                DsLowerCaseArm lowered;
                memset(&lowered, 0, sizeof(lowered));
                lowered.span = arm->span;
                for (size_t j = 0; j < arm->patterns.len; j++) {
                    DsLowerCasePattern pattern = lower_case_pattern(&arm->patterns.items[j]);
                    if (pattern.kind == DS_CASE_PATTERN_DEFAULT) {
                        if (seen_default) ds_diag_error(lower->diag, pattern.span, "case statements may contain only one `_` default arm");
                        seen_default = true;
                        if (i + 1 != stmt->as.case_stmt.arms.len || j + 1 != arm->patterns.len) {
                            ds_diag_error(lower->diag, pattern.span, "case `_` default arm must be final");
                        }
                    } else {
                        validate_case_pattern_duplicate(lower, &out->as.case_stmt.arms, &pattern);
                        validate_case_pattern_duplicate_in_arm(lower, &lowered.patterns, &pattern);
                    }
                    lower_case_pattern_vec_push(&lowered.patterns, pattern);
                }
                lowered.body = lower_block(lower, arm->body, true);
                lower_case_arm_vec_push(&out->as.case_stmt.arms, lowered);
            }
            return out;
        }
        case DS_STMT_PUSH: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_PUSH, stmt->span);
            out->as.push_stmt.name = str_clone(stmt->as.push_stmt.name);
            Symbol *sym = scope_find(lower->scope, stmt->as.push_stmt.name);
            if (!sym) {
                ds_diag_error(lower->diag, stmt->span, "unknown array `%.*s`", (int)stmt->as.push_stmt.name.len, stmt->as.push_stmt.name.data);
            } else if (sym->kind != SYM_ARRAY && sym->kind != SYM_UNKNOWN && sym->kind != SYM_TOPLEVEL_PREDECLARED) {
                ds_diag_error(lower->diag, stmt->span, "`push` requires an array variable in v0.10.0");
            } else {
                lower_validate_handler_capture(lower, sym, stmt->as.push_stmt.name, stmt->span);
            }
            SymKind value_kind = SYM_UNKNOWN;
            out->as.push_stmt.value = lower_expr(lower, stmt->as.push_stmt.value, &value_kind);
            if (value_kind == SYM_MAP && lower_expr_is_row(out->as.push_stmt.value)) {
                const DsLowerRowSchema *schema = NULL;
                lower_expr_row_schema(out->as.push_stmt.value, &schema);
                if (sym) {
                    if (sym->saw_scalar_array_value && !sym->is_row_array) {
                        ds_diag_error(lower->diag, stmt->span, "cannot push a row into a scalar array in v0.37.0");
                    } else if (sym->element_kind == SYM_UNKNOWN && !sym->is_row_array) {
                        symbol_set_row_array(sym, schema);
                    } else if (!sym->is_row_array) {
                        ds_diag_error(lower->diag, stmt->span, "cannot push a row into a scalar array in v0.37.0");
                    } else if (!row_schema_equal(&sym->row_schema, schema)) {
                        ds_diag_error(lower->diag, stmt->as.push_stmt.value->span,
                                      "pushed row must match the row-array field names and scalar kinds in v0.37.0");
                    }
                    if (sym->is_row_array) {
                        out->as.push_stmt.target_is_row_array = true;
                        row_schema_clone(&sym->row_schema, &out->as.push_stmt.row_schema);
                    }
                }
            } else if (value_kind == SYM_ARRAY || value_kind == SYM_MAP) {
                ds_diag_error(lower->diag, stmt->as.push_stmt.value->span, "pushing collection values is deferred in v0.10.0");
            } else if (sym && sym->is_row_array) {
                ds_diag_error(lower->diag, stmt->as.push_stmt.value->span, "cannot push a scalar value into a row array in v0.37.0");
            } else if (!lower_collection_element_is_portable(out->as.push_stmt.value)) {
                ds_diag_error(lower->diag, stmt->as.push_stmt.value->span, "collection element expressions must be scalar Bash-emittable values in v0.10.0; bind the expression to a variable first");
            } else if (sym && value_kind != SYM_UNKNOWN) {
                /*
                 * Empty-array literals learn whether they are scalar arrays or
                 * row arrays from the first push.  Without recording the first
                 * scalar push here, a later row push into the same array still
                 * looked like an untyped empty array and silently converted the
                 * variable into a row array during lowering.  Keep this as a
                 * separate row-vs-scalar marker instead of merging the public
                 * element kind; older scalar arrays intentionally preserve
                 * dynamic mixed-element behavior for VM/Bash parity.
                 */
                sym->saw_scalar_array_value = true;
            }
            return out;
        }
        case DS_STMT_ASSERT: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_ASSERT, stmt->span);
            SymKind cond_kind = SYM_UNKNOWN;
            out->as.assert_stmt.condition = lower_expr(lower, stmt->as.assert_stmt.condition, &cond_kind);
            return out;
        }
        case DS_STMT_RETURN: {
            if (stmt->as.return_stmt.value && stmt->as.return_stmt.value->kind == DS_EXPR_RUN) {
                DsCommand command_copy;
                Scope temp_scope;
                Scope *saved_scope = NULL;
                DsLowerStmt *block = lower_materialize_run_in_stmt(lower, &stmt->as.return_stmt.value->as.run, stmt->span, &temp_scope, &saved_scope, &command_copy);
                if (block) {
                    DsExpr fake = lower_run_copy_expr(stmt->as.return_stmt.value, command_copy);
                    DsLowerStmt *out = lower_return_with_value(lower, stmt, &fake);
                    lower_stmt_vec_push(&block->as.block_stmt.statements, out);
                    lower_temp_scope_end(lower, &temp_scope, saved_scope);
                    ds_command_free(&command_copy);
                    return block;
                }
                ds_command_free(&command_copy);
            }
            return lower_return_with_value(lower, stmt, stmt->as.return_stmt.value);
        }
        case DS_STMT_DEFER:
        case DS_STMT_TRAP: {
            DsLowerStmt *out = stmt_new(stmt->kind == DS_STMT_DEFER ? DS_LOWER_STMT_DEFER : DS_LOWER_STMT_TRAP, stmt->span);
            out->as.handler_stmt.signal = stmt->as.handler_stmt.signal;
            lower_validate_handler_signal(lower, stmt);
            int saved_handler_depth = lower->handler_depth;
            int saved_handler_function_depth = lower->handler_function_depth;
            lower->handler_depth++;
            lower->handler_function_depth = lower->function_depth;
            out->as.handler_stmt.body = lower_block(lower, stmt->as.handler_stmt.body, true);
            lower->handler_depth = saved_handler_depth;
            lower->handler_function_depth = saved_handler_function_depth;
            return out;
        }
        case DS_STMT_TEST:
            return stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
        case DS_STMT_BLOCK:
            return lower_block(lower, stmt, true);
        case DS_STMT_IMPORT:
            ds_diag_error(lower->diag, stmt->span, "unresolved import `%.*s`", (int)stmt->as.import_stmt.path.len, stmt->as.import_stmt.path.data);
            return stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
        case DS_STMT_FN:
            return stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
    }
    return stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
}
