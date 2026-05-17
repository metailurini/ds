#include "ds.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const DsTokenVec *tokens;
    size_t pos;
    DsDiag *diag;
} Parser;

static DsToken *peek(Parser *p) { return &p->tokens->items[p->pos]; }
static DsToken *previous(Parser *p) { return &p->tokens->items[p->pos - 1]; }
static bool at(Parser *p, DsTokenKind kind) { return peek(p)->kind == kind; }
static bool at_end(Parser *p) { return at(p, DS_TOK_EOF); }

static bool advance_if(Parser *p, DsTokenKind kind) {
    if (!at(p, kind)) return false;
    p->pos++;
    return true;
}

static DsToken *advance(Parser *p) {
    if (!at_end(p)) p->pos++;
    return previous(p);
}

static DsStr copy_token_text(const DsToken *token) {
    DsStr s;
    s.data = ds_str_dup_range(token->text.data, token->text.len);
    s.len = token->text.len;
    return s;
}

static void stmt_vec_push(DsStmtVec *vec, DsStmt *stmt) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 16;
        vec->items = (DsStmt **)ds_xrealloc(vec->items, vec->cap * sizeof(DsStmt *));
    }
    vec->items[vec->len++] = stmt;
}

static void word_vec_push(DsWordVec *vec, DsStr word) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsStr *)ds_xrealloc(vec->items, vec->cap * sizeof(DsStr));
    }
    vec->items[vec->len++] = word;
}

static void script_decl_vec_push(DsScriptDeclVec *vec, DsScriptDecl decl) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsScriptDecl *)ds_xrealloc(vec->items, vec->cap * sizeof(DsScriptDecl));
    }
    vec->items[vec->len++] = decl;
}

static DsExpr *new_expr(DsExprKind kind, DsSpan span) {
    DsExpr *expr = (DsExpr *)ds_xcalloc(1, sizeof(DsExpr));
    expr->kind = kind;
    expr->span = span;
    return expr;
}

static DsStmt *new_stmt(DsStmtKind kind, DsSpan span) {
    DsStmt *stmt = (DsStmt *)ds_xcalloc(1, sizeof(DsStmt));
    stmt->kind = kind;
    stmt->span = span;
    return stmt;
}

static void skip_newlines(Parser *p) {
    while (advance_if(p, DS_TOK_NEWLINE)) {}
}

static bool is_stmt_end(Parser *p) {
    return at(p, DS_TOK_NEWLINE) || at(p, DS_TOK_EOF) || at(p, DS_TOK_RBRACE);
}

static void consume_statement_end(Parser *p) {
    if (at(p, DS_TOK_NEWLINE)) {
        skip_newlines(p);
    }
}

static bool expect(Parser *p, DsTokenKind kind, const char *message) {
    if (advance_if(p, kind)) return true;
    ds_diag_error(p->diag, peek(p)->span, "%s", message);
    return false;
}

static bool is_identifier_like(DsTokenKind kind) {
    return kind == DS_TOK_IDENT || kind == DS_TOK_SCRIPT || kind == DS_TOK_IMPORT || kind == DS_TOK_ARG ||
           kind == DS_TOK_OPTION || kind == DS_TOK_FLAG || kind == DS_TOK_TYPE_STRING ||
           kind == DS_TOK_TYPE_INT || kind == DS_TOK_TYPE_BOOL || kind == DS_TOK_RUN;
}

static bool is_redirect_token(DsTokenKind kind) {
    return kind == DS_TOK_REDIRECT_OUT || kind == DS_TOK_REDIRECT_OUT_APPEND ||
           kind == DS_TOK_REDIRECT_ERR || kind == DS_TOK_REDIRECT_ERR_APPEND ||
           kind == DS_TOK_REDIRECT_ALL || kind == DS_TOK_REDIRECT_ALL_APPEND;
}

