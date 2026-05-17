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
static bool next_at(Parser *p, DsTokenKind kind) { return p->pos + 1 < p->tokens->len && p->tokens->items[p->pos + 1].kind == kind; }
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

static void word_vec_push(DsWordVec *vec, DsWord word) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsWord *)ds_xrealloc(vec->items, vec->cap * sizeof(DsWord));
    }
    vec->items[vec->len++] = word;
}

static void expr_vec_push(DsExprVec *vec, DsExpr *expr) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsExpr **)ds_xrealloc(vec->items, vec->cap * sizeof(DsExpr *));
    }
    vec->items[vec->len++] = expr;
}

static void map_entry_vec_push(DsMapEntryVec *vec, DsMapEntry entry) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsMapEntry *)ds_xrealloc(vec->items, vec->cap * sizeof(DsMapEntry));
    }
    vec->items[vec->len++] = entry;
}

static void fn_param_vec_push(DsFnParamVec *vec, DsFnParam param) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = (DsFnParam *)ds_xrealloc(vec->items, vec->cap * sizeof(DsFnParam));
    }
    vec->items[vec->len++] = param;
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
           kind == DS_TOK_TYPE_INT || kind == DS_TOK_TYPE_BOOL || kind == DS_TOK_RUN || kind == DS_TOK_FN;
}

static bool is_redirect_token(DsTokenKind kind) {
    return kind == DS_TOK_REDIRECT_OUT || kind == DS_TOK_REDIRECT_OUT_APPEND ||
           kind == DS_TOK_REDIRECT_ERR || kind == DS_TOK_REDIRECT_ERR_APPEND ||
           kind == DS_TOK_REDIRECT_ALL || kind == DS_TOK_REDIRECT_ALL_APPEND;
}

static bool is_unsupported_command_operator(const DsToken *tok) {
    if (tok->kind == DS_TOK_UNKNOWN && tok->text.len == 1 &&
        (tok->text.data[0] == '|' || tok->text.data[0] == '&')) return true;
    return tok->kind == DS_TOK_GREATER || tok->kind == DS_TOK_GREATER_EQUAL || tok->kind == DS_TOK_LESS || tok->kind == DS_TOK_LESS_EQUAL;
}

