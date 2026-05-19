#include "parser_internal.h"

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

void parse_command_words_until_end(Parser *p, DsWordVec *words, DsSpan *span, bool reject_redirection) {
    DsWord current = {0};
    size_t current_cap = 0;
    size_t prev_end = 0;
    bool have_current = false;
    while (!parser_is_stmt_end(p)) {
        if (is_unsupported_command_operator(parser_peek(p))) {
            report_unsupported_command_operator(p, parser_peek(p));
            while (!parser_is_stmt_end(p)) parser_advance(p);
            break;
        }
        if (parser_is_redirect_token(parser_peek(p)->kind)) {
            if (reject_redirection) {
                ds_diag_error(p->diag, parser_peek(p)->span, "captured `run` commands do not support redirection in v0.7.0");
                while (!parser_is_stmt_end(p)) parser_advance(p);
            }
            break;
        }
        DsToken *tok = parser_advance(p);
        bool adjacent = have_current && tok->span.start.offset == prev_end;
        if (!adjacent && have_current) {
            parser_word_vec_push(words, current);
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
    if (have_current) parser_word_vec_push(words, current);
}

DsExpr *parse_run_expr(Parser *p) {
    DsToken *run = parser_previous(p);
    DsExpr *expr = parser_new_expr(DS_EXPR_RUN, run->span);
    parse_command_words_until_end(p, &expr->as.run.words, &expr->span, true);
    if (expr->as.run.words.len == 0) {
        ds_diag_error(p->diag, run->span, "expected command after `run`");
    }
    return expr;
}

DsStmt *parse_cmd(Parser *p) {
    DsToken *start = parser_peek(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_CMD, start->span);
    DsWord current = {0};
    size_t current_cap = 0;
    size_t prev_end = 0;
    bool have_current = false;

    while (!parser_is_stmt_end(p)) {
        if (is_unsupported_command_operator(parser_peek(p))) {
            if (have_current) {
                parser_word_vec_push(&stmt->as.cmd_stmt.words, current);
                current.text.data = NULL;
                current.text.len = 0;
                current_cap = 0;
                have_current = false;
            }
            report_unsupported_command_operator(p, parser_peek(p));
            while (!parser_is_stmt_end(p)) parser_advance(p);
            break;
        }
        if (parser_is_redirect_token(parser_peek(p)->kind)) {
            if (have_current) {
                parser_word_vec_push(&stmt->as.cmd_stmt.words, current);
                current.text.data = NULL;
                current.text.len = 0;
                current_cap = 0;
                have_current = false;
            }
            DsToken *op = parser_advance(p);
            if (stmt->as.cmd_stmt.redirect.kind != DS_REDIRECT_NONE) {
                ds_diag_error(p->diag, op->span, "duplicate redirection suffix");
            }
            stmt->as.cmd_stmt.redirect.kind = parser_redirect_kind_from_token(op->kind);
            stmt->as.cmd_stmt.redirect.op_span = op->span;
            if (parser_is_stmt_end(p)) {
                ds_diag_error(p->diag, op->span, "expected string redirection target after `%.*s`", (int)op->text.len, op->text.data);
                break;
            }
            if (!parser_advance_if(p, DS_TOK_STRING)) {
                ds_diag_error(p->diag, parser_peek(p)->span, "redirection target must be a string literal in v0.7.0");
                while (!parser_is_stmt_end(p)) parser_advance(p);
                break;
            }
            DsToken *target = parser_previous(p);
            stmt->as.cmd_stmt.redirect.target = parser_copy_token_text(target);
            stmt->as.cmd_stmt.redirect.target_span = target->span;
            stmt->span.end = target->span.end;
            if (!parser_is_stmt_end(p)) {
                if (parser_is_redirect_token(parser_peek(p)->kind)) ds_diag_error(p->diag, parser_peek(p)->span, "duplicate redirection suffix");
                else ds_diag_error(p->diag, parser_peek(p)->span, "expected end of redirected command");
                while (!parser_is_stmt_end(p)) parser_advance(p);
            }
            break;
        }
        DsToken *tok = parser_advance(p);
        bool adjacent = have_current && tok->span.start.offset == prev_end;
        if (!adjacent && have_current) {
            parser_word_vec_push(&stmt->as.cmd_stmt.words, current);
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
        parser_word_vec_push(&stmt->as.cmd_stmt.words, current);
    }
    parser_consume_statement_end(p);
    return stmt;
}
