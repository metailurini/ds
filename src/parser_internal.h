#ifndef DS_PARSER_INTERNAL_H
#define DS_PARSER_INTERNAL_H

#include "frontend.h"

typedef struct {
    const DsTokenVec *tokens;
    size_t pos;
    DsDiag *diag;
    int test_depth;
    int function_depth;
} Parser;

DsToken *parser_peek(Parser *p);
DsToken *parser_previous(Parser *p);
bool parser_at(Parser *p, DsTokenKind kind);
bool parser_next_at(Parser *p, DsTokenKind kind);
bool parser_peek2_at(Parser *p, DsTokenKind kind);
bool parser_at_end(Parser *p);
bool parser_token_text_eq(const DsToken *token, const char *text);
bool parser_at_ident_text(Parser *p, const char *text);
bool parser_next_ident_text(Parser *p, const char *text);
bool parser_at_env_dot(Parser *p);
bool parser_advance_if(Parser *p, DsTokenKind kind);
DsToken *parser_advance(Parser *p);
DsStr parser_copy_token_text(const DsToken *token);
DsStr parser_copy_dotted_name(const DsToken *left, const DsToken *right);
DsStr parser_copy_bang_name(const DsToken *name);
void parser_skip_newlines(Parser *p);
bool parser_is_stmt_end(Parser *p);
void parser_skip_to_stmt_end(Parser *p);
void parser_skip_to_stmt_end_or(Parser *p, DsTokenKind stop);
void parser_consume_statement_end(Parser *p);
void parser_expect_stmt_end(Parser *p, const char *description);
bool parser_expect(Parser *p, DsTokenKind kind, const char *message);
bool parser_reject_trailing_comma(Parser *p, DsTokenKind closing_kind, const char *message);
bool parser_expect_expr(Parser *p, DsSpan span, const char *message);
bool parser_is_identifier_like(DsTokenKind kind);
bool parser_expect_identifier_like(Parser *p, const char *message);
bool parser_is_redirect_token(DsTokenKind kind);
DsRedirectKind parser_redirect_kind_from_token(DsTokenKind kind);

DsExpr *parse_expr(Parser *p);
void parse_call_args(Parser *p, DsExprVec *args);
void parse_command_pipeline(Parser *p, DsCommand *command, bool reject_redirection);
DsExpr *parse_run_expr(Parser *p);
DsStmt *parse_stmt(Parser *p);
DsStmt *parse_block(Parser *p);
DsStmt *parse_import_stmt(Parser *p, bool top_level, bool after_executable);
bool parse_script_type(Parser *p, DsScriptType *out);
bool parse_script_block(Parser *p, DsAst *ast);
DsStmt *parse_fn(Parser *p, bool top_level);
DsStmt *parse_test(Parser *p, bool top_level);
DsStmt *parse_cmd(Parser *p);

#endif
