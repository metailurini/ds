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
    if (name->text.len == 3 && memcmp(name->text.data, "env", 3) == 0) {
        ds_diag_error(p->diag, name->span, "`env` is a reserved environment namespace in v0.27.0");
    }
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

static DsStmt *parse_assign(Parser *p) {
    DsToken *name = parser_advance(p);
    DsAssignOp op = DS_ASSIGN_SET;
    DsSpan op_span = parser_peek(p)->span;
    if (parser_advance_if(p, DS_TOK_EQUAL)) {
        op = DS_ASSIGN_SET;
    } else if ((parser_at(p, DS_TOK_PLUS) || parser_at(p, DS_TOK_MINUS) || parser_at(p, DS_TOK_STAR) || parser_at(p, DS_TOK_SLASH) || parser_at(p, DS_TOK_PERCENT)) && parser_next_at(p, DS_TOK_EQUAL)) {
        if (parser_at(p, DS_TOK_PLUS)) op = DS_ASSIGN_ADD;
        else if (parser_at(p, DS_TOK_MINUS)) op = DS_ASSIGN_SUB;
        else if (parser_at(p, DS_TOK_STAR)) op = DS_ASSIGN_MUL;
        else if (parser_at(p, DS_TOK_SLASH)) op = DS_ASSIGN_DIV;
        else op = DS_ASSIGN_MOD;
        op_span = parser_peek(p)->span;
        parser_advance(p);
        parser_advance(p);
    } else {
        ds_diag_error(p->diag, op_span, "expected assignment operator");
        return NULL;
    }
    if (parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected expression after assignment operator");
        return NULL;
    }
    DsExpr *value = parse_expr(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_ASSIGN, (DsSpan){name->span.start, value ? value->span.end : name->span.end, name->span.source});
    stmt->as.assign_stmt.name = parser_copy_token_text(name);
    stmt->as.assign_stmt.op = op;
    stmt->as.assign_stmt.value = value;
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of assignment statement");
        while (!parser_is_stmt_end(p)) parser_advance(p);
    }
    parser_consume_statement_end(p);
    return stmt;
}

static bool parse_assignment_operator(Parser *p, DsAssignOp *op) {
    if (parser_advance_if(p, DS_TOK_EQUAL)) {
        *op = DS_ASSIGN_SET;
        return true;
    }
    if ((parser_at(p, DS_TOK_PLUS) || parser_at(p, DS_TOK_MINUS) || parser_at(p, DS_TOK_STAR) ||
         parser_at(p, DS_TOK_SLASH) || parser_at(p, DS_TOK_PERCENT)) && parser_next_at(p, DS_TOK_EQUAL)) {
        if (parser_at(p, DS_TOK_PLUS)) *op = DS_ASSIGN_ADD;
        else if (parser_at(p, DS_TOK_MINUS)) *op = DS_ASSIGN_SUB;
        else if (parser_at(p, DS_TOK_STAR)) *op = DS_ASSIGN_MUL;
        else if (parser_at(p, DS_TOK_SLASH)) *op = DS_ASSIGN_DIV;
        else *op = DS_ASSIGN_MOD;
        parser_advance(p);
        parser_advance(p);
        return true;
    }
    return false;
}

static bool token_is_assignment_operator_at(const DsTokenVec *tokens, size_t i) {
    DsTokenKind kind = tokens->items[i].kind;
    if (kind == DS_TOK_EQUAL) return true;
    return i + 1 < tokens->len &&
           (kind == DS_TOK_PLUS || kind == DS_TOK_MINUS || kind == DS_TOK_STAR ||
            kind == DS_TOK_SLASH || kind == DS_TOK_PERCENT) &&
           tokens->items[i + 1].kind == DS_TOK_EQUAL;
}

