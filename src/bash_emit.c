#include "ds.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} EmitBuf;

typedef struct {
    DsStr *items;
    size_t len;
    size_t cap;
} SymbolVec;

typedef struct {
    const DsSource *source;
    DsDiag *diag;
    SymbolVec symbols;
    EmitBuf out;
} BashEmitter;

static void buf_reserve(EmitBuf *buf, size_t need) {
    if (need <= buf->cap) return;
    size_t cap = buf->cap ? buf->cap : 256;
    while (cap < need) cap *= 2;
    buf->data = (char *)ds_xrealloc(buf->data, cap);
    buf->cap = cap;
}

static void buf_append_len(EmitBuf *buf, const char *data, size_t len) {
    buf_reserve(buf, buf->len + len + 1);
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void buf_append(EmitBuf *buf, const char *text) {
    buf_append_len(buf, text, strlen(text));
}

static void buf_appendf(EmitBuf *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        va_end(args);
        return;
    }
    size_t start = buf->len;
    buf_reserve(buf, buf->len + (size_t)n + 1);
    vsnprintf(buf->data + start, (size_t)n + 1, fmt, args);
    va_end(args);
    buf->len += (size_t)n;
}

static void symbol_vec_push(SymbolVec *vec, DsStr name) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 16;
        vec->items = (DsStr *)ds_xrealloc(vec->items, vec->cap * sizeof(DsStr));
    }
    vec->items[vec->len++] = name;
}

static bool str_eq(DsStr a, const char *b) {
    size_t len = strlen(b);
    return a.len == len && memcmp(a.data, b, len) == 0;
}

static bool symbol_exists(const SymbolVec *symbols, DsStr name) {
    for (size_t i = 0; i < symbols->len; i++) {
        DsStr existing = symbols->items[i];
        if (existing.len == name.len && memcmp(existing.data, name.data, name.len) == 0) return true;
    }
    return false;
}

static void free_symbols(SymbolVec *symbols) {
    for (size_t i = 0; i < symbols->len; i++) free(symbols->items[i].data);
    free(symbols->items);
}

static void emit_indent(EmitBuf *out, int indent) {
    for (int i = 0; i < indent; i++) buf_append(out, "  ");
}

static bool is_safe_identifier(DsStr name) {
    if (name.len == 0) return false;
    char first = name.data[0];
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) return false;
    for (size_t i = 1; i < name.len; i++) {
        char c = name.data[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

static void emit_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_");
    buf_append_len(out, name.data, name.len);
}

static void emit_bash_single_quoted(EmitBuf *out, const char *data, size_t len) {
    buf_append(out, "'");
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\'') buf_append(out, "'\\''");
        else buf_append_len(out, data + i, 1);
    }
    buf_append(out, "'");
}

static bool decode_string_literal(DsDiag *diag, const DsExpr *expr, char **out_data, size_t *out_len) {
    DsStr text = expr->as.text;
    if (text.len < 2 || text.data[0] != '"' || text.data[text.len - 1] != '"') {
        ds_diag_error(diag, expr->span, "invalid string literal for Bash emission");
        return false;
    }

    char *buf = (char *)ds_xcalloc(text.len, 1);
    size_t len = 0;
    for (size_t i = 1; i + 1 < text.len; i++) {
        char c = text.data[i];
        if (c == '\\' && i + 1 < text.len - 1) {
            char escaped = text.data[++i];
            if (escaped == 'n') c = '\n';
            else if (escaped == 't') c = '\t';
            else if (escaped == '"') c = '"';
            else if (escaped == '\\') c = '\\';
            else c = escaped;
        }
        buf[len++] = c;
    }
    buf[len] = '\0';
    *out_data = buf;
    *out_len = len;
    return true;
}

static bool emit_interpolated_string(BashEmitter *e, const DsExpr *expr, EmitBuf *out) {
    char *decoded = NULL;
    size_t len = 0;
    if (!decode_string_literal(e->diag, expr, &decoded, &len)) return false;

    buf_append(out, "\"");
    for (size_t i = 0; i < len; i++) {
        char c = decoded[i];
        if (c == '{') {
            size_t start = i + 1;
            size_t j = start;
            if (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || decoded[j] == '_')) {
                j++;
                while (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || (decoded[j] >= '0' && decoded[j] <= '9') || decoded[j] == '_')) j++;
                if (j < len && decoded[j] == '}') {
                    DsStr name = {decoded + start, j - start};
                    if (!symbol_exists(&e->symbols, name)) {
                        ds_diag_error(e->diag, expr->span, "unknown interpolation variable `%.*s`", (int)name.len, name.data);
                        free(decoded);
                        return false;
                    }
                    buf_append(out, "${__ds_");
                    buf_append_len(out, name.data, name.len);
                    buf_append(out, "}");
                    i = j;
                    continue;
                }
            }
            ds_diag_error(e->diag, expr->span, "unsupported string interpolation; expected `{name}`");
            free(decoded);
            return false;
        }
        if (c == '"' || c == '\\' || c == '$' || c == '`') buf_append(out, "\\");
        buf_append_len(out, &c, 1);
    }
    buf_append(out, "\"");
    free(decoded);
    return true;
}

