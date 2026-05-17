#include "ds.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum { SYM_BOOL, SYM_INT, SYM_STRING, SYM_UNKNOWN } SymKind;

typedef struct {
    char *name;
    SymKind kind;
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
} Lower;

static bool str_eq(DsStr a, const char *b) {
    size_t len = strlen(b);
    return a.len == len && memcmp(a.data, b, len) == 0;
}

static bool name_eq(DsStr a, const char *b) {
    size_t len = strlen(b);
    return a.len == len && memcmp(a.data, b, len) == 0;
}

static void scope_init(Scope *scope, Scope *parent) {
    scope->parent = parent;
    scope->items = NULL;
    scope->len = 0;
    scope->cap = 0;
}

static void scope_free(Scope *scope) {
    for (size_t i = 0; i < scope->len; i++) free(scope->items[i].name);
    free(scope->items);
}

static Symbol *scope_find_current(Scope *scope, DsStr name) {
    for (size_t i = 0; i < scope->len; i++) {
        if (name_eq(name, scope->items[i].name)) return &scope->items[i];
    }
    return NULL;
}

static Symbol *scope_find(Scope *scope, DsStr name) {
    for (Scope *s = scope; s; s = s->parent) {
        Symbol *sym = scope_find_current(s, name);
        if (sym) return sym;
    }
    return NULL;
}

static void scope_define(Lower *lower, Scope *scope, DsStr name, SymKind kind, DsSpan span) {
    if (scope_find(scope, name)) {
        ds_diag_error(lower->diag, span, "duplicate variable `%.*s` in this scope", (int)name.len, name.data);
        return;
    }
    if (scope->len == scope->cap) {
        scope->cap = scope->cap ? scope->cap * 2 : 16;
        scope->items = (Symbol *)ds_xrealloc(scope->items, scope->cap * sizeof(Symbol));
    }
    scope->items[scope->len].name = ds_str_dup_range(name.data, name.len);
    scope->items[scope->len].kind = kind;
    scope->len++;
}

static bool validate_interpolation(Lower *lower, DsStr text, DsSpan span) {
    if (text.len < 2 || text.data[0] != '"' || text.data[text.len - 1] != '"') return true;
    for (size_t i = 1; i + 1 < text.len; i++) {
        char c = text.data[i];
        if (c == '\\') {
            if (i + 1 < text.len - 1) i++;
            continue;
        }
        if (c != '{') continue;
        size_t start = i + 1;
        size_t j = start;
        if (j < text.len - 1 && ((text.data[j] >= 'A' && text.data[j] <= 'Z') || (text.data[j] >= 'a' && text.data[j] <= 'z') || text.data[j] == '_')) {
            j++;
            while (j < text.len - 1 && ((text.data[j] >= 'A' && text.data[j] <= 'Z') || (text.data[j] >= 'a' && text.data[j] <= 'z') || (text.data[j] >= '0' && text.data[j] <= '9') || text.data[j] == '_')) j++;
            if (j < text.len - 1 && text.data[j] == '}') {
                DsStr name = {text.data + start, j - start};
                if (!scope_find(lower->scope, name)) {
                    ds_diag_error(lower->diag, span, "unknown interpolation variable `%.*s`", (int)name.len, name.data);
                    return false;
                }
                i = j;
                continue;
            }
        }
        ds_diag_error(lower->diag, span, "unsupported string interpolation; expected `{name}`");
        return false;
    }
    return true;
}

static SymKind expr_kind(Lower *lower, const DsExpr *expr);

static SymKind binary_kind(Lower *lower, const DsExpr *expr) {
    (void)expr_kind(lower, expr->as.binary.left);
    (void)expr_kind(lower, expr->as.binary.right);
    if (str_eq(expr->as.binary.op, "==") || str_eq(expr->as.binary.op, "!=") ||
        str_eq(expr->as.binary.op, ">") || str_eq(expr->as.binary.op, ">=") ||
        str_eq(expr->as.binary.op, "<") || str_eq(expr->as.binary.op, "<=")) {
        return SYM_BOOL;
    }
    ds_diag_error(lower->diag, expr->span,
                  "this expression cannot be emitted as a Bash assignment in v0.2.0; unsupported operator `%.*s` in v0.3.0",
                  (int)expr->as.binary.op.len, expr->as.binary.op.data);
    return SYM_UNKNOWN;
}

