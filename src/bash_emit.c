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

static const char *script_basename(const DsSource *source) {
    const char *path = source && source->path ? source->path : "<script>";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *script_type_name(DsScriptType type) {
    switch (type) {
        case DS_SCRIPT_TYPE_STRING: return "string";
        case DS_SCRIPT_TYPE_INT: return "int";
        case DS_SCRIPT_TYPE_BOOL: return "bool";
    }
    return "unknown";
}

static void bash_single_quote(EmitBuf *out, const char *data, size_t len) {
    buf_append(out, "'");
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\'') buf_append(out, "'\\''");
        else buf_append_len(out, &data[i], 1);
    }
    buf_append(out, "'");
}

static void emit_script_usage(BashEmitter *e, const DsLowerProgram *program) {
    buf_append(&e->out, "__ds_usage() {\n");
    buf_append(&e->out, "  cat <<'__DS_USAGE__'\n");
    buf_appendf(&e->out, "Usage: %s", script_basename(e->source));
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_ARG) buf_appendf(&e->out, " <%.*s>", (int)decl->name.len, decl->name.data);
    }
    bool has_options = false;
    for (size_t i = 0; i < program->script_decls.len; i++) if (program->script_decls.items[i].kind != DS_SCRIPT_DECL_ARG) has_options = true;
    if (has_options) buf_append(&e->out, " [options]");
    buf_append(&e->out, "\n");
    bool has_args = false;
    for (size_t i = 0; i < program->script_decls.len; i++) if (program->script_decls.items[i].kind == DS_SCRIPT_DECL_ARG) has_args = true;
    if (has_args) {
        buf_append(&e->out, "\nArguments:\n");
        for (size_t i = 0; i < program->script_decls.len; i++) {
            const DsLowerScriptDecl *decl = &program->script_decls.items[i];
            if (decl->kind == DS_SCRIPT_DECL_ARG) buf_appendf(&e->out, "  %.*s %s\n", (int)decl->name.len, decl->name.data, script_type_name(decl->type));
        }
    }
    buf_append(&e->out, "\nOptions:\n");
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_OPTION) {
            buf_appendf(&e->out, "  --%.*s %s    default: ", (int)decl->name.len, decl->name.data, script_type_name(decl->type));
            if (decl->type == DS_SCRIPT_TYPE_STRING) buf_append_len(&e->out, decl->default_text.data ? decl->default_text.data : "", decl->default_text.len);
            else if (decl->type == DS_SCRIPT_TYPE_INT) buf_appendf(&e->out, "%lld", (long long)decl->default_int);
            else buf_append(&e->out, decl->default_bool ? "true" : "false");
            buf_append(&e->out, "\n");
        } else if (decl->kind == DS_SCRIPT_DECL_FLAG) {
            buf_appendf(&e->out, "  --%.*s            boolean flag\n", (int)decl->name.len, decl->name.data);
        }
    }
    buf_append(&e->out, "  --help             show this help\n");
    buf_append(&e->out, "__DS_USAGE__\n");
    buf_append(&e->out, "}\n\n");
}

