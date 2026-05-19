#include "ds.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { SYM_LET, SYM_PARAM, SYM_LOOP, SYM_SCRIPT } SymKind;

typedef struct {
    DsStr name;
    DsSpan span;
    SymKind kind;
    size_t depth;
    bool used;
} Symbol;

typedef struct {
    Symbol *items;
    size_t len;
    size_t cap;
    FILE *out;
    size_t warnings;
} Checker;

static bool str_eq(DsStr a, const char *b) {
    size_t len = strlen(b);
    return a.len == len && memcmp(a.data, b, len) == 0;
}

static bool dsstr_eq(DsStr a, DsStr b) {
    return a.len == b.len && memcmp(a.data, b.data, a.len) == 0;
}

static void symbol_push(Checker *c, DsStr name, DsSpan span, SymKind kind, size_t depth) {
    if (c->len == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->items = (Symbol *)ds_xrealloc(c->items, c->cap * sizeof(Symbol));
    }
    c->items[c->len++] = (Symbol){name, span, kind, depth, false};
}

static void print_source_line(FILE *out, const DsSource *source, int wanted_line, int column) {
    if (!source || !source->data || wanted_line <= 0) return;
    const char *start = source->data;
    int line = 1;
    bool found = wanted_line == 1;
    for (size_t i = 0; i < source->len; i++) {
        if (line == wanted_line) {
            start = source->data + i;
            found = true;
            break;
        }
        if (source->data[i] == '\n') line++;
    }
    if (!found) return;
    const char *end = start;
    while (*end && *end != '\n' && *end != '\r') end++;
    fprintf(out, "\n  %.*s\n  ", (int)(end - start), start);
    for (int i = 1; i < column; i++) fputc(' ', out);
    fputs("^\n", out);
}

static void warning(Checker *c, DsSpan span, const char *kind, DsStr name) {
    char location[1024];
    ds_diag_format_location(span.source, span, location, sizeof(location));
    fprintf(c->out, "%s: warning: unused %s `%.*s`\n", location, kind, (int)name.len, name.data);
    print_source_line(c->out, span.source, span.start.line, span.start.column);
    c->warnings++;
}

static void use_name(Checker *c, DsStr name) {
    for (size_t i = c->len; i > 0; i--) {
        Symbol *sym = &c->items[i - 1];
        if (dsstr_eq(sym->name, name)) {
            sym->used = true;
            return;
        }
    }
}

static void use_cstr(Checker *c, const char *data, size_t len) {
    DsStr s = {(char *)data, len};
    use_name(c, s);
}

static void scan_text_for_uses(Checker *c, DsStr text) {
    for (size_t i = 0; i < text.len; i++) {
        if (text.data[i] == '$' && i + 1 < text.len && (isalpha((unsigned char)text.data[i + 1]) || text.data[i + 1] == '_')) {
            size_t start = ++i;
            while (i < text.len && (isalnum((unsigned char)text.data[i]) || text.data[i] == '_')) i++;
            use_cstr(c, text.data + start, i - start);
            if (i > 0) i--;
        } else if (text.data[i] == '{' && i + 1 < text.len && (isalpha((unsigned char)text.data[i + 1]) || text.data[i + 1] == '_')) {
            size_t start = i + 1;
            size_t j = start;
            while (j < text.len && (isalnum((unsigned char)text.data[j]) || text.data[j] == '_')) j++;
            if (j < text.len && text.data[j] == '}') {
                use_cstr(c, text.data + start, j - start);
                i = j;
            }
        }
    }
}

static void check_expr(Checker *c, const DsExpr *expr);
static void check_stmt(Checker *c, const DsStmt *stmt, size_t depth);

static void check_expr_vec(Checker *c, const DsExprVec *vec) {
    for (size_t i = 0; i < vec->len; i++) check_expr(c, vec->items[i]);
}

static void check_words(Checker *c, const DsWordVec *words) {
    for (size_t i = 0; i < words->len; i++) scan_text_for_uses(c, words->items[i].text);
}

