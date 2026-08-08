#include "vm_internal.h"

static bool parse_runtime_bool(const char *text, bool *out) {
    if (strcmp(text, "true") == 0) { *out = true; return true; }
    if (strcmp(text, "false") == 0) { *out = false; return true; }
    return false;
}

static void print_script_help(const DsSource *source, const DsLowerProgram *program, FILE *out) {
    fprintf(out, "Usage: %s", ds_source_basename(source));
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_ARG) fprintf(out, " <%.*s>", (int)decl->name.len, decl->name.data);
    }
    bool has_options = false;
    for (size_t i = 0; i < program->script_decls.len; i++) if (program->script_decls.items[i].kind != DS_SCRIPT_DECL_ARG) has_options = true;
    if (has_options) fputs(" [options]", out);
    fputs("\n", out);

    bool has_args = false;
    for (size_t i = 0; i < program->script_decls.len; i++) if (program->script_decls.items[i].kind == DS_SCRIPT_DECL_ARG) has_args = true;
    if (has_args) {
        fputs("\nArguments:\n", out);
        for (size_t i = 0; i < program->script_decls.len; i++) {
            const DsLowerScriptDecl *decl = &program->script_decls.items[i];
            if (decl->kind == DS_SCRIPT_DECL_ARG) fprintf(out, "  %.*s %s\n", (int)decl->name.len, decl->name.data, ds_script_type_name(decl->type));
        }
    }

    fputs("\nOptions:\n", out);
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_OPTION) {
            fprintf(out, "  --%.*s %s    default: ", (int)decl->name.len, decl->name.data, ds_script_type_name(decl->type));
            if (decl->type == DS_SCRIPT_TYPE_STRING) ds_fprint_str(out, decl->default_text);
            else if (decl->type == DS_SCRIPT_TYPE_INT) fprintf(out, "%lld", (long long)decl->default_int);
            else fprintf(out, "%s", decl->default_bool ? "true" : "false");
            fputc('\n', out);
        } else if (decl->kind == DS_SCRIPT_DECL_FLAG) {
            fprintf(out, "  --%.*s            boolean flag\n", (int)decl->name.len, decl->name.data);
        }
    }
    fputs("  --help             show this help\n", out);
}

static int find_decl_by_option(const DsLowerProgram *program, const char *name) {
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind != DS_SCRIPT_DECL_ARG && ds_str_eq_cstr(decl->name, name)) return (int)i;
    }
    return -1;
}

static DsSpan script_error_span(const DsLowerProgram *program) {
    if (program && program->script_decls.len > 0) return program->script_decls.items[0].span;
    DsSpan span;
    memset(&span, 0, sizeof(span));
    span.start.line = 1;
    span.start.column = 1;
    span.end = span.start;
    return span;
}

static bool set_var_from_decl(Vm *vm, const DsLowerScriptDecl *decl, const char *text, DsSpan span) {
    DsValue value = ds_value_null();
    if (decl->type == DS_SCRIPT_TYPE_STRING) {
        DsString s;
        ds_string_from_cstr(&s, text ? text : "");
        value = ds_value_string_take(&s);
    } else if (decl->type == DS_SCRIPT_TYPE_INT) {
        int64_t parsed = 0;
        if (!ds_parse_i64_range(text, strlen(text), &parsed)) {
            ds_diag_error(vm->diag, span, "invalid int value `%s` for `%.*s`", text ? text : "", (int)decl->name.len, decl->name.data);
            return false;
        }
        value = ds_value_int(parsed);
    } else {
        bool parsed = false;
        if (!parse_runtime_bool(text, &parsed)) {
            ds_diag_error(vm->diag, span, "invalid bool value `%s` for `%.*s`", text ? text : "", (int)decl->name.len, decl->name.data);
            return false;
        }
        value = ds_value_bool(parsed);
    }
    ds_map_set(&vm->scope->vars, decl->name, value);
    return true;
}

static bool set_default_from_decl(Vm *vm, const DsLowerScriptDecl *decl) {
    DsValue value = ds_value_null();
    if (decl->type == DS_SCRIPT_TYPE_STRING) {
        DsString s;
        ds_string_from_range(&s, decl->default_text.data ? decl->default_text.data : "", decl->default_text.len);
        value = ds_value_string_take(&s);
    } else if (decl->type == DS_SCRIPT_TYPE_INT) {
        value = ds_value_int(decl->default_int);
    } else {
        value = ds_value_bool(decl->default_bool);
    }
    ds_map_set(&vm->scope->vars, decl->name, value);
    return true;
}

int bind_script_args(Vm *vm, const DsLowerProgram *program, int argc, char **argv) {
    if (!program->has_script) {
        if (argc > 0) {
            fprintf(stderr, "%s: error: unexpected script arguments\n", ds_source_basename(vm->source));
            return 1;
        }
        return 0;
    }

    bool *seen = (bool *)ds_xcalloc(program->script_decls.len, sizeof(bool));
    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind != DS_SCRIPT_DECL_ARG) set_default_from_decl(vm, decl);
    }

    size_t next_arg = 0;
    bool end_options = false;
    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];
        if (!end_options && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)) {
            print_script_help(vm->source, program, stdout);
            free(seen);
            return 2;
        }
        if (!end_options && strcmp(arg, "--") == 0) {
            end_options = true;
            continue;
        }
        if (!end_options && strncmp(arg, "--", 2) == 0) {
            int idx = find_decl_by_option(program, arg + 2);
            if (idx < 0) {
                ds_diag_error(vm->diag, script_error_span(program), "unknown option `%s`", arg);
                free(seen);
                return 1;
            }
            if (seen[idx]) {
                const DsLowerScriptDecl *decl = &program->script_decls.items[idx];
                ds_diag_error(vm->diag, decl->span, "duplicate option `%s`", arg);
                free(seen);
                return 1;
            }
            seen[idx] = true;
            const DsLowerScriptDecl *decl = &program->script_decls.items[idx];
            if (decl->kind == DS_SCRIPT_DECL_FLAG) {
                ds_map_set(&vm->scope->vars, decl->name, ds_value_bool(true));
            } else {
                if (i + 1 >= argc || strncmp(argv[i + 1], "--", 2) == 0) {
                    ds_diag_error(vm->diag, decl->span, "option `%s` requires a value", arg);
                    free(seen);
                    return 1;
                }
                i++;
                if (!set_var_from_decl(vm, decl, argv[i], decl->span)) {
                    free(seen);
                    return 1;
                }
            }
            continue;
        }
        while (next_arg < program->script_decls.len && program->script_decls.items[next_arg].kind != DS_SCRIPT_DECL_ARG) next_arg++;
        if (next_arg >= program->script_decls.len) {
            ds_diag_error(vm->diag, script_error_span(program), "unexpected extra positional argument `%s`", arg);
            free(seen);
            return 1;
        }
        if (!set_var_from_decl(vm, &program->script_decls.items[next_arg], arg, program->script_decls.items[next_arg].span)) {
            free(seen);
            return 1;
        }
        seen[next_arg] = true;
        next_arg++;
    }

    for (size_t i = 0; i < program->script_decls.len; i++) {
        const DsLowerScriptDecl *decl = &program->script_decls.items[i];
        if (decl->kind == DS_SCRIPT_DECL_ARG && !seen[i]) {
            ds_diag_error(vm->diag, decl->span, "missing required argument `%.*s`", (int)decl->name.len, decl->name.data);
            free(seen);
            return 1;
        }
    }
    free(seen);
    return 0;
}
