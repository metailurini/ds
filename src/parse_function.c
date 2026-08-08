#include "parser_internal.h"

DsStmt *parse_fn(Parser *p, bool top_level) {
    DsToken *start = parser_previous(p);
    if (!top_level) ds_diag_error(p->diag, start->span, "function declarations are only allowed at top level in v0.9.0");
    if (!parser_expect_identifier_like(p, "expected function name after `fn`")) return NULL;
    DsToken *name = parser_previous(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_FN, start->span);
    stmt->as.fn_stmt.name = parser_copy_token_text(name);
    if (!parser_expect(p, DS_TOK_LPAREN, "expected `(` after function name")) return stmt;
    bool seen_default = false;
    if (!parser_at(p, DS_TOK_RPAREN)) {
        while (!parser_at_end(p) && !parser_at(p, DS_TOK_RPAREN)) {
            DsFnParam param;
            memset(&param, 0, sizeof(param));
            if (!parser_expect_identifier_like(p, "expected parameter name")) break;
            DsToken *param_name = parser_previous(p);
            param.name = parser_copy_token_text(param_name);
            param.span = param_name->span;
            if (parser_advance_if(p, DS_TOK_COLON)) {
                param.has_type = true;
                parse_script_type(p, &param.type);
            }
            if (parser_advance_if(p, DS_TOK_EQUAL)) {
                seen_default = true;
                if (parser_is_stmt_end(p) || parser_at(p, DS_TOK_COMMA) || parser_at(p, DS_TOK_RPAREN)) {
                    ds_diag_error(p->diag, parser_previous(p)->span, "expected literal default value after `=`");
                } else {
                    param.default_value = parse_expr(p);
                    if (param.default_value) param.span.end = param.default_value->span.end;
                }
            } else if (seen_default) {
                ds_diag_error(p->diag, param.span,
                              "required parameter `%.*s` cannot follow a default parameter",
                              (int)param.name.len, param.name.data);
            }
            parser_fn_param_vec_push(&stmt->as.fn_stmt.params, param);
            if (!parser_advance_if(p, DS_TOK_COMMA)) break;
            if (parser_reject_trailing_comma(p, DS_TOK_RPAREN, "expected parameter name after `,`")) break;
        }
    }
    if (!parser_expect(p, DS_TOK_RPAREN, "expected `)` after function parameters")) return stmt;
    if (!parser_expect(p, DS_TOK_LBRACE, "expected `{` after function declaration")) return stmt;
    p->function_depth++;
    stmt->as.fn_stmt.body = parse_block(p);
    p->function_depth--;
    stmt->span = (DsSpan){start->span.start, stmt->as.fn_stmt.body ? stmt->as.fn_stmt.body->span.end : parser_previous(p)->span.end, start->span.source};
    parser_consume_statement_end(p);
    return stmt;
}

DsStmt *parse_test(Parser *p, bool top_level) {
    DsToken *start = parser_previous(p);
    if (!top_level) ds_diag_error(p->diag, start->span, "`test` declarations are only allowed at top level in v0.14.0");
    if (!parser_expect(p, DS_TOK_STRING, "expected string literal test name after `test`")) return NULL;
    DsToken *name = parser_previous(p);
    DsStr decoded;
    if (!parser_decode_string_literal(name->text, &decoded)) {
        ds_diag_error(p->diag, name->span, "invalid test name string literal");
        return NULL;
    }
    if (decoded.len == 0) ds_diag_error(p->diag, name->span, "test name cannot be empty");
    if (!parser_expect(p, DS_TOK_LBRACE, "expected `{` after test name")) {
        free(decoded.data);
        return NULL;
    }
    p->test_depth++;
    DsStmt *body = parse_block(p);
    p->test_depth--;
    DsStmt *stmt = parser_new_stmt(DS_STMT_TEST, (DsSpan){start->span.start, body ? body->span.end : parser_previous(p)->span.end, start->span.source});
    stmt->as.test_stmt.name = decoded;
    stmt->as.test_stmt.body = body;
    parser_consume_statement_end(p);
    return stmt;
}

