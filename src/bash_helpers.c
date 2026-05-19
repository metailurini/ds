#include "bash_helpers.h"

const char *ds_bash_debug_helpers_source(void) {
    return
        "__ds_trace_cmd() {\n"
        "  [[ \"${DS_TRACE_CMD:-}\" == 1 ]] || return 0\n"
        "  local __ds_loc=$1\n"
        "  shift\n"
        "  printf 'trace: cmd %s:' \"$__ds_loc\" >&2\n"
        "  local __ds_arg\n"
        "  for __ds_arg in \"$@\"; do printf ' %q' \"$__ds_arg\" >&2; done\n"
        "  printf '\\n' >&2\n"
        "}\n"
        "__ds_fail() {\n"
        "  local __ds_loc=$1 __ds_code=$2\n"
        "  echo \"$__ds_loc: error: command failed with exit $__ds_code\" >&2\n"
        "  exit \"$__ds_code\"\n"
        "}\n\n";
}

const char *ds_bash_command_result_helpers_source(void) {
    return
        "__ds_error() { echo \"${0##*/}: error: $1\" >&2; exit 1; }\n"
        "__ds_capture() {\n"
        "  local __ds_prefix=$1\n"
        "  shift\n"
        "  local __ds_loc=$1\n"
        "  shift\n"
        "  local __ds_tmpdir\n"
        "  __ds_tmpdir=$(mktemp -d) || __ds_error 'failed to create command capture temp dir'\n"
        "  local __ds_stdout=\"$__ds_tmpdir/stdout\"\n"
        "  local __ds_stderr=\"$__ds_tmpdir/stderr\"\n"
        "  set +e\n"
        "  __ds_trace_cmd \"$__ds_loc\" \"$@\"\n"
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
        "}\n\n";
}

const char *ds_bash_collection_helpers_source(void) {
    return
        "__ds_error() { echo \"${0##*/}: error: $1\" >&2; exit 1; }\n"
        "__ds_array_get() {\n"
        "  local __ds_name=$1 __ds_index=$2 __ds_len\n"
        "  [[ \"$__ds_index\" =~ ^[0-9]+$ ]] || __ds_error \"array index $__ds_index is not an int\"\n"
        "  eval \"__ds_len=\\${#${__ds_name}[@]}\"\n"
        "  if (( __ds_index < 0 || __ds_index >= __ds_len )); then\n"
        "    __ds_error \"array index $__ds_index out of range\"\n"
        "  fi\n"
        "  eval \"printf '%s' \\\"\\${${__ds_name}[${__ds_index}]}\\\"\"\n"
        "}\n"
        "__ds_map_get() {\n"
        "  local __ds_name=$1 __ds_key=$2\n"
        "  eval \"[[ \\${${__ds_name}[\\$__ds_key]+__ds_set} == __ds_set ]]\" || __ds_error \"missing map key '$__ds_key'\"\n"
        "  eval \"printf '%s' \\\"\\${${__ds_name}[\\$__ds_key]}\\\"\"\n"
        "}\n\n";
}

