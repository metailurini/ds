#include "lower_internal.h"

#include <stdlib.h>
#include <string.h>

DsLowerStmt *stmt_new(DsLowerStmtKind kind, DsSpan span) {
    DsLowerStmt *stmt = (DsLowerStmt *)ds_xcalloc(1, sizeof(DsLowerStmt));
    stmt->kind = kind;
    stmt->span = span;
    return stmt;
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
    for (size_t i = 0; i < stmt->as.call_stmt.args.len; i++) {
        SymKind arg_kind = SYM_UNKNOWN;
        lower_expr_vec_push(&out->as.call_stmt.args, lower_expr(lower, stmt->as.call_stmt.args.items[i], &arg_kind));
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
    if (stdlib && stmt->as.call_stmt.args.len > 0) validate_glob_pattern_arg(lower, stmt->as.call_stmt.name, stmt->as.call_stmt.args.items[0]);
    return out;
}

DsLowerStmt *lower_block(Lower *lower, const DsStmt *block, bool child_scope) {
    DsLowerStmt *out = stmt_new(DS_LOWER_STMT_BLOCK, block->span);
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

DsLowerStmt *lower_stmt(Lower *lower, const DsStmt *stmt) {
    switch (stmt->kind) {
        case DS_STMT_LET: {
            SymKind kind = SYM_UNKNOWN;
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_LET, stmt->span);
            out->as.let_stmt.name = str_clone(stmt->as.let_stmt.name);
            out->as.let_stmt.value = lower_expr(lower, stmt->as.let_stmt.value, &kind);
            scope_define(lower, lower->scope, stmt->as.let_stmt.name, kind, stmt->span);
            return out;
        }
        case DS_STMT_CMD: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_CMD, stmt->span);
            ds_command_init(&out->as.cmd_stmt, DS_COMMAND_PLAIN, stmt->span);
            for (size_t i = 0; i < stmt->as.cmd_stmt.words.len; i++) {
                validate_cmd_word(lower, stmt->as.cmd_stmt.words.items[i].text, stmt->as.cmd_stmt.words.items[i].span);
            }
            ds_word_vec_clone(&out->as.cmd_stmt.words, &stmt->as.cmd_stmt.words);
            if (stmt->as.cmd_stmt.redirect.kind != DS_REDIRECT_NONE) {
                if (stmt->as.cmd_stmt.redirect.target.len == 0) {
                    ds_diag_error(lower->diag, stmt->as.cmd_stmt.redirect.op_span, "expected redirection target");
                } else {
                    ds_redirect_clone(&out->as.cmd_stmt.redirect, &stmt->as.cmd_stmt.redirect);
                    validate_interpolation(lower, stmt->as.cmd_stmt.redirect.target, stmt->as.cmd_stmt.redirect.target_span);
                }
            }
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
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_FOR_ARRAY, stmt->span);
            out->as.for_stmt.name = str_clone(stmt->as.for_stmt.key_name);
            if (stmt->as.for_stmt.has_value_name) {
                ds_diag_error(lower->diag, stmt->span, "map iteration is deferred in v0.10.0; use direct map access instead");
            }
            SymKind iterable_kind = SYM_UNKNOWN;
            out->as.for_stmt.iterable = lower_expr(lower, stmt->as.for_stmt.iterable, &iterable_kind);
            if (!stmt->as.for_stmt.has_value_name && iterable_kind != SYM_ARRAY && iterable_kind != SYM_UNKNOWN) {
                ds_diag_error(lower->diag, stmt->as.for_stmt.iterable->span, "for loop iterable must be an array in v0.10.0");
            }
            Scope local;
            scope_init(&local, lower->scope);
            Scope *saved = lower->scope;
            lower->scope = &local;
            scope_define(lower, &local, stmt->as.for_stmt.key_name, SYM_UNKNOWN, stmt->span);
            out->as.for_stmt.body = lower_block(lower, stmt->as.for_stmt.body, false);
            lower->scope = saved;
            scope_free(&local);
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
