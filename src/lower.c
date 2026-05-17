#include "ds.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef enum { SYM_BOOL, SYM_INT, SYM_STRING, SYM_COMMAND_RESULT, SYM_UNKNOWN } SymKind;

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

static DsStr str_clone(DsStr s) {
    DsStr out = {ds_str_dup_range(s.data, s.len), s.len};
    return out;
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

static DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out);
static bool validate_cmd_word(Lower *lower, DsStr word, DsSpan span);

static DsLowerExpr *expr_new(DsLowerExprKind kind, DsSpan span) {
    DsLowerExpr *expr = (DsLowerExpr *)ds_xcalloc(1, sizeof(DsLowerExpr));
    expr->kind = kind;
    expr->span = span;
    return expr;
}

static bool is_result_field(DsStr field, SymKind *kind_out) {
    if (str_eq(field, "stdout") || str_eq(field, "stderr")) { *kind_out = SYM_STRING; return true; }
    if (str_eq(field, "code")) { *kind_out = SYM_INT; return true; }
    if (str_eq(field, "ok") || str_eq(field, "failed")) { *kind_out = SYM_BOOL; return true; }
    return false;
}

static void clone_words(DsWordVec *dst, const DsWordVec *src) {
    dst->len = src->len;
    dst->cap = src->len;
    dst->items = (DsWord *)ds_xcalloc(dst->len ? dst->len : 1, sizeof(DsWord));
    for (size_t i = 0; i < src->len; i++) {
        dst->items[i].text = str_clone(src->items[i].text);
        dst->items[i].span = src->items[i].span;
    }
}

static DsLowerExpr *lower_binary_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    SymKind left_kind = SYM_UNKNOWN;
    SymKind right_kind = SYM_UNKNOWN;
    DsLowerExpr *left = lower_expr(lower, expr->as.binary.left, &left_kind);
    DsLowerExpr *right = lower_expr(lower, expr->as.binary.right, &right_kind);
    DsLowerExpr *out = expr_new(DS_LOWER_EXPR_BINARY, expr->span);
    out->as.binary.left = left;
    out->as.binary.op = str_clone(expr->as.binary.op);
    out->as.binary.right = right;
    if (str_eq(expr->as.binary.op, "==") || str_eq(expr->as.binary.op, "!=") ||
        str_eq(expr->as.binary.op, ">") || str_eq(expr->as.binary.op, ">=") ||
        str_eq(expr->as.binary.op, "<") || str_eq(expr->as.binary.op, "<=")) {
        *kind_out = SYM_BOOL;
        return out;
    }
    ds_diag_error(lower->diag, expr->span,
                  "this expression cannot be emitted as a Bash assignment in v0.2.0; unsupported operator `%.*s` in v0.3.0",
                  (int)expr->as.binary.op.len, expr->as.binary.op.data);
    *kind_out = SYM_UNKNOWN;
    return out;
}