static SymKind expr_kind(Lower *lower, const DsExpr *expr) {
    if (!expr) return SYM_UNKNOWN;
    switch (expr->kind) {
        case DS_EXPR_IDENT: {
            Symbol *sym = scope_find(lower->scope, expr->as.text);
            if (!sym) {
                ds_diag_error(lower->diag, expr->span, "unknown variable `%.*s`", (int)expr->as.text.len, expr->as.text.data);
                return SYM_UNKNOWN;
            }
            return sym->kind;
        }
        case DS_EXPR_STRING:
            validate_interpolation(lower, expr->as.text, expr->span);
            return SYM_STRING;
        case DS_EXPR_INT:
            return SYM_INT;
        case DS_EXPR_BOOL:
            return SYM_BOOL;
        case DS_EXPR_UNARY:
            if (!str_eq(expr->as.unary.op, "!")) {
                ds_diag_error(lower->diag, expr->span, "unsupported unary operator `%.*s` in v0.3.0", (int)expr->as.unary.op.len, expr->as.unary.op.data);
                return SYM_UNKNOWN;
            }
            (void)expr_kind(lower, expr->as.unary.right);
            return SYM_BOOL;
        case DS_EXPR_BINARY:
            return binary_kind(lower, expr);
        case DS_EXPR_ERROR:
            return SYM_UNKNOWN;
    }
    return SYM_UNKNOWN;
}

static bool validate_cmd_word(Lower *lower, DsStr word, DsSpan span) {
    if (word.len >= 2 && word.data[0] == '$') {
        DsStr name = {word.data + 1, word.len - 1};
        if (!scope_find(lower->scope, name)) {
            ds_diag_error(lower->diag, span, "unknown command variable `%.*s`", (int)name.len, name.data);
            return false;
        }
    }
    if (word.len >= 2 && word.data[0] == '"' && word.data[word.len - 1] == '"') {
        return validate_interpolation(lower, word, span);
    }
    return true;
}

static void validate_stmt(Lower *lower, const DsStmt *stmt);

static void validate_block(Lower *lower, const DsStmt *block, bool child_scope) {
    Scope *saved = lower->scope;
    Scope *local = NULL;
    if (child_scope) {
        local = (Scope *)ds_xcalloc(1, sizeof(Scope));
        scope_init(local, saved);
        lower->scope = local;
    }
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) {
        validate_stmt(lower, block->as.block_stmt.statements.items[i]);
    }
    if (child_scope) {
        lower->scope = saved;
        scope_free(local);
        free(local);
    }
}

static void validate_stmt(Lower *lower, const DsStmt *stmt) {
    switch (stmt->kind) {
        case DS_STMT_LET: {
            SymKind kind = expr_kind(lower, stmt->as.let_stmt.value);
            scope_define(lower, lower->scope, stmt->as.let_stmt.name, kind, stmt->span);
            break;
        }
        case DS_STMT_CMD:
            for (size_t i = 0; i < stmt->as.cmd_stmt.words.len; i++) {
                validate_cmd_word(lower, stmt->as.cmd_stmt.words.items[i], stmt->span);
            }
            break;
        case DS_STMT_IF:
            (void)expr_kind(lower, stmt->as.if_stmt.condition);
            validate_block(lower, stmt->as.if_stmt.then_branch, true);
            if (stmt->as.if_stmt.else_branch) validate_block(lower, stmt->as.if_stmt.else_branch, true);
            break;
        case DS_STMT_BLOCK:
            validate_block(lower, stmt, true);
            break;
    }
}

bool ds_lower_validate(const DsAst *ast, DsDiag *diag) {
    Scope root;
    scope_init(&root, NULL);
    Lower lower = {diag, &root};
    for (size_t i = 0; i < ast->statements.len; i++) {
        validate_stmt(&lower, ast->statements.items[i]);
    }
    scope_free(&root);
    return !diag->has_error;
}