static DsStmt *parse_unsupported_collection_assign(Parser *p) {
    DsToken *start = parser_peek(p);
    bool saw_field = false;
    size_t index_count = 0;

    size_t op_pos = p->pos;
    for (; op_pos < p->tokens->len; op_pos++) {
        DsTokenKind kind = p->tokens->items[op_pos].kind;
        if (kind == DS_TOK_NEWLINE || kind == DS_TOK_EOF || kind == DS_TOK_RBRACE) break;
        if (token_is_assignment_operator_at(p->tokens, op_pos)) break;
        if (kind == DS_TOK_DOT) saw_field = true;
        if (kind == DS_TOK_LBRACKET) index_count++;
    }

    DsSpan target_span = start->span;
    if (op_pos > p->pos) {
        DsToken *last = &p->tokens->items[op_pos - 1];
        target_span.end = last->span.end;
    }

    while (p->pos < op_pos) parser_advance(p);

    DsAssignOp op = DS_ASSIGN_SET;
    if (!parse_assignment_operator(p, &op)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected assignment operator");
        while (!parser_is_stmt_end(p)) parser_advance(p);
        parser_consume_statement_end(p);
        return NULL;
    }

    if (parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected expression after assignment operator");
        parser_consume_statement_end(p);
        return NULL;
    }

    if (index_count > 1) {
        ds_diag_error(p->diag, target_span, "nested collection mutation is deferred in v0.10.0");
    } else if (saw_field) {
        ds_diag_error(p->diag, target_span, "map field assignment is deferred in v0.10.0; bind a new map value instead");
    } else if (index_count > 0) {
        ds_diag_error(p->diag, target_span, "index assignment is deferred in v0.10.0; use array.push for append-only list mutation");
    } else {
        ds_diag_error(p->diag, target_span, "unsupported collection assignment target in v0.10.0");
    }

    while (!parser_is_stmt_end(p)) parser_advance(p);
    parser_consume_statement_end(p);
    return NULL;
}

static bool parser_invalid_hyphenated_env_name(Parser *p, const DsToken *field, const char *version) {
    if (!parser_at(p, DS_TOK_MINUS)) return false;
    if (p->pos + 1 >= p->tokens->len || !parser_is_identifier_like(p->tokens->items[p->pos + 1].kind)) return false;
    DsToken *dash = parser_peek(p);
    DsToken *suffix = &p->tokens->items[p->pos + 1];
    size_t len = field->text.len + 1 + suffix->text.len;
    char *name = (char *)ds_xcalloc(len + 1, 1);
    memcpy(name, field->text.data, field->text.len);
    name[field->text.len] = '-';
    memcpy(name + field->text.len + 1, suffix->text.data, suffix->text.len);
    ds_diag_error(p->diag, dash->span, "invalid environment variable name `%.*s` in %s", (int)len, name, version);
    free(name);
    while (!parser_is_stmt_end(p)) parser_advance(p);
    parser_consume_statement_end(p);
    return true;
}

static DsStmt *parse_env_assign(Parser *p) {
    DsToken *env_tok = parser_advance(p);
    if (!parser_expect(p, DS_TOK_DOT, "expected `.` after `env` in environment assignment")) return NULL;
    if (!parser_expect_identifier_like(p, "expected environment variable name after `env.`")) return NULL;
    DsToken *field = parser_previous(p);
    DsSpan op_span = parser_peek(p)->span;
    if (parser_invalid_hyphenated_env_name(p, field, "v0.27.0")) return NULL;
    if (!parser_advance_if(p, DS_TOK_EQUAL)) {
        ds_diag_error(p->diag, op_span, "environment assignment supports only `=` in v0.27.0");
        return NULL;
    }
    if (parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected expression after environment assignment operator");
        return NULL;
    }
    DsExpr *value = parse_expr(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_ASSIGN, (DsSpan){env_tok->span.start, value ? value->span.end : field->span.end, env_tok->span.source});
    size_t len = 4 + field->text.len;
    char *name = (char *)ds_xcalloc(len + 1, 1);
    memcpy(name, "env.", 4);
    memcpy(name + 4, field->text.data, field->text.len);
    stmt->as.assign_stmt.name = (DsStr){name, len};
    stmt->as.assign_stmt.op = DS_ASSIGN_SET;
    stmt->as.assign_stmt.value = value;
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of environment assignment statement");
        while (!parser_is_stmt_end(p)) parser_advance(p);
    }
    parser_consume_statement_end(p);
    return stmt;
}

static DsStr quoted_env_name_from_token(const DsToken *field) {
    size_t len = field->text.len + 2;
    char *data = (char *)ds_xcalloc(len + 1, 1);
    data[0] = '"';
    memcpy(data + 1, field->text.data, field->text.len);
    data[len - 1] = '"';
    return (DsStr){data, len};
}