static void check_expr(Checker *c, const DsExpr *expr) {
    if (!expr) return;
    switch (expr->kind) {
        case DS_EXPR_IDENT:
            use_name(c, expr->as.text);
            break;
        case DS_EXPR_STRING:
            scan_text_for_uses(c, expr->as.text);
            break;
        case DS_EXPR_INT:
        case DS_EXPR_BOOL:
        case DS_EXPR_ERROR:
            break;
        case DS_EXPR_RUN:
            check_words(c, &expr->as.run.words);
            break;
        case DS_EXPR_FIELD:
            check_expr(c, expr->as.field.object);
            break;
        case DS_EXPR_UNARY:
            check_expr(c, expr->as.unary.right);
            break;
        case DS_EXPR_BINARY:
            check_expr(c, expr->as.binary.left);
            check_expr(c, expr->as.binary.right);
            break;
        case DS_EXPR_CALL:
            check_expr_vec(c, &expr->as.call.args);
            break;
        case DS_EXPR_ARRAY:
            check_expr_vec(c, &expr->as.array.elements);
            break;
        case DS_EXPR_MAP:
            for (size_t i = 0; i < expr->as.map.entries.len; i++) check_expr(c, expr->as.map.entries.items[i].value);
            break;
        case DS_EXPR_INDEX:
            check_expr(c, expr->as.index.object);
            check_expr(c, expr->as.index.index);
            break;
    }
}

static void finish_scope(Checker *c, size_t base) {
    for (size_t i = base; i < c->len; i++) {
        Symbol *sym = &c->items[i];
        if (!sym->used) {
            if (sym->kind == SYM_PARAM) warning(c, sym->span, "parameter", sym->name);
            else if (sym->kind == SYM_LET) warning(c, sym->span, "variable", sym->name);
        }
    }
    c->len = base;
}

static void check_block(Checker *c, const DsStmt *block, size_t depth) {
    if (!block || block->kind != DS_STMT_BLOCK) return;
    size_t base = c->len;
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) check_stmt(c, block->as.block_stmt.statements.items[i], depth + 1);
    finish_scope(c, base);
}

static void check_stmt(Checker *c, const DsStmt *stmt, size_t depth) {
    if (!stmt) return;
    switch (stmt->kind) {
        case DS_STMT_LET:
            check_expr(c, stmt->as.let_stmt.value);
            if (!str_eq(stmt->as.let_stmt.name, "_")) symbol_push(c, stmt->as.let_stmt.name, stmt->span, SYM_LET, depth);
            break;
        case DS_STMT_IF:
            check_expr(c, stmt->as.if_stmt.condition);
            check_block(c, stmt->as.if_stmt.then_branch, depth + 1);
            check_block(c, stmt->as.if_stmt.else_branch, depth + 1);
            break;
        case DS_STMT_BLOCK:
            check_block(c, stmt, depth + 1);
            break;
        case DS_STMT_CMD:
            check_words(c, &stmt->as.cmd_stmt.words);
            scan_text_for_uses(c, stmt->as.cmd_stmt.redirect.target);
            break;
        case DS_STMT_IMPORT:
            break;
        case DS_STMT_FN: {
            size_t base = c->len;
            for (size_t i = 0; i < stmt->as.fn_stmt.params.len; i++) {
                DsFnParam *param = &stmt->as.fn_stmt.params.items[i];
                if (param->default_value) check_expr(c, param->default_value);
                if (!str_eq(param->name, "_")) symbol_push(c, param->name, param->span, SYM_PARAM, depth + 1);
            }
            check_block(c, stmt->as.fn_stmt.body, depth + 1);
            finish_scope(c, base);
            break;
        }
        case DS_STMT_CALL:
            check_expr_vec(c, &stmt->as.call_stmt.args);
            break;
        case DS_STMT_FOR: {
            check_expr(c, stmt->as.for_stmt.iterable);
            size_t base = c->len;
            symbol_push(c, stmt->as.for_stmt.key_name, stmt->span, SYM_LOOP, depth + 1);
            if (stmt->as.for_stmt.has_value_name) symbol_push(c, stmt->as.for_stmt.value_name, stmt->span, SYM_LOOP, depth + 1);
            check_block(c, stmt->as.for_stmt.body, depth + 1);
            c->len = base;
            break;
        }
        case DS_STMT_PUSH:
            use_name(c, stmt->as.push_stmt.name);
            check_expr(c, stmt->as.push_stmt.value);
            break;
        case DS_STMT_TEST:
            check_block(c, stmt->as.test_stmt.body, depth + 1);
            break;
        case DS_STMT_ASSERT:
            check_expr(c, stmt->as.assert_stmt.condition);
            break;
    }
}

bool ds_check_warnings_ast(const DsAst *ast, FILE *out) {
    Checker c = {0};
    c.out = out ? out : stderr;
    size_t base = c.len;
    if (ast->has_script) {
        for (size_t i = 0; i < ast->script.declarations.len; i++) {
            const DsScriptDecl *decl = &ast->script.declarations.items[i];
            if (decl->default_value) check_expr(&c, decl->default_value);
            symbol_push(&c, decl->name, decl->span, SYM_SCRIPT, 0);
        }
    }
    for (size_t i = 0; i < ast->statements.len; i++) check_stmt(&c, ast->statements.items[i], 0);
    finish_scope(&c, base);
    free(c.items);
    return true;
}