static DsLowerExpr *lower_expr(Lower *lower, const DsExpr *expr, SymKind *kind_out) {
    *kind_out = SYM_UNKNOWN;
    if (!expr) return expr_new(DS_LOWER_EXPR_ERROR, (DsSpan){0});
    switch (expr->kind) {
        case DS_EXPR_IDENT: {
            Symbol *sym = scope_find(lower->scope, expr->as.text);
            if (!sym) ds_diag_error(lower->diag, expr->span, "unknown variable `%.*s`", (int)expr->as.text.len, expr->as.text.data);
            else *kind_out = sym->kind;
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_IDENT, expr->span);
            out->as.text = str_clone(expr->as.text);
            return out;
        }
        case DS_EXPR_STRING: {
            validate_interpolation(lower, expr->as.text, expr->span);
            *kind_out = SYM_STRING;
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_STRING, expr->span);
            out->as.text = str_clone(expr->as.text);
            return out;
        }
        case DS_EXPR_INT: {
            *kind_out = SYM_INT;
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_INT, expr->span);
            out->as.text = str_clone(expr->as.text);
            return out;
        }
        case DS_EXPR_BOOL: {
            *kind_out = SYM_BOOL;
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_BOOL, expr->span);
            out->as.boolean = expr->as.boolean;
            return out;
        }
        case DS_EXPR_RUN: {
            if (expr->as.run.words.len == 0) {
                ds_diag_error(lower->diag, expr->span, "expected command after `run`");
            }
            for (size_t i = 0; i < expr->as.run.words.len; i++) validate_cmd_word(lower, expr->as.run.words.items[i].text, expr->as.run.words.items[i].span);
            *kind_out = SYM_COMMAND_RESULT;
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_RUN, expr->span);
            clone_words(&out->as.run.words, &expr->as.run.words);
            return out;
        }
        case DS_EXPR_FIELD: {
            SymKind object_kind = SYM_UNKNOWN;
            DsLowerExpr *object = lower_expr(lower, expr->as.field.object, &object_kind);
            SymKind field_kind = SYM_UNKNOWN;
            if (object_kind != SYM_COMMAND_RESULT) {
                ds_diag_error(lower->diag, expr->span, "field access is only supported on command results in v0.7.0");
            } else if (!is_result_field(expr->as.field.field, &field_kind)) {
                ds_diag_error(lower->diag, expr->span, "unknown command result field `%.*s`", (int)expr->as.field.field.len, expr->as.field.field.data);
            }
            *kind_out = field_kind;
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_FIELD, expr->span);
            out->as.field.object = object;
            out->as.field.field = str_clone(expr->as.field.field);
            return out;
        }
        case DS_EXPR_UNARY: {
            SymKind right_kind = SYM_UNKNOWN;
            DsLowerExpr *right = lower_expr(lower, expr->as.unary.right, &right_kind);
            if (!str_eq(expr->as.unary.op, "!")) {
                ds_diag_error(lower->diag, expr->span, "unsupported unary operator `%.*s` in v0.3.0", (int)expr->as.unary.op.len, expr->as.unary.op.data);
                *kind_out = SYM_UNKNOWN;
            } else {
                *kind_out = SYM_BOOL;
            }
            DsLowerExpr *out = expr_new(DS_LOWER_EXPR_UNARY, expr->span);
            out->as.unary.op = str_clone(expr->as.unary.op);
            out->as.unary.right = right;
            return out;
        }
        case DS_EXPR_BINARY:
            return lower_binary_expr(lower, expr, kind_out);
        case DS_EXPR_ERROR:
            return expr_new(DS_LOWER_EXPR_ERROR, expr->span);
    }
    return expr_new(DS_LOWER_EXPR_ERROR, expr->span);
}

static bool validate_cmd_word(Lower *lower, DsStr word, DsSpan span) {
    if (word.len >= 2 && word.data[0] == '$') {
        DsStr name = {word.data + 1, word.len - 1};
        if (!scope_find(lower->scope, name)) {
            ds_diag_error(lower->diag, span, "unknown command variable `%.*s`", (int)name.len, name.data);
            return false;
        }
    }
    if (word.len >= 2 && word.data[0] == '"' && word.data[word.len - 1] == '"') return validate_interpolation(lower, word, span);
    for (size_t i = 1; i < word.len; i++) {
        if (word.data[i] == '.') {
            DsStr name = {word.data, i};
            DsStr field = {word.data + i + 1, word.len - i - 1};
            DsSpan field_span = span;
            field_span.start.offset = span.start.offset + (int)i + 1;
            field_span.start.column = span.start.column + (int)i + 1;
            field_span.end.offset = field_span.start.offset + (int)field.len;
            field_span.end.column = field_span.start.column + (int)field.len;
            if (field.len == 0) {
                ds_diag_error(lower->diag, field_span, "expected field name after `.`");
                return false;
            }
            SymKind field_kind = SYM_UNKNOWN;
            Symbol *sym = scope_find(lower->scope, name);
            if (!sym) {
                DsSpan name_span = span;
                name_span.end.offset = name_span.start.offset + (int)name.len;
                name_span.end.column = name_span.start.column + (int)name.len;
                ds_diag_error(lower->diag, name_span, "unknown command variable `%.*s`", (int)name.len, name.data);
                return false;
            }
            if (sym->kind != SYM_COMMAND_RESULT) {
                ds_diag_error(lower->diag, field_span, "field access is only supported on command results in v0.7.0");
                return false;
            }
            if (!is_result_field(field, &field_kind)) {
                ds_diag_error(lower->diag, field_span, "unsupported command result field `%.*s`", (int)field.len, field.data);
                return false;
            }
        }
    }
    return true;
}