static DsStmt *parse_env_unset(Parser *p) {
    DsToken *unset_tok = parser_advance(p);
    DsToken *env_tok = parser_advance(p);
    (void)env_tok;
    if (!parser_expect(p, DS_TOK_DOT, "expected `.` after `env` in environment unset")) return NULL;
    if (!parser_expect_identifier_like(p, "expected environment variable name after `env.`")) return NULL;
    DsToken *field = parser_previous(p);
    if (parser_invalid_hyphenated_env_name(p, field, "v0.27.0")) return NULL;

    DsStmt *stmt = parser_new_stmt(DS_STMT_CALL, (DsSpan){unset_tok->span.start, field->span.end, unset_tok->span.source});
    stmt->as.call_stmt.name = (DsStr){ds_str_dup_range("env.unset", strlen("env.unset")), strlen("env.unset")};
    DsExpr *arg = parser_new_expr(DS_EXPR_STRING, field->span);
    arg->as.text = quoted_env_name_from_token(field);
    parser_expr_vec_push(&stmt->as.call_stmt.args, arg);

    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of environment unset statement");
        while (!parser_is_stmt_end(p)) parser_advance(p);
    }
    parser_consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_bad_unset(Parser *p) {
    DsToken *unset_tok = parser_advance(p);
    ds_diag_error(p->diag, unset_tok->span, "unset requires an environment target like `unset env.NAME` in v0.27.0");
    while (!parser_is_stmt_end(p)) parser_advance(p);
    parser_consume_statement_end(p);
    return NULL;
}

static bool stmt_contains_assignment_operator(const Parser *p) {
    for (size_t i = p->pos; i < p->tokens->len; i++) {
        DsTokenKind kind = p->tokens->items[i].kind;
        if (kind == DS_TOK_NEWLINE || kind == DS_TOK_EOF || kind == DS_TOK_RBRACE) return false;
        if (token_is_assignment_operator_at(p->tokens, i)) return true;
    }
    return false;
}

static DsStmt *parse_return(Parser *p) {
    DsToken *start = parser_previous(p);
    if (parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, start->span, "expected expression after `return`");
        parser_consume_statement_end(p);
        return NULL;
    }
    DsExpr *value = parse_expr(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_RETURN, (DsSpan){start->span.start, value ? value->span.end : start->span.end, start->span.source});
    stmt->as.return_stmt.value = value;
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of return statement");
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

static DsStmt *parse_while(Parser *p) {
    DsToken *start = parser_previous(p);
    if (parser_at(p, DS_TOK_LBRACE)) ds_diag_error(p->diag, parser_peek(p)->span, "expected condition after `while`");
    DsExpr *condition = parse_expr(p);
    if (!parser_expect(p, DS_TOK_LBRACE, "expected `{` after while condition")) return NULL;
    DsStmt *body = parse_block(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_WHILE, (DsSpan){start->span.start, body ? body->span.end : start->span.end, start->span.source});
    stmt->as.while_stmt.condition = condition;
    stmt->as.while_stmt.body = body;
    parser_consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_loop_control(Parser *p, DsStmtKind kind) {
    DsToken *start = parser_previous(p);
    DsStmt *stmt = parser_new_stmt(kind, start->span);
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, kind == DS_STMT_BREAK ? "`break` does not accept arguments" : "`continue` does not accept arguments");
        while (!parser_is_stmt_end(p)) parser_advance(p);
    }
    parser_consume_statement_end(p);
    return stmt;
}

static bool parse_case_pattern(Parser *p, DsCasePattern *pattern) {
    memset(pattern, 0, sizeof(*pattern));
    if (parser_advance_if(p, DS_TOK_STRING)) {
        DsToken *tok = parser_previous(p);
        pattern->kind = DS_CASE_PATTERN_STRING;
        pattern->text = parser_copy_token_text(tok);
        pattern->span = tok->span;
        return true;
    }
    if (parser_advance_if(p, DS_TOK_INT)) {
        DsToken *tok = parser_previous(p);
        pattern->kind = DS_CASE_PATTERN_INT;
        pattern->text = parser_copy_token_text(tok);
        pattern->span = tok->span;
        return true;
    }
    if (parser_advance_if(p, DS_TOK_TRUE) || parser_advance_if(p, DS_TOK_FALSE)) {
        DsToken *tok = parser_previous(p);
        pattern->kind = DS_CASE_PATTERN_BOOL;
        pattern->boolean = tok->kind == DS_TOK_TRUE;
        pattern->span = tok->span;
        return true;
    }
    if (parser_at(p, DS_TOK_IDENT) && parser_peek(p)->text.len == 1 && parser_peek(p)->text.data[0] == '_') {
        DsToken *tok = parser_advance(p);
        pattern->kind = DS_CASE_PATTERN_DEFAULT;
        pattern->span = tok->span;
        return true;
    }
    ds_diag_error(p->diag, parser_peek(p)->span, "expected case pattern literal or `_`");
    return false;
}

