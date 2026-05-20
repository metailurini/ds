#include "parser_internal.h"

DsStmt *parse_stmt(Parser *p);

DsStmt *parse_import_stmt(Parser *p, bool top_level, bool after_executable) {
    DsToken *start = parser_previous(p);
    if (!top_level) {
        ds_diag_error(p->diag, start->span, "`import` is only allowed at top level");
    }
    if (after_executable) {
        ds_diag_error(p->diag, start->span, "`import` must appear before executable statements");
    }
    if (!parser_expect(p, DS_TOK_STRING, "expected string literal import path")) return NULL;
    DsToken *path = parser_previous(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_IMPORT, (DsSpan){start->span.start, path->span.end, start->span.source});
    stmt->as.import_stmt.path = parser_copy_token_text(path);
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of import statement");
        while (!parser_is_stmt_end(p)) parser_advance(p);
    }
    parser_consume_statement_end(p);
    return stmt;
}


DsStmt *parse_block(Parser *p) {
    DsToken *open = parser_previous(p);
    DsStmt *block = parser_new_stmt(DS_STMT_BLOCK, open->span);
    parser_skip_newlines(p);

    while (!parser_at_end(p) && !parser_at(p, DS_TOK_RBRACE)) {
        DsStmt *stmt = parse_stmt(p);
        if (stmt) parser_stmt_vec_push(&block->as.block_stmt.statements, stmt);
        parser_skip_newlines(p);
    }

    if (!parser_expect(p, DS_TOK_RBRACE, "expected `}` to close block")) {
        return block;
    }
    block->span.end = parser_previous(p)->span.end;
    return block;
}

static DsStmt *parse_let(Parser *p) {
    DsToken *start = parser_previous(p);
    if (!parser_expect_identifier_like(p, "expected identifier after `let`")) return NULL;
    DsToken *name = parser_previous(p);
    if (!parser_expect(p, DS_TOK_EQUAL, "expected `=` after variable name")) return NULL;
    if (parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected expression after `=`");
        return NULL;
    }
    DsExpr *value = parse_expr(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_LET, (DsSpan){start->span.start, value ? value->span.end : start->span.end, start->span.source});
    stmt->as.let_stmt.name = parser_copy_token_text(name);
    stmt->as.let_stmt.value = value;
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of statement");
        while (!parser_is_stmt_end(p)) parser_advance(p);
    }
    parser_consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_if(Parser *p) {
    DsToken *start = parser_previous(p);
    if (parser_at(p, DS_TOK_LBRACE)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected condition after `if`");
    }
    DsExpr *condition = parse_expr(p);
    if (!parser_expect(p, DS_TOK_LBRACE, "expected `{` after if condition")) return NULL;
    DsStmt *then_branch = parse_block(p);
    DsStmt *else_branch = NULL;

    parser_skip_newlines(p);
    if (parser_advance_if(p, DS_TOK_ELSE)) {
        if (!parser_expect(p, DS_TOK_LBRACE, "expected `{` after `else`")) return NULL;
        else_branch = parse_block(p);
    }

    DsSpan span = {start->span.start, else_branch ? else_branch->span.end : then_branch->span.end, start->span.source};
    DsStmt *stmt = parser_new_stmt(DS_STMT_IF, span);
    stmt->as.if_stmt.condition = condition;
    stmt->as.if_stmt.then_branch = then_branch;
    stmt->as.if_stmt.else_branch = else_branch;
    return stmt;
}

