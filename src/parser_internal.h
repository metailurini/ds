#ifndef DS_PARSER_INTERNAL_H
#define DS_PARSER_INTERNAL_H

#include "frontend.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const DsTokenVec *tokens;
    size_t pos;
    DsDiag *diag;
    int test_depth;
    int function_depth;
} Parser;

static inline DsToken *parser_peek(Parser *p) { return &p->tokens->items[p->pos]; }
static inline DsToken *parser_previous(Parser *p) { return &p->tokens->items[p->pos - 1]; }
static inline bool parser_at(Parser *p, DsTokenKind kind) { return parser_peek(p)->kind == kind; }
static inline bool parser_next_at(Parser *p, DsTokenKind kind) { return p->pos + 1 < p->tokens->len && p->tokens->items[p->pos + 1].kind == kind; }
static inline bool parser_peek2_at(Parser *p, DsTokenKind kind) { return p->pos + 2 < p->tokens->len && p->tokens->items[p->pos + 2].kind == kind; }
static inline bool parser_at_end(Parser *p) { return parser_at(p, DS_TOK_EOF); }

static inline bool parser_advance_if(Parser *p, DsTokenKind kind) {
    if (!parser_at(p, kind)) return false;
    p->pos++;
    return true;
}

static inline DsToken *parser_advance(Parser *p) {
    if (!parser_at_end(p)) p->pos++;
    return parser_previous(p);
}

static inline DsStr parser_copy_token_text(const DsToken *token) {
    DsStr s;
    s.data = ds_str_dup_range(token->text.data, token->text.len);
    s.len = token->text.len;
    return s;
}

static inline DsStr parser_copy_dotted_name(const DsToken *left, const DsToken *right) {
    DsStr s;
    s.len = left->text.len + 1 + right->text.len;
    s.data = (char *)ds_xcalloc(s.len + 1, 1);
    memcpy(s.data, left->text.data, left->text.len);
    s.data[left->text.len] = '.';
    memcpy(s.data + left->text.len + 1, right->text.data, right->text.len);
    return s;
}

static inline DsStr parser_copy_bang_name(const DsToken *name) {
    DsStr s;
    s.len = name->text.len + 1;
    s.data = (char *)ds_xcalloc(s.len + 1, 1);
    memcpy(s.data, name->text.data, name->text.len);
    s.data[name->text.len] = '!';
    return s;
}

static inline void parser_stmt_vec_push(DsStmtVec *vec, DsStmt *stmt) {
    DS_VEC_PUSH(vec, stmt, 16);
}

static inline void parser_word_vec_push(DsWordVec *vec, DsWord word) {
    DS_VEC_PUSH(vec, word, 8);
}

static inline void parser_expr_vec_push(DsExprVec *vec, DsExpr *expr) {
    DS_VEC_PUSH(vec, expr, 8);
}

static inline void parser_map_entry_vec_push(DsMapEntryVec *vec, DsMapEntry entry) {
    DS_VEC_PUSH(vec, entry, 8);
}

static inline void parser_fn_param_vec_push(DsFnParamVec *vec, DsFnParam param) {
    DS_VEC_PUSH(vec, param, 8);
}

static inline void parser_script_decl_vec_push(DsScriptDeclVec *vec, DsScriptDecl decl) {
    DS_VEC_PUSH(vec, decl, 8);
}

static inline DsExpr *parser_new_expr(DsExprKind kind, DsSpan span) {
    DsExpr *expr = (DsExpr *)ds_xcalloc(1, sizeof(DsExpr));
    expr->kind = kind;
    expr->span = span;
    return expr;
}

static inline DsStmt *parser_new_stmt(DsStmtKind kind, DsSpan span) {
    DsStmt *stmt = (DsStmt *)ds_xcalloc(1, sizeof(DsStmt));
    stmt->kind = kind;
    stmt->span = span;
    return stmt;
}

static inline void parser_skip_newlines(Parser *p) {
    while (parser_advance_if(p, DS_TOK_NEWLINE)) {}
}

static inline bool parser_is_stmt_end(Parser *p) {
    return parser_at(p, DS_TOK_NEWLINE) || parser_at(p, DS_TOK_EOF) || parser_at(p, DS_TOK_RBRACE);
}