static DsStmt *parse_case(Parser *p) {
    DsToken *start = parser_previous(p);
    if (parser_at(p, DS_TOK_DOLLAR_IDENT)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "case selectors use expression syntax; write `case name`, not `case $name`");
        parser_advance(p);
        if (parser_at(p, DS_TOK_LBRACE)) {
            int depth = 0;
            do {
                if (parser_at(p, DS_TOK_LBRACE)) depth++;
                else if (parser_at(p, DS_TOK_RBRACE)) depth--;
                parser_advance(p);
            } while (!parser_at_end(p) && depth > 0);
        } else {
            while (!parser_at_end(p) && !parser_is_stmt_end(p)) parser_advance(p);
        }
        parser_consume_statement_end(p);
        return NULL;
    }
    if (parser_at(p, DS_TOK_LBRACE)) ds_diag_error(p->diag, parser_peek(p)->span, "expected selector expression after `case`");
    DsExpr *selector = parse_expr(p);
    if (!parser_expect(p, DS_TOK_LBRACE, "expected `{` after case selector")) return NULL;
    DsStmt *stmt = parser_new_stmt(DS_STMT_CASE, start->span);
    stmt->as.case_stmt.selector = selector;
    parser_skip_newlines(p);
    while (!parser_at_end(p) && !parser_at(p, DS_TOK_RBRACE)) {
        DsCaseArm arm;
        memset(&arm, 0, sizeof(arm));
        DsToken *arm_start = parser_peek(p);
        do {
            DsCasePattern pattern;
            if (!parse_case_pattern(p, &pattern)) break;
            parser_case_pattern_vec_push(&arm.patterns, pattern);
        } while (parser_advance_if(p, DS_TOK_PIPE));
        if (!parser_expect(p, DS_TOK_LBRACE, "expected `{` after case pattern")) {
            while (!parser_at_end(p) && !parser_at(p, DS_TOK_RBRACE) && !parser_is_stmt_end(p)) parser_advance(p);
            parser_skip_newlines(p);
            continue;
        }
        arm.body = parse_block(p);
        arm.span = (DsSpan){arm_start->span.start, arm.body ? arm.body->span.end : arm_start->span.end, arm_start->span.source};
        parser_case_arm_vec_push(&stmt->as.case_stmt.arms, arm);
        parser_skip_newlines(p);
    }
    if (!parser_expect(p, DS_TOK_RBRACE, "expected `}` to close case statement")) return stmt;
    stmt->span.end = parser_previous(p)->span.end;
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

static bool parse_handler_signal(Parser *p, const char *form, DsHandlerSignal *signal, DsStr *signal_text) {
    if (!parser_expect(p, DS_TOK_STRING, form[0] == 't' ? "expected signal string after `trap`" : "expected signal string after `defer on:`")) return false;
    DsToken *tok = parser_previous(p);
    DsStr decoded = {0};
    bool ok = parser_decode_string_literal(tok->text, &decoded);
    if (!ok) {
        ds_diag_error(p->diag, tok->span, "%s signal must be a string literal", form);
        return false;
    }
    if (decoded.len == 4 && memcmp(decoded.data, "EXIT", 4) == 0) *signal = DS_HANDLER_EXIT;
    else if (decoded.len == 3 && memcmp(decoded.data, "INT", 3) == 0) *signal = DS_HANDLER_INT;
    else if (decoded.len == 4 && memcmp(decoded.data, "TERM", 4) == 0) *signal = DS_HANDLER_TERM;
    else *signal = DS_HANDLER_INVALID;
    *signal_text = decoded;
    return true;
}