static void emit_script_args(BashEmitter *e, const DsLowerProgram *program) {
    if (!program->has_script) return;
    emit_script_usage(e, program);
    buf_append(&e->out, "__ds_error() { echo \"${0##*/}: error: $1\" >&2; exit 1; }\n");
    buf_append(&e->out, "__ds_parse_int() {\n");
    buf_append(&e->out, "  [[ \"$1\" =~ ^[+-]?[0-9]+$ ]] || return 1\n");
    buf_append(&e->out, "  [[ \"$1\" != \"+\" && \"$1\" != \"-\" ]] || return 1\n");
    buf_append(&e->out, "  local __ds_abs=\"$1\" __ds_limit=9223372036854775807\n");
    buf_append(&e->out, "  if [[ \"$__ds_abs\" == -* ]]; then __ds_abs=\"${__ds_abs#-}\"; __ds_limit=9223372036854775808; elif [[ \"$__ds_abs\" == +* ]]; then __ds_abs=\"${__ds_abs#+}\"; fi\n");
    buf_append(&e->out, "  while [[ ${#__ds_abs} -gt 1 && \"$__ds_abs\" == 0* ]]; do __ds_abs=\"${__ds_abs#0}\"; done\n");
    buf_append(&e->out, "  [[ ${#__ds_abs} -lt ${#__ds_limit} ]] && return 0\n");
    buf_append(&e->out, "  [[ ${#__ds_abs} -gt ${#__ds_limit} ]] && return 1\n");
    buf_append(&e->out, "  [[ \"$__ds_abs\" < \"$__ds_limit\" || \"$__ds_abs\" == \"$__ds_limit\" ]]\n");
    buf_append(&e->out, "}\n\n");
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_OPTION) {
            emit_var_name(&e->out, decl->name);
            buf_append(&e->out, "=");
            if (decl->type == DS_SCRIPT_TYPE_STRING) bash_single_quote(&e->out, decl->default_text.data ? decl->default_text.data : "", decl->default_text.len);
            else if (decl->type == DS_SCRIPT_TYPE_INT) buf_appendf(&e->out, "%lld", (long long)decl->default_int);
            else buf_append(&e->out, decl->default_bool ? "true" : "false");
            buf_append(&e->out, "\n");
        } else if (decl->kind == DS_SCRIPT_DECL_FLAG) {
            emit_var_name(&e->out, decl->name);
            buf_append(&e->out, "=false\n");
        } else {
            emit_var_name(&e->out, decl->name);
            buf_append(&e->out, "=\n");
        }
        if (decl->kind != DS_SCRIPT_DECL_ARG) {
            buf_append(&e->out, "__ds_seen_");
            buf_append_len(&e->out, decl->name.data, decl->name.len);
            buf_append(&e->out, "=false\n");
        }
    }
    buf_append(&e->out, "__ds_positionals=()\n");
    buf_append(&e->out, "while (($#)); do\n");
    buf_append(&e->out, "  case \"$1\" in\n");
    buf_append(&e->out, "    --help|-h) __ds_usage; exit 0 ;;\n");
    buf_append(&e->out, "    --) shift; while (($#)); do __ds_positionals+=(\"$1\"); shift; done; break ;;\n");
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_ARG) continue;
        buf_appendf(&e->out, "    --%.*s)\n", (int)decl->name.len, decl->name.data);
        buf_append(&e->out, "      $__ds_seen_");
        buf_append_len(&e->out, decl->name.data, decl->name.len);
        buf_append(&e->out, " && __ds_error 'duplicate option `--");
        buf_append_len(&e->out, decl->name.data, decl->name.len);
        buf_append(&e->out, "`'\n");
        buf_append(&e->out, "      __ds_seen_");
        buf_append_len(&e->out, decl->name.data, decl->name.len);
        buf_append(&e->out, "=true\n");
        if (decl->kind == DS_SCRIPT_DECL_FLAG) {
            emit_var_name(&e->out, decl->name);
            buf_append(&e->out, "=true\n");
        } else {
            buf_append(&e->out, "      shift\n");
            buf_append(&e->out, "      [[ $# -gt 0 && \"$1\" != --* ]] || __ds_error 'option `--");
            buf_append_len(&e->out, decl->name.data, decl->name.len);
            buf_append(&e->out, "` requires a value'\n");
            if (decl->type == DS_SCRIPT_TYPE_INT) {
                buf_append(&e->out, "      __ds_parse_int \"$1\" || __ds_error 'invalid int value `'\"$1\"'` for `");
                buf_append_len(&e->out, decl->name.data, decl->name.len);
                buf_append(&e->out, "`'\n");
            } else if (decl->type == DS_SCRIPT_TYPE_BOOL) {
                buf_append(&e->out, "      [[ \"$1\" == true || \"$1\" == false ]] || __ds_error 'invalid bool value `'\"$1\"'` for `");
                buf_append_len(&e->out, decl->name.data, decl->name.len);
                buf_append(&e->out, "`'\n");
            }
            emit_var_name(&e->out, decl->name);
            buf_append(&e->out, "=\"$1\"\n");
        }
        buf_append(&e->out, "      ;;\n");
    }
    buf_append(&e->out, "    --*) __ds_error 'unknown option `'\"$1\"'`' ;;\n");
    buf_append(&e->out, "    *) __ds_positionals+=(\"$1\") ;;\n");
    buf_append(&e->out, "  esac\n");
    buf_append(&e->out, "  shift\n");
    buf_append(&e->out, "done\n\n");

    size_t arg_index = 0;
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind != DS_SCRIPT_DECL_ARG) continue;
        buf_appendf(&e->out, "[[ ${#__ds_positionals[@]} -gt %zu ]] || __ds_error 'missing required argument `%.*s`'\n", arg_index, (int)decl->name.len, decl->name.data);
        if (decl->type == DS_SCRIPT_TYPE_INT) {
            buf_appendf(&e->out, "__ds_parse_int \"${__ds_positionals[%zu]}\" || __ds_error 'invalid int value `'\"${__ds_positionals[%zu]}\"'` for `%.*s`'\n", arg_index, arg_index, (int)decl->name.len, decl->name.data);
        }
        emit_var_name(&e->out, decl->name);
        buf_appendf(&e->out, "=\"${__ds_positionals[%zu]}\"\n", arg_index);
        arg_index++;
    }
    buf_appendf(&e->out, "[[ ${#__ds_positionals[@]} -eq %zu ]] || __ds_error 'unexpected extra positional argument `'\"${__ds_positionals[%zu]}\"'`'\n\n", arg_index, arg_index);
}