static void lower_stmt_vec_push(DsLowerStmtVec *vec, DsLowerStmt *stmt) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 16;
        vec->items = (DsLowerStmt **)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerStmt *));
    }
    vec->items[vec->len++] = stmt;
}

static void lower_decl_vec_push(DsLowerScriptDeclVec *vec, DsLowerScriptDecl decl) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsLowerScriptDecl *)ds_xrealloc(vec->items, vec->cap * sizeof(DsLowerScriptDecl));
    }
    vec->items[vec->len++] = decl;
}

static bool parse_i64(DsStr text, int64_t *out) {
    char *tmp = ds_str_dup_range(text.data, text.len);
    char *end = NULL;
    errno = 0;
    long long value = strtoll(tmp, &end, 10);
    bool ok = errno != ERANGE && end && *end == '\0';
    if (ok) *out = (int64_t)value;
    free(tmp);
    return ok;
}

static bool decode_string_text(DsStr text, DsStr *out) {
    out->data = NULL;
    out->len = 0;
    if (text.len < 2 || text.data[0] != '"' || text.data[text.len - 1] != '"') return false;
    char *buf = (char *)ds_xcalloc(text.len, 1);
    size_t len = 0;
    for (size_t i = 1; i + 1 < text.len; i++) {
        char c = text.data[i];
        if (c == '\\' && i + 1 < text.len - 1) {
            char escaped = text.data[++i];
            if (escaped == 'n') c = '\n';
            else if (escaped == 't') c = '\t';
            else if (escaped == '"') c = '"';
            else if (escaped == '\\') c = '\\';
            else c = escaped;
        }
        buf[len++] = c;
    }
    out->data = buf;
    out->len = len;
    return true;
}

static SymKind script_type_to_sym(DsScriptType type) {
    switch (type) {
        case DS_SCRIPT_TYPE_STRING: return SYM_STRING;
        case DS_SCRIPT_TYPE_INT: return SYM_INT;
        case DS_SCRIPT_TYPE_BOOL: return SYM_BOOL;
    }
    return SYM_UNKNOWN;
}

static const char *script_type_name(DsScriptType type) {
    switch (type) {
        case DS_SCRIPT_TYPE_STRING: return "string";
        case DS_SCRIPT_TYPE_INT: return "int";
        case DS_SCRIPT_TYPE_BOOL: return "bool";
    }
    return "unknown";
}

static bool lower_script_decl(Lower *lower, const DsScriptDecl *decl, DsLowerProgram *program) {
    DsLowerScriptDecl out;
    memset(&out, 0, sizeof(out));
    out.kind = decl->kind;
    out.type = decl->type;
    out.name = str_clone(decl->name);
    out.span = decl->span;

    if (decl->kind == DS_SCRIPT_DECL_ARG && decl->type == DS_SCRIPT_TYPE_BOOL) {
        ds_diag_error(lower->diag, decl->span, "bool positional args are not supported in v0.5.0");
    }
    if (decl->kind == DS_SCRIPT_DECL_FLAG && decl->type != DS_SCRIPT_TYPE_BOOL) {
        ds_diag_error(lower->diag, decl->span, "flag `%.*s` must have type `bool`", (int)decl->name.len, decl->name.data);
    }

    if (decl->default_value) {
        out.has_default = true;
        switch (decl->type) {
            case DS_SCRIPT_TYPE_STRING:
                if (decl->default_value->kind != DS_EXPR_STRING) {
                    ds_diag_error(lower->diag, decl->default_value->span, "default for `%.*s` must be a string", (int)decl->name.len, decl->name.data);
                } else {
                    decode_string_text(decl->default_value->as.text, &out.default_text);
                }
                break;
            case DS_SCRIPT_TYPE_INT:
                if (decl->default_value->kind != DS_EXPR_INT || !parse_i64(decl->default_value->as.text, &out.default_int)) {
                    ds_diag_error(lower->diag, decl->default_value->span, "default for `%.*s` must be an int", (int)decl->name.len, decl->name.data);
                }
                break;
            case DS_SCRIPT_TYPE_BOOL:
                if (decl->default_value->kind != DS_EXPR_BOOL) {
                    ds_diag_error(lower->diag, decl->default_value->span, "default for `%.*s` must be a bool", (int)decl->name.len, decl->name.data);
                } else {
                    out.default_bool = decl->default_value->as.boolean;
                    if (decl->kind == DS_SCRIPT_DECL_FLAG && out.default_bool) {
                        ds_diag_error(lower->diag, decl->default_value->span, "flag `%.*s` default `true` is deferred until `--no-name` support exists", (int)decl->name.len, decl->name.data);
                    }
                }
                break;
        }
    } else if (decl->kind != DS_SCRIPT_DECL_ARG) {
        ds_diag_error(lower->diag, decl->span, "%s `%.*s` requires a default value", decl->kind == DS_SCRIPT_DECL_OPTION ? "option" : "flag", (int)decl->name.len, decl->name.data);
    }

    scope_define(lower, lower->scope, decl->name, script_type_to_sym(decl->type), decl->span);
    lower_decl_vec_push(&program->script_decls, out);
    (void)script_type_name;
    return !lower->diag->has_error;
}