static DsStmt *parse_defer(Parser *p) {
    DsToken *start = parser_previous(p);
    DsHandlerSignal signal = DS_HANDLER_EXIT;
    DsStr signal_text = {ds_str_dup_range("EXIT", 4), 4};
    if (parser_at(p, DS_TOK_IDENT) && parser_peek(p)->text.len == 2 && memcmp(parser_peek(p)->text.data, "on", 2) == 0) {
        parser_advance(p);
        if (!parser_expect(p, DS_TOK_COLON, "expected `:` after `defer on`")) {
            free(signal_text.data);
            return NULL;
        }
        free(signal_text.data);
        signal_text = (DsStr){0};
        if (!parse_handler_signal(p, "defer on:", &signal, &signal_text)) return NULL;
    }
    if (!parser_expect(p, DS_TOK_LBRACE, "expected `{` after defer handler")) {
        free(signal_text.data);
        return NULL;
    }
    DsStmt *body = parse_block(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_DEFER, (DsSpan){start->span.start, body ? body->span.end : start->span.end, start->span.source});
    stmt->as.handler_stmt.signal = signal;
    stmt->as.handler_stmt.signal_text = signal_text;
    stmt->as.handler_stmt.body = body;
    parser_consume_statement_end(p);
    return stmt;
}

static DsStmt *parse_trap(Parser *p) {
    DsToken *start = parser_previous(p);
    DsHandlerSignal signal = DS_HANDLER_EXIT;
    DsStr signal_text = {0};
    if (!parse_handler_signal(p, "trap", &signal, &signal_text)) return NULL;
    if (!parser_expect(p, DS_TOK_LBRACE, "expected `{` after trap signal")) {
        free(signal_text.data);
        return NULL;
    }
    DsStmt *body = parse_block(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_TRAP, (DsSpan){start->span.start, body ? body->span.end : start->span.end, start->span.source});
    stmt->as.handler_stmt.signal = signal;
    stmt->as.handler_stmt.signal_text = signal_text;
    stmt->as.handler_stmt.body = body;
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
    if (parser_at(p, DS_TOK_IDENT) && parser_peek(p)->text.len == 5 && memcmp(parser_peek(p)->text.data, "unset", 5) == 0 &&
        parser_next_at(p, DS_TOK_IDENT) && p->tokens->items[p->pos + 1].text.len == 3 && memcmp(p->tokens->items[p->pos + 1].text.data, "env", 3) == 0 &&
        parser_peek2_at(p, DS_TOK_DOT)) return parse_env_unset(p);
    if (parser_at(p, DS_TOK_IDENT) && parser_peek(p)->text.len == 5 && memcmp(parser_peek(p)->text.data, "unset", 5) == 0) return parse_bad_unset(p);
    if (parser_at(p, DS_TOK_IDENT) && parser_peek(p)->text.len == 3 && memcmp(parser_peek(p)->text.data, "env", 3) == 0 &&
        parser_next_at(p, DS_TOK_DOT) && stmt_contains_assignment_operator(p)) return parse_env_assign(p);
    if (parser_at(p, DS_TOK_IDENT) && (parser_next_at(p, DS_TOK_LBRACKET) || parser_next_at(p, DS_TOK_DOT)) && stmt_contains_assignment_operator(p)) return parse_unsupported_collection_assign(p);
    if (parser_at(p, DS_TOK_IDENT) && (parser_next_at(p, DS_TOK_EQUAL) ||
        ((parser_next_at(p, DS_TOK_PLUS) || parser_next_at(p, DS_TOK_MINUS) || parser_next_at(p, DS_TOK_STAR) || parser_next_at(p, DS_TOK_SLASH) || parser_next_at(p, DS_TOK_PERCENT)) && parser_peek2_at(p, DS_TOK_EQUAL)))) return parse_assign(p);
    if (parser_advance_if(p, DS_TOK_RETURN)) return parse_return(p);
    if (parser_advance_if(p, DS_TOK_DEFER)) return parse_defer(p);
    if (parser_advance_if(p, DS_TOK_TRAP)) return parse_trap(p);
    if (parser_advance_if(p, DS_TOK_IF)) return parse_if(p);
    if (parser_advance_if(p, DS_TOK_FOR)) return parse_for(p);
    if (parser_advance_if(p, DS_TOK_WHILE)) return parse_while(p);
    if (parser_advance_if(p, DS_TOK_BREAK)) return parse_loop_control(p, DS_STMT_BREAK);
    if (parser_advance_if(p, DS_TOK_CONTINUE)) return parse_loop_control(p, DS_STMT_CONTINUE);
    if (parser_advance_if(p, DS_TOK_CASE)) return parse_case(p);
    if (parser_advance_if(p, DS_TOK_LBRACE)) return parse_block(p);
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
    return parse_cmd(p);
}
