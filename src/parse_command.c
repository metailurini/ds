#include "parser_internal.h"

static bool is_word_separator(Parser *p) {
    return parser_is_stmt_end(p) || parser_at(p, DS_TOK_PIPE) || parser_is_redirect_token(parser_peek(p)->kind);
}

static bool is_unsupported_command_operator(const DsToken *tok) {
    if (tok->kind == DS_TOK_UNKNOWN && tok->text.len == 1 && tok->text.data[0] == '&') return true;
    if (tok->kind == DS_TOK_AND_AND || tok->kind == DS_TOK_OR_OR) return true;
    return tok->kind == DS_TOK_GREATER || tok->kind == DS_TOK_GREATER_EQUAL || tok->kind == DS_TOK_LESS || tok->kind == DS_TOK_LESS_EQUAL;
}

static void report_unsupported_command_operator(Parser *p, const DsToken *tok) {
    if (tok->kind == DS_TOK_OR_OR) {
        ds_diag_error(p->diag, tok->span, "logical OR `||` is not supported in command syntax");
        return;
    }
    if ((tok->kind == DS_TOK_UNKNOWN && tok->text.len == 1 && tok->text.data[0] == '&') || tok->kind == DS_TOK_AND_AND || tok->kind == DS_TOK_OR_OR) {
        ds_diag_error(p->diag, tok->span, "logical/background command operators are not supported in command syntax");
    } else {
        ds_diag_error(p->diag, tok->span, "unsupported command operator `%.*s`; use `|` for pipelines or `|>`, `|>>`, `!>`, `!>>`, `&>`, or `&>>` for whole-command redirection", (int)tok->text.len, tok->text.data);
    }
}

static void flush_word(DsWordVec *words, DsWord *current, size_t *cap, bool *have_current) {
    if (!*have_current) return;
    parser_word_vec_push(words, *current);
    current->text.data = NULL;
    current->text.len = 0;
    *cap = 0;
    *have_current = false;
}

static void parse_stage_words(Parser *p, DsWordVec *words, DsSpan *span) {
    DsWord current = {0};
    size_t current_cap = 0;
    size_t prev_end = 0;
    bool have_current = false;
    while (!is_word_separator(p)) {
        if (is_unsupported_command_operator(parser_peek(p))) {
            flush_word(words, &current, &current_cap, &have_current);
            report_unsupported_command_operator(p, parser_peek(p));
            parser_skip_to_stmt_end(p);
            break;
        }
        DsToken *tok = parser_advance(p);
        bool adjacent = have_current && tok->span.start.offset == prev_end;
        if (!adjacent) flush_word(words, &current, &current_cap, &have_current);
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
    flush_word(words, &current, &current_cap, &have_current);
}

void parse_command_words_until_end(Parser *p, DsWordVec *words, DsSpan *span, bool reject_redirection) {
    (void)reject_redirection;
    parse_stage_words(p, words, span);
}

void parse_command_pipeline(Parser *p, DsCommand *command, bool reject_redirection) {
    bool expect_stage = true;
    while (!parser_is_stmt_end(p)) {
        if (parser_at(p, DS_TOK_PIPE)) {
            ds_diag_error(p->diag, parser_peek(p)->span, expect_stage ? "missing command before `|`" : "missing command between pipeline separators");
            parser_advance(p);
            expect_stage = true;
            continue;
        }
        DsCommandStage stage;
        ds_command_stage_init(&stage);
        stage.span = parser_peek(p)->span;
        parse_stage_words(p, &stage.words, &stage.span);
        if (stage.words.len == 0) {
            ds_command_stage_free(&stage);
            break;
        }
        if (command->stages.len == 0) command->span.start = stage.span.start;
        command->span.end = stage.span.end;
        DS_VEC_PUSH(&command->stages, stage, 4);
        expect_stage = false;

        if (parser_at(p, DS_TOK_PIPE)) {
            DsToken *pipe = parser_advance(p);
            command->span.end = pipe->span.end;
            if (parser_at(p, DS_TOK_PIPE)) {
                DsToken *next_pipe = parser_peek(p);
                if (pipe->span.end.offset == next_pipe->span.start.offset) {
                    ds_diag_error(p->diag, next_pipe->span, "logical OR `||` is not supported in v0.18.0");
                } else {
                    ds_diag_error(p->diag, next_pipe->span, "missing command between pipeline separators");
                }
                parser_skip_to_stmt_end(p);
                break;
            }
            if (parser_is_stmt_end(p)) {
                ds_diag_error(p->diag, pipe->span, "missing command after `|`");
                break;
            }
            if (parser_is_redirect_token(parser_peek(p)->kind)) {
                ds_diag_error(p->diag, parser_peek(p)->span, "redirection cannot appear as a pipeline stage");
                parser_skip_to_stmt_end(p);
                break;
            }
            expect_stage = true;
            continue;
        }
        if (parser_is_redirect_token(parser_peek(p)->kind)) {
            DsToken *op = parser_advance(p);
            if (reject_redirection) {
                ds_diag_error(p->diag, op->span, "captured `run` commands do not support redirection");
                parser_skip_to_stmt_end(p);
                break;
            }
            if (command->redirect.kind != DS_REDIRECT_NONE) ds_diag_error(p->diag, op->span, "duplicate redirection suffix");
            command->redirect.kind = parser_redirect_kind_from_token(op->kind);
            command->redirect.op_span = op->span;
            if (parser_is_stmt_end(p)) {
                ds_diag_error(p->diag, op->span, "expected string redirection target after `%.*s`", (int)op->text.len, op->text.data);
                break;
            }
            if (!parser_advance_if(p, DS_TOK_STRING)) {
                ds_diag_error(p->diag, parser_peek(p)->span, "redirection target must be a string literal in v0.18.0");
                parser_skip_to_stmt_end(p);
                break;
            }
            DsToken *target = parser_previous(p);
            command->redirect.target = parser_copy_token_text(target);
            command->redirect.target_span = target->span;
            command->span.end = target->span.end;
            if (!parser_is_stmt_end(p)) {
                if (parser_at(p, DS_TOK_PIPE)) ds_diag_error(p->diag, parser_peek(p)->span, "redirection must apply to the whole pipeline and cannot appear before another stage");
                else if (parser_is_redirect_token(parser_peek(p)->kind)) ds_diag_error(p->diag, parser_peek(p)->span, "duplicate redirection suffix");
                else ds_diag_error(p->diag, parser_peek(p)->span, "expected end of redirected command");
                parser_skip_to_stmt_end(p);
            }
            break;
        }
    }
}

DsExpr *parse_run_expr(Parser *p) {
    DsToken *run = parser_previous(p);
    DsExpr *expr = parser_new_expr(DS_EXPR_RUN, run->span);
    ds_command_init(&expr->as.run, DS_COMMAND_CAPTURE, run->span);
    parse_command_pipeline(p, &expr->as.run, true);
    if (expr->as.run.stages.len == 0) {
        ds_diag_error(p->diag, run->span, "expected command after `run`");
    }
    expr->span = (DsSpan){run->span.start, expr->as.run.span.end, run->span.source};
    expr->as.run.span = expr->span;
    return expr;
}

DsStmt *parse_cmd(Parser *p) {
    DsToken *start = parser_peek(p);
    DsStmt *stmt = parser_new_stmt(DS_STMT_CMD, start->span);
    ds_command_init(&stmt->as.cmd_stmt, DS_COMMAND_PLAIN, start->span);
    parse_command_pipeline(p, &stmt->as.cmd_stmt, false);
    stmt->span = stmt->as.cmd_stmt.span;
    parser_consume_statement_end(p);
    return stmt;
}