const char *ds_bash_stdlib_helpers_source(void) {
    return
        "__ds_error() { echo \"${0##*/}: error: $1\" >&2; exit 1; }\n"
        "__ds_stdlib_file_exists() { [[ -e \"$1\" ]] && printf '%s' true || printf '%s' false; }\n"
        "__ds_stdlib_file_is_file() { [[ -f \"$1\" ]] && printf '%s' true || printf '%s' false; }\n"
        "__ds_stdlib_dir_exists() { [[ -d \"$1\" ]] && printf '%s' true || printf '%s' false; }\n"
        "__ds_stdlib_has_nul() { ! cmp -s <(LC_ALL=C tr -d '\\000' <\"$1\") \"$1\"; }\n"
        "__ds_stdlib_reject_nul() { __ds_stdlib_has_nul \"$1\" && __ds_error \"$2 '$1' contains embedded NUL bytes\" || true; }\n"
        "__ds_stdlib_file_read() { [[ -f \"$1\" ]] || __ds_error \"failed to read file '$1'\"; __ds_stdlib_reject_nul \"$1\" file; cat -- \"$1\"; }\n"
        "__ds_stdlib_file_write() { printf '%s' \"$2\" >\"$1\" || __ds_error \"failed to write file '$1'\"; }\n"
        "__ds_stdlib_file_append() { printf '%s' \"$2\" >>\"$1\" || __ds_error \"failed to append file '$1'\"; }\n"
        "__ds_stdlib_path_cwd() { pwd -P; }\n"
        "__ds_stdlib_path_join() { local out=\"$1\" part; shift; for part in \"$@\"; do out=\"${out%/}/${part#/}\"; done; printf '%s' \"$out\"; }\n"
        "__ds_stdlib_path_basename() { local p=\"$1\"; printf '%s' \"${p##*/}\"; }\n"
        "__ds_stdlib_path_dirname() { local p=\"$1\"; if [[ \"$p\" != */* ]]; then printf .; elif [[ \"${p%/*}\" == \"\" ]]; then printf /; else printf '%s' \"${p%/*}\"; fi; }\n"
        "__ds_stdlib_path_ext() { local b=\"${1##*/}\"; if [[ \"$b\" == .* || \"$b\" != *.* ]]; then printf ''; else printf '%s' \".${b##*.}\"; fi; }\n"
        "__ds_stdlib_cmd_found() { local c=\"$1\" d; if [[ \"$c\" == */* ]]; then [[ -x \"$c\" && ! -d \"$c\" ]] && return 0 || return 1; fi; IFS=: read -r -a __ds_path_parts <<<\"${PATH:-}\"; for d in \"${__ds_path_parts[@]}\"; do [[ -z \"$d\" ]] && d=.; [[ -x \"$d/$c\" && ! -d \"$d/$c\" ]] && return 0; done; return 1; }\n"
        "__ds_stdlib_cmd_exists() { __ds_stdlib_cmd_found \"$1\" && printf '%s' true || printf '%s' false; }\n"
        "__ds_stdlib_cmd_require() { __ds_stdlib_cmd_found \"$1\" || __ds_error \"required command '$1' was not found\"; }\n"
        "__ds_stdlib_env_valid() { [[ \"$1\" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || __ds_error \"invalid environment variable name '$1' in v0.11.0\"; }\n"
        "__ds_stdlib_env_get() { local n=\"$1\"; __ds_stdlib_env_valid \"$n\"; if [[ ${!n+x} ]]; then printf '%s' \"${!n}\"; elif [[ $# -ge 2 ]]; then printf '%s' \"$2\"; fi; }\n"
        "__ds_stdlib_env_set() { __ds_stdlib_env_valid \"$1\"; export \"$1=$2\"; }\n"
        "__ds_stdlib_env_unset() { __ds_stdlib_env_valid \"$1\"; unset \"$1\"; }\n"
        "__ds_stdlib_capture() { local __ds_var=\"$1\" __ds_data __ds_status; shift; set +e; __ds_data=\"$(\"$@\"; printf x)\"; __ds_status=$?; set -e; if (( __ds_status != 0 )); then exit \"$__ds_status\"; fi; __ds_data=\"${__ds_data%x}\"; printf -v \"$__ds_var\" '%s' \"$__ds_data\"; }\n"
        "__ds_stdlib_reject_recursive_glob() { [[ \"$1\" != *'**'* ]] || __ds_error \"recursive '**' glob patterns are deferred in v0.11.0\"; }\n"
        "__ds_stdlib_glob() { __ds_stdlib_reject_recursive_glob \"$1\"; { compgen -G \"$1\" || true; } | sort; }\n"
        "__ds_stdlib_glob_required() { local out; out=$(__ds_stdlib_glob \"$1\"); [[ -n \"$out\" ]] || __ds_error \"required glob '$1' had no matches\"; printf '%s\n' \"$out\"; }\n"
        "__ds_stdlib_lines() { [[ -f \"$1\" ]] || __ds_error \"failed to read lines from '$1'\"; __ds_stdlib_reject_nul \"$1\" \"lines from\"; while IFS= read -r line || [[ -n \"$line\" ]]; do printf '%s\n' \"$line\"; done <\"$1\"; }\n\n";
}