static DsRedirectKind redirect_kind_from_token(DsTokenKind kind) {
    switch (kind) {
        case DS_TOK_REDIRECT_OUT: return DS_REDIRECT_OUT;
        case DS_TOK_REDIRECT_OUT_APPEND: return DS_REDIRECT_OUT_APPEND;
        case DS_TOK_REDIRECT_ERR: return DS_REDIRECT_ERR;
        case DS_TOK_REDIRECT_ERR_APPEND: return DS_REDIRECT_ERR_APPEND;
        case DS_TOK_REDIRECT_ALL: return DS_REDIRECT_ALL;
        case DS_TOK_REDIRECT_ALL_APPEND: return DS_REDIRECT_ALL_APPEND;
        default: return DS_REDIRECT_NONE;
    }
}

static bool expect_identifier_like(Parser *p, const char *message) {
    if (is_identifier_like(peek(p)->kind)) {
        advance(p);
        return true;
    }
    ds_diag_error(p->diag, peek(p)->span, "%s", message);
    return false;
}

static int precedence(DsTokenKind kind) {
    switch (kind) {
        case DS_TOK_EQUAL_EQUAL:
        case DS_TOK_BANG_EQUAL: return 1;
        case DS_TOK_GREATER:
        case DS_TOK_GREATER_EQUAL:
        case DS_TOK_LESS:
        case DS_TOK_LESS_EQUAL: return 2;
        case DS_TOK_PLUS:
        case DS_TOK_MINUS: return 3;
        case DS_TOK_STAR:
        case DS_TOK_SLASH: return 4;
        default: return 0;
    }
}

static DsExpr *parse_expr_prec(Parser *p, int min_prec);

static void parse_command_words_until_end(Parser *p, DsWordVec *words, DsSpan *span, bool reject_redirection) {
    DsStr current = {0};
    size_t current_cap = 0;
    size_t prev_end = 0;
    bool have_current = false;
    while (!is_stmt_end(p)) {
        if (is_redirect_token(peek(p)->kind)) {
            if (reject_redirection) {
                ds_diag_error(p->diag, peek(p)->span, "captured `run` commands do not support redirection in v0.7.0");
                while (!is_stmt_end(p)) advance(p);
            }
            break;
        }
        DsToken *tok = advance(p);
        bool adjacent = have_current && tok->span.start.offset == prev_end;
        if (!adjacent && have_current) {
            word_vec_push(words, current);
            current.data = NULL;
            current.len = 0;
            current_cap = 0;
            have_current = false;
        }
        if (!have_current) {
            current_cap = tok->text.len + 1;
            current.data = (char *)ds_xcalloc(current_cap, 1);
            current.len = 0;
            have_current = true;
        } else if (current.len + tok->text.len + 1 > current_cap) {
            current_cap = (current.len + tok->text.len + 1) * 2;
            current.data = (char *)ds_xrealloc(current.data, current_cap);
        }
        memcpy(current.data + current.len, tok->text.data, tok->text.len);
        current.len += tok->text.len;
        current.data[current.len] = '\0';
        prev_end = tok->span.end.offset;
        if (span) span->end = tok->span.end;
    }
    if (have_current) word_vec_push(words, current);
}

static DsExpr *parse_run_expr(Parser *p) {
    DsToken *run = previous(p);
    DsExpr *expr = new_expr(DS_EXPR_RUN, run->span);
    parse_command_words_until_end(p, &expr->as.run.words, &expr->span, true);
    if (expr->as.run.words.len == 0) {
        ds_diag_error(p->diag, run->span, "expected command after `run`");
    }
    return expr;
}

