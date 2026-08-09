#include "bash_internal.h"
#include "bash_helpers.h"
#include "ds_signal.h"

#include <errno.h>

static void emit_type_var_name(EmitBuf *out, DsStr name) {
    buf_append(out, "__ds_type_");
    buf_append_dsstr(out, name);
}

static void emit_script_type_assignment(BashEmitter *e, DsStr name, DsScriptType type) {
    if (!e->needs_case_types) return;
    emit_type_var_name(&e->out, name);
    buf_append(&e->out, "=");
    const char *type_name = ds_script_type_name(type);
    bash_single_quote(&e->out, type_name, strlen(type_name));
    buf_append(&e->out, "\n");
}

static void emit_script_usage(BashEmitter *e, const DsLowerProgram *program) {
    buf_append(&e->out, "__ds_usage() {\n");
    buf_append(&e->out, "  cat <<'__DS_USAGE__'\n");
    DsStr help = ds_lower_program_script_help(e->source, program);
    buf_append_dsstr(&e->out, help);
    free(help.data);
    buf_append(&e->out, "__DS_USAGE__\n");
    buf_append(&e->out, "}\n\n");
}

static void emit_script_args(BashEmitter *e, const DsLowerProgram *program) {
    if (!program->has_script) return;
    emit_script_usage(e, program);
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
            if (decl->type == DS_SCRIPT_TYPE_STRING) bash_single_quote(&e->out, ds_str_data(decl->default_text), decl->default_text.len);
            else if (decl->type == DS_SCRIPT_TYPE_INT) ds_string_appendf(&e->out, "%lld", (long long)decl->default_int);
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
            buf_append_dsstr(&e->out, decl->name);
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
        ds_string_appendf(&e->out, "    --%.*s)\n", (int)decl->name.len, decl->name.data);
        buf_append(&e->out, "      $__ds_seen_");
        buf_append_dsstr(&e->out, decl->name);
        buf_append(&e->out, " && __ds_error 'duplicate option `--");
        buf_append_dsstr(&e->out, decl->name);
        buf_append(&e->out, "`'\n");
        buf_append(&e->out, "      __ds_seen_");
        buf_append_dsstr(&e->out, decl->name);
        buf_append(&e->out, "=true\n");
        if (decl->kind == DS_SCRIPT_DECL_FLAG) {
            emit_var_name(&e->out, decl->name);
            buf_append(&e->out, "=true\n");
        } else {
            buf_append(&e->out, "      shift\n");
            buf_append(&e->out, "      [[ $# -gt 0 && \"$1\" != --* ]] || __ds_error 'option `--");
            buf_append_dsstr(&e->out, decl->name);
            buf_append(&e->out, "` requires a value'\n");
            if (decl->type == DS_SCRIPT_TYPE_INT) {
                buf_append(&e->out, "      __ds_parse_int \"$1\" || __ds_error 'invalid int value `'\"$1\"'` for `");
                buf_append_dsstr(&e->out, decl->name);
                buf_append(&e->out, "`'\n");
            } else if (decl->type == DS_SCRIPT_TYPE_BOOL) {
                buf_append(&e->out, "      [[ \"$1\" == true || \"$1\" == false ]] || __ds_error 'invalid bool value `'\"$1\"'` for `");
                buf_append_dsstr(&e->out, decl->name);
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
        ds_string_appendf(&e->out, "[[ ${#__ds_positionals[@]} -gt %zu ]] || __ds_error 'missing required argument `%.*s`'\n", arg_index, (int)decl->name.len, decl->name.data);
        if (decl->type == DS_SCRIPT_TYPE_INT) {
            ds_string_appendf(&e->out, "__ds_parse_int \"${__ds_positionals[%zu]}\" || __ds_error 'invalid int value `'\"${__ds_positionals[%zu]}\"'` for `%.*s`'\n", arg_index, arg_index, (int)decl->name.len, decl->name.data);
        }
        emit_var_name(&e->out, decl->name);
        ds_string_appendf(&e->out, "=\"${__ds_positionals[%zu]}\"\n", arg_index);
        arg_index++;
    }
    ds_string_appendf(&e->out, "[[ ${#__ds_positionals[@]} -eq %zu ]] || __ds_error 'unexpected extra positional argument `'\"${__ds_positionals[%zu]}\"'`'\n\n", arg_index, arg_index);
}

static void emit_helper_source(BashEmitter *e, const char *source) {
    buf_append(&e->out, source);
}

static void emit_plain_temp_cleanup_trap(BashEmitter *e) {
    buf_append(&e->out, "trap '__ds_rc=$?; __ds_temp_cleanup; exit \"$__ds_rc\"' EXIT\n\n");
}

static void emit_regex_helpers(BashEmitter *e, bool needs_match, bool needs_replace) {
    buf_append(&e->out, ds_bash_regex_helpers_source());
    if (needs_match) buf_append(&e->out, ds_bash_regex_match_helpers_source());
    if (needs_replace) buf_append(&e->out, ds_bash_regex_replace_helpers_source());
}

static void emit_function_value_helpers(BashEmitter *e) {
    buf_append(&e->out, ds_bash_function_value_capture_helpers_source());
    buf_append(&e->out, ds_bash_function_value_materialize_helpers_source());
}

static void emit_error_helper(BashEmitter *e) {
    buf_append(&e->out, "__ds_error() { echo \"${0##*/}: error: $1\" >&2; exit 1; }\n\n");
}

static void emit_pipe_helpers(BashEmitter *e) {
    buf_append(&e->out, "__ds_stdout_is_pipe_like() { [[ -p /dev/stdout || -S /dev/stdout ]]; }\n");
    buf_append(&e->out, "__ds_is_quiet_broken_pipe() { local __ds_code=$1 __ds_allow=${2:-0}; (( __ds_allow == 1 && __ds_code == 141 )) && __ds_stdout_is_pipe_like; }\n");
}

static void emit_plain_command_fail_helper(BashEmitter *e) {
    emit_pipe_helpers(e);
    buf_append(&e->out, "__ds_fail() {\n");
    buf_append(&e->out, "  local __ds_loc=$1 __ds_code=$2 __ds_allow=${3:-0}\n");
    buf_append(&e->out, "  if __ds_is_quiet_broken_pipe \"$__ds_code\" \"$__ds_allow\"; then exit 0; fi\n");
    buf_append(&e->out, "  echo \"$__ds_loc: error: command failed with exit $__ds_code\" >&2\n");
    buf_append(&e->out, "  exit \"$__ds_code\"\n");
    buf_append(&e->out, "}\n\n");
}

static void emit_plain_control_helpers(BashEmitter *e) {
    buf_append(&e->out, "__ds_control_fail() { local __ds_loc=$1; shift; local __ds_msg=\"$*\"; if [[ -n \"$__ds_msg\" ]]; then echo \"$__ds_loc: error: $__ds_msg\" >&2; else echo \"$__ds_loc: error: fail\" >&2; fi; exit 1; }\n");
    buf_append(&e->out, "__ds_control_exit() { local __ds_loc=$1; shift; if (( $# != 1 )); then echo \"$__ds_loc: error: `exit` expects exactly one integer code\" >&2; exit 1; fi; if [[ ! \"$1\" =~ ^[0-9]+$ ]] || (( $1 < 0 || $1 > 255 )); then echo \"$__ds_loc: error: `exit` code must be an integer from 0 to 255\" >&2; exit 1; fi; exit \"$1\"; }\n\n");
}

static void emit_cleanup_helpers(BashEmitter *e) {
    const char *int_name = ds_handler_signal_name(DS_HANDLER_INT);
    const char *term_name = ds_handler_signal_name(DS_HANDLER_TERM);
    int int_status = ds_handler_signal_default_status(DS_HANDLER_INT);
    int term_status = ds_handler_signal_default_status(DS_HANDLER_TERM);
    /*
     * Trap/defer/signal parity boundary: emitted Bash consumes accepted HIR
     * handlers only. These helpers implement the same cleanup order as the VM:
     * signal trap, matching signal defers in LIFO order, then EXIT cleanup.
     */
    buf_append(&e->out, "declare -a __ds_defer_EXIT=() __ds_defer_INT=() __ds_defer_TERM=()\n");
    buf_append(&e->out, "__ds_trap_EXIT=\n__ds_trap_INT=\n__ds_trap_TERM=\n__ds_cleanup_running=false\n__ds_handler_exit_requested=false\n__ds_stack_exit_requested=false\n__ds_stack_status=0\n__ds_foreground_pid=\n");
    emit_pipe_helpers(e);
    buf_append(&e->out, "__ds_fail() { local __ds_loc=$1 __ds_code=$2 __ds_allow=${3:-0}; if __ds_is_quiet_broken_pipe \"$__ds_code\" \"$__ds_allow\"; then if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then return 0; fi; exit 0; fi; echo \"$__ds_loc: error: command failed with exit $__ds_code\" >&2; if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then return \"$__ds_code\"; fi; exit \"$__ds_code\"; }\n");
    buf_append(&e->out, "__ds_control_fail() { local __ds_loc=$1; shift; local __ds_msg=\"$*\"; if [[ -n \"$__ds_msg\" ]]; then echo \"$__ds_loc: error: $__ds_msg\" >&2; else echo \"$__ds_loc: error: fail\" >&2; fi; if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then return 1; fi; exit 1; }\n");
    buf_append(&e->out, "__ds_control_exit() { local __ds_loc=$1; shift; if (( $# != 1 )); then echo \"$__ds_loc: error: `exit` expects exactly one integer code\" >&2; if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then return 1; fi; exit 1; fi; if [[ ! \"$1\" =~ ^[0-9]+$ ]] || (( $1 < 0 || $1 > 255 )); then echo \"$__ds_loc: error: `exit` code must be an integer from 0 to 255\" >&2; if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then return 1; fi; exit 1; fi; if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then __ds_handler_exit_requested=true; return \"$1\"; fi; exit \"$1\"; }\n");
    buf_append(&e->out, "__ds_run_stack_lifo() { local __ds_arr=$1 __ds_cleanup_stack_index __ds_cleanup_stack_fn __ds_code __ds_status=0; __ds_stack_exit_requested=false; __ds_stack_status=0; eval \"__ds_cleanup_stack_index=\\${#${__ds_arr}[@]}\"; while ((__ds_cleanup_stack_index > 0)); do __ds_cleanup_stack_index=$((__ds_cleanup_stack_index - 1)); eval \"__ds_cleanup_stack_fn=\\${${__ds_arr}[__ds_cleanup_stack_index]}\"; __ds_handler_exit_requested=false; set +e; \"$__ds_cleanup_stack_fn\"; __ds_code=$?; set -e; if [[ \"$__ds_handler_exit_requested\" == true ]]; then __ds_stack_exit_requested=true; __ds_status=$__ds_code; elif (( __ds_code != 0 )); then __ds_status=$__ds_code; fi; done; __ds_stack_status=$__ds_status; return \"$__ds_status\"; }\n");
    buf_append(&e->out, "__ds_run_cleanup() { local __ds_rc=${1:-0} __ds_code; if $__ds_cleanup_running; then __ds_temp_cleanup; exit \"$__ds_rc\"; fi; __ds_cleanup_running=true; trap - EXIT INT TERM; if [[ -n \"${__ds_trap_EXIT:-}\" ]]; then __ds_handler_exit_requested=false; set +e; \"$__ds_trap_EXIT\"; __ds_code=$?; set -e; if [[ \"$__ds_handler_exit_requested\" == true ]] || (( __ds_code != 0 )); then __ds_rc=$__ds_code; fi; fi; __ds_run_stack_lifo __ds_defer_EXIT; __ds_code=$?; if [[ \"$__ds_stack_exit_requested\" == true ]] || (( __ds_code != 0 )); then __ds_rc=$__ds_stack_status; fi; __ds_temp_cleanup; exit \"$__ds_rc\"; }\n");
    buf_append(&e->out, "__ds_run_signal() { local __ds_sig=$1 __ds_rc=$2 __ds_trap __ds_stack __ds_code; if [[ -n \"${__ds_foreground_pid:-}\" ]]; then kill -\"$__ds_sig\" \"-$__ds_foreground_pid\" 2>/dev/null || kill -\"$__ds_sig\" \"$__ds_foreground_pid\" 2>/dev/null || true; fi; __ds_trap=__ds_trap_${__ds_sig}; __ds_stack=__ds_defer_${__ds_sig}; if $__ds_cleanup_running; then __ds_temp_cleanup; exit \"$__ds_rc\"; fi; __ds_cleanup_running=true; trap - EXIT INT TERM; local __ds_fn=\"${!__ds_trap:-}\"; if [[ -n \"$__ds_fn\" ]]; then __ds_handler_exit_requested=false; set +e; \"$__ds_fn\"; __ds_code=$?; set -e; if [[ \"$__ds_handler_exit_requested\" == true ]] || (( __ds_code != 0 )); then __ds_rc=$__ds_code; fi; fi; __ds_run_stack_lifo \"$__ds_stack\"; __ds_code=$?; if [[ \"$__ds_stack_exit_requested\" == true ]] || (( __ds_code != 0 )); then __ds_rc=$__ds_stack_status; fi; if [[ -n \"${__ds_trap_EXIT:-}\" ]]; then __ds_handler_exit_requested=false; set +e; \"$__ds_trap_EXIT\"; __ds_code=$?; set -e; if [[ \"$__ds_handler_exit_requested\" == true ]] || (( __ds_code != 0 )); then __ds_rc=$__ds_code; fi; fi; __ds_run_stack_lifo __ds_defer_EXIT; __ds_code=$?; if [[ \"$__ds_stack_exit_requested\" == true ]] || (( __ds_code != 0 )); then __ds_rc=$__ds_stack_status; fi; __ds_temp_cleanup; exit \"$__ds_rc\"; }\n");
    /*
     * Direct command/pipeline helpers are Bash runtime mechanics for accepted
     * HIR. They preserve the VM cleanup contract: classify INT/TERM foreground
     * exits as signal cleanup, keep conventional statuses from the shared
     * signal contract unless a handler overrides them, and avoid reporting
     * generic command/pipeline failures for signal-triggered cleanup.
     */
    ds_string_appendf(&e->out, "__ds_run_direct_command() { local __ds_loc=$1 __ds_allow_quiet=$2; shift 2; local __ds_pid __ds_code; set +e; (trap - %s %s; exec \"$@\") & __ds_pid=$!; __ds_foreground_pid=$__ds_pid; trap '' %s %s; wait \"$__ds_pid\"; __ds_code=$?; __ds_foreground_pid=; trap '__ds_run_signal %s %d' %s; trap '__ds_run_signal %s %d' %s; set -e; if (( __ds_code == %d )); then __ds_run_signal %s %d; fi; if (( __ds_code == %d )); then __ds_run_signal %s %d; fi; if (( __ds_code != 0 )); then set +e; __ds_fail \"$__ds_loc\" \"$__ds_code\" \"$__ds_allow_quiet\"; __ds_code=$?; return \"$__ds_code\"; fi; return 0; }\n",
                int_name, term_name, int_name, term_name,
                int_name, int_status, int_name, term_name, term_status, term_name,
                int_status, int_name, int_status, term_status, term_name, term_status);
    buf_append(&e->out, "if command -v setsid >/dev/null 2>&1; then __ds_setsid() { setsid \"$@\"; }; else __ds_setsid() { python3 -c 'import os,sys; os.setsid(); os.execvp(sys.argv[1],sys.argv[1:])' \"$@\"; }; fi\n");
    ds_string_appendf(&e->out, "__ds_run_pipeline() { local __ds_loc=$1 __ds_allow_quiet=$2 __ds_pipeline=$3 __ds_pid __ds_code; set +e; __ds_setsid bash -c '__ds_child_int(){ kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; exit %d; }; __ds_child_term(){ kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; exit %d; }; trap __ds_child_int %s; trap __ds_child_term %s; eval \"$1\" & wait \"$!\"' bash \"$__ds_pipeline\" & __ds_pid=$!; __ds_foreground_pid=$__ds_pid; trap '__ds_run_signal %s %d' %s; trap '__ds_run_signal %s %d' %s; wait \"$__ds_pid\"; __ds_code=$?; __ds_foreground_pid=; trap '__ds_run_signal %s %d' %s; trap '__ds_run_signal %s %d' %s; set -e; if (( __ds_code == %d )); then __ds_run_signal %s %d; fi; if (( __ds_code == %d )); then __ds_run_signal %s %d; fi; if __ds_is_quiet_broken_pipe \"$__ds_code\" \"$__ds_allow_quiet\"; then if [[ \"${__ds_cleanup_running:-false}\" == true ]]; then return 0; fi; exit 0; fi; if (( __ds_code != 0 )); then printf '%%s: error: pipeline failed with exit %%s\\n' \"$__ds_loc\" \"$__ds_code\" >&2; return \"$__ds_code\"; fi; return 0; }\n",
                int_status, term_status, int_name, term_name,
                int_name, int_status, int_name, term_name, term_status, term_name,
                int_name, int_status, int_name, term_name, term_status, term_name,
                int_status, int_name, int_status, term_status, term_name, term_status);
    buf_append(&e->out, "trap '__ds_run_cleanup \"$?\"' EXIT\n");
    ds_string_appendf(&e->out, "trap '__ds_run_signal %s %d' %s\n", int_name, int_status, int_name);
    ds_string_appendf(&e->out, "trap '__ds_run_signal %s %d' %s\n\n", term_name, term_status, term_name);
}

bool ds_emit_bash_program(const DsSource *source, const DsLowerProgram *lowered, const char *output_path, DsDiag *diag) {
    BashEmitter e;
    memset(&e, 0, sizeof(e));
    e.source = source;
    e.diag = diag;
    bool ok = false;
    e.needs_case_types = program_uses_case(lowered) || program_uses_membership(lowered) || program_uses_function_param_types(lowered);

    buf_append(&e.out, "#!/usr/bin/env bash\n");
    buf_append(&e.out, "set -euo pipefail\n\n");

    bool needs_map_iteration = program_uses_map_iteration(lowered);
    bool needs_map_assignment = program_uses_map_assignment(lowered);
    bool needs_map_guard = program_uses_map_literal(lowered) || needs_map_iteration || needs_map_assignment;
    bool needs_collection_helpers = program_uses_collection_index(lowered) || needs_map_iteration;
    bool needs_array_helpers = program_uses_array_helpers(lowered);
    bool needs_map_helpers = program_uses_map_helpers(lowered) || needs_map_iteration;
    bool needs_dynamic_index_helper = needs_array_helpers && needs_map_helpers && needs_collection_helpers;
    bool needs_stdlib = program_uses_stdlib(lowered);
    bool needs_stdlib_capture = program_uses_stdlib_capture(lowered);
    unsigned string_helper_mask = program_string_helper_mask(lowered);
    bool needs_string_array_capture = (string_helper_mask & DS_BASH_STRING_HELPER_SPLIT) != 0;
    bool needs_glob_helpers = program_uses_glob_helpers(lowered);
    bool needs_recursive_glob_helpers = program_uses_recursive_glob_helpers(lowered);
    bool needs_regex_helpers = program_uses_regex_base_helpers(lowered);
    bool needs_regex_match_helpers = program_uses_regex_match_helpers(lowered);
    bool needs_regex_replace_helpers = program_uses_regex_replace_helpers(lowered);
    bool needs_debug = program_has_command(lowered);
    bool needs_int_helpers = program_uses_int_helpers(lowered) || program_uses_function_value_helpers(lowered);
    bool needs_function_value_helpers = program_uses_function_value_helpers(lowered);
    bool needs_cleanup_helpers = program_uses_handlers(lowered);
    bool needs_signal_handlers = program_uses_signal_handlers(lowered);
    bool needs_control_helpers = program_uses_control_commands(lowered);
    bool needs_temp_helpers = needs_function_value_helpers || program_uses_run(lowered) ||
                              needs_collection_helpers || needs_stdlib || needs_glob_helpers ||
                              needs_string_array_capture || program_uses_membership(lowered) || needs_cleanup_helpers;
    unsigned string_helpers_needing_error = DS_BASH_STRING_HELPER_REPLACE |
                                            DS_BASH_STRING_HELPER_SPLIT |
                                            DS_BASH_STRING_HELPER_CHAR_AT |
                                            DS_BASH_STRING_HELPER_SLICE;
    bool needs_error_helper = lowered->has_script || needs_int_helpers || needs_function_value_helpers ||
                              program_uses_run(lowered) || needs_collection_helpers || needs_stdlib ||
                              needs_glob_helpers || needs_regex_helpers ||
                              needs_temp_helpers ||
                              ((string_helper_mask & string_helpers_needing_error) != 0);
    e.has_cleanup_helpers = needs_cleanup_helpers;
    e.has_signal_handlers = needs_signal_handlers;
    if (needs_map_guard) {
        buf_append(&e.out, "if (( BASH_VERSINFO[0] < 4 )); then\n");
        buf_append(&e.out, "  echo \"${0##*/}: error: v0.10.0 maps require Bash 4 or newer\" >&2\n");
        buf_append(&e.out, "  exit 1\n");
        buf_append(&e.out, "fi\n\n");
    }

    for (size_t i = 0; i < lowered->script_decls.len; i++) {
        bash_register_symbol(&e, lowered->script_decls.items[i].name);
    }
    for (size_t i = 0; i < lowered->statements.len; i++) {
        if (lowered->statements.items[i]->kind == DS_LOWER_STMT_LET && !symbol_exists(&e.symbols, lowered->statements.items[i]->as.let_stmt.name)) {
            bash_register_symbol(&e, lowered->statements.items[i]->as.let_stmt.name);
        }
    }

    if (needs_error_helper) emit_error_helper(&e);
    emit_script_args(&e, lowered);
    if (needs_temp_helpers) emit_helper_source(&e, ds_bash_temp_helpers_source());
    if (needs_int_helpers) emit_helper_source(&e, ds_bash_int_helpers_source());
    if (needs_function_value_helpers) emit_function_value_helpers(&e);
    if (needs_debug) emit_helper_source(&e, ds_bash_debug_helpers_source());
    if (needs_cleanup_helpers) emit_cleanup_helpers(&e);
    else {
        if (needs_temp_helpers) emit_plain_temp_cleanup_trap(&e);
        if (needs_debug) emit_plain_command_fail_helper(&e);
        if (needs_control_helpers) emit_plain_control_helpers(&e);
    }
    if (program_uses_run(lowered)) emit_helper_source(&e, ds_bash_command_result_helpers_source());
    if (needs_array_helpers) emit_helper_source(&e, ds_bash_array_helpers_source());
    if (needs_map_helpers) emit_helper_source(&e, ds_bash_map_helpers_source());
    if (needs_dynamic_index_helper) emit_helper_source(&e, ds_bash_dynamic_index_helper_source());
    if (needs_stdlib) emit_helper_source(&e, ds_bash_stdlib_helpers_source());
    else if (needs_stdlib_capture) emit_helper_source(&e, ds_bash_stdlib_capture_helper_source());
    if (string_helper_mask) emit_helper_source(&e, ds_bash_string_helpers_source(string_helper_mask));
    if (needs_glob_helpers) {
        emit_helper_source(&e, needs_recursive_glob_helpers ? ds_bash_recursive_glob_helpers_source() : ds_bash_glob_helpers_source());
    }
    if (needs_regex_helpers) emit_regex_helpers(&e, needs_regex_match_helpers, needs_regex_replace_helpers);

    for (size_t i = 0; i < lowered->functions.len; i++) {
        if (!emit_function(&e, &lowered->functions.items[i])) goto cleanup;
    }

    for (size_t i = 0; i < lowered->statements.len; i++) {
        if (!emit_stmt(&e, lowered->statements.items[i], 0)) goto cleanup;
    }

    FILE *fp = fopen(output_path, "wb");
    if (!fp) {
        DsSpan span = lowered->span;
        ds_diag_error(diag, span, "failed to open output file `%s`: %s", output_path, strerror(errno));
        goto cleanup;
    }
    size_t written = fwrite(emit_buf_data(&e.out), 1, e.out.len, fp);
    int close_rc = fclose(fp);
    if (written != e.out.len || close_rc != 0) {
        ds_diag_error(diag, lowered->span, "failed to write output file `%s`: %s", output_path, strerror(errno));
        goto cleanup;
    }

    ok = true;
cleanup:
    free_symbols(&e.symbols);
    free(e.out.data);
    return ok;
}