static void report_unsupported_command_operator(Parser *p, const DsToken *tok) {
    if (tok->text.len == 1 && tok->text.data[0] == '|') {
        ds_diag_error(p->diag, tok->span, "pipelines are not supported in v0.7.0");
    } else {
        ds_diag_error(p->diag, tok->span, "unsupported command operator `%.*s` in v0.7.0; use `|>`, `|>>`, `!>`, `!>>`, `&>`, or `&>>` for redirection", (int)tok->text.len, tok->text.data);
    }
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
static DsExpr *parse_expr(Parser *p);
static void parse_call_args(Parser *p, DsExprVec *args);

static void skip_collection_newlines(Parser *p) { skip_newlines(p); }

static DsExpr *parse_array_literal(Parser *p) {
    DsToken *open = previous(p);
    DsExpr *expr = new_expr(DS_EXPR_ARRAY, open->span);
    skip_collection_newlines(p);
    if (!at(p, DS_TOK_RBRACKET)) {
        while (!at_end(p) && !at(p, DS_TOK_RBRACKET)) {
            expr_vec_push(&expr->as.array.elements, parse_expr(p));
            skip_collection_newlines(p);
            if (!advance_if(p, DS_TOK_COMMA)) break;
            skip_collection_newlines(p);
            if (at(p, DS_TOK_RBRACKET)) {
                ds_diag_error(p->diag, peek(p)->span, "expected array element after `,`");
                break;
            }
        }
    }
    if (!expect(p, DS_TOK_RBRACKET, "expected `]` to close array literal")) return expr;
    expr->span.end = previous(p)->span.end;
    return expr;
}

static DsExpr *parse_map_literal(Parser *p) {
    DsToken *open = previous(p);
    DsExpr *expr = new_expr(DS_EXPR_MAP, open->span);
    skip_collection_newlines(p);
    if (at(p, DS_TOK_RBRACE)) {
        ds_diag_error(p->diag, peek(p)->span, "empty map literals are deferred in v0.10.0");
        advance(p);
        expr->span.end = previous(p)->span.end;
        return expr;
    }
    while (!at_end(p) && !at(p, DS_TOK_RBRACE)) {
        DsMapEntry entry;
        memset(&entry, 0, sizeof(entry));
        if (advance_if(p, DS_TOK_IDENT) || advance_if(p, DS_TOK_STRING)) {
            DsToken *key = previous(p);
            entry.key = copy_token_text(key);
            entry.quoted_key = key->kind == DS_TOK_STRING;
            entry.span = key->span;
        } else {
            ds_diag_error(p->diag, peek(p)->span, "expected map key before `:`");
            break;
        }
        if (!expect(p, DS_TOK_COLON, "expected `:` after map key")) break;
        skip_collection_newlines(p);
        if (at(p, DS_TOK_COMMA) || at(p, DS_TOK_RBRACE) || at_end(p)) {
            ds_diag_error(p->diag, peek(p)->span, "expected map value after `:`");
            break;
        }
        entry.value = parse_expr(p);
        if (entry.value) entry.span.end = entry.value->span.end;
        map_entry_vec_push(&expr->as.map.entries, entry);
        skip_collection_newlines(p);
        if (!advance_if(p, DS_TOK_COMMA)) break;
        skip_collection_newlines(p);
        if (at(p, DS_TOK_RBRACE)) {
            ds_diag_error(p->diag, peek(p)->span, "expected map entry after `,`");
            break;
        }
    }
    if (!expect(p, DS_TOK_RBRACE, "expected `}` to close map literal")) return expr;
    expr->span.end = previous(p)->span.end;
    return expr;
}

static void parse_command_words_until_end(Parser *p, DsWordVec *words, DsSpan *span, bool reject_redirection) {
    DsWord current = {0};
    size_t current_cap = 0;
    size_t prev_end = 0;
    bool have_current = false;
    while (!is_stmt_end(p)) {
        if (is_unsupported_command_operator(peek(p))) {
            report_unsupported_command_operator(p, peek(p));
            while (!is_stmt_end(p)) advance(p);
            break;
        }
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
            current.text.data = NULL;
            current.text.len = 0;
            current_cap = 0;
            have_current = false;
        }
        if (!have_current) {
            current_cap = tok->text.len + 1;
            current.text.data = (char *)ds_xcalloc(current_cap, 1);
            current.text.len = 0;
            current.span = tok->span;
            have_current = true;
        } else if (current.text.len + tok->text.len + 1 > current_cap) {
            current_cap = (current.text.len + tok->text.len + 1) * 2;
            current.text.data = (char *)ds_xrealloc(current.text.data, current_cap);
        }
        memcpy(current.text.data + current.text.len, tok->text.data, tok->text.len);
        current.text.len += tok->text.len;
        current.text.data[current.text.len] = '\0';
        current.span.end = tok->span.end;
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
    if (advance_if(p, DS_TOK_LBRACKET)) {
        return parse_array_literal(p);
    }
    if (advance_if(p, DS_TOK_LBRACE)) {
        return parse_map_literal(p);
    }

    ds_diag_error(p->diag, tok->span, "expected expression");
    return new_expr(DS_EXPR_ERROR, tok->span);
}

static DsExpr *parse_postfix(Parser *p) {
    DsExpr *expr = parse_primary(p);
    while (expr && expr->kind == DS_EXPR_IDENT && advance_if(p, DS_TOK_LPAREN)) {
        DsToken *open = previous(p);
        DsExpr *call = new_expr(DS_EXPR_CALL, (DsSpan){expr->span.start, open->span.end, expr->span.source});
        call->as.call.name = copy_token_text(&(DsToken){.text = expr->as.text, .span = expr->span});
        parse_call_args(p, &call->as.call.args);
        if (!expect(p, DS_TOK_RPAREN, "expected `)` after function call arguments")) break;
        call->span.end = previous(p)->span.end;
        expr = call;
    }
    while (advance_if(p, DS_TOK_LBRACKET)) {
        DsToken *open = previous(p);
        DsExpr *idx = NULL;
        if (at(p, DS_TOK_RBRACKET)) {
            ds_diag_error(p->diag, open->span, "expected index expression after `[` ");
        } else {
            idx = parse_expr(p);
        }
        if (!expect(p, DS_TOK_RBRACKET, "expected `]` after index expression")) break;
        DsExpr *index_expr = new_expr(DS_EXPR_INDEX, (DsSpan){expr ? expr->span.start : open->span.start, previous(p)->span.end, open->span.source});
        index_expr->as.index.object = expr;
        index_expr->as.index.index = idx;
        expr = index_expr;
    }
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

    while (!is_stmt_end(p) && !at(p, DS_TOK_LBRACE) && !at(p, DS_TOK_RBRACE) && !at(p, DS_TOK_RPAREN) && !at(p, DS_TOK_RBRACKET) && !at(p, DS_TOK_COMMA)) {
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

static void parse_call_args(Parser *p, DsExprVec *args) {
    if (at(p, DS_TOK_RPAREN)) return;
    while (!at_end(p) && !at(p, DS_TOK_RPAREN)) {
        expr_vec_push(args, parse_expr(p));
        if (!advance_if(p, DS_TOK_COMMA)) break;
        if (at(p, DS_TOK_RPAREN)) {
            ds_diag_error(p->diag, peek(p)->span, "expected function call argument after `,`");
            break;
        }
    }
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

static DsStmt *parse_call_stmt(Parser *p) {
    DsToken *name = advance(p);
    DsToken *open = NULL;
    if (!expect(p, DS_TOK_LPAREN, "expected `(` after function name")) return NULL;
    open = previous(p);
    DsStmt *stmt = new_stmt(DS_STMT_CALL, (DsSpan){name->span.start, open->span.end, name->span.source});
    stmt->as.call_stmt.name = copy_token_text(name);
    parse_call_args(p, &stmt->as.call_stmt.args);
    if (!expect(p, DS_TOK_RPAREN, "expected `)` after function call arguments")) return stmt;
    stmt->span.end = previous(p)->span.end;
    if (!is_stmt_end(p)) {
        ds_diag_error(p->diag, peek(p)->span, "expected end of function call statement");
        while (!is_stmt_end(p)) advance(p);
    }
    consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_push_stmt(Parser *p) {
    DsToken *name = advance(p);
    DsStmt *stmt = new_stmt(DS_STMT_PUSH, name->span);
    stmt->as.push_stmt.name = copy_token_text(name);
    expect(p, DS_TOK_DOT, "expected `.` before `push`");
    DsToken *push_name = advance(p);
    if (!(push_name->text.len == 4 && memcmp(push_name->text.data, "push", 4) == 0)) {
        ds_diag_error(p->diag, push_name->span, "only `push` collection method is supported in v0.10.0");
    }
    if (!expect(p, DS_TOK_LPAREN, "expected `(` after `push`")) return stmt;
    if (at(p, DS_TOK_RPAREN)) {
        ds_diag_error(p->diag, peek(p)->span, "expected value argument for `push`");
    } else {
        stmt->as.push_stmt.value = parse_expr(p);
    }
    if (advance_if(p, DS_TOK_COMMA)) {
        ds_diag_error(p->diag, previous(p)->span, "`push` accepts exactly one argument in v0.10.0");
        while (!at_end(p) && !at(p, DS_TOK_RPAREN) && !is_stmt_end(p)) advance(p);
    }
    if (!expect(p, DS_TOK_RPAREN, "expected `)` after `push` argument")) return stmt;
    stmt->span.end = previous(p)->span.end;
    if (!is_stmt_end(p)) {
        ds_diag_error(p->diag, peek(p)->span, "expected end of push statement");
        while (!is_stmt_end(p)) advance(p);
    }
    consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_for(Parser *p) {
    DsToken *start = previous(p);
    if (!expect_identifier_like(p, "expected loop variable after `for`")) return NULL;
    DsToken *name = previous(p);
    DsStmt *stmt = new_stmt(DS_STMT_FOR, start->span);
    stmt->as.for_stmt.key_name = copy_token_text(name);
    if (advance_if(p, DS_TOK_COMMA)) {
        stmt->as.for_stmt.has_value_name = true;
        if (!expect_identifier_like(p, "expected value loop variable after `,`")) return stmt;
        stmt->as.for_stmt.value_name = copy_token_text(previous(p));
    }
    if (!expect(p, DS_TOK_IN, "expected `in` after loop variable")) return stmt;
    if (at(p, DS_TOK_LBRACE)) ds_diag_error(p->diag, peek(p)->span, "expected iterable expression after `in`");
    stmt->as.for_stmt.iterable = parse_expr(p);
    if (!expect(p, DS_TOK_LBRACE, "expected `{` after for iterable")) return stmt;
    stmt->as.for_stmt.body = parse_block(p);
    stmt->span = (DsSpan){start->span.start, stmt->as.for_stmt.body ? stmt->as.for_stmt.body->span.end : previous(p)->span.end, start->span.source};
    consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_fn(Parser *p, bool top_level) {
    DsToken *start = previous(p);
    if (!top_level) ds_diag_error(p->diag, start->span, "function declarations are only allowed at top level in v0.9.0");
    if (!expect_identifier_like(p, "expected function name after `fn`")) return NULL;
    DsToken *name = previous(p);
    DsStmt *stmt = new_stmt(DS_STMT_FN, start->span);
    stmt->as.fn_stmt.name = copy_token_text(name);
    if (!expect(p, DS_TOK_LPAREN, "expected `(` after function name")) return stmt;
    bool seen_default = false;
    if (!at(p, DS_TOK_RPAREN)) {
        while (!at_end(p) && !at(p, DS_TOK_RPAREN)) {
            DsFnParam param;
            memset(&param, 0, sizeof(param));
            if (!expect_identifier_like(p, "expected parameter name")) break;
            DsToken *param_name = previous(p);
            param.name = copy_token_text(param_name);
            param.span = param_name->span;
            if (advance_if(p, DS_TOK_COLON)) {
                param.has_type = true;
                parse_type(p, &param.type);
            }
            if (advance_if(p, DS_TOK_EQUAL)) {
                seen_default = true;
                if (is_stmt_end(p) || at(p, DS_TOK_COMMA) || at(p, DS_TOK_RPAREN)) {
                    ds_diag_error(p->diag, previous(p)->span, "expected literal default value after `=`");
                } else {
                    param.default_value = parse_expr(p);
                    if (param.default_value) param.span.end = param.default_value->span.end;
                }
            } else if (seen_default) {
                ds_diag_error(p->diag, param.span,
                              "required parameter `%.*s` cannot follow a default parameter",
                              (int)param.name.len, param.name.data);
            }
            fn_param_vec_push(&stmt->as.fn_stmt.params, param);
            if (!advance_if(p, DS_TOK_COMMA)) break;
            if (at(p, DS_TOK_RPAREN)) {
                ds_diag_error(p->diag, peek(p)->span, "expected parameter name after `,`");
                break;
            }
        }
    }
    if (!expect(p, DS_TOK_RPAREN, "expected `)` after function parameters")) return stmt;
    if (!expect(p, DS_TOK_LBRACE, "expected `{` after function declaration")) return stmt;
    stmt->as.fn_stmt.body = parse_block(p);
    stmt->span = (DsSpan){start->span.start, stmt->as.fn_stmt.body ? stmt->as.fn_stmt.body->span.end : previous(p)->span.end, start->span.source};
    consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_cmd(Parser *p) {
    DsToken *start = peek(p);
    DsStmt *stmt = new_stmt(DS_STMT_CMD, start->span);
    DsWord current = {0};
    size_t current_cap = 0;
    size_t prev_end = 0;
    bool have_current = false;

    while (!is_stmt_end(p)) {
        if (is_unsupported_command_operator(peek(p))) {
            if (have_current) {
                word_vec_push(&stmt->as.cmd_stmt.words, current);
                current.text.data = NULL;
                current.text.len = 0;
                current_cap = 0;
                have_current = false;
            }
            report_unsupported_command_operator(p, peek(p));
            while (!is_stmt_end(p)) advance(p);
            break;
        }
        if (is_redirect_token(peek(p)->kind)) {
            if (have_current) {
                word_vec_push(&stmt->as.cmd_stmt.words, current);
                current.text.data = NULL;
                current.text.len = 0;
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
            current.text.data = NULL;
            current.text.len = 0;
            current_cap = 0;
            have_current = false;
        }

        if (!have_current) {
            current_cap = tok->text.len + 1;
            current.text.data = (char *)ds_xcalloc(current_cap, 1);
            current.text.len = 0;
            current.span = tok->span;
            have_current = true;
        } else if (current.text.len + tok->text.len + 1 > current_cap) {
            current_cap = (current.text.len + tok->text.len + 1) * 2;
            current.text.data = (char *)ds_xrealloc(current.text.data, current_cap);
        }

        memcpy(current.text.data + current.text.len, tok->text.data, tok->text.len);
        current.text.len += tok->text.len;
        current.text.data[current.text.len] = '\0';
        current.span.end = tok->span.end;
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
    if (advance_if(p, DS_TOK_FN)) return parse_fn(p, false);
    if (at(p, DS_TOK_SCRIPT)) {
        ds_diag_error(p->diag, peek(p)->span, "`script` block is only allowed at top level before executable statements");
        advance(p);
        return NULL;
    }
    if (advance_if(p, DS_TOK_LET)) return parse_let(p);
    if (advance_if(p, DS_TOK_IF)) return parse_if(p);
    if (advance_if(p, DS_TOK_FOR)) return parse_for(p);
    if (advance_if(p, DS_TOK_WHILE)) {
        ds_diag_error(p->diag, previous(p)->span, "`while` loops are deferred in v0.10.0 because reassignment is not implemented yet");
        while (!at_end(p) && !at(p, DS_TOK_LBRACE) && !is_stmt_end(p)) advance(p);
        if (advance_if(p, DS_TOK_LBRACE)) {
            int depth = 1;
            while (!at_end(p) && depth > 0) {
                if (advance_if(p, DS_TOK_LBRACE)) depth++;
                else if (advance_if(p, DS_TOK_RBRACE)) depth--;
                else advance(p);
            }
        } else {
            while (!is_stmt_end(p)) advance(p);
        }
        consume_statement_end(p);
        return NULL;
    }
    if (at(p, DS_TOK_IDENT) && next_at(p, DS_TOK_DOT)) return parse_push_stmt(p);
    if (at(p, DS_TOK_IDENT) && next_at(p, DS_TOK_LPAREN)) return parse_call_stmt(p);
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
        if (advance_if(&p, DS_TOK_FN)) {
            DsStmt *stmt = parse_fn(&p, true);
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