static DsExpr *parse_primary(Parser *p) {
    DsToken *tok = peek(p);
    if (advance_if(p, DS_TOK_RUN)) {
        return parse_run_expr(p);
    }
    if (advance_if(p, DS_TOK_IDENT)) {
        DsExpr *expr = new_expr(DS_EXPR_IDENT, tok->span);
        expr->as.text = copy_token_text(tok);
        return expr;
    }
    if (advance_if(p, DS_TOK_TYPE_STRING) || advance_if(p, DS_TOK_TYPE_INT) ||
        advance_if(p, DS_TOK_TYPE_BOOL) || advance_if(p, DS_TOK_SCRIPT) ||
        advance_if(p, DS_TOK_IMPORT) || advance_if(p, DS_TOK_ARG) || advance_if(p, DS_TOK_OPTION) || advance_if(p, DS_TOK_FLAG)) {
        DsToken *used = previous(p);
        DsExpr *expr = new_expr(DS_EXPR_IDENT, used->span);
        expr->as.text = copy_token_text(used);
        return expr;
    }
    if (advance_if(p, DS_TOK_STRING)) {
        DsExpr *expr = new_expr(DS_EXPR_STRING, tok->span);
        expr->as.text = copy_token_text(tok);
        return expr;
    }
    if (advance_if(p, DS_TOK_INT)) {
        DsExpr *expr = new_expr(DS_EXPR_INT, tok->span);
        expr->as.text = copy_token_text(tok);
        return expr;
    }
    if (advance_if(p, DS_TOK_TRUE) || advance_if(p, DS_TOK_FALSE)) {
        DsToken *used = previous(p);
        DsExpr *expr = new_expr(DS_EXPR_BOOL, used->span);
        expr->as.boolean = used->kind == DS_TOK_TRUE;
        return expr;
    }
    if (advance_if(p, DS_TOK_LPAREN)) {
        DsToken *open = previous(p);
        DsExpr *expr = parse_expr_prec(p, 1);
        if (!expect(p, DS_TOK_RPAREN, "expected `)` after expression")) {
            return expr ? expr : new_expr(DS_EXPR_ERROR, open->span);
        }
        if (expr) expr->span.end = previous(p)->span.end;
        return expr;
    }

    ds_diag_error(p->diag, tok->span, "expected expression");
    return new_expr(DS_EXPR_ERROR, tok->span);
}

static DsExpr *parse_postfix(Parser *p) {
    DsExpr *expr = parse_primary(p);
    while (advance_if(p, DS_TOK_DOT)) {
        DsToken *dot = previous(p);
        if (!expect_identifier_like(p, "expected field name after `.`")) {
            break;
        }
        DsToken *field = previous(p);
        DsExpr *field_expr = new_expr(DS_EXPR_FIELD, (DsSpan){expr ? expr->span.start : dot->span.start, field->span.end, dot->span.source});
        field_expr->as.field.object = expr;
        field_expr->as.field.field = copy_token_text(field);
        expr = field_expr;
    }
    return expr;
}

static DsExpr *parse_unary(Parser *p) {
    if (advance_if(p, DS_TOK_BANG)) {
        DsToken *op = previous(p);
        DsExpr *right = parse_unary(p);
        DsExpr *expr = new_expr(DS_EXPR_UNARY, (DsSpan){op->span.start, right ? right->span.end : op->span.end, op->span.source});
        expr->as.unary.op = copy_token_text(op);
        expr->as.unary.right = right;
        return expr;
    }
    return parse_postfix(p);
}

static DsExpr *parse_expr_prec(Parser *p, int min_prec) {
    DsExpr *left = parse_unary(p);

    while (!is_stmt_end(p) && !at(p, DS_TOK_LBRACE) && !at(p, DS_TOK_RPAREN)) {
        DsTokenKind op_kind = peek(p)->kind;
        int prec = precedence(op_kind);
        if (prec < min_prec || prec == 0) break;

        DsToken *op = advance(p);
        DsExpr *right = parse_expr_prec(p, prec + 1);
        DsSpan span = {left ? left->span.start : op->span.start, right ? right->span.end : op->span.end, left ? left->span.source : op->span.source};
        DsExpr *binary = new_expr(DS_EXPR_BINARY, span);
        binary->as.binary.left = left;
        binary->as.binary.op = copy_token_text(op);
        binary->as.binary.right = right;
        left = binary;
    }

    return left;
}

static DsExpr *parse_expr(Parser *p) {
    return parse_expr_prec(p, 1);
}

static DsStmt *parse_stmt(Parser *p);

