#include "frontend.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static DsTokenKind keyword_kind(const char *text, size_t len) {
    if (len == 3 && strncmp(text, "let", 3) == 0) return DS_TOK_LET;
    if (len == 2 && strncmp(text, "if", 2) == 0) return DS_TOK_IF;
    if (len == 4 && strncmp(text, "else", 4) == 0) return DS_TOK_ELSE;
    if (len == 6 && strncmp(text, "script", 6) == 0) return DS_TOK_SCRIPT;
    if (len == 6 && strncmp(text, "import", 6) == 0) return DS_TOK_IMPORT;
    if (len == 3 && strncmp(text, "arg", 3) == 0) return DS_TOK_ARG;
    if (len == 6 && strncmp(text, "option", 6) == 0) return DS_TOK_OPTION;
    if (len == 4 && strncmp(text, "flag", 4) == 0) return DS_TOK_FLAG;
    if (len == 3 && strncmp(text, "run", 3) == 0) return DS_TOK_RUN;
    if (len == 6 && strncmp(text, "string", 6) == 0) return DS_TOK_TYPE_STRING;
    if (len == 3 && strncmp(text, "int", 3) == 0) return DS_TOK_TYPE_INT;
    if (len == 4 && strncmp(text, "bool", 4) == 0) return DS_TOK_TYPE_BOOL;
    if (len == 4 && strncmp(text, "true", 4) == 0) return DS_TOK_TRUE;
    if (len == 5 && strncmp(text, "false", 5) == 0) return DS_TOK_FALSE;
    if (len == 2 && strncmp(text, "fn", 2) == 0) return DS_TOK_FN;
    if (len == 3 && strncmp(text, "for", 3) == 0) return DS_TOK_FOR;
    if (len == 2 && strncmp(text, "in", 2) == 0) return DS_TOK_IN;
    if (len == 7 && strncmp(text, "matches", 7) == 0) return DS_TOK_MATCHES;
    if (len == 5 && strncmp(text, "while", 5) == 0) return DS_TOK_WHILE;
    if (len == 5 && strncmp(text, "break", 5) == 0) return DS_TOK_BREAK;
    if (len == 8 && strncmp(text, "continue", 8) == 0) return DS_TOK_CONTINUE;
    if (len == 4 && strncmp(text, "case", 4) == 0) return DS_TOK_CASE;
    if (len == 4 && strncmp(text, "test", 4) == 0) return DS_TOK_TEST;
    if (len == 6 && strncmp(text, "assert", 6) == 0) return DS_TOK_ASSERT;
    if (len == 6 && strncmp(text, "return", 6) == 0) return DS_TOK_RETURN;
    if (len == 5 && strncmp(text, "defer", 5) == 0) return DS_TOK_DEFER;
    if (len == 4 && strncmp(text, "trap", 4) == 0) return DS_TOK_TRAP;
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
    DS_VEC_PUSH(out, token, 64);
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
        case DS_TOK_RUN: return "RUN";
        case DS_TOK_TYPE_STRING: return "TYPE_STRING";
        case DS_TOK_TYPE_INT: return "TYPE_INT";
        case DS_TOK_TYPE_BOOL: return "TYPE_BOOL";
        case DS_TOK_TRUE: return "TRUE";
        case DS_TOK_FALSE: return "FALSE";
        case DS_TOK_FN: return "FN";
        case DS_TOK_FOR: return "FOR";
        case DS_TOK_IN: return "IN";
        case DS_TOK_MATCHES: return "MATCHES";
        case DS_TOK_WHILE: return "WHILE";
        case DS_TOK_BREAK: return "BREAK";
        case DS_TOK_CONTINUE: return "CONTINUE";
        case DS_TOK_CASE: return "CASE";
        case DS_TOK_PIPE: return "PIPE";
        case DS_TOK_AND_AND: return "AND_AND";
        case DS_TOK_OR_OR: return "OR_OR";
        case DS_TOK_TEST: return "TEST";
        case DS_TOK_ASSERT: return "ASSERT";
        case DS_TOK_RETURN: return "RETURN";
        case DS_TOK_DEFER: return "DEFER";
        case DS_TOK_TRAP: return "TRAP";
        case DS_TOK_COLON: return "COLON";
        case DS_TOK_COMMA: return "COMMA";
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
        case DS_TOK_STAR_STAR: return "STAR_STAR";
        case DS_TOK_SLASH: return "SLASH";
        case DS_TOK_PERCENT: return "PERCENT";
        case DS_TOK_DOT: return "DOT";
        case DS_TOK_DOT_DOT: return "DOT_DOT";
        case DS_TOK_REGEX: return "REGEX";
        case DS_TOK_REDIRECT_OUT: return "REDIRECT_OUT";
        case DS_TOK_REDIRECT_OUT_APPEND: return "REDIRECT_OUT_APPEND";
        case DS_TOK_REDIRECT_ERR: return "REDIRECT_ERR";
        case DS_TOK_REDIRECT_ERR_APPEND: return "REDIRECT_ERR_APPEND";
        case DS_TOK_REDIRECT_ALL: return "REDIRECT_ALL";
        case DS_TOK_REDIRECT_ALL_APPEND: return "REDIRECT_ALL_APPEND";
        case DS_TOK_LBRACE: return "LBRACE";
        case DS_TOK_RBRACE: return "RBRACE";
        case DS_TOK_LBRACKET: return "LBRACKET";
        case DS_TOK_RBRACKET: return "RBRACKET";
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
    bool expect_regex = false;
    enum { REGEX_HELPER_NONE, REGEX_HELPER_SAW_REGEX, REGEX_HELPER_SAW_DOT, REGEX_HELPER_SAW_NAME } regex_helper_state = REGEX_HELPER_NONE;
    int regex_helper_paren_depth[32];
    size_t regex_helper_arg_index[32];
    size_t regex_helper_stack_len = 0;

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

        if (ds_is_ident_start(c)) {
            size_t start = i;
            int start_col = col;
            while (i < source->len && ds_is_ident_continue(source->data[i])) {
                i++;
                col++;
            }
            DsLoc start_loc = {start, line, start_col};
            DsLoc end = {i, line, col};
            DsTokenKind kind = keyword_kind(source->data + start, i - start);
            add_token(out, source, kind, source->data + start, i - start, start_loc, end);
            bool ident_regex = kind == DS_TOK_IDENT && (i - start) == 5 && memcmp(source->data + start, "regex", 5) == 0;
            bool ident_regex_helper = kind == DS_TOK_IDENT &&
                                      (((i - start) == 5 && memcmp(source->data + start, "match", 5) == 0) ||
                                       ((i - start) == 7 && memcmp(source->data + start, "replace", 7) == 0));
            if (ident_regex) regex_helper_state = REGEX_HELPER_SAW_REGEX;
            else if (regex_helper_state == REGEX_HELPER_SAW_DOT && ident_regex_helper) regex_helper_state = REGEX_HELPER_SAW_NAME;
            else regex_helper_state = REGEX_HELPER_NONE;
            expect_regex = kind == DS_TOK_MATCHES;
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
            DsLoc start_loc = {start, line, start_col};
            if (i + 2 < source->len && source->data[i + 1] == '"' && source->data[i + 2] == '"') {
                i += 3;
                col += 3;
                bool terminated = false;
                while (i < source->len) {
                    if (i + 2 < source->len && source->data[i] == '"' && source->data[i + 1] == '"' && source->data[i + 2] == '"') {
                        i += 3;
                        col += 3;
                        terminated = true;
                        break;
                    }
                    if (source->data[i] == '\0') {
                        ds_diag_error(diag, (DsSpan){(DsLoc){i, line, col}, (DsLoc){i + 1, line, col + 1}, source}, "string literals do not support embedded NUL bytes");
                    }
                    if (source->data[i] == '\n') {
                        i++;
                        line++;
                        col = 1;
                    } else {
                        i++;
                        col++;
                    }
                }
                DsLoc end = {i, line, col};
                if (!terminated) {
                    ds_diag_error(diag, (DsSpan){start_loc, end, source}, "unterminated triple-quoted string literal");
                    continue;
                }
                add_token(out, source, DS_TOK_STRING, source->data + start, i - start, start_loc, end);
                continue;
            }
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
                if (ch == '\0') {
                    ds_diag_error(diag, (DsSpan){(DsLoc){i, line, col}, (DsLoc){i + 1, line, col + 1}, source}, "string literals do not support embedded NUL bytes");
                    invalid_escape = true;
                }
                i++;
                col++;
            }
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

        if (c == '$' && i + 1 < source->len && ds_is_ident_start(source->data[i + 1])) {
            size_t start = i;
            int start_col = col;
            i += 2;
            col += 2;
            while (i < source->len && ds_is_ident_continue(source->data[i])) {
                i++;
                col++;
            }
            DsLoc start_loc = {start, line, start_col};
            DsLoc end = {i, line, col};
            add_token(out, source, DS_TOK_DOLLAR_IDENT, source->data + start, i - start, start_loc, end);
            continue;
        }

        bool helper_expects_regex = regex_helper_stack_len > 0 &&
                                    regex_helper_paren_depth[regex_helper_stack_len - 1] == 1 &&
                                    regex_helper_arg_index[regex_helper_stack_len - 1] == 1;
        if ((expect_regex || helper_expects_regex) && c == '/') {
            size_t start = i;
            int start_col = col;
            DsLoc start_loc = {start, line, start_col};
            i++;
            col++;
            bool terminated = false;
            bool empty = true;
            while (i < source->len) {
                char ch = source->data[i];
                if (ch == '\n') break;
                if (ch == '\\') {
                    if (i + 1 >= source->len || source->data[i + 1] == '\n') break;
                    empty = false;
                    i += 2;
                    col += 2;
                    continue;
                }
                if (ch == '/') {
                    i++;
                    col++;
                    terminated = true;
                    break;
                }
                empty = false;
                i++;
                col++;
            }
            if (terminated && i < source->len && isalpha((unsigned char)source->data[i])) {
                if (source->data[i] == 'i') { i++; col++; }
                while (i < source->len && isalpha((unsigned char)source->data[i])) { i++; col++; }
            }
            DsLoc end = {i, line, col};
            if (!terminated) {
                ds_diag_error(diag, (DsSpan){start_loc, end, source}, "unterminated regex literal");
                while (i < source->len && source->data[i] != '\n') { i++; col++; }
                expect_regex = false;
                continue;
            }
            if (empty) ds_diag_error(diag, (DsSpan){start_loc, end, source}, "empty regex literals are not supported in v0.23.0");
            add_token(out, source, DS_TOK_REGEX, source->data + start, i - start, start_loc, end);
            expect_regex = false;
            regex_helper_state = REGEX_HELPER_NONE;
            continue;
        }

        DsTokenKind kind = DS_TOK_UNKNOWN;
        size_t len = 1;
        if (c == '|' && i + 1 < source->len && source->data[i + 1] == '|') { kind = DS_TOK_OR_OR; len = 2; }
        else if (c == '|' && i + 1 < source->len && source->data[i + 1] == '>') { kind = DS_TOK_REDIRECT_OUT; len = 2; if (i + 2 < source->len && source->data[i + 2] == '>') { kind = DS_TOK_REDIRECT_OUT_APPEND; len = 3; } }
        else if (c == '|') { kind = DS_TOK_PIPE; len = 1; }
        else if (c == '!' && i + 1 < source->len && source->data[i + 1] == '>') { kind = DS_TOK_REDIRECT_ERR; len = 2; if (i + 2 < source->len && source->data[i + 2] == '>') { kind = DS_TOK_REDIRECT_ERR_APPEND; len = 3; } }
        else if (c == '&' && i + 1 < source->len && source->data[i + 1] == '&') { kind = DS_TOK_AND_AND; len = 2; }
        else if (c == '&' && i + 1 < source->len && source->data[i + 1] == '>') { kind = DS_TOK_REDIRECT_ALL; len = 2; if (i + 2 < source->len && source->data[i + 2] == '>') { kind = DS_TOK_REDIRECT_ALL_APPEND; len = 3; } }
        else if (c == '=' && i + 1 < source->len && source->data[i + 1] == '=') { kind = DS_TOK_EQUAL_EQUAL; len = 2; }
        else if (c == '!' && i + 1 < source->len && source->data[i + 1] == '=') { kind = DS_TOK_BANG_EQUAL; len = 2; }
        else if (c == '>' && i + 1 < source->len && source->data[i + 1] == '=') { kind = DS_TOK_GREATER_EQUAL; len = 2; }
        else if (c == '<' && i + 1 < source->len && source->data[i + 1] == '=') { kind = DS_TOK_LESS_EQUAL; len = 2; }
        else if (c == '=') kind = DS_TOK_EQUAL;
        else if (c == ':') kind = DS_TOK_COLON;
        else if (c == ',') kind = DS_TOK_COMMA;
        else if (c == '!') kind = DS_TOK_BANG;
        else if (c == '>') kind = DS_TOK_GREATER;
        else if (c == '<') kind = DS_TOK_LESS;
        else if (c == '+') kind = DS_TOK_PLUS;
        else if (c == '-') kind = DS_TOK_MINUS;
        else if (c == '*' && i + 1 < source->len && source->data[i + 1] == '*') { kind = DS_TOK_STAR_STAR; len = 2; }
        else if (c == '*') kind = DS_TOK_STAR;
        else if (c == '/') kind = DS_TOK_SLASH;
        else if (c == '%') kind = DS_TOK_PERCENT;
        else if (c == '.' && i + 1 < source->len && source->data[i + 1] == '.') { kind = DS_TOK_DOT_DOT; len = 2; }
        else if (c == '.') kind = DS_TOK_DOT;
        else if (c == '{') kind = DS_TOK_LBRACE;
        else if (c == '}') kind = DS_TOK_RBRACE;
        else if (c == '[') kind = DS_TOK_LBRACKET;
        else if (c == ']') kind = DS_TOK_RBRACKET;
        else if (c == '(') kind = DS_TOK_LPAREN;
        else if (c == ')') kind = DS_TOK_RPAREN;

        DsLoc end = {i + len, line, col + (int)len};
        add_token(out, source, kind, source->data + i, len, loc, end);
        if (kind == DS_TOK_DOT && regex_helper_state == REGEX_HELPER_SAW_REGEX) {
            regex_helper_state = REGEX_HELPER_SAW_DOT;
        } else if (kind == DS_TOK_LPAREN) {
            if (regex_helper_state == REGEX_HELPER_SAW_NAME && regex_helper_stack_len < DS_ARRAY_LEN(regex_helper_paren_depth)) {
                regex_helper_paren_depth[regex_helper_stack_len] = 1;
                regex_helper_arg_index[regex_helper_stack_len] = 0;
                regex_helper_stack_len++;
            } else if (regex_helper_stack_len > 0) {
                regex_helper_paren_depth[regex_helper_stack_len - 1]++;
            }
            regex_helper_state = REGEX_HELPER_NONE;
        } else if (kind == DS_TOK_RPAREN) {
            if (regex_helper_stack_len > 0) {
                if (regex_helper_paren_depth[regex_helper_stack_len - 1] > 1) regex_helper_paren_depth[regex_helper_stack_len - 1]--;
                else regex_helper_stack_len--;
            }
            regex_helper_state = REGEX_HELPER_NONE;
        } else if (kind == DS_TOK_COMMA) {
            if (regex_helper_stack_len > 0 && regex_helper_paren_depth[regex_helper_stack_len - 1] == 1) regex_helper_arg_index[regex_helper_stack_len - 1]++;
            regex_helper_state = REGEX_HELPER_NONE;
        } else if (kind != DS_TOK_UNKNOWN) {
            regex_helper_state = REGEX_HELPER_NONE;
        }
        expect_regex = false;
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