static inline void parser_skip_to_stmt_end(Parser *p) {
    while (!parser_is_stmt_end(p)) parser_advance(p);
}

static inline void parser_consume_statement_end(Parser *p) {
    if (parser_at(p, DS_TOK_NEWLINE)) {
        parser_skip_newlines(p);
    }
}

static inline void parser_expect_stmt_end(Parser *p, const char *description) {
    if (!parser_is_stmt_end(p)) {
        ds_diag_error(p->diag, parser_peek(p)->span, "expected end of %s", description);
        parser_skip_to_stmt_end(p);
    }
    parser_consume_statement_end(p);
}

static inline bool parser_expect(Parser *p, DsTokenKind kind, const char *message) {
    if (parser_advance_if(p, kind)) return true;
    ds_diag_error(p->diag, parser_peek(p)->span, "%s", message);
    return false;
}

static inline bool parser_reject_trailing_comma(Parser *p, DsTokenKind closing_kind, const char *message) {
    if (!parser_at(p, closing_kind)) return false;
    ds_diag_error(p->diag, parser_peek(p)->span, "%s", message);
    return true;
}

static inline bool parser_is_identifier_like(DsTokenKind kind) {
    return kind == DS_TOK_IDENT || kind == DS_TOK_SCRIPT || kind == DS_TOK_IMPORT || kind == DS_TOK_ARG ||
           kind == DS_TOK_OPTION || kind == DS_TOK_FLAG || kind == DS_TOK_TYPE_STRING ||
           kind == DS_TOK_TYPE_INT || kind == DS_TOK_TYPE_BOOL || kind == DS_TOK_RUN || kind == DS_TOK_FN ||
           kind == DS_TOK_DEFER || kind == DS_TOK_TRAP;
}

static inline void parser_case_pattern_vec_push(DsCasePatternVec *vec, DsCasePattern pattern) {
    DS_VEC_PUSH(vec, pattern, 4);
}

static inline void parser_case_arm_vec_push(DsCaseArmVec *vec, DsCaseArm arm) {
    DS_VEC_PUSH(vec, arm, 4);
}

static inline bool parser_expect_identifier_like(Parser *p, const char *message) {
    if (parser_is_identifier_like(parser_peek(p)->kind)) {
        parser_advance(p);
        return true;
    }
    ds_diag_error(p->diag, parser_peek(p)->span, "%s", message);
    return false;
}

static inline bool parser_decode_string_literal(DsStr literal, DsStr *out) {
    out->data = NULL;
    out->len = 0;
    if (literal.len < 2 || literal.data[0] != '"' || literal.data[literal.len - 1] != '"') return false;
    char *buf = (char *)ds_xcalloc(literal.len, 1);
    size_t len = 0;
    for (size_t i = 1; i + 1 < literal.len; i++) {
        char c = literal.data[i];
        if (c == '\\' && i + 1 < literal.len - 1) {
            char escaped = literal.data[++i];
            if (escaped == 'n') c = '\n';
            else if (escaped == 't') c = '\t';
            else if (escaped == '"') c = '"';
            else if (escaped == '\\') c = '\\';
            else c = escaped;
        }
        buf[len++] = c;
    }
    buf[len] = '\0';
    out->data = buf;
    out->len = len;
    return true;
}

static inline bool parser_is_redirect_token(DsTokenKind kind) {
    return kind == DS_TOK_REDIRECT_OUT || kind == DS_TOK_REDIRECT_OUT_APPEND ||
           kind == DS_TOK_REDIRECT_ERR || kind == DS_TOK_REDIRECT_ERR_APPEND ||
           kind == DS_TOK_REDIRECT_ALL || kind == DS_TOK_REDIRECT_ALL_APPEND;
}

static inline DsRedirectKind parser_redirect_kind_from_token(DsTokenKind kind) {
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

DsExpr *parse_expr(Parser *p);
void parse_call_args(Parser *p, DsExprVec *args);
void parse_command_words_until_end(Parser *p, DsWordVec *words, DsSpan *span, bool reject_redirection);
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
