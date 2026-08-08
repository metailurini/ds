#include "parser_internal.h"
#include "ds_signal.h"

DsStmt *parse_stmt(Parser *p);
static bool parse_assignment_operator(Parser *p, DsAssignOp *op);

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
    parser_expect_stmt_end(p, "import statement");
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
    if (parser_token_text_eq(name, "env")) {
        ds_diag_error(p->diag, name->span, "`env` is a reserved environment namespace in v0.27.0");
    }
    if (!parser_expect(p, DS_TOK_EQUAL, "expected `=` after variable name")) return NULL;
    if (!parser_expect_expr(p, parser_peek(p)->span, "expected expression after `=`")) return NULL;
    DsExpr *value = parse_expr(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_LET, (DsSpan){start->span.start, value ? value->span.end : start->span.end, start->span.source});
    stmt->as.let_stmt.name = parser_copy_token_text(name);
    stmt->as.let_stmt.value = value;
    parser_expect_stmt_end(p, "statement");
    return stmt;
}

static DsStmt *parse_assign(Parser *p) {
    DsToken *name = parser_advance(p);
    DsAssignOp op = DS_ASSIGN_SET;
    DsSpan op_span = parser_peek(p)->span;
    if (!parse_assignment_operator(p, &op)) {
        ds_diag_error(p->diag, op_span, "expected assignment operator");
        return NULL;
    }
    if (!parser_expect_expr(p, parser_peek(p)->span, "expected expression after assignment operator")) return NULL;
    DsExpr *value = parse_expr(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_ASSIGN, (DsSpan){name->span.start, value ? value->span.end : name->span.end, name->span.source});
    stmt->as.assign_stmt.name = parser_copy_token_text(name);
    stmt->as.assign_stmt.op = op;
    stmt->as.assign_stmt.value = value;
    parser_expect_stmt_end(p, "assignment statement");
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

static DsStmt *parse_index_assign_stmt(Parser *p) {
    DsToken *start = parser_peek(p);
    for (size_t i = p->pos; i < p->tokens->len; i++) {
        DsTokenKind kind = p->tokens->items[i].kind;
        if (kind == DS_TOK_NEWLINE || kind == DS_TOK_EOF || kind == DS_TOK_RBRACE) break;
        if (token_is_assignment_operator_at(p->tokens, i)) {
            if (kind != DS_TOK_EQUAL) {
                ds_diag_error(p->diag, p->tokens->items[i].span,
                              "compound index assignment is unsupported in v0.30.0; use `target[index] = value`");
                parser_skip_to_stmt_end(p);
                parser_consume_statement_end(p);
                return NULL;
            }
            break;
        }
    }
    DsExpr *target = parse_expr(p);
    DsAssignOp op = DS_ASSIGN_SET;
    if (!parse_assignment_operator(p, &op)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected assignment operator");
        parser_skip_to_stmt_end(p);
        parser_consume_statement_end(p);
        return NULL;
    }

    if (!parser_expect_expr(p, parser_peek(p)->span, "expected expression after assignment operator")) {
        parser_consume_statement_end(p);
        return NULL;
    }
    DsExpr *value = parse_expr(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_INDEX_ASSIGN,
                                   (DsSpan){start->span.start, value ? value->span.end : (target ? target->span.end : start->span.end), start->span.source});
    stmt->as.index_assign_stmt.target = target;
    stmt->as.index_assign_stmt.op = op;
    stmt->as.index_assign_stmt.value = value;
    parser_expect_stmt_end(p, "index assignment statement");
    return stmt;
}

static bool parser_invalid_hyphenated_env_name(Parser *p, const DsToken *field, const char *version) {
    if (!parser_at(p, DS_TOK_MINUS)) return false;
    if (p->pos + 1 >= p->tokens->len || !parser_is_identifier_like(p->tokens->items[p->pos + 1].kind)) return false;
    DsToken *dash = parser_peek(p);
    DsToken *suffix = &p->tokens->items[p->pos + 1];
    DsStr name = ds_str_join_char(field->text, '-', suffix->text);
    ds_diag_error(p->diag, dash->span, "invalid environment variable name `%.*s` in %s", (int)name.len, name.data, version);
    free(name.data);
    parser_skip_to_stmt_end(p);
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
    if (!parser_expect_expr(p, parser_peek(p)->span, "expected expression after environment assignment operator")) return NULL;
    DsExpr *value = parse_expr(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_ASSIGN, (DsSpan){env_tok->span.start, value ? value->span.end : field->span.end, env_tok->span.source});
    stmt->as.assign_stmt.name = ds_str_join_char((DsStr){"env", 3}, '.', field->text);
    stmt->as.assign_stmt.op = DS_ASSIGN_SET;
    stmt->as.assign_stmt.value = value;
    parser_expect_stmt_end(p, "environment assignment statement");
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
    stmt->as.call_stmt.name = (DsStr){ds_str_dup_cstr("env.unset"), 9};
    DsExpr *arg = parser_new_expr(DS_EXPR_STRING, field->span);
    arg->as.text = quoted_env_name_from_token(field);
    parser_expr_vec_push(&stmt->as.call_stmt.args, arg);

    parser_expect_stmt_end(p, "environment unset statement");
    return stmt;
}

static DsStmt *parse_bad_unset(Parser *p) {
    DsToken *unset_tok = parser_advance(p);
    ds_diag_error(p->diag, unset_tok->span, "unset requires an environment target like `unset env.NAME` in v0.27.0");
    parser_skip_to_stmt_end(p);
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

static bool stmt_has_bracket_before_assignment(const Parser *p) {
    for (size_t i = p->pos; i < p->tokens->len; i++) {
        DsTokenKind kind = p->tokens->items[i].kind;
        if (kind == DS_TOK_NEWLINE || kind == DS_TOK_EOF || kind == DS_TOK_RBRACE) return false;
        if (token_is_assignment_operator_at(p->tokens, i)) return false;
        if (kind == DS_TOK_LBRACKET) return true;
    }
    return false;
}

static DsStmt *parse_return(Parser *p) {
    DsToken *start = parser_previous(p);
    if (!parser_expect_expr(p, start->span, "expected expression after `return`")) {
        parser_consume_statement_end(p);
        return NULL;
    }
    DsExpr *value = parse_expr(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_RETURN, (DsSpan){start->span.start, value ? value->span.end : start->span.end, start->span.source});
    stmt->as.return_stmt.value = value;
    parser_expect_stmt_end(p, "return statement");
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
    parser_expect_stmt_end(p, "function call statement");
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
    parser_expect_stmt_end(p, "helper call statement");
    return stmt;
}

static DsStmt *parse_push_stmt(Parser *p) {
    DsToken *name = parser_advance(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_PUSH, name->span);
    stmt->as.push_stmt.name = parser_copy_token_text(name);
    parser_expect(p, DS_TOK_DOT, "expected `.` before `push`");
    DsToken *push_name = parser_advance(p);
    if (!parser_token_text_eq(push_name, "push")) {
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
        parser_skip_to_stmt_end_or(p, DS_TOK_RPAREN);
    }
    if (!parser_expect(p, DS_TOK_RPAREN, "expected `)` after `push` argument")) return stmt;
    stmt->span.end = parser_previous(p)->span.end;
    parser_expect_stmt_end(p, "push statement");
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
    if (parser_at(p, DS_TOK_LBRACE)) {
        if (stmt->as.for_stmt.has_value_name) {
            ds_diag_error(p->diag, parser_peek(p)->span,
                          "temporary map literals are not supported as map loop iterables in v0.29.0; bind the map to a variable first");
        } else {
            ds_diag_error(p->diag, parser_peek(p)->span, "expected iterable expression after `in`");
        }
    }
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
        parser_skip_to_stmt_end(p);
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
            parser_skip_to_stmt_end(p);
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
            parser_skip_to_stmt_end_or(p, DS_TOK_RBRACE);
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
    if (!parser_expect_expr(p, start->span, "expected expression after `assert`")) {
        parser_consume_statement_end(p);
        return NULL;
    }
    DsExpr *condition = parse_expr(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_ASSERT, (DsSpan){start->span.start, condition ? condition->span.end : start->span.end, start->span.source});
    stmt->as.assert_stmt.condition = condition;
    parser_expect_stmt_end(p, "assert statement");
    return stmt;
}

static bool parse_handler_signal(Parser *p, const char *form, DsHandlerSignal *signal, DsStr *signal_text) {
    if (!parser_expect(p, DS_TOK_STRING, form[0] == 't' ? "expected signal string after `trap`" : "expected signal string after `defer on:`")) return false;
    DsToken *tok = parser_previous(p);
    DsStr decoded = {0};
    bool ok = ds_decode_string_literal(tok->text, &decoded);
    if (!ok) {
        ds_diag_error(p->diag, tok->span, "%s signal must be a string literal", form);
        return false;
    }
    *signal = ds_handler_signal_parse(decoded);
    *signal_text = decoded;
    return true;
}

static DsStmt *parse_handler(Parser *p, DsStmtKind kind) {
    DsToken *start = parser_previous(p);
    DsHandlerSignal signal = DS_HANDLER_EXIT;
    bool is_defer = kind == DS_STMT_DEFER;
    DsStr signal_text = is_defer ? (DsStr){ds_str_dup_cstr("EXIT"), 4} : (DsStr){0};
    if (is_defer && parser_at(p, DS_TOK_IDENT) && ds_str_eq_cstr(parser_peek(p)->text, "on")) {
        parser_advance(p);
        if (!parser_expect(p, DS_TOK_COLON, "expected `:` after `defer on`")) {
            free(signal_text.data);
            return NULL;
        }
        free(signal_text.data);
        signal_text = (DsStr){0};
        if (!parse_handler_signal(p, "defer on:", &signal, &signal_text)) return NULL;
    } else if (!is_defer && !parse_handler_signal(p, "trap", &signal, &signal_text)) {
        return NULL;
    }
    const char *brace_error = is_defer ? "expected `{` after defer handler" : "expected `{` after trap signal";
    if (!parser_expect(p, DS_TOK_LBRACE, brace_error)) {
        free(signal_text.data);
        return NULL;
    }
    DsStmt *body = parse_block(p);
    DsStmt *stmt = parser_new_stmt(kind, (DsSpan){start->span.start, body ? body->span.end : start->span.end, start->span.source});
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
    if (parser_at_ident_text(p, "unset") && parser_next_ident_text(p, "env") &&
        parser_peek2_at(p, DS_TOK_DOT)) return parse_env_unset(p);
    if (parser_at_ident_text(p, "unset")) return parse_bad_unset(p);
    if (parser_at_env_dot(p) && stmt_contains_assignment_operator(p) && stmt_has_bracket_before_assignment(p)) return parse_index_assign_stmt(p);
    if (parser_at_env_dot(p) && stmt_contains_assignment_operator(p)) return parse_env_assign(p);
    if (((parser_at(p, DS_TOK_IDENT) && (parser_next_at(p, DS_TOK_LBRACKET) || parser_next_at(p, DS_TOK_DOT) || parser_next_at(p, DS_TOK_LPAREN))) ||
         parser_at(p, DS_TOK_LBRACKET)) && stmt_contains_assignment_operator(p)) return parse_index_assign_stmt(p);
    if (parser_at(p, DS_TOK_IDENT) && (parser_next_at(p, DS_TOK_EQUAL) ||
        ((parser_next_at(p, DS_TOK_PLUS) || parser_next_at(p, DS_TOK_MINUS) || parser_next_at(p, DS_TOK_STAR) || parser_next_at(p, DS_TOK_SLASH) || parser_next_at(p, DS_TOK_PERCENT)) && parser_peek2_at(p, DS_TOK_EQUAL)))) return parse_assign(p);
    if (parser_advance_if(p, DS_TOK_RETURN)) return parse_return(p);
    if (parser_advance_if(p, DS_TOK_DEFER)) return parse_handler(p, DS_STMT_DEFER);
    if (parser_advance_if(p, DS_TOK_TRAP)) return parse_handler(p, DS_STMT_TRAP);
    if (parser_advance_if(p, DS_TOK_IF)) return parse_if(p);
    if (parser_advance_if(p, DS_TOK_FOR)) return parse_for(p);
    if (parser_advance_if(p, DS_TOK_WHILE)) return parse_while(p);
    if (parser_advance_if(p, DS_TOK_BREAK)) return parse_loop_control(p, DS_STMT_BREAK);
    if (parser_advance_if(p, DS_TOK_CONTINUE)) return parse_loop_control(p, DS_STMT_CONTINUE);
    if (parser_advance_if(p, DS_TOK_CASE)) return parse_case(p);
    if (parser_advance_if(p, DS_TOK_LBRACE)) return parse_block(p);
    if (parser_at(p, DS_TOK_IDENT) && parser_next_at(p, DS_TOK_DOT)) {
        if (p->pos + 3 < p->tokens->len && p->tokens->items[p->pos + 2].kind == DS_TOK_IDENT &&
            parser_token_text_eq(&p->tokens->items[p->pos + 2], "push") &&
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