static bool emit_value_expr(BashEmitter *e, const DsExpr *expr, EmitBuf *out) {
    switch (expr->kind) {
        case DS_EXPR_STRING: {
            char *decoded = NULL;
            size_t len = 0;
            if (!decode_string_literal(e->diag, expr, &decoded, &len)) return false;
            emit_bash_single_quoted(out, decoded, len);
            free(decoded);
            return true;
        }
        case DS_EXPR_INT:
            buf_append_len(out, expr->as.text.data, expr->as.text.len);
            return true;
        case DS_EXPR_BOOL:
            buf_append(out, expr->as.boolean ? "true" : "false");
            return true;
        default:
            ds_diag_error(e->diag, expr->span, "this expression cannot be emitted as a Bash assignment in v0.2.0");
            return false;
    }
}

static bool emit_condition_operand(BashEmitter *e, const DsExpr *expr, EmitBuf *out) {
    switch (expr->kind) {
        case DS_EXPR_IDENT:
            if (!symbol_exists(&e->symbols, expr->as.text)) {
                ds_diag_error(e->diag, expr->span, "unknown variable `%.*s`", (int)expr->as.text.len, expr->as.text.data);
                return false;
            }
            buf_append(out, "\"$");
            emit_var_name(out, expr->as.text);
            buf_append(out, "\"");
            return true;
        case DS_EXPR_STRING:
            return emit_interpolated_string(e, expr, out);
        case DS_EXPR_INT:
            buf_append_len(out, expr->as.text.data, expr->as.text.len);
            return true;
        case DS_EXPR_BOOL:
            buf_append(out, expr->as.boolean ? "true" : "false");
            return true;
        default:
            ds_diag_error(e->diag, expr->span, "unsupported condition operand for Bash emission");
            return false;
    }
}

static bool emit_condition(BashEmitter *e, const DsExpr *expr, EmitBuf *out) {
    if (expr->kind == DS_EXPR_IDENT) {
        if (!symbol_exists(&e->symbols, expr->as.text)) {
            ds_diag_error(e->diag, expr->span, "unknown variable `%.*s`", (int)expr->as.text.len, expr->as.text.data);
            return false;
        }
        buf_append(out, "[[ \"$");
        emit_var_name(out, expr->as.text);
        buf_append(out, "\" == true ]]");
        return true;
    }
    if (expr->kind == DS_EXPR_BOOL) {
        buf_append(out, expr->as.boolean ? "true" : "false");
        return true;
    }
    if (expr->kind == DS_EXPR_UNARY && str_eq(expr->as.unary.op, "!")) {
        buf_append(out, "! ");
        return emit_condition(e, expr->as.unary.right, out);
    }
    if (expr->kind == DS_EXPR_BINARY) {
        const char *op = NULL;
        if (str_eq(expr->as.binary.op, "==")) op = "==";
        else if (str_eq(expr->as.binary.op, "!=")) op = "!=";
        else if (str_eq(expr->as.binary.op, ">")) op = ">";
        else if (str_eq(expr->as.binary.op, ">=")) op = ">=";
        else if (str_eq(expr->as.binary.op, "<")) op = "<";
        else if (str_eq(expr->as.binary.op, "<=")) op = "<=";
        if (!op) {
            ds_diag_error(e->diag, expr->span, "operator `%.*s` cannot be emitted in a Bash condition in v0.2.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
            return false;
        }
        buf_append(out, "[[ ");
        if (!emit_condition_operand(e, expr->as.binary.left, out)) return false;
        buf_appendf(out, " %s ", op);
        if (!emit_condition_operand(e, expr->as.binary.right, out)) return false;
        buf_append(out, " ]]");
        return true;
    }
    ds_diag_error(e->diag, expr->span, "unsupported condition for Bash emission");
    return false;
}

static bool emit_command_word(BashEmitter *e, DsStr word, EmitBuf *out, DsSpan span) {
    if (word.len >= 2 && word.data[0] == '"' && word.data[word.len - 1] == '"') {
        DsExpr fake = {.kind = DS_EXPR_STRING, .span = span};
        fake.as.text = word;
        return emit_interpolated_string(e, &fake, out);
    }
    if (word.len >= 2 && word.data[0] == '$') {
        DsStr name = {word.data + 1, word.len - 1};
        if (!symbol_exists(&e->symbols, name)) {
            ds_diag_error(e->diag, span, "unknown command variable `%.*s`", (int)name.len, name.data);
            return false;
        }
        buf_append(out, "\"$");
        emit_var_name(out, name);
        buf_append(out, "\"");
        return true;
    }
    buf_append_len(out, word.data, word.len);
    return true;
}

static bool emit_stmt(BashEmitter *e, const DsStmt *stmt, int indent);

static bool emit_block_body(BashEmitter *e, const DsStmt *block, int indent) {
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) {
        if (!emit_stmt(e, block->as.block_stmt.statements.items[i], indent)) return false;
    }
    return true;
}