static DsStmt *parse_import_stmt(Parser *p, bool top_level, bool after_executable) {
    DsToken *start = previous(p);
    if (!top_level) {
        ds_diag_error(p->diag, start->span, "`import` is only allowed at top level");
    }
    if (after_executable) {
        ds_diag_error(p->diag, start->span, "`import` must appear before executable statements");
    }
    if (!expect(p, DS_TOK_STRING, "expected string literal import path")) return NULL;
    DsToken *path = previous(p);
    DsStmt *stmt = new_stmt(DS_STMT_IMPORT, (DsSpan){start->span.start, path->span.end, start->span.source});
    stmt->as.import_stmt.path = copy_token_text(path);
    if (!is_stmt_end(p)) {
        ds_diag_error(p->diag, peek(p)->span, "expected end of import statement");
        while (!is_stmt_end(p)) advance(p);
    }
    consume_statement_end(p);
    return stmt;
}

static bool parse_type(Parser *p, DsScriptType *out) {
    if (advance_if(p, DS_TOK_TYPE_STRING)) { *out = DS_SCRIPT_TYPE_STRING; return true; }
    if (advance_if(p, DS_TOK_TYPE_INT)) { *out = DS_SCRIPT_TYPE_INT; return true; }
    if (advance_if(p, DS_TOK_TYPE_BOOL)) { *out = DS_SCRIPT_TYPE_BOOL; return true; }
    ds_diag_error(p->diag, peek(p)->span, "expected type name `string`, `int`, or `bool`");
    return false;
}

static bool parse_script_decl(Parser *p, DsScriptBlock *script) {
    DsToken *start = peek(p);
    DsScriptDecl decl;
    memset(&decl, 0, sizeof(decl));

    if (advance_if(p, DS_TOK_ARG)) {
        decl.kind = DS_SCRIPT_DECL_ARG;
    } else if (advance_if(p, DS_TOK_OPTION)) {
        decl.kind = DS_SCRIPT_DECL_OPTION;
    } else if (advance_if(p, DS_TOK_FLAG)) {
        decl.kind = DS_SCRIPT_DECL_FLAG;
    } else {
        ds_diag_error(p->diag, peek(p)->span, "expected `arg`, `option`, or `flag` declaration");
        while (!is_stmt_end(p)) advance(p);
        consume_statement_end(p);
        return false;
    }

    if (!expect_identifier_like(p, "expected declaration name")) return false;
    DsToken *name = previous(p);
    decl.name = copy_token_text(name);
    if (!expect(p, DS_TOK_COLON, "expected `:` after declaration name")) return false;
    if (!parse_type(p, &decl.type)) return false;

    if (decl.kind == DS_SCRIPT_DECL_ARG) {
        if (advance_if(p, DS_TOK_EQUAL)) {
            ds_diag_error(p->diag, previous(p)->span, "`arg` declarations do not support defaults in v0.5.0");
            if (!is_stmt_end(p)) decl.default_value = parse_expr(p);
        }
    } else {
        if (!expect(p, DS_TOK_EQUAL, decl.kind == DS_SCRIPT_DECL_OPTION ?
                    "expected `=` after option type" : "expected `=` after flag type")) return false;
        if (is_stmt_end(p)) {
            ds_diag_error(p->diag, peek(p)->span, "expected default value after `=`");
            return false;
        }
        decl.default_value = parse_expr(p);
    }

    decl.span = (DsSpan){start->span.start, (decl.default_value ? decl.default_value->span.end : previous(p)->span.end), start->span.source};
    if (!is_stmt_end(p)) {
        ds_diag_error(p->diag, peek(p)->span, "expected end of declaration");
        while (!is_stmt_end(p)) advance(p);
    }
    consume_statement_end(p);
    script_decl_vec_push(&script->declarations, decl);
    return true;
}

static bool parse_script_block(Parser *p, DsAst *ast) {
    DsToken *start = previous(p);
    if (ast->statements.len > 0) {
        ds_diag_error(p->diag, start->span, "`script` block must appear before executable statements");
    }
    if (ast->has_script) {
        ds_diag_error(p->diag, start->span, "duplicate `script` block");
    }
    ast->has_script = true;
    ast->script.span = start->span;
    if (!expect(p, DS_TOK_LBRACE, "expected `{` after `script`")) return false;
    skip_newlines(p);
    while (!at_end(p) && !at(p, DS_TOK_RBRACE)) {
        parse_script_decl(p, &ast->script);
        skip_newlines(p);
    }
    if (!expect(p, DS_TOK_RBRACE, "expected `}` to close script block")) return false;
    ast->script.span = (DsSpan){start->span.start, previous(p)->span.end, start->span.source};
    consume_statement_end(p);
    return true;
}

