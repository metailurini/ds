#include "lower_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

DsLowerStmt *stmt_new(DsLowerStmtKind kind, DsSpan span) {
    DsLowerStmt *stmt = (DsLowerStmt *)ds_xcalloc(1, sizeof(DsLowerStmt));
    stmt->kind = kind;
    stmt->span = span;
    return stmt;
}

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
                  form, (int)text.len, text.data ? text.data : "");
    return false;
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
            ds_diag_error(lower->diag, stmt->span, "unknown standard-library helper `%.*s`", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
        } else if (!ds_stdlib_arity_ok(stdlib_helper, stmt->as.call_stmt.args.len)) {
            if (stdlib_helper->min_arity == stdlib_helper->max_arity) ds_diag_error(lower->diag, stmt->span, "helper `%.*s` expects %zu arguments but got %zu", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data, stdlib_helper->min_arity, stmt->as.call_stmt.args.len);
            else ds_diag_error(lower->diag, stmt->span, "helper `%.*s` expects %zu to %zu arguments but got %zu", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data, stdlib_helper->min_arity, stdlib_helper->max_arity, stmt->as.call_stmt.args.len);
        } else if (!stdlib_helper->statement_only) {
            ds_diag_error(lower->diag, stmt->span, "helper `%.*s` returns a value in v0.11.0; assign it with `let` or use it in an expression", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
        }
    } else if (!fn) {
        DsStr ns = {0}, member = {0};
        if (split_member_name(stmt->as.call_stmt.name, &ns, &member) && ds_stdlib_is_namespace(ns)) ds_diag_error(lower->diag, stmt->span, "unknown standard-library helper `%.*s`", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
        else if (split_member_name(stmt->as.call_stmt.name, &ns, &member)) ds_diag_error(lower->diag, stmt->span, "only `push` collection method is supported in v0.10.0");
        else ds_diag_error(lower->diag, stmt->span, "unknown function `%.*s`", (int)stmt->as.call_stmt.name.len, stmt->as.call_stmt.name.data);
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
        if (lower_decode_string_text(stmt->as.call_stmt.args.items[0]->as.text, &decoded)) {
            if (!is_env_name_text(decoded)) ds_diag_error(lower->diag, stmt->as.call_stmt.args.items[0]->span, "invalid environment variable name `%.*s` in v0.11.0", (int)decoded.len, decoded.data);
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

static DsLowerAssignOp lower_assign_op(DsAssignOp op) {
    switch (op) {
        case DS_ASSIGN_ADD: return DS_LOWER_ASSIGN_ADD;
        case DS_ASSIGN_SUB: return DS_LOWER_ASSIGN_SUB;
        case DS_ASSIGN_MUL: return DS_LOWER_ASSIGN_MUL;
        case DS_ASSIGN_DIV: return DS_LOWER_ASSIGN_DIV;
        case DS_ASSIGN_MOD: return DS_LOWER_ASSIGN_MOD;
        case DS_ASSIGN_SET: return DS_LOWER_ASSIGN_SET;
    }
    return DS_LOWER_ASSIGN_SET;
}

static void maybe_update_symbol(Symbol *sym, SymKind kind) {
    if (!sym) return;
    if (kind != SYM_UNKNOWN && sym->kind != SYM_TOPLEVEL_PREDECLARED) sym->kind = kind;
}

static bool command_quoted_word_needs_value_call_materialization(Lower *lower, DsStr word) {
    if (word.len < 2 || word.data[0] != '"' || word.data[word.len - 1] != '"') return false;
    DsStr decoded = {0};
    if (!lower_decode_string_text(word, &decoded)) return false;
    bool found = false;
    for (size_t i = 0; i < decoded.len && !found; i++) {
        if (decoded.data[i] != '{') continue;
        size_t j = i + 1;
        while (j < decoded.len && (decoded.data[j] == ' ' || decoded.data[j] == '\t')) j++;
        if (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') ||
                                (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') ||
                                decoded.data[j] == '_')) {
            j++;
            while (j < decoded.len && ((decoded.data[j] >= 'A' && decoded.data[j] <= 'Z') ||
                                       (decoded.data[j] >= 'a' && decoded.data[j] <= 'z') ||
                                       (decoded.data[j] >= '0' && decoded.data[j] <= '9') ||
                                       decoded.data[j] == '_' || decoded.data[j] == '.')) j++;
            while (j < decoded.len && (decoded.data[j] == ' ' || decoded.data[j] == '\t')) j++;
            if (j < decoded.len && decoded.data[j] == '(') found = true;
        }
    }
    free(decoded.data);
    (void)lower;
    return found;
}

static DsStr lower_make_temp_name(Lower *lower, const char *prefix) {
    char buf[96];
    do {
        snprintf(buf, sizeof(buf), "__ds_%s_%zu", prefix, lower->temp_counter++);
        DsStr candidate = {buf, strlen(buf)};
        if (!scope_find(lower->scope, candidate)) break;
    } while (true);
    return (DsStr){ds_str_dup_range(buf, strlen(buf)), strlen(buf)};
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

static DsLowerStmt *lower_command_interpolation_temp_string_let(Lower *lower, DsStr name, DsStr quoted_text, DsSpan span) {
    DsExpr fake;
    memset(&fake, 0, sizeof(fake));
    fake.kind = DS_EXPR_STRING;
    fake.span = span;
    fake.as.text = quoted_text;
    SymKind kind = SYM_UNKNOWN;
    DsLowerStmt *let = stmt_new(DS_LOWER_STMT_LET, span);
    let->as.let_stmt.name = str_clone(name);
    let->as.let_stmt.value = lower_expr(lower, &fake, &kind);
    if (kind != SYM_STRING && kind != SYM_UNKNOWN) {
        ds_diag_error(lower->diag, span, "function call in command interpolation must return a scalar string-renderable value in v0.27.0");
    }
    scope_define(lower, lower->scope, name, SYM_STRING, span);
    return let;
}

static DsStr lower_command_temp_word(DsStr name) {
    DsString s;
    ds_string_init(&s);
    ds_string_append_char(&s, '$');
    ds_string_append_range(&s, name.data, name.len);
    return (DsStr){s.data, s.len};
}

static DsStr lower_redirect_temp_target(DsStr name) {
    DsString s;
    ds_string_init(&s);
    ds_string_append_char(&s, '"');
    ds_string_append_char(&s, '{');
    ds_string_append_range(&s, name.data, name.len);
    ds_string_append_char(&s, '}');
    ds_string_append_char(&s, '"');
    return (DsStr){s.data, s.len};
}

static bool lower_materialize_command_value_call_interpolation(Lower *lower, DsCommand *command, DsLowerStmt *block) {
    /*
     * M3.4 command-word contract: direct scalar value-call interpolation in a
     * quoted command word is not handed to VM/Bash as a backend-specific
     * command substitution problem. Lowering evaluates the interpolation as a
     * normal string expression into a private temporary, then rewrites the
     * command word to an ordinary `$temp` argument. Unsupported return kinds are
     * diagnosed while lowering the temporary string expression.
     */
    bool changed = false;
    for (size_t s = 0; s < command->stages.len; s++) {
        for (size_t i = 0; i < command->stages.items[s].words.len; i++) {
            DsWord *word = &command->stages.items[s].words.items[i];
            if (!command_quoted_word_needs_value_call_materialization(lower, word->text)) continue;
            DsStr tmp = lower_make_temp_name(lower, "cmd_interp");
            lower_stmt_vec_push(&block->as.block_stmt.statements, lower_command_interpolation_temp_string_let(lower, tmp, word->text, word->span));
            free(word->text.data);
            word->text = lower_command_temp_word(tmp);
            free(tmp.data);
            changed = true;
        }
    }
    if (command->redirect.kind != DS_REDIRECT_NONE && command_quoted_word_needs_value_call_materialization(lower, command->redirect.target)) {
        DsStr tmp = lower_make_temp_name(lower, "redir_interp");
        lower_stmt_vec_push(&block->as.block_stmt.statements, lower_command_interpolation_temp_string_let(lower, tmp, command->redirect.target, command->redirect.target_span));
        free(command->redirect.target.data);
        command->redirect.target = lower_redirect_temp_target(tmp);
        free(tmp.data);
        changed = true;
    }
    return changed;
}

static bool pattern_equal(const DsLowerCasePattern *a, const DsLowerCasePattern *b) {
    if (a->kind != b->kind) return false;
    if (a->kind == DS_LOWER_CASE_PATTERN_BOOL) return a->boolean == b->boolean;
    if (a->kind == DS_LOWER_CASE_PATTERN_DEFAULT) return true;
    return a->text.len == b->text.len && memcmp(a->text.data, b->text.data, a->text.len) == 0;
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

static bool lower_for_array_iterable_has_portable_backend_representation(const DsLowerExpr *iterable) {
    if (!iterable) return false;
    if (iterable->kind == DS_LOWER_EXPR_IDENT) return true;
    if (iterable->kind == DS_LOWER_EXPR_CALL && ds_stdlib_is_name(iterable->as.call.name)) {
        const DsStdlibHelper *helper = ds_stdlib_lookup(iterable->as.call.name);
        return helper && helper->return_kind == DS_STDLIB_RETURN_ARRAY;
    }
    return false;
}

static void lower_reject_nonportable_for_array_iterable(Lower *lower, DsSpan span) {
    /*
     * The VM can iterate some temporary array values directly, but standalone
     * Bash has portable loop representations only for named arrays and known
     * stdlib array streams. Keep that acceptance rule in lowering so Bash
     * emission remains a renderer of accepted HIR, not the semantic owner.
     */
    ds_diag_error(lower->diag, span,
                  "for loop iterable must be a named array or known stdlib array result for VM/Bash parity in v0.10.0; bind temporary arrays to a variable first");
}

static bool lower_return_value_has_portable_backend_representation(DsLowerValueKind return_kind, const DsLowerExpr *value) {
    if (!value) return false;
    if (value->kind == DS_LOWER_EXPR_CALL && value->as.call.is_user_function) return true;
    switch (return_kind) {
        case DS_LOWER_VALUE_ARRAY:
            return value->kind == DS_LOWER_EXPR_ARRAY || value->kind == DS_LOWER_EXPR_IDENT;
        case DS_LOWER_VALUE_MAP:
            return value->kind == DS_LOWER_EXPR_MAP || value->kind == DS_LOWER_EXPR_IDENT;
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
    /*
     * Function return kind is a lowerer/HIR contract. Backends consume
     * out->as.return_stmt.return_kind and may keep defensive invariants, but
     * user-facing return-kind acceptance must be decided here.
     */
    out->as.return_stmt.return_kind = lower_value_kind_from_sym(value_kind);
    lower_validate_return_backend_representation(lower, out);

    if (!lower->current_function) return;

    DsLowerValueKind ret = lower_value_kind_from_sym(value_kind);
    if (ret == DS_LOWER_VALUE_UNKNOWN) {
        ds_diag_error(lower->diag, src->span, "function return value kind must be known in v0.21.0");
    } else if (!lower->current_function->has_return) {
        lower->current_function->has_return = true;
        lower->current_function->return_kind = ret;
    } else if (lower->current_function->return_kind != ret) {
        ds_diag_error(lower->diag, src->span, "all return statements in a function must have the same value kind in v0.21.0");
    }
}

static DsLowerCasePattern lower_case_pattern(const DsCasePattern *pattern) {
    DsLowerCasePattern out;
    memset(&out, 0, sizeof(out));
    out.span = pattern->span;
    out.boolean = pattern->boolean;
    switch (pattern->kind) {
        case DS_CASE_PATTERN_STRING: out.kind = DS_LOWER_CASE_PATTERN_STRING; out.text = str_clone(pattern->text); break;
        case DS_CASE_PATTERN_INT: out.kind = DS_LOWER_CASE_PATTERN_INT; out.text = str_clone(pattern->text); break;
        case DS_CASE_PATTERN_BOOL: out.kind = DS_LOWER_CASE_PATTERN_BOOL; break;
        case DS_CASE_PATTERN_DEFAULT: out.kind = DS_LOWER_CASE_PATTERN_DEFAULT; break;
    }
    return out;
}

DsLowerStmt *lower_stmt(Lower *lower, const DsStmt *stmt) {
    switch (stmt->kind) {
        case DS_STMT_LET: {
            if (stmt->as.let_stmt.value && stmt->as.let_stmt.value->kind == DS_EXPR_RUN) {
                DsCommand command_copy;
                ds_command_clone(&command_copy, &stmt->as.let_stmt.value->as.run);
                DsLowerStmt *block = stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
                block->as.block_stmt.scoped = false;
                Scope temp_scope;
                Scope *saved_scope = NULL;
                lower_temp_scope_begin(lower, &temp_scope, &saved_scope);
                bool materialized = lower_materialize_command_value_call_interpolation(lower, &command_copy, block);
                if (materialized) {
                    DsExpr fake;
                    memset(&fake, 0, sizeof(fake));
                    fake.kind = DS_EXPR_RUN;
                    fake.span = stmt->as.let_stmt.value->span;
                    fake.as.run = command_copy;
                    SymKind kind = SYM_UNKNOWN;
                    DsLowerStmt *out = stmt_new(DS_LOWER_STMT_LET, stmt->span);
                    out->as.let_stmt.name = str_clone(stmt->as.let_stmt.name);
                    out->as.let_stmt.value = lower_expr(lower, &fake, &kind);
                    lower_temp_scope_end(lower, &temp_scope, saved_scope);
                    scope_define_array(lower, lower->scope, stmt->as.let_stmt.name, kind,
                                       kind == SYM_ARRAY ? infer_array_element_kind(lower, out->as.let_stmt.value) : SYM_UNKNOWN,
                                       stmt->span);
                    lower_stmt_vec_push(&block->as.block_stmt.statements, out);
                    ds_command_free(&command_copy);
                    return block;
                }
                lower_temp_scope_end(lower, &temp_scope, saved_scope);
                lower_stmt_free(block);
                ds_command_free(&command_copy);
            }
            SymKind kind = SYM_UNKNOWN;
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_LET, stmt->span);
            out->as.let_stmt.name = str_clone(stmt->as.let_stmt.name);
            out->as.let_stmt.value = lower_expr(lower, stmt->as.let_stmt.value, &kind);
            scope_define_array(lower, lower->scope, stmt->as.let_stmt.name, kind,
                               kind == SYM_ARRAY ? infer_array_element_kind(lower, out->as.let_stmt.value) : SYM_UNKNOWN,
                               stmt->span);
            return out;
        }
        case DS_STMT_ASSIGN: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_ASSIGN, stmt->span);
            out->as.assign_stmt.name = str_clone(stmt->as.assign_stmt.name);
            out->as.assign_stmt.op = lower_assign_op(stmt->as.assign_stmt.op);
            bool env_assign = stmt->as.assign_stmt.name.len > 4 && memcmp(stmt->as.assign_stmt.name.data, "env.", 4) == 0;
            if (env_assign) {
                DsStr env_name = {stmt->as.assign_stmt.name.data + 4, stmt->as.assign_stmt.name.len - 4};
                if (!is_env_name_text(env_name)) {
                    ds_diag_error(lower->diag, stmt->span, "invalid environment variable name `%.*s` in v0.27.0", (int)env_name.len, env_name.data);
                }
                if (stmt->as.assign_stmt.op != DS_ASSIGN_SET) {
                    ds_diag_error(lower->diag, stmt->span, "environment assignment supports only `=` in v0.27.0");
                }
                SymKind value_kind = SYM_UNKNOWN;
                out->as.assign_stmt.value = lower_expr(lower, stmt->as.assign_stmt.value, &value_kind);
                if (value_kind != SYM_STRING && value_kind != SYM_INT && value_kind != SYM_BOOL && value_kind != SYM_UNKNOWN) {
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
        case DS_STMT_COLLECTION_ASSIGN: {
            /*
             * M3.6 collection boundary: the parser preserves collection
             * assignment syntax so the lowerer, not parser/VM/Bash, owns the
             * unsupported mutation diagnostic. There is intentionally no HIR
             * mutation node until a future milestone defines VM/Bash parity for
             * index assignment, map field assignment, and nested mutation.
             */
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
            out->as.block_stmt.scoped = false;
            SymKind ignored_kind = SYM_UNKNOWN;
            DsLowerExpr *target = lower_expr(lower, stmt->as.collection_assign_stmt.target, &ignored_kind);
            ignored_kind = SYM_UNKNOWN;
            DsLowerExpr *value = lower_expr(lower, stmt->as.collection_assign_stmt.value, &ignored_kind);
            lower_expr_free(target);
            lower_expr_free(value);

            const DsExpr *target_ast = stmt->as.collection_assign_stmt.target;
            if (target_ast && target_ast->kind == DS_EXPR_FIELD) {
                ds_diag_error(lower->diag, target_ast->span, "map field assignment is deferred in v0.10.0; bind a new map value instead");
            } else if (target_ast && target_ast->kind == DS_EXPR_INDEX && target_ast->as.index.object && target_ast->as.index.object->kind == DS_EXPR_INDEX) {
                ds_diag_error(lower->diag, target_ast->span, "nested collection mutation is deferred in v0.10.0");
            } else if (target_ast && target_ast->kind == DS_EXPR_INDEX) {
                ds_diag_error(lower->diag, target_ast->span, "index assignment is deferred in v0.10.0; use array.push for append-only list mutation");
            } else {
                ds_diag_error(lower->diag, stmt->span, "unsupported collection assignment target in v0.10.0");
            }
            return out;
        }
        case DS_STMT_CMD: {
            DsCommand command_copy;
            ds_command_clone(&command_copy, &stmt->as.cmd_stmt);
            DsLowerStmt *block = stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
            block->as.block_stmt.scoped = false;
            Scope temp_scope;
            Scope *saved_scope = NULL;
            lower_temp_scope_begin(lower, &temp_scope, &saved_scope);
            bool materialized = lower_materialize_command_value_call_interpolation(lower, &command_copy, block);
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
            if (materialized) {
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
            out->as.if_stmt.then_branch = lower_block(lower, stmt->as.if_stmt.then_branch, true);
            if (stmt->as.if_stmt.else_branch) out->as.if_stmt.else_branch = lower_block(lower, stmt->as.if_stmt.else_branch, true);
            return out;
        }
        case DS_STMT_FOR: {
            DsLowerStmt *out = stmt_new(stmt->as.for_stmt.iterable && stmt->as.for_stmt.iterable->kind == DS_EXPR_RANGE ? DS_LOWER_STMT_FOR_RANGE : DS_LOWER_STMT_FOR_ARRAY, stmt->span);
            out->as.for_stmt.name = str_clone(stmt->as.for_stmt.key_name);
            if (stmt->as.for_stmt.has_value_name) {
                ds_diag_error(lower->diag, stmt->span, "map iteration is deferred in v0.10.0; use direct map access instead");
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
            } else {
                SymKind iterable_kind = SYM_UNKNOWN;
                out->as.for_stmt.iterable = lower_expr(lower, stmt->as.for_stmt.iterable, &iterable_kind);
                if (!stmt->as.for_stmt.has_value_name && iterable_kind != SYM_ARRAY && iterable_kind != SYM_UNKNOWN) {
                    ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span, "for loop iterable must be an array in v0.10.0");
                }
                if (iterable_kind == SYM_ARRAY && !lower_for_array_iterable_has_portable_backend_representation(out->as.for_stmt.iterable)) {
                    lower_reject_nonportable_for_array_iterable(lower, stmt->as.for_stmt.iterable->span);
                }
                element_kind = infer_array_element_kind(lower, out->as.for_stmt.iterable);
            }
            out->as.for_stmt.element_kind = lower_value_kind_from_sym(element_kind);
            Scope local;
            scope_init(&local, lower->scope);
            Scope *saved = lower->scope;
            int saved_depth = lower->loop_depth;
            lower->loop_depth++;
            lower->scope = &local;
            scope_define(lower, &local, stmt->as.for_stmt.key_name, element_kind, stmt->span);
            out->as.for_stmt.body = lower_block(lower, stmt->as.for_stmt.body, false);
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
            if (lower->loop_depth <= 0) ds_diag_error(lower->diag, stmt->span, "`break` is only allowed inside a loop");
            return stmt_new(DS_LOWER_STMT_BREAK, stmt->span);
        case DS_STMT_CONTINUE:
            if (lower->loop_depth <= 0) ds_diag_error(lower->diag, stmt->span, "`continue` is only allowed inside a loop");
            return stmt_new(DS_LOWER_STMT_CONTINUE, stmt->span);
        case DS_STMT_CASE: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_CASE, stmt->span);
            SymKind selector_kind = SYM_UNKNOWN;
            out->as.case_stmt.selector = lower_expr(lower, stmt->as.case_stmt.selector, &selector_kind);
            if (selector_kind == SYM_ARRAY || selector_kind == SYM_MAP || selector_kind == SYM_COMMAND_RESULT || selector_kind == SYM_UNKNOWN) {
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
                    if (pattern.kind == DS_LOWER_CASE_PATTERN_DEFAULT) {
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
            if (value_kind == SYM_ARRAY || value_kind == SYM_MAP) {
                ds_diag_error(lower->diag, stmt->as.push_stmt.value->span, "pushing collection values is deferred in v0.10.0");
            } else if (!collection_element_supported_in_bash(out->as.push_stmt.value)) {
                ds_diag_error(lower->diag, stmt->as.push_stmt.value->span, "collection element expressions must be scalar Bash-emittable values in v0.10.0; bind the expression to a variable first");
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
                ds_command_clone(&command_copy, &stmt->as.return_stmt.value->as.run);
                DsLowerStmt *block = stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
                block->as.block_stmt.scoped = false;
                Scope temp_scope;
                Scope *saved_scope = NULL;
                lower_temp_scope_begin(lower, &temp_scope, &saved_scope);
                bool materialized = lower_materialize_command_value_call_interpolation(lower, &command_copy, block);
                if (materialized) {
                    DsExpr fake;
                    memset(&fake, 0, sizeof(fake));
                    fake.kind = DS_EXPR_RUN;
                    fake.span = stmt->as.return_stmt.value->span;
                    fake.as.run = command_copy;
                    DsLowerStmt *out = stmt_new(DS_LOWER_STMT_RETURN, stmt->span);
                    if (lower->handler_depth > 0) {
                        ds_diag_error(lower->diag, stmt->span, "`return` from a cleanup handler is not supported in v0.22.0; move the return into a function called by the handler or use `exit`");
                    }
                    if (lower->function_depth <= 0 || !lower->current_function) {
                        ds_diag_error(lower->diag, stmt->span, "`return` is only allowed inside a function");
                    }
                    SymKind value_kind = SYM_UNKNOWN;
                    out->as.return_stmt.value = lower_expr(lower, &fake, &value_kind);
                    lower_validate_function_return_contract(lower, stmt, out, value_kind);
                    lower_stmt_vec_push(&block->as.block_stmt.statements, out);
                    lower_temp_scope_end(lower, &temp_scope, saved_scope);
                    ds_command_free(&command_copy);
                    return block;
                }
                lower_temp_scope_end(lower, &temp_scope, saved_scope);
                lower_stmt_free(block);
                ds_command_free(&command_copy);
            }
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_RETURN, stmt->span);
            if (lower->handler_depth > 0) {
                ds_diag_error(lower->diag, stmt->span, "`return` from a cleanup handler is not supported in v0.22.0; move the return into a function called by the handler or use `exit`");
            }
            if (lower->function_depth <= 0 || !lower->current_function) {
                ds_diag_error(lower->diag, stmt->span, "`return` is only allowed inside a function");
            }
            SymKind value_kind = SYM_UNKNOWN;
            out->as.return_stmt.value = lower_expr(lower, stmt->as.return_stmt.value, &value_kind);
            lower_validate_function_return_contract(lower, stmt, out, value_kind);
            return out;
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