static DsLowerStmt *stmt_new(DsLowerStmtKind kind, DsSpan span) {
    DsLowerStmt *stmt = (DsLowerStmt *)ds_xcalloc(1, sizeof(DsLowerStmt));
    stmt->kind = kind;
    stmt->span = span;
    return stmt;
}

static DsLowerStmt *lower_stmt(Lower *lower, const DsStmt *stmt);

static DsLowerStmt *lower_block(Lower *lower, const DsStmt *block, bool child_scope) {
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

static DsLowerStmt *lower_stmt(Lower *lower, const DsStmt *stmt) {
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
            out->as.cmd_stmt.words.len = stmt->as.cmd_stmt.words.len;
            out->as.cmd_stmt.words.cap = stmt->as.cmd_stmt.words.len;
            out->as.cmd_stmt.words.items = (DsWord *)ds_xcalloc(out->as.cmd_stmt.words.len ? out->as.cmd_stmt.words.len : 1, sizeof(DsWord));
            for (size_t i = 0; i < stmt->as.cmd_stmt.words.len; i++) {
                validate_cmd_word(lower, stmt->as.cmd_stmt.words.items[i].text, stmt->as.cmd_stmt.words.items[i].span);
                out->as.cmd_stmt.words.items[i].text = str_clone(stmt->as.cmd_stmt.words.items[i].text);
                out->as.cmd_stmt.words.items[i].span = stmt->as.cmd_stmt.words.items[i].span;
            }
            out->as.cmd_stmt.redirect.kind = stmt->as.cmd_stmt.redirect.kind;
            out->as.cmd_stmt.redirect.op_span = stmt->as.cmd_stmt.redirect.op_span;
            out->as.cmd_stmt.redirect.target_span = stmt->as.cmd_stmt.redirect.target_span;
            if (stmt->as.cmd_stmt.redirect.kind != DS_REDIRECT_NONE) {
                if (stmt->as.cmd_stmt.redirect.target.len == 0) {
                    ds_diag_error(lower->diag, stmt->as.cmd_stmt.redirect.op_span, "expected redirection target");
                } else {
                    out->as.cmd_stmt.redirect.target = str_clone(stmt->as.cmd_stmt.redirect.target);
                    validate_interpolation(lower, stmt->as.cmd_stmt.redirect.target, stmt->as.cmd_stmt.redirect.target_span);
                }
            }
            return out;
        }
        case DS_STMT_IF: {
            DsLowerStmt *out = stmt_new(DS_LOWER_STMT_IF, stmt->span);
            SymKind cond_kind = SYM_UNKNOWN;
            out->as.if_stmt.condition = lower_expr(lower, stmt->as.if_stmt.condition, &cond_kind);
            out->as.if_stmt.then_branch = lower_block(lower, stmt->as.if_stmt.then_branch, true);
            if (stmt->as.if_stmt.else_branch) out->as.if_stmt.else_branch = lower_block(lower, stmt->as.if_stmt.else_branch, true);
            return out;
        }
        case DS_STMT_BLOCK:
            return lower_block(lower, stmt, true);
        case DS_STMT_IMPORT:
            ds_diag_error(lower->diag, stmt->span, "unresolved import `%.*s`", (int)stmt->as.import_stmt.path.len, stmt->as.import_stmt.path.data);
            return stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
    }
    return stmt_new(DS_LOWER_STMT_BLOCK, stmt->span);
}

