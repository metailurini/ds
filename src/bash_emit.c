#include "bash_internal.h"
#include "bash_helpers.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void emit_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_type_");
    buf_append_len(out, name.data, name.len);
}

static void emit_script_type_assignment(BashEmitter *e, DsStr name, DsScriptType type) {
    if (!e->needs_case_types) return;
    emit_type_var_name(&e->out, name);
    buf_append(&e->out, "=");
    const char *type_name = script_type_name(type);
    bash_single_quote(&e->out, type_name, strlen(type_name));
    buf_append(&e->out, "\n");
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
            emit_script_type_assignment(e, decl->name, decl->type);
        } else if (decl->kind == DS_SCRIPT_DECL_FLAG) {
            emit_var_name(&e->out, decl->name);
            buf_append(&e->out, "=false\n");
            emit_script_type_assignment(e, decl->name, decl->type);
        } else {
            emit_var_name(&e->out, decl->name);
            buf_append(&e->out, "=\n");
            emit_script_type_assignment(e, decl->name, decl->type);
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

static void emit_command_result_helpers(BashEmitter *e) {
    buf_append(&e->out, ds_bash_command_result_helpers_source());
}

static void emit_collection_helpers(BashEmitter *e) {
    buf_append(&e->out, ds_bash_collection_helpers_source());
}

static void emit_stdlib_helpers(BashEmitter *e) {
    buf_append(&e->out, ds_bash_stdlib_helpers_source());
    buf_append(&e->out, ds_bash_string_helpers_source());
}

static void emit_debug_helpers(BashEmitter *e) {
    buf_append(&e->out, ds_bash_debug_helpers_source());
}

static void emit_int_helpers(BashEmitter *e) {
    buf_append(&e->out, ds_bash_int_helpers_source());
}

static void emit_function_value_helpers(BashEmitter *e) {
    buf_append(&e->out, ds_bash_function_value_helpers_source());
}

static void emit_cleanup_helpers(BashEmitter *e) {
    buf_append(&e->out,
        "declare -a __ds_defer_EXIT=() __ds_defer_INT=() __ds_defer_TERM=()\n"
        "__ds_trap_EXIT=\n__ds_trap_INT=\n__ds_trap_TERM=\n__ds_cleanup_running=false\n"
        "__ds_fail() { local __ds_loc=$1 __ds_code=$2; echo \"$__ds_loc: error: command failed with exit $__ds_code\" >&2; if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then return \"$__ds_code\"; fi; exit \"$__ds_code\"; }\n"
        "__ds_control_fail() { local __ds_loc=$1; shift; local __ds_msg=\"$*\"; if [[ -n \"$__ds_msg\" ]]; then echo \"$__ds_loc: error: $__ds_msg\" >&2; else echo \"$__ds_loc: error: fail\" >&2; fi; if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then return 1; fi; exit 1; }\n"
        "__ds_control_exit() { local __ds_loc=$1; shift; if (( $# != 1 )); then echo \"$__ds_loc: error: `exit` expects exactly one integer code\" >&2; if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then return 1; fi; exit 1; fi; if [[ ! \"$1\" =~ ^[0-9]+$ ]] || (( $1 < 0 || $1 > 255 )); then echo \"$__ds_loc: error: `exit` code must be an integer from 0 to 255\" >&2; if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then return 1; fi; exit 1; fi; if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then return \"$1\"; fi; exit \"$1\"; }\n"
        "__ds_run_stack_lifo() { local __ds_arr=$1 __ds_i __ds_fn __ds_code __ds_status=0; eval \"__ds_i=\\${#${__ds_arr}[@]}\"; while ((__ds_i > 0)); do ((__ds_i--)); eval \"__ds_fn=\\${${__ds_arr}[__ds_i]}\"; set +e; \"$__ds_fn\"; __ds_code=$?; set -e; if (( __ds_code != 0 )); then __ds_status=$__ds_code; fi; done; return \"$__ds_status\"; }\n"
        "__ds_run_cleanup() { local __ds_rc=${1:-0} __ds_code; if $__ds_cleanup_running; then exit \"$__ds_rc\"; fi; __ds_cleanup_running=true; trap - EXIT INT TERM; if [[ -n \"${__ds_trap_EXIT:-}\" ]]; then set +e; \"$__ds_trap_EXIT\"; __ds_code=$?; set -e; (( __ds_code == 0 )) || __ds_rc=$__ds_code; fi; __ds_run_stack_lifo __ds_defer_EXIT || __ds_rc=$?; exit \"$__ds_rc\"; }\n"
        "__ds_run_signal() { local __ds_sig=$1 __ds_rc=$2 __ds_trap=__ds_trap_${__ds_sig} __ds_stack=__ds_defer_${__ds_sig} __ds_code; if $__ds_cleanup_running; then exit \"$__ds_rc\"; fi; __ds_cleanup_running=true; trap - EXIT INT TERM; local __ds_fn=\"${!__ds_trap:-}\"; if [[ -n \"$__ds_fn\" ]]; then set +e; \"$__ds_fn\"; __ds_code=$?; set -e; (( __ds_code == 0 )) || __ds_rc=$__ds_code; fi; __ds_run_stack_lifo \"$__ds_stack\" || __ds_rc=$?; if [[ -n \"${__ds_trap_EXIT:-}\" ]]; then set +e; \"$__ds_trap_EXIT\"; __ds_code=$?; set -e; (( __ds_code == 0 )) || __ds_rc=$__ds_code; fi; __ds_run_stack_lifo __ds_defer_EXIT || __ds_rc=$?; exit \"$__ds_rc\"; }\n"
        "trap '__ds_run_cleanup \"$?\"' EXIT\n"
        "trap '__ds_run_signal INT 130' INT\n"
        "trap '__ds_run_signal TERM 143' TERM\n\n");
}

bool ds_emit_bash_program(const DsSource *source, const DsLowerProgram *lowered, const char *output_path, DsDiag *diag) {
    BashEmitter e;
    memset(&e, 0, sizeof(e));
    e.source = source;
    e.diag = diag;
    e.needs_case_types = program_uses_case(lowered);

    buf_append(&e.out, "#!/usr/bin/env bash\n");
    buf_append(&e.out, "set -euo pipefail\n\n");

    bool needs_map_guard = program_uses_map_literal(lowered);
    bool needs_collection_helpers = program_uses_collection_index(lowered);
    bool needs_stdlib = program_uses_stdlib(lowered);
    bool needs_debug = program_has_command(lowered);
    bool needs_int_helpers = program_uses_int_helpers(lowered);
    bool needs_function_value_helpers = program_uses_function_value_helpers(lowered);
    bool needs_cleanup_helpers = program_uses_handlers(lowered);
    e.has_cleanup_helpers = needs_cleanup_helpers;
    if (needs_map_guard || needs_stdlib) {
        buf_append(&e.out, "if (( BASH_VERSINFO[0] < 4 )); then\n");
        if (needs_map_guard) buf_append(&e.out, "  echo \"${0##*/}: error: v0.10.0 maps require Bash 4 or newer\" >&2\n");
        else buf_append(&e.out, "  echo \"${0##*/}: error: v0.11.0 stdlib helpers require Bash 4 or newer\" >&2\n");
        buf_append(&e.out, "  exit 1\n");
        buf_append(&e.out, "fi\n\n");
    }

    for (size_t i = 0; i < lowered->script_decls.len; i++) {
        DsStr copy = {ds_str_dup_range(lowered->script_decls.items[i].name.data, lowered->script_decls.items[i].name.len), lowered->script_decls.items[i].name.len};
        symbol_vec_push(&e.symbols, copy);
    }
    for (size_t i = 0; i < lowered->statements.len; i++) {
        if (lowered->statements.items[i]->kind == DS_LOWER_STMT_LET && !symbol_exists(&e.symbols, lowered->statements.items[i]->as.let_stmt.name)) {
            DsStr copy = {ds_str_dup_range(lowered->statements.items[i]->as.let_stmt.name.data, lowered->statements.items[i]->as.let_stmt.name.len), lowered->statements.items[i]->as.let_stmt.name.len};
            symbol_vec_push(&e.symbols, copy);
        }
    }

    emit_script_args(&e, lowered);
    if (needs_int_helpers) emit_int_helpers(&e);
    if (needs_function_value_helpers) emit_function_value_helpers(&e);
    if (needs_debug) emit_debug_helpers(&e);
    if (needs_cleanup_helpers) emit_cleanup_helpers(&e);
    if (program_uses_run(lowered)) emit_command_result_helpers(&e);
    if (needs_collection_helpers) emit_collection_helpers(&e);
    if (needs_stdlib) emit_stdlib_helpers(&e);

    for (size_t i = 0; i < lowered->functions.len; i++) {
        if (!emit_function(&e, &lowered->functions.items[i])) {
            free_symbols(&e.symbols);
            free(e.out.data);
            return false;
        }
    }

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
