#include "lower_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

bool parse_i64(DsStr text, int64_t *out) {
    char *tmp = ds_str_dup_range(text.data, text.len);
    char *end = NULL;
    errno = 0;
    long long value = strtoll(tmp, &end, 10);
    bool ok = errno != ERANGE && end && *end == '\0';
    if (ok) *out = (int64_t)value;
    free(tmp);
    return ok;
}

bool lower_decode_string_text(DsStr text, DsStr *out) {
    out->data = NULL;
    out->len = 0;
    if (text.len >= 6 && memcmp(text.data, "\"\"\"", 3) == 0 && memcmp(text.data + text.len - 3, "\"\"\"", 3) == 0) {
        out->data = ds_str_dup_range(text.data + 3, text.len - 6);
        out->len = text.len - 6;
        return true;
    }
    if (text.len < 2 || text.data[0] != '"' || text.data[text.len - 1] != '"') return false;
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
    out->data = buf;
    out->len = len;
    return true;
}

SymKind script_type_to_sym(DsScriptType type) {
    switch (type) {
        case DS_SCRIPT_TYPE_STRING: return SYM_STRING;
        case DS_SCRIPT_TYPE_INT: return SYM_INT;
        case DS_SCRIPT_TYPE_BOOL: return SYM_BOOL;
    }
    return SYM_UNKNOWN;
}

const char *script_type_name(DsScriptType type) {
    switch (type) {
        case DS_SCRIPT_TYPE_STRING: return "string";
        case DS_SCRIPT_TYPE_INT: return "int";
        case DS_SCRIPT_TYPE_BOOL: return "bool";
    }
    return "unknown";
}

bool lower_script_decl(Lower *lower, const DsScriptDecl *decl, DsLowerProgram *program) {
    DsLowerScriptDecl out;
    memset(&out, 0, sizeof(out));
    out.kind = decl->kind;
    out.type = decl->type;
    out.name = str_clone(decl->name);
    out.span = decl->span;

    if (decl->kind == DS_SCRIPT_DECL_ARG && decl->type == DS_SCRIPT_TYPE_BOOL) {
        ds_diag_error(lower->diag, decl->span, "bool positional args are not supported in v0.5.0");
    }
    if (decl->kind == DS_SCRIPT_DECL_FLAG && decl->type != DS_SCRIPT_TYPE_BOOL) {
        ds_diag_error(lower->diag, decl->span, "flag `%.*s` must have type `bool`", (int)decl->name.len, decl->name.data);
    }

    if (decl->default_value) {
        out.has_default = true;
        switch (decl->type) {
            case DS_SCRIPT_TYPE_STRING:
                if (decl->default_value->kind != DS_EXPR_STRING) {
                    ds_diag_error(lower->diag, decl->default_value->span, "default for `%.*s` must be a string", (int)decl->name.len, decl->name.data);
                } else {
                    lower_decode_string_text(decl->default_value->as.text, &out.default_text);
                }
                break;
            case DS_SCRIPT_TYPE_INT:
                if (decl->default_value->kind != DS_EXPR_INT || !parse_i64(decl->default_value->as.text, &out.default_int)) {
                    ds_diag_error(lower->diag, decl->default_value->span, "default for `%.*s` must be an int", (int)decl->name.len, decl->name.data);
                }
                break;
            case DS_SCRIPT_TYPE_BOOL:
                if (decl->default_value->kind != DS_EXPR_BOOL) {
                    ds_diag_error(lower->diag, decl->default_value->span, "default for `%.*s` must be a bool", (int)decl->name.len, decl->name.data);
                } else {
                    out.default_bool = decl->default_value->as.boolean;
                    if (decl->kind == DS_SCRIPT_DECL_FLAG && out.default_bool) {
                        ds_diag_error(lower->diag, decl->default_value->span, "flag `%.*s` default `true` is deferred until `--no-name` support exists", (int)decl->name.len, decl->name.data);
                    }
                }
                break;
        }
    } else if (decl->kind != DS_SCRIPT_DECL_ARG) {
        ds_diag_error(lower->diag, decl->span, "%s `%.*s` requires a default value", decl->kind == DS_SCRIPT_DECL_OPTION ? "option" : "flag", (int)decl->name.len, decl->name.data);
    }

    scope_define(lower, lower->scope, decl->name, script_type_to_sym(decl->type), decl->span);
    lower_decl_vec_push(&program->script_decls, out);
    (void)script_type_name;
    return !lower->diag->has_error;
}
