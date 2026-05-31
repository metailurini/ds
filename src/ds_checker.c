#include "ds_checker.h"
#include "frontend.h"

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
    bool in_test;
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

static void warning_name(Checker *c, DsSpan span, const char *kind, DsStr name) {
    ds_diag_report(c->out, span.source, span, "warning", "unused %s `%.*s`", kind, (int)name.len, name.data);
    c->warnings++;
}

static void warning_text(Checker *c, DsSpan span, const char *message) {
    ds_diag_report(c->out, span.source, span, "warning", "%s", message);
    c->warnings++;
}

static const Symbol *find_visible_symbol(const Checker *c, DsStr name) {
    for (size_t i = c->len; i > 0; i--) {
        const Symbol *sym = &c->items[i - 1];
        if (dsstr_eq(sym->name, name)) return sym;
    }
    return NULL;
}

static void warn_if_shadowing(Checker *c, DsStr name, DsSpan span) {
    const Symbol *shadowed = find_visible_symbol(c, name);
    if (!shadowed) return;
    char message[256];
    snprintf(message, sizeof(message), "declaration `%.*s` shadows an outer declaration", (int)name.len, name.data);
    warning_text(c, span, message);
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

static void scan_fragment_for_ident_uses(Checker *c, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '"' || data[i] == '\'') {
            char quote = data[i++];
            while (i < len && data[i] != quote) {
                if (data[i] == '\\' && i + 1 < len) i += 2;
                else i++;
            }
        } else if (isalpha((unsigned char)data[i]) || data[i] == '_') {
            size_t start = i;
            while (i < len && (isalnum((unsigned char)data[i]) || data[i] == '_')) i++;
            if (start == 0 || data[start - 1] != '.') use_cstr(c, data + start, i - start);
            if (i > 0) i--;
        }
    }
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
            size_t name_end = j;
            size_t index_start = 0;
            size_t index_len = 0;
            size_t call_args_start = 0;
            size_t call_args_len = 0;
            if (j < text.len && text.data[j] == '[') {
                j++;
                while (j < text.len && isspace((unsigned char)text.data[j])) j++;
                if (j < text.len && (isalpha((unsigned char)text.data[j]) || text.data[j] == '_')) {
                    index_start = j;
                    while (j < text.len && (isalnum((unsigned char)text.data[j]) || text.data[j] == '_')) j++;
                    index_len = j - index_start;
                    while (j < text.len && isspace((unsigned char)text.data[j])) j++;
                } else if (j < text.len && (text.data[j] == '"' || text.data[j] == '\'')) {
                    char quote = text.data[j++];
                    while (j < text.len && text.data[j] != quote) {
                        if (text.data[j] == '\\' && j + 1 < text.len) j += 2;
                        else j++;
                    }
                    if (j < text.len) j++;
                    while (j < text.len && isspace((unsigned char)text.data[j])) j++;
                } else {
                    while (j < text.len && text.data[j] != ']') j++;
                }
                if (j < text.len && text.data[j] == ']') j++;
            } else if (j < text.len && text.data[j] == '(') {
                int depth = 1;
                j++;
                call_args_start = j;
                while (j < text.len && depth > 0) {
                    if (text.data[j] == '"' || text.data[j] == '\'') {
                        char quote = text.data[j++];
                        while (j < text.len && text.data[j] != quote) {
                            if (text.data[j] == '\\' && j + 1 < text.len) j += 2;
                            else j++;
                        }
                        if (j < text.len) j++;
                    } else if (text.data[j] == '(') {
                        depth++;
                        j++;
                    } else if (text.data[j] == ')') {
                        depth--;
                        j++;
                    } else {
                        j++;
                    }
                }
                if (depth == 0 && j > call_args_start) call_args_len = (j - 1) - call_args_start;
            } else if (j < text.len && text.data[j] == '.') {
                j++;
                if (j < text.len && (isalpha((unsigned char)text.data[j]) || text.data[j] == '_')) {
                    while (j < text.len && (isalnum((unsigned char)text.data[j]) || text.data[j] == '_')) j++;
                }
            }
            if (j < text.len && text.data[j] == ':') {
                j++;
                while (j < text.len && text.data[j] != '}') j++;
            }
            if (j < text.len && text.data[j] == '}') {
                use_cstr(c, text.data + start, name_end - start);
                if (index_len > 0) use_cstr(c, text.data + index_start, index_len);
                if (call_args_len > 0) scan_fragment_for_ident_uses(c, text.data + call_args_start, call_args_len);
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

static void check_command(Checker *c, const DsCommand *command) {
    for (size_t s = 0; s < command->stages.len; s++) check_words(c, &command->stages.items[s].words);
    scan_text_for_uses(c, command->redirect.target);
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
        case DS_EXPR_REGEX:
        case DS_EXPR_ERROR:
            break;
        case DS_EXPR_RUN:
            check_command(c, &expr->as.run);
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
        case DS_EXPR_RANGE:
            check_expr(c, expr->as.range.start);
            check_expr(c, expr->as.range.end);
            break;
    }
}

static void finish_scope(Checker *c, size_t base) {
    for (size_t i = base; i < c->len; i++) {
        Symbol *sym = &c->items[i];
        if (!sym->used) {
            if (sym->kind == SYM_PARAM) warning_name(c, sym->span, "parameter", sym->name);
            else if (sym->kind == SYM_LET) warning_name(c, sym->span, "variable", sym->name);
        }
    }
    c->len = base;
}