static void lower_expr_free(DsLowerExpr *expr) {
    if (!expr) return;
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT:
        case DS_LOWER_EXPR_STRING:
        case DS_LOWER_EXPR_INT:
            free(expr->as.text.data);
            break;
        case DS_LOWER_EXPR_RUN:
            for (size_t i = 0; i < expr->as.run.words.len; i++) free(expr->as.run.words.items[i].text.data);
            free(expr->as.run.words.items);
            break;
        case DS_LOWER_EXPR_FIELD:
            lower_expr_free(expr->as.field.object);
            free(expr->as.field.field.data);
            break;
        case DS_LOWER_EXPR_UNARY:
            free(expr->as.unary.op.data);
            lower_expr_free(expr->as.unary.right);
            break;
        case DS_LOWER_EXPR_BINARY:
            lower_expr_free(expr->as.binary.left);
            free(expr->as.binary.op.data);
            lower_expr_free(expr->as.binary.right);
            break;
        case DS_LOWER_EXPR_BOOL:
        case DS_LOWER_EXPR_ERROR:
            break;
    }
    free(expr);
}

static void lower_stmt_free(DsLowerStmt *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET:
            free(stmt->as.let_stmt.name.data);
            lower_expr_free(stmt->as.let_stmt.value);
            break;
        case DS_LOWER_STMT_IF:
            lower_expr_free(stmt->as.if_stmt.condition);
            lower_stmt_free(stmt->as.if_stmt.then_branch);
            lower_stmt_free(stmt->as.if_stmt.else_branch);
            break;
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) lower_stmt_free(stmt->as.block_stmt.statements.items[i]);
            free(stmt->as.block_stmt.statements.items);
            break;
        case DS_LOWER_STMT_CMD:
            for (size_t i = 0; i < stmt->as.cmd_stmt.words.len; i++) free(stmt->as.cmd_stmt.words.items[i].text.data);
            free(stmt->as.cmd_stmt.words.items);
            free(stmt->as.cmd_stmt.redirect.target.data);
            break;
    }
    free(stmt);
}

DsLowerProgram *ds_lower_program(const DsAst *ast, DsDiag *diag) {
    Scope root;
    scope_init(&root, NULL);
    Lower lower = {diag, &root};
    DsLowerProgram *program = (DsLowerProgram *)ds_xcalloc(1, sizeof(DsLowerProgram));
    program->span = ast->span;
    program->has_script = ast->has_script;
    if (ast->has_script) {
        for (size_t i = 0; i < ast->script.declarations.len; i++) lower_script_decl(&lower, &ast->script.declarations.items[i], program);
    }
    for (size_t i = 0; i < ast->statements.len; i++) lower_stmt_vec_push(&program->statements, lower_stmt(&lower, ast->statements.items[i]));
    scope_free(&root);
    if (diag->has_error) {
        ds_lower_program_free(program);
        return NULL;
    }
    return program;
}

bool ds_lower_validate(const DsAst *ast, DsDiag *diag) {
    DsLowerProgram *program = ds_lower_program(ast, diag);
    if (!program) return false;
    ds_lower_program_free(program);
    return true;
}

void ds_lower_program_free(DsLowerProgram *program) {
    if (!program) return;
    for (size_t i = 0; i < program->script_decls.len; i++) {
        free(program->script_decls.items[i].name.data);
        free(program->script_decls.items[i].default_text.data);
    }
    free(program->script_decls.items);
    for (size_t i = 0; i < program->statements.len; i++) lower_stmt_free(program->statements.items[i]);
    free(program->statements.items);
    free(program);
}