static DsStmt *parse_block(Parser *p) {
    DsToken *open = previous(p);
    DsStmt *block = new_stmt(DS_STMT_BLOCK, open->span);
    skip_newlines(p);

    while (!at_end(p) && !at(p, DS_TOK_RBRACE)) {
        DsStmt *stmt = parse_stmt(p);
        if (stmt) stmt_vec_push(&block->as.block_stmt.statements, stmt);
        skip_newlines(p);
    }

    if (!expect(p, DS_TOK_RBRACE, "expected `}` to close block")) {
        return block;
    }
    block->span.end = previous(p)->span.end;
    return block;
}

static DsStmt *parse_let(Parser *p) {
    DsToken *start = previous(p);
    if (!expect_identifier_like(p, "expected identifier after `let`")) return NULL;
    DsToken *name = previous(p);
    if (!expect(p, DS_TOK_EQUAL, "expected `=` after variable name")) return NULL;
    if (is_stmt_end(p)) {
        ds_diag_error(p->diag, peek(p)->span, "expected expression after `=`");
        return NULL;
    }
    DsExpr *value = parse_expr(p);
    DsStmt *stmt = new_stmt(DS_STMT_LET, (DsSpan){start->span.start, value ? value->span.end : start->span.end, start->span.source});
    stmt->as.let_stmt.name = copy_token_text(name);
    stmt->as.let_stmt.value = value;
    if (!is_stmt_end(p)) {
        ds_diag_error(p->diag, peek(p)->span, "expected end of statement");
        while (!is_stmt_end(p)) advance(p);
    }
    consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_if(Parser *p) {
    DsToken *start = previous(p);
    if (at(p, DS_TOK_LBRACE)) {
        ds_diag_error(p->diag, peek(p)->span, "expected condition after `if`");
    }
    DsExpr *condition = parse_expr(p);
    if (!expect(p, DS_TOK_LBRACE, "expected `{` after if condition")) return NULL;
    DsStmt *then_branch = parse_block(p);
    DsStmt *else_branch = NULL;

    skip_newlines(p);
    if (advance_if(p, DS_TOK_ELSE)) {
        if (!expect(p, DS_TOK_LBRACE, "expected `{` after `else`")) return NULL;
        else_branch = parse_block(p);
    }

    DsSpan span = {start->span.start, else_branch ? else_branch->span.end : then_branch->span.end, start->span.source};
    DsStmt *stmt = new_stmt(DS_STMT_IF, span);
    stmt->as.if_stmt.condition = condition;
    stmt->as.if_stmt.then_branch = then_branch;
    stmt->as.if_stmt.else_branch = else_branch;
    return stmt;
}

static DsStmt *parse_cmd(Parser *p) {
    DsToken *start = peek(p);
    DsStmt *stmt = new_stmt(DS_STMT_CMD, start->span);
    DsStr current = {0};
    size_t current_cap = 0;
    size_t prev_end = 0;
    bool have_current = false;

    while (!is_stmt_end(p)) {
        if (is_redirect_token(peek(p)->kind)) {
            if (have_current) {
                word_vec_push(&stmt->as.cmd_stmt.words, current);
                current.data = NULL;
                current.len = 0;
                current_cap = 0;
                have_current = false;
            }
            DsToken *op = advance(p);
            if (stmt->as.cmd_stmt.redirect.kind != DS_REDIRECT_NONE) {
                ds_diag_error(p->diag, op->span, "duplicate redirection suffix");
            }
            stmt->as.cmd_stmt.redirect.kind = redirect_kind_from_token(op->kind);
            stmt->as.cmd_stmt.redirect.op_span = op->span;
            if (is_stmt_end(p)) {
                ds_diag_error(p->diag, op->span, "expected string redirection target after `%.*s`", (int)op->text.len, op->text.data);
                break;
            }
            if (!advance_if(p, DS_TOK_STRING)) {
                ds_diag_error(p->diag, peek(p)->span, "redirection target must be a string literal in v0.7.0");
                while (!is_stmt_end(p)) advance(p);
                break;
            }
            DsToken *target = previous(p);
            stmt->as.cmd_stmt.redirect.target = copy_token_text(target);
            stmt->as.cmd_stmt.redirect.target_span = target->span;
            stmt->span.end = target->span.end;
            if (!is_stmt_end(p)) {
                if (is_redirect_token(peek(p)->kind)) ds_diag_error(p->diag, peek(p)->span, "duplicate redirection suffix");
                else ds_diag_error(p->diag, peek(p)->span, "expected end of redirected command");
                while (!is_stmt_end(p)) advance(p);
            }
            break;
        }
        DsToken *tok = advance(p);
        bool adjacent = have_current && tok->span.start.offset == prev_end;
        if (!adjacent && have_current) {
            word_vec_push(&stmt->as.cmd_stmt.words, current);
            current.data = NULL;
            current.len = 0;
            current_cap = 0;
            have_current = false;
        }

        if (!have_current) {
            current_cap = tok->text.len + 1;
            current.data = (char *)ds_xcalloc(current_cap, 1);
            current.len = 0;
            have_current = true;
        } else if (current.len + tok->text.len + 1 > current_cap) {
            current_cap = (current.len + tok->text.len + 1) * 2;
            current.data = (char *)ds_xrealloc(current.data, current_cap);
        }

        memcpy(current.data + current.len, tok->text.data, tok->text.len);
        current.len += tok->text.len;
        current.data[current.len] = '\0';
        prev_end = tok->span.end.offset;
        stmt->span.end = tok->span.end;
    }

    if (have_current) {
        word_vec_push(&stmt->as.cmd_stmt.words, current);
    }
    consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_stmt(Parser *p) {
    if (advance_if(p, DS_TOK_IMPORT)) return parse_import_stmt(p, false, false);
    if (at(p, DS_TOK_SCRIPT)) {
        ds_diag_error(p->diag, peek(p)->span, "`script` block is only allowed at top level before executable statements");
        advance(p);
        return NULL;
    }
    if (advance_if(p, DS_TOK_LET)) return parse_let(p);
    if (advance_if(p, DS_TOK_IF)) return parse_if(p);
    if (at(p, DS_TOK_ELSE)) {
        ds_diag_error(p->diag, peek(p)->span, "unexpected `else` without matching `if`");
        advance(p);
        return NULL;
    }
    if (at(p, DS_TOK_RBRACE)) {
        ds_diag_error(p->diag, peek(p)->span, "unexpected `}`");
        advance(p);
        return NULL;
    }
    return parse_cmd(p);
}

DsAst *ds_parse(const DsTokenVec *tokens, DsDiag *diag) {
    Parser p = {tokens, 0, diag};
    DsAst *ast = (DsAst *)ds_xcalloc(1, sizeof(DsAst));
    bool after_executable = false;
    if (tokens->len > 0) ast->span.start = tokens->items[0].span.start;
    skip_newlines(&p);
    while (!at_end(&p)) {
        if (advance_if(&p, DS_TOK_SCRIPT)) {
            parse_script_block(&p, ast);
            skip_newlines(&p);
            continue;
        }
        if (advance_if(&p, DS_TOK_IMPORT)) {
            DsStmt *stmt = parse_import_stmt(&p, true, after_executable);
            if (stmt) stmt_vec_push(&ast->statements, stmt);
            skip_newlines(&p);
            continue;
        }
        DsStmt *stmt = parse_stmt(&p);
        if (stmt) stmt_vec_push(&ast->statements, stmt);
        after_executable = true;
        skip_newlines(&p);
    }
    if (tokens->len > 0) ast->span.end = tokens->items[tokens->len - 1].span.end;
    return ast;
}