static bool is_test_terminal_command(const DsStmt *stmt) {
    if (!stmt || stmt->kind != DS_STMT_CMD || stmt->as.cmd_stmt.stages.len != 1 || stmt->as.cmd_stmt.stages.items[0].words.len == 0) return false;
    DsStr first = stmt->as.cmd_stmt.stages.items[0].words.items[0].text;
    return str_eq(first, "fail") || str_eq(first, "exit");
}

static void check_block(Checker *c, const DsStmt *block, size_t depth) {
    if (!block || block->kind != DS_STMT_BLOCK) return;
    size_t base = c->len;
    bool unreachable = false;
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) {
        DsStmt *stmt = block->as.block_stmt.statements.items[i];
        if (unreachable) {
            warning_text(c, stmt->span, "unreachable statement after test-only `fail` or `exit`");
        }
        check_stmt(c, stmt, depth + 1);
        if (c->in_test && is_test_terminal_command(stmt)) unreachable = true;
    }
    finish_scope(c, base);
}

static void check_stmt(Checker *c, const DsStmt *stmt, size_t depth) {
    if (!stmt) return;
    switch (stmt->kind) {
        case DS_STMT_LET:
            check_expr(c, stmt->as.let_stmt.value);
            if (!str_eq(stmt->as.let_stmt.name, "_")) {
                warn_if_shadowing(c, stmt->as.let_stmt.name, stmt->span);
                symbol_push(c, stmt->as.let_stmt.name, stmt->span, SYM_LET, depth);
            }
            break;
        case DS_STMT_ASSIGN:
            use_name(c, stmt->as.assign_stmt.name);
            check_expr(c, stmt->as.assign_stmt.value);
            break;
        case DS_STMT_INDEX_ASSIGN:
            check_expr(c, stmt->as.index_assign_stmt.target);
            check_expr(c, stmt->as.index_assign_stmt.value);
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
            check_command(c, &stmt->as.cmd_stmt);
            break;
        case DS_STMT_IMPORT:
            break;
        case DS_STMT_FN: {
            size_t base = c->len;
            for (size_t i = 0; i < stmt->as.fn_stmt.params.len; i++) {
                DsFnParam *param = &stmt->as.fn_stmt.params.items[i];
                if (param->default_value) check_expr(c, param->default_value);
                if (!str_eq(param->name, "_")) {
                    warn_if_shadowing(c, param->name, param->span);
                    symbol_push(c, param->name, param->span, SYM_PARAM, depth + 1);
                }
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
            warn_if_shadowing(c, stmt->as.for_stmt.key_name, stmt->span);
            symbol_push(c, stmt->as.for_stmt.key_name, stmt->span, SYM_LOOP, depth + 1);
            if (stmt->as.for_stmt.has_value_name) {
                warn_if_shadowing(c, stmt->as.for_stmt.value_name, stmt->span);
                symbol_push(c, stmt->as.for_stmt.value_name, stmt->span, SYM_LOOP, depth + 1);
            }
            check_block(c, stmt->as.for_stmt.body, depth + 1);
            c->len = base;
            break;
        }
        case DS_STMT_WHILE:
            check_expr(c, stmt->as.while_stmt.condition);
            check_block(c, stmt->as.while_stmt.body, depth + 1);
            break;
        case DS_STMT_BREAK:
        case DS_STMT_CONTINUE:
            break;
        case DS_STMT_CASE:
            check_expr(c, stmt->as.case_stmt.selector);
            for (size_t i = 0; i < stmt->as.case_stmt.arms.len; i++) {
                check_block(c, stmt->as.case_stmt.arms.items[i].body, depth + 1);
            }
            break;
        case DS_STMT_PUSH:
            use_name(c, stmt->as.push_stmt.name);
            check_expr(c, stmt->as.push_stmt.value);
            break;
        case DS_STMT_TEST: {
            bool was_in_test = c->in_test;
            c->in_test = true;
            check_block(c, stmt->as.test_stmt.body, depth + 1);
            c->in_test = was_in_test;
            break;
        }
        case DS_STMT_ASSERT:
            check_expr(c, stmt->as.assert_stmt.condition);
            break;
        case DS_STMT_RETURN:
            check_expr(c, stmt->as.return_stmt.value);
            break;
        case DS_STMT_DEFER:
        case DS_STMT_TRAP:
            check_block(c, stmt->as.handler_stmt.body, depth + 1);
            break;
    }
}

size_t ds_check_warnings_ast(const DsAst *ast, FILE *out) {
    Checker c = {0};
    c.out = out ? out : stderr;
    size_t base = c.len;
    if (ast->has_script) {
        for (size_t i = 0; i < ast->script.declarations.len; i++) {
            const DsScriptDecl *decl = &ast->script.declarations.items[i];
            if (decl->default_value) check_expr(&c, decl->default_value);
            warn_if_shadowing(&c, decl->name, decl->span);
            symbol_push(&c, decl->name, decl->span, SYM_SCRIPT, 0);
        }
    }
    for (size_t i = 0; i < ast->statements.len; i++) check_stmt(&c, ast->statements.items[i], 0);
    finish_scope(&c, base);
    size_t warnings = c.warnings;
    free(c.items);
    return warnings;
}