static bool decode_string_literal(DsDiag *diag, const DsLowerExpr *expr, char **out_data, size_t *out_len) {
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

static bool emit_interpolated_string(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
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
                if (j < len && (decoded[j] == '}' || decoded[j] == '.')) {
                    DsStr name = {decoded + start, j - start};
                    if (!symbol_exists(&e->symbols, name)) {
                        ds_diag_error(e->diag, expr->span, "unknown interpolation variable `%.*s`", (int)name.len, name.data);
                        free(decoded);
                        return false;
                    }
                    buf_append(out, "${__ds_");
                    buf_append_len(out, name.data, name.len);
                    if (decoded[j] == '.') {
                        size_t field_start = ++j;
                        if (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || decoded[j] == '_')) {
                            j++;
                            while (j < len && ((decoded[j] >= 'A' && decoded[j] <= 'Z') || (decoded[j] >= 'a' && decoded[j] <= 'z') || (decoded[j] >= '0' && decoded[j] <= '9') || decoded[j] == '_')) j++;
                        }
                        if (j >= len || decoded[j] != '}') {
                            ds_diag_error(e->diag, expr->span, "unsupported string interpolation; expected `{name}` or `{name.field}`");
                            free(decoded);
                            return false;
                        }
                        buf_append(out, "_");
                        buf_append_len(out, decoded + field_start, j - field_start);
                    }
                    buf_append(out, "}");
                    i = j;
                    continue;
                }
            }
            ds_diag_error(e->diag, expr->span, "unsupported string interpolation; expected `{name}` or `{name.field}`");
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

static bool emit_value_expr(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_STRING:
            return emit_interpolated_string(e, expr, out);
        case DS_LOWER_EXPR_INT:
            buf_append_len(out, expr->as.text.data, expr->as.text.len);
            return true;
        case DS_LOWER_EXPR_BOOL:
            buf_append(out, expr->as.boolean ? "true" : "false");
            return true;
        case DS_LOWER_EXPR_FIELD:
            if (expr->as.field.object->kind != DS_LOWER_EXPR_IDENT) {
                ds_diag_error(e->diag, expr->span, "unsupported command result field receiver for Bash emission");
                return false;
            }
            buf_append(out, "\"$");
            emit_var_name(out, expr->as.field.object->as.text);
            buf_append(out, "_");
            buf_append_len(out, expr->as.field.field.data, expr->as.field.field.len);
            buf_append(out, "\"");
            return true;
        default:
            ds_diag_error(e->diag, expr->span, "this expression cannot be emitted as a Bash assignment in v0.2.0");
            return false;
    }
}

static bool emit_condition_operand(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    switch (expr->kind) {
        case DS_LOWER_EXPR_IDENT:
            if (!symbol_exists(&e->symbols, expr->as.text)) {
                ds_diag_error(e->diag, expr->span, "unknown variable `%.*s`", (int)expr->as.text.len, expr->as.text.data);
                return false;
            }
            buf_append(out, "\"$");
            emit_var_name(out, expr->as.text);
            buf_append(out, "\"");
            return true;
        case DS_LOWER_EXPR_STRING:
            return emit_interpolated_string(e, expr, out);
        case DS_LOWER_EXPR_INT:
            buf_append_len(out, expr->as.text.data, expr->as.text.len);
            return true;
        case DS_LOWER_EXPR_BOOL:
            buf_append(out, expr->as.boolean ? "true" : "false");
            return true;
        case DS_LOWER_EXPR_FIELD:
            return emit_value_expr(e, expr, out);
        default:
            ds_diag_error(e->diag, expr->span, "unsupported condition operand for Bash emission");
            return false;
    }
}