static bool emit_stmt(BashEmitter *e, const DsStmt *stmt, int indent) {
    emit_indent(&e->out, indent);
    buf_appendf(&e->out, "# ds: %s:%d\n", e->source->path ? e->source->path : "<source>", stmt->span.start.line);

    switch (stmt->kind) {
        case DS_STMT_LET:
            if (!is_safe_identifier(stmt->as.let_stmt.name)) {
                ds_diag_error(e->diag, stmt->span, "cannot emit unsafe Bash variable name `%.*s`", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);
                return false;
            }
            emit_indent(&e->out, indent);
            emit_var_name(&e->out, stmt->as.let_stmt.name);
            buf_append(&e->out, "=");
            if (!emit_value_expr(e, stmt->as.let_stmt.value, &e->out)) return false;
            buf_append(&e->out, "\n\n");
            if (!symbol_exists(&e->symbols, stmt->as.let_stmt.name)) {
                DsStr copy = {ds_str_dup_range(stmt->as.let_stmt.name.data, stmt->as.let_stmt.name.len), stmt->as.let_stmt.name.len};
                symbol_vec_push(&e->symbols, copy);
            }
            return true;

        case DS_STMT_CMD:
            emit_indent(&e->out, indent);
            for (size_t i = 0; i < stmt->as.cmd_stmt.words.len; i++) {
                if (i > 0) buf_append(&e->out, " ");
                if (!emit_command_word(e, stmt->as.cmd_stmt.words.items[i], &e->out, stmt->span)) return false;
            }
            buf_append(&e->out, "\n\n");
            return true;

        case DS_STMT_IF:
            emit_indent(&e->out, indent);
            buf_append(&e->out, "if ");
            if (!emit_condition(e, stmt->as.if_stmt.condition, &e->out)) return false;
            buf_append(&e->out, "; then\n");
            if (!emit_block_body(e, stmt->as.if_stmt.then_branch, indent + 1)) return false;
            if (stmt->as.if_stmt.else_branch) {
                emit_indent(&e->out, indent);
                buf_append(&e->out, "else\n");
                if (!emit_block_body(e, stmt->as.if_stmt.else_branch, indent + 1)) return false;
            }
            emit_indent(&e->out, indent);
            buf_append(&e->out, "fi\n\n");
            return true;

        case DS_STMT_BLOCK:
            return emit_block_body(e, stmt, indent);
    }
    return true;
}

bool ds_emit_bash(const DsSource *source, const DsAst *ast, const char *output_path, DsDiag *diag) {
    BashEmitter e;
    memset(&e, 0, sizeof(e));
    e.source = source;
    e.diag = diag;

    buf_append(&e.out, "#!/usr/bin/env bash\n");
    buf_append(&e.out, "set -euo pipefail\n\n");

    for (size_t i = 0; i < ast->statements.len; i++) {
        if (!emit_stmt(&e, ast->statements.items[i], 0)) {
            free_symbols(&e.symbols);
            free(e.out.data);
            return false;
        }
    }

    FILE *fp = fopen(output_path, "wb");
    if (!fp) {
        DsSpan span = ast->span;
        ds_diag_error(diag, span, "failed to open output file `%s`: %s", output_path, strerror(errno));
        free_symbols(&e.symbols);
        free(e.out.data);
        return false;
    }
    size_t written = fwrite(e.out.data ? e.out.data : "", 1, e.out.len, fp);
    if (written != e.out.len || fclose(fp) != 0) {
        ds_diag_error(diag, ast->span, "failed to write output file `%s`: %s", output_path, strerror(errno));
        free_symbols(&e.symbols);
        free(e.out.data);
        return false;
    }

    free_symbols(&e.symbols);
    free(e.out.data);
    return true;
}