static DsStmt *parse_call_stmt(Parser *p) {
    DsToken *name = parser_advance(p);
    DsToken *open = NULL;
    if (!parser_expect(p, DS_TOK_LPAREN, "expected `(` after function name")) return NULL;
    open = parser_previous(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_CALL, (DsSpan){name->span.start, open->span.end, name->span.source});
    stmt->as.call_stmt.name = parser_copy_token_text(name);
    parse_call_args(p, &stmt->as.call_stmt.args);
    if (!parser_expect(p, DS_TOK_RPAREN, "expected `)` after function call arguments")) return stmt;
    stmt->span.end = parser_previous(p)->span.end;
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of function call statement");
        while (!parser_is_stmt_end(p)) parser_advance(p);
    }
    parser_consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_member_call_stmt(Parser *p) {
    DsToken *object = parser_advance(p);
    parser_expect(p, DS_TOK_DOT, "expected `.` after namespace name");
    DsToken *member = parser_advance(p);
    if (!parser_expect(p, DS_TOK_LPAREN, "expected `(` after helper name")) return NULL;
    DsStmt *stmt = parser_new_stmt(DS_STMT_CALL, (DsSpan){object->span.start, parser_previous(p)->span.end, object->span.source});
    stmt->as.call_stmt.name = parser_copy_dotted_name(object, member);
    parse_call_args(p, &stmt->as.call_stmt.args);
    if (!parser_expect(p, DS_TOK_RPAREN, "expected `)` after function call arguments")) return stmt;
    stmt->span.end = parser_previous(p)->span.end;
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of helper call statement");
        while (!parser_is_stmt_end(p)) parser_advance(p);
    }
    parser_consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_push_stmt(Parser *p) {
    DsToken *name = parser_advance(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_PUSH, name->span);
    stmt->as.push_stmt.name = parser_copy_token_text(name);
    parser_expect(p, DS_TOK_DOT, "expected `.` before `push`");
    DsToken *push_name = parser_advance(p);
    if (!(push_name->text.len == 4 && memcmp(push_name->text.data, "push", 4) == 0)) {
        ds_diag_error(p->diag, push_name->span, "only `push` collection method is supported in v0.10.0");
    }
    if (!parser_expect(p, DS_TOK_LPAREN, "expected `(` after `push`")) return stmt;
    if (parser_at(p, DS_TOK_RPAREN)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected value argument for `push`");
    } else {
        stmt->as.push_stmt.value = parse_expr(p);
    }
    if (parser_advance_if(p, DS_TOK_COMMA)) {
        ds_diag_error(p->diag, parser_previous(p)->span, "`push` accepts exactly one argument in v0.10.0");
        while (!parser_at_end(p) && !parser_at(p, DS_TOK_RPAREN) && !parser_is_stmt_end(p)) parser_advance(p);
    }
    if (!parser_expect(p, DS_TOK_RPAREN, "expected `)` after `push` argument")) return stmt;
    stmt->span.end = parser_previous(p)->span.end;
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of push statement");
        while (!parser_is_stmt_end(p)) parser_advance(p);
    }
    parser_consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_for(Parser *p) {
    DsToken *start = parser_previous(p);
    if (!parser_expect_identifier_like(p, "expected loop variable after `for`")) return NULL;
    DsToken *name = parser_previous(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_FOR, start->span);
    stmt->as.for_stmt.key_name = parser_copy_token_text(name);
    if (parser_advance_if(p, DS_TOK_COMMA)) {
        stmt->as.for_stmt.has_value_name = true;
        if (!parser_expect_identifier_like(p, "expected value loop variable after `,`")) return stmt;
        stmt->as.for_stmt.value_name = parser_copy_token_text(parser_previous(p));
    }
    if (!parser_expect(p, DS_TOK_IN, "expected `in` after loop variable")) return stmt;
    if (parser_at(p, DS_TOK_LBRACE)) ds_diag_error(p->diag, parser_peek(p)->span, "expected iterable expression after `in`");
    stmt->as.for_stmt.iterable = parse_expr(p);
    if (!parser_expect(p, DS_TOK_LBRACE, "expected `{` after for iterable")) return stmt;
    stmt->as.for_stmt.body = parse_block(p);
    stmt->span = (DsSpan){start->span.start, stmt->as.for_stmt.body ? stmt->as.for_stmt.body->span.end : parser_previous(p)->span.end, start->span.source};
    parser_consume_statement_end(p);
    return stmt;
}


static DsStmt *parse_assert(Parser *p) {
    DsToken *start = parser_previous(p);
    if (p->test_depth <= 0) {
        ds_diag_error(p->diag, start->span, "`assert` is only allowed inside a test block in v0.14.0");
    }
    if (parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, start->span, "expected expression after `assert`");
        parser_consume_statement_end(p);
        return NULL;
    }
    DsExpr *condition = parse_expr(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_ASSERT, (DsSpan){start->span.start, condition ? condition->span.end : start->span.end, start->span.source});
    stmt->as.assert_stmt.condition = condition;
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of assert statement");
        while (!parser_is_stmt_end(p)) parser_advance(p);
    }
    parser_consume_statement_end(p);
    return stmt;
}