static bool emit_condition(BashEmitter *e, const DsLowerExpr *expr, EmitBuf *out) {
    if (expr->kind == DS_LOWER_EXPR_IDENT) {
        if (!symbol_exists(&e->symbols, expr->as.text)) {
            ds_diag_error(e->diag, expr->span, "unknown variable `%.*s`", (int)expr->as.text.len, expr->as.text.data);
            return false;
        }
        buf_append(out, "[[ \"$");
        emit_var_name(out, expr->as.text);
        buf_append(out, "\" == true ]]");
        return true;
    }
    if (expr->kind == DS_LOWER_EXPR_FIELD) {
        if (str_eq(expr->as.field.field, "ok") || str_eq(expr->as.field.field, "failed")) {
            buf_append(out, "[[ ");
            emit_value_expr(e, expr, out);
            buf_append(out, " == true ]]");
            return true;
        }
        buf_append(out, "[[ -n ");
        emit_value_expr(e, expr, out);
        buf_append(out, " ]]");
        return true;
    }
    if (expr->kind == DS_LOWER_EXPR_BOOL) {
        buf_append(out, expr->as.boolean ? "true" : "false");
        return true;
    }
    if (expr->kind == DS_LOWER_EXPR_UNARY && str_eq(expr->as.unary.op, "!")) {
        buf_append(out, "! ");
        return emit_condition(e, expr->as.unary.right, out);
    }
    if (expr->kind == DS_LOWER_EXPR_BINARY) {
        const char *op = NULL;
        bool negate = false;
        if (str_eq(expr->as.binary.op, "==")) op = "==";
        else if (str_eq(expr->as.binary.op, "!=")) op = "!=";
        else if (str_eq(expr->as.binary.op, ">")) op = ">";
        else if (str_eq(expr->as.binary.op, ">=")) {
            op = "<";
            negate = true;
        } else if (str_eq(expr->as.binary.op, "<")) op = "<";
        else if (str_eq(expr->as.binary.op, "<=")) {
            op = ">";
            negate = true;
        }
        if (!op) {
            ds_diag_error(e->diag, expr->span, "operator `%.*s` cannot be emitted in a Bash condition in v0.2.0", (int)expr->as.binary.op.len, expr->as.binary.op.data);
            return false;
        }
        if (negate) buf_append(out, "! ");
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

static bool emit_command_word(BashEmitter *e, DsWord command_word, EmitBuf *out) {
    DsStr word = command_word.text;
    DsSpan span = command_word.span;
    if (word.len >= 2 && word.data[0] == '"' && word.data[word.len - 1] == '"') {
        DsLowerExpr fake = {.kind = DS_LOWER_EXPR_STRING, .span = span};
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
    for (size_t i = 1; i + 1 < word.len; i++) {
        if (word.data[i] == '.') {
            DsStr name = {word.data, i};
            DsStr field = {word.data + i + 1, word.len - i - 1};
            if (!symbol_exists(&e->symbols, name)) {
                ds_diag_error(e->diag, span, "unknown command variable `%.*s`", (int)name.len, name.data);
                return false;
            }
            buf_append(out, "\"$");
            emit_var_name(out, name);
            buf_append(out, "_");
            buf_append_len(out, field.data, field.len);
            buf_append(out, "\"");
            return true;
        }
    }
    buf_append_len(out, word.data, word.len);
    return true;
}

static bool emit_redirect(BashEmitter *e, const DsRedirect *redirect, EmitBuf *out, DsSpan span) {
    if (redirect->kind == DS_REDIRECT_NONE) return true;
    DsLowerExpr fake = {.kind = DS_LOWER_EXPR_STRING, .span = redirect->target_span};
    fake.as.text = redirect->target;
    switch (redirect->kind) {
        case DS_REDIRECT_OUT: buf_append(out, " > "); break;
        case DS_REDIRECT_OUT_APPEND: buf_append(out, " >> "); break;
        case DS_REDIRECT_ERR: buf_append(out, " 2> "); break;
        case DS_REDIRECT_ERR_APPEND: buf_append(out, " 2>> "); break;
        case DS_REDIRECT_ALL: buf_append(out, " > "); break;
        case DS_REDIRECT_ALL_APPEND: buf_append(out, " >> "); break;
        case DS_REDIRECT_NONE: break;
    }
    if (!emit_interpolated_string(e, &fake, out)) return false;
    if (redirect->kind == DS_REDIRECT_ALL || redirect->kind == DS_REDIRECT_ALL_APPEND) buf_append(out, " 2>&1");
    (void)span;
    return true;
}

static bool emit_capture_words(BashEmitter *e, const DsWordVec *words, EmitBuf *out, DsSpan span) {
    for (size_t i = 0; i < words->len; i++) {
        buf_append(out, " ");
        if (!emit_command_word(e, words->items[i], out)) return false;
    }
    (void)span;
    return true;
}

static void emit_command_result_helpers(BashEmitter *e) {
    buf_append(&e->out,
        "__ds_error() { echo \"${0##*/}: error: $1\" >&2; exit 1; }\n"
        "__ds_capture() {\n"
        "  local __ds_prefix=$1\n"
        "  shift\n"
        "  local __ds_tmpdir\n"
        "  __ds_tmpdir=$(mktemp -d) || __ds_error 'failed to create command capture temp dir'\n"
        "  local __ds_stdout=\"$__ds_tmpdir/stdout\"\n"
        "  local __ds_stderr=\"$__ds_tmpdir/stderr\"\n"
        "  set +e\n"
        "  \"$@\" >\"$__ds_stdout\" 2>\"$__ds_stderr\"\n"
        "  local __ds_code=$?\n"
        "  set -e\n"
        "  local __ds_data\n"
        "  __ds_data=$(cat \"$__ds_stdout\"; printf x)\n"
        "  printf -v \"${__ds_prefix}_stdout\" '%s' \"${__ds_data%x}\"\n"
        "  __ds_data=$(cat \"$__ds_stderr\"; printf x)\n"
        "  printf -v \"${__ds_prefix}_stderr\" '%s' \"${__ds_data%x}\"\n"
        "  printf -v \"${__ds_prefix}_code\" '%s' \"$__ds_code\"\n"
        "  if [[ $__ds_code -eq 0 ]]; then\n"
        "    printf -v \"${__ds_prefix}_ok\" '%s' true\n"
        "    printf -v \"${__ds_prefix}_failed\" '%s' false\n"
        "  else\n"
        "    printf -v \"${__ds_prefix}_ok\" '%s' false\n"
        "    printf -v \"${__ds_prefix}_failed\" '%s' true\n"
        "  fi\n"
        "  rm -rf \"$__ds_tmpdir\"\n"
        "}\n\n");
}

static bool expr_uses_run(const DsLowerExpr *expr) {
    if (!expr) return false;
    switch (expr->kind) {
        case DS_LOWER_EXPR_RUN: return true;
        case DS_LOWER_EXPR_FIELD: return expr_uses_run(expr->as.field.object);
        case DS_LOWER_EXPR_UNARY: return expr_uses_run(expr->as.unary.right);
        case DS_LOWER_EXPR_BINARY: return expr_uses_run(expr->as.binary.left) || expr_uses_run(expr->as.binary.right);
        default: return false;
    }
}

static bool stmt_uses_run(const DsLowerStmt *stmt) {
    switch (stmt->kind) {
        case DS_LOWER_STMT_LET: return expr_uses_run(stmt->as.let_stmt.value);
        case DS_LOWER_STMT_IF:
            return expr_uses_run(stmt->as.if_stmt.condition) || stmt_uses_run(stmt->as.if_stmt.then_branch) ||
                   (stmt->as.if_stmt.else_branch && stmt_uses_run(stmt->as.if_stmt.else_branch));
        case DS_LOWER_STMT_BLOCK:
            for (size_t i = 0; i < stmt->as.block_stmt.statements.len; i++) if (stmt_uses_run(stmt->as.block_stmt.statements.items[i])) return true;
            return false;
        case DS_LOWER_STMT_CMD: return false;
    }
    return false;
}

static bool program_uses_run(const DsLowerProgram *program) {
    for (size_t i = 0; i < program->statements.len; i++) if (stmt_uses_run(program->statements.items[i])) return true;
    return false;
}

static bool emit_stmt(BashEmitter *e, const DsLowerStmt *stmt, int indent);

static bool emit_block_body(BashEmitter *e, const DsLowerStmt *block, int indent) {
    for (size_t i = 0; i < block->as.block_stmt.statements.len; i++) {
        if (!emit_stmt(e, block->as.block_stmt.statements.items[i], indent)) return false;
    }
    return true;
}

static bool emit_stmt(BashEmitter *e, const DsLowerStmt *stmt, int indent) {
    emit_indent(&e->out, indent);
    const DsSource *stmt_source = stmt->span.source ? stmt->span.source : e->source;
    buf_appendf(&e->out, "# ds: %s:%d\n", stmt_source && stmt_source->path ? stmt_source->path : "<source>", stmt->span.start.line);

    switch (stmt->kind) {
        case DS_LOWER_STMT_LET:
            if (!is_safe_identifier(stmt->as.let_stmt.name)) {
                ds_diag_error(e->diag, stmt->span, "cannot emit unsafe Bash variable name `%.*s`", (int)stmt->as.let_stmt.name.len, stmt->as.let_stmt.name.data);
                return false;
            }
            emit_indent(&e->out, indent);
            if (stmt->as.let_stmt.value->kind == DS_LOWER_EXPR_RUN) {
                buf_append(&e->out, "__ds_capture ");
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                if (!emit_capture_words(e, &stmt->as.let_stmt.value->as.run.words, &e->out, stmt->span)) return false;
            } else {
                emit_var_name(&e->out, stmt->as.let_stmt.name);
                buf_append(&e->out, "=");
                if (!emit_value_expr(e, stmt->as.let_stmt.value, &e->out)) return false;
            }
            buf_append(&e->out, "\n\n");
            if (!symbol_exists(&e->symbols, stmt->as.let_stmt.name)) {
                DsStr copy = {ds_str_dup_range(stmt->as.let_stmt.name.data, stmt->as.let_stmt.name.len), stmt->as.let_stmt.name.len};
                symbol_vec_push(&e->symbols, copy);
            }
            return true;

        case DS_LOWER_STMT_CMD:
            emit_indent(&e->out, indent);
            for (size_t i = 0; i < stmt->as.cmd_stmt.words.len; i++) {
                if (i > 0) buf_append(&e->out, " ");
                if (!emit_command_word(e, stmt->as.cmd_stmt.words.items[i], &e->out)) return false;
            }
            if (!emit_redirect(e, &stmt->as.cmd_stmt.redirect, &e->out, stmt->span)) return false;
            buf_append(&e->out, "\n\n");
            return true;

        case DS_LOWER_STMT_IF:
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

        case DS_LOWER_STMT_BLOCK:
            return emit_block_body(e, stmt, indent);
    }
    return true;
}

bool ds_emit_bash_program(const DsSource *source, const DsLowerProgram *lowered, const char *output_path, DsDiag *diag) {
    BashEmitter e;
    memset(&e, 0, sizeof(e));
    e.source = source;
    e.diag = diag;

    buf_append(&e.out, "#!/usr/bin/env bash\n");
    buf_append(&e.out, "set -euo pipefail\n\n");

    for (size_t i = 0; i < lowered->script_decls.len; i++) {
        DsStr copy = {ds_str_dup_range(lowered->script_decls.items[i].name.data, lowered->script_decls.items[i].name.len), lowered->script_decls.items[i].name.len};
        symbol_vec_push(&e.symbols, copy);
    }

    emit_script_args(&e, lowered);
    if (program_uses_run(lowered)) emit_command_result_helpers(&e);

    for (size_t i = 0; i < lowered->statements.len; i++) {
        if (!emit_stmt(&e, lowered->statements.items[i], 0)) {
            free_symbols(&e.symbols);
            free(e.out.data);
            return false;
        }
    }

    FILE *fp = fopen(output_path, "wb");
    if (!fp) {
        DsSpan span = lowered->span;
        ds_diag_error(diag, span, "failed to open output file `%s`: %s", output_path, strerror(errno));
        free_symbols(&e.symbols);
        free(e.out.data);
        return false;
    }
    size_t written = fwrite(e.out.data ? e.out.data : "", 1, e.out.len, fp);
    if (written != e.out.len || fclose(fp) != 0) {
        ds_diag_error(diag, lowered->span, "failed to write output file `%s`: %s", output_path, strerror(errno));
        free_symbols(&e.symbols);
        free(e.out.data);
        return false;
    }

    free_symbols(&e.symbols);
    free(e.out.data);
    return true;
}

bool ds_emit_bash(const DsSource *source, const DsAst *ast, const char *output_path, DsDiag *diag) {
    DsLowerProgram *lowered = ds_lower_program(ast, diag);
    if (!lowered) return false;
    bool ok = ds_emit_bash_program(source, lowered, output_path, diag);
    ds_lower_program_free(lowered);
    return ok;
}
