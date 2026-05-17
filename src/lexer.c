#include "ds.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void token_vec_push(DsTokenVec *vec, DsToken token) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 64;
        vec->items = (DsToken *)ds_xrealloc(vec->items, vec->cap * sizeof(DsToken));
    }
    vec->items[vec->len++] = token;
}

static bool is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_continue(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static DsTokenKind keyword_kind(const char *text, size_t len) {
    if (len == 3 && strncmp(text, "let", 3) == 0) return DS_TOK_LET;
    if (len == 2 && strncmp(text, "if", 2) == 0) return DS_TOK_IF;
    if (len == 4 && strncmp(text, "else", 4) == 0) return DS_TOK_ELSE;
    if (len == 6 && strncmp(text, "script", 6) == 0) return DS_TOK_SCRIPT;
    if (len == 6 && strncmp(text, "import", 6) == 0) return DS_TOK_IMPORT;
    if (len == 3 && strncmp(text, "arg", 3) == 0) return DS_TOK_ARG;
    if (len == 6 && strncmp(text, "option", 6) == 0) return DS_TOK_OPTION;
    if (len == 4 && strncmp(text, "flag", 4) == 0) return DS_TOK_FLAG;
    if (len == 6 && strncmp(text, "string", 6) == 0) return DS_TOK_TYPE_STRING;
    if (len == 3 && strncmp(text, "int", 3) == 0) return DS_TOK_TYPE_INT;
    if (len == 4 && strncmp(text, "bool", 4) == 0) return DS_TOK_TYPE_BOOL;
    if (len == 4 && strncmp(text, "true", 4) == 0) return DS_TOK_TRUE;
    if (len == 5 && strncmp(text, "false", 5) == 0) return DS_TOK_FALSE;
    return DS_TOK_IDENT;
}

static void add_token(DsTokenVec *out, const DsSource *source, DsTokenKind kind, const char *start, size_t len, DsLoc loc, DsLoc end) {
    DsToken token;
    token.kind = kind;
    token.text.data = ds_str_dup_range(start, len);
    token.text.len = len;
    token.span.start = loc;
    token.span.end = end;
    token.span.source = source;
    token_vec_push(out, token);
}

const char *ds_token_kind_name(DsTokenKind kind) {
    switch (kind) {
        case DS_TOK_EOF: return "EOF";
        case DS_TOK_NEWLINE: return "NEWLINE";
        case DS_TOK_IDENT: return "IDENT";
        case DS_TOK_INT: return "INT";
        case DS_TOK_STRING: return "STRING";
        case DS_TOK_DOLLAR_IDENT: return "DOLLAR_IDENT";
        case DS_TOK_LET: return "LET";
        case DS_TOK_IF: return "IF";
        case DS_TOK_ELSE: return "ELSE";
        case DS_TOK_SCRIPT: return "SCRIPT";
        case DS_TOK_IMPORT: return "IMPORT";
        case DS_TOK_ARG: return "ARG";
        case DS_TOK_OPTION: return "OPTION";
        case DS_TOK_FLAG: return "FLAG";
        case DS_TOK_TYPE_STRING: return "TYPE_STRING";
        case DS_TOK_TYPE_INT: return "TYPE_INT";
        case DS_TOK_TYPE_BOOL: return "TYPE_BOOL";
        case DS_TOK_TRUE: return "TRUE";
        case DS_TOK_FALSE: return "FALSE";
        case DS_TOK_COLON: return "COLON";
        case DS_TOK_EQUAL: return "EQUAL";
        case DS_TOK_EQUAL_EQUAL: return "EQUAL_EQUAL";
        case DS_TOK_BANG: return "BANG";
        case DS_TOK_BANG_EQUAL: return "BANG_EQUAL";
        case DS_TOK_GREATER: return "GREATER";
        case DS_TOK_GREATER_EQUAL: return "GREATER_EQUAL";
        case DS_TOK_LESS: return "LESS";
        case DS_TOK_LESS_EQUAL: return "LESS_EQUAL";
        case DS_TOK_PLUS: return "PLUS";
        case DS_TOK_MINUS: return "MINUS";
        case DS_TOK_STAR: return "STAR";
        case DS_TOK_SLASH: return "SLASH";
        case DS_TOK_LBRACE: return "LBRACE";
        case DS_TOK_RBRACE: return "RBRACE";
        case DS_TOK_LPAREN: return "LPAREN";
        case DS_TOK_RPAREN: return "RPAREN";
        case DS_TOK_UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool ds_lex(const DsSource *source, DsTokenVec *out, DsDiag *diag) {
    memset(out, 0, sizeof(*out));
    size_t i = 0;
    int line = 1;
    int col = 1;

    while (i < source->len) {
        char c = source->data[i];
        DsLoc loc = {i, line, col};

        if (c == ' ' || c == '\t' || c == '\r') {
            i++;
            col++;
            continue;
        }

        if (c == '#') {
            while (i < source->len && source->data[i] != '\n') {
                i++;
                col++;
            }
            continue;
        }

        if (c == '\n') {
            DsLoc end = {i + 1, line + 1, 1};
            add_token(out, source, DS_TOK_NEWLINE, source->data + i, 1, loc, end);
            i++;
            line++;
            col = 1;
            continue;
        }

        if (is_ident_start(c)) {
            size_t start = i;
            int start_col = col;
            while (i < source->len && is_ident_continue(source->data[i])) {
                i++;
                col++;
            }
            DsLoc start_loc = {start, line, start_col};
            DsLoc end = {i, line, col};
            DsTokenKind kind = keyword_kind(source->data + start, i - start);
            add_token(out, source, kind, source->data + start, i - start, start_loc, end);
            continue;
        }

        if (isdigit((unsigned char)c)) {
            size_t start = i;
            int start_col = col;
            while (i < source->len && isdigit((unsigned char)source->data[i])) {
                i++;
                col++;
            }
            DsLoc start_loc = {start, line, start_col};
            DsLoc end = {i, line, col};
            add_token(out, source, DS_TOK_INT, source->data + start, i - start, start_loc, end);
            continue;
        }

        if (c == '"') {
            size_t start = i;
            int start_col = col;
            i++;
            col++;
            bool terminated = false;
            bool invalid_escape = false;
            while (i < source->len) {
                char ch = source->data[i];
                if (ch == '\\') {
                    DsLoc escape_loc = {i, line, col};
                    if (i + 1 < source->len && source->data[i + 1] != '\n') {
                        char escaped = source->data[i + 1];
                        if (!(escaped == 'n' || escaped == 't' || escaped == '"' || escaped == '\\')) {
                            DsLoc escape_end = {i + 2, line, col + 2};
                            ds_diag_error(diag, (DsSpan){escape_loc, escape_end, source},
                                          "invalid escape sequence `\\%c`; supported escapes are `\\n`, `\\t`, `\\\"`, and `\\\\`",
                                          escaped);
                            invalid_escape = true;
                        }
                        i += 2;
                        col += 2;
                        continue;
                    }
                    DsLoc escape_end = {i + 1, line, col + 1};
                    ds_diag_error(diag, (DsSpan){escape_loc, escape_end, source}, "invalid trailing escape in string literal");
                    invalid_escape = true;
                    i++;
                    col++;
                    continue;
                }
                if (ch == '"') {
                    i++;
                    col++;
                    terminated = true;
                    break;
                }
                if (ch == '\n') break;
                i++;
                col++;
            }
            DsLoc start_loc = {start, line, start_col};
            DsLoc end = {i, line, col};
            if (!terminated) {
                ds_diag_error(diag, (DsSpan){start_loc, end, source}, "unterminated string literal");
                while (i < source->len && source->data[i] != '\n') {
                    i++;
                    col++;
                }
                continue;
            }
            if (invalid_escape) continue;
            add_token(out, source, DS_TOK_STRING, source->data + start, i - start, start_loc, end);
            continue;
        }

        if (c == '$' && i + 1 < source->len && is_ident_start(source->data[i + 1])) {
            size_t start = i;
            int start_col = col;
            i += 2;
            col += 2;
            while (i < source->len && is_ident_continue(source->data[i])) {
                i++;
                col++;
            }
            DsLoc start_loc = {start, line, start_col};
            DsLoc end = {i, line, col};
            add_token(out, source, DS_TOK_DOLLAR_IDENT, source->data + start, i - start, start_loc, end);
            continue;
        }

        DsTokenKind kind = DS_TOK_UNKNOWN;
        size_t len = 1;
        if (c == '=' && i + 1 < source->len && source->data[i + 1] == '=') { kind = DS_TOK_EQUAL_EQUAL; len = 2; }
        else if (c == '!' && i + 1 < source->len && source->data[i + 1] == '=') { kind = DS_TOK_BANG_EQUAL; len = 2; }
        else if (c == '>' && i + 1 < source->len && source->data[i + 1] == '=') { kind = DS_TOK_GREATER_EQUAL; len = 2; }
        else if (c == '<' && i + 1 < source->len && source->data[i + 1] == '=') { kind = DS_TOK_LESS_EQUAL; len = 2; }
        else if (c == '=') kind = DS_TOK_EQUAL;
        else if (c == ':') kind = DS_TOK_COLON;
        else if (c == '!') kind = DS_TOK_BANG;
        else if (c == '>') kind = DS_TOK_GREATER;
        else if (c == '<') kind = DS_TOK_LESS;
        else if (c == '+') kind = DS_TOK_PLUS;
        else if (c == '-') kind = DS_TOK_MINUS;
        else if (c == '*') kind = DS_TOK_STAR;
        else if (c == '/') kind = DS_TOK_SLASH;
        else if (c == '{') kind = DS_TOK_LBRACE;
        else if (c == '}') kind = DS_TOK_RBRACE;
        else if (c == '(') kind = DS_TOK_LPAREN;
        else if (c == ')') kind = DS_TOK_RPAREN;

        DsLoc end = {i + len, line, col + (int)len};
        add_token(out, source, kind, source->data + i, len, loc, end);
        /*
         * Keep otherwise unknown printable characters as tokens instead of
         * failing in the lexer. Command statements are intentionally shell-like
         * and may contain path or flag punctuation that is not part of the
         * v0.1.0 expression grammar yet. The parser decides whether a token is
         * valid in its current context.
         */
        i += len;
        col += (int)len;
    }

    DsLoc loc = {source->len, line, col};
    add_token(out, source, DS_TOK_EOF, "", 0, loc, loc);
    return !diag->has_error;
}

void ds_tokens_free(DsTokenVec *tokens) {
    for (size_t i = 0; i < tokens->len; i++) {
        free(tokens->items[i].text.data);
    }
    free(tokens->items);
    tokens->items = NULL;
    tokens->len = 0;
    tokens->cap = 0;
}

void ds_tokens_print(const DsTokenVec *tokens, FILE *out) {
    for (size_t i = 0; i < tokens->len; i++) {
        const DsToken *t = &tokens->items[i];
        fprintf(out, "%d:%d  %-14s \"", t->span.start.line, t->span.start.column,
                ds_token_kind_name(t->kind));
        for (size_t j = 0; j < t->text.len; j++) {
            unsigned char ch = (unsigned char)t->text.data[j];
            if (ch == '\\') fputs("\\\\", out);
            else if (ch == '"') fputs("\\\"", out);
            else if (ch == '\n') fputs("\\n", out);
            else if (ch == '\r') fputs("\\r", out);
            else if (ch == '\t') fputs("\\t", out);
            else fputc(ch, out);
        }
        fputs("\"\n", out);
    }
}