DsStmt *parse_stmt(Parser *p) {
    if (parser_advance_if(p, DS_TOK_IMPORT)) return parse_import_stmt(p, false, false);
    if (parser_advance_if(p, DS_TOK_FN)) return parse_fn(p, false);
    if (parser_at(p, DS_TOK_TEST) && parser_next_at(p, DS_TOK_STRING) && parser_peek2_at(p, DS_TOK_LBRACE)) {
        parser_advance(p);
        return parse_test(p, false);
    }
    if (parser_advance_if(p, DS_TOK_ASSERT)) return parse_assert(p);
    if (parser_at(p, DS_TOK_SCRIPT)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "`script` block is only allowed at top level before executable statements");
        parser_advance(p);
        return NULL;
    }
    if (parser_advance_if(p, DS_TOK_LET)) return parse_let(p);
    if (parser_advance_if(p, DS_TOK_IF)) return parse_if(p);
    if (parser_advance_if(p, DS_TOK_FOR)) return parse_for(p);
    if (parser_advance_if(p, DS_TOK_WHILE)) {
        ds_diag_error(p->diag, parser_previous(p)->span, "`while` loops are deferred in v0.10.0 because reassignment is not implemented yet");
        while (!parser_at_end(p) && !parser_at(p, DS_TOK_LBRACE) && !parser_is_stmt_end(p)) parser_advance(p);
        if (parser_advance_if(p, DS_TOK_LBRACE)) {
            int depth = 1;
            while (!parser_at_end(p) && depth > 0) {
                if (parser_advance_if(p, DS_TOK_LBRACE)) depth++;
                else if (parser_advance_if(p, DS_TOK_RBRACE)) depth--;
                else parser_advance(p);
            }
        } else {
            while (!parser_is_stmt_end(p)) parser_advance(p);
        }
        parser_consume_statement_end(p);
        return NULL;
    }
    if (parser_advance_if(p, DS_TOK_BREAK)) {
        ds_diag_error(p->diag, parser_previous(p)->span, "`break` is deferred in v0.10.0 loop control");
        while (!parser_is_stmt_end(p)) parser_advance(p);
        parser_consume_statement_end(p);
        return NULL;
    }
    if (parser_advance_if(p, DS_TOK_CONTINUE)) {
        ds_diag_error(p->diag, parser_previous(p)->span, "`continue` is deferred in v0.10.0 loop control");
        while (!parser_is_stmt_end(p)) parser_advance(p);
        parser_consume_statement_end(p);
        return NULL;
    }
    if (parser_at(p, DS_TOK_IDENT) && parser_next_at(p, DS_TOK_DOT)) {
        if (p->pos + 3 < p->tokens->len && p->tokens->items[p->pos + 2].kind == DS_TOK_IDENT &&
            p->tokens->items[p->pos + 2].text.len == 4 && memcmp(p->tokens->items[p->pos + 2].text.data, "push", 4) == 0 &&
            p->tokens->items[p->pos + 3].kind == DS_TOK_LPAREN) return parse_push_stmt(p);
        return parse_member_call_stmt(p);
    }
    if (parser_at(p, DS_TOK_IDENT) && parser_next_at(p, DS_TOK_LPAREN)) return parse_call_stmt(p);
    if (parser_at(p, DS_TOK_ELSE)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "unexpected `else` without matching `if`");
        parser_advance(p);
        return NULL;
    }
    if (parser_at(p, DS_TOK_RBRACE)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "unexpected `}`");
        parser_advance(p);
        return NULL;
    }
    if (parser_at(p, DS_TOK_IDENT) && parser_peek(p)->text.len == 6 &&
        memcmp(parser_peek(p)->text.data, "return", 6) == 0) {
        ds_diag_error(p->diag, parser_peek(p)->span, "function return values are deferred in v0.9.0");
        while (!parser_is_stmt_end(p)) parser_advance(p);
        parser_consume_statement_end(p);
        return NULL;
    }
    return parse_cmd(p);
}
