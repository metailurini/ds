#include "bash_helpers.h"

const char *ds_bash_debug_helpers_source(void) {
    return
        "__ds_trace_quote() {\n"
        "  local __ds_q=$1\n"
        "  __ds_q=${__ds_q//\\\\/\\\\\\\\}\n"
        "  __ds_q=${__ds_q//\\\"/\\\\\\\"}\n"
        "  printf '\"%s\"' \"$__ds_q\"\n"
        "}\n"
        "__ds_trace_cmd() {\n"
        "  [[ \"${DS_TRACE_CMD:-}\" == 1 ]] || return 0\n"
        "  local __ds_loc=$1\n"
        "  shift\n"
        "  printf 'trace: cmd %s:' \"$__ds_loc\" >&2\n"
        "  local __ds_arg\n"
        "  for __ds_arg in \"$@\"; do\n"
        "    case \"$__ds_arg\" in\n"
        "      '>'|'>>'|'2>'|'2>>'|'&>'|'&>>') printf ' %s' \"$__ds_arg\" >&2 ;;\n"
        "      *) printf ' ' >&2; __ds_trace_quote \"$__ds_arg\" >&2 ;;\n"
        "    esac\n"
        "  done\n"
        "  printf '\\n' >&2\n"
        "}\n";
}

const char *ds_bash_int_helpers_source(void) {
    return
        "__ds_int_check() {\n"
        "  [[ \"$1\" =~ ^[+-]?[0-9]+$ ]] || return 1\n"
        "  [[ \"$1\" != '+' && \"$1\" != '-' ]] || return 1\n"
        "  local __ds_abs=\"$1\" __ds_limit=9223372036854775807\n"
        "  if [[ \"$__ds_abs\" == -* ]]; then __ds_abs=\"${__ds_abs#-}\"; __ds_limit=9223372036854775808; elif [[ \"$__ds_abs\" == +* ]]; then __ds_abs=\"${__ds_abs#+}\"; fi\n"
        "  while [[ ${#__ds_abs} -gt 1 && \"$__ds_abs\" == 0* ]]; do __ds_abs=\"${__ds_abs#0}\"; done\n"
        "  [[ ${#__ds_abs} -lt ${#__ds_limit} ]] && return 0\n"
        "  [[ ${#__ds_abs} -gt ${#__ds_limit} ]] && return 1\n"
        "  [[ \"$__ds_abs\" < \"$__ds_limit\" || \"$__ds_abs\" == \"$__ds_limit\" ]]\n"
        "}\n"
        "__ds_int_overflow() { __ds_error \"integer overflow in operator '$1'\"; }\n"
        "__ds_int_mul_checked() { local __ds_op_name=\"$1\" __ds_l=\"$2\" __ds_r=\"$3\" __ds_out=0; __ds_out=$((__ds_l * __ds_r)); (( __ds_l != 0 && __ds_out / __ds_l != __ds_r )) && __ds_int_overflow \"$__ds_op_name\"; printf '%s' \"$__ds_out\"; }\n"
        "__ds_int_bin() {\n"
        "  local __ds_op=\"$1\" __ds_l=\"$2\" __ds_r=\"$3\" __ds_out=0\n"
        "  __ds_int_check \"$__ds_l\" && __ds_int_check \"$__ds_r\" || __ds_error \"integer arithmetic received a non-int operand\"\n"
        "  case \"$__ds_op\" in\n"
        "    '+') __ds_out=$((__ds_l + __ds_r)); (( (__ds_r > 0 && __ds_out < __ds_l) || (__ds_r < 0 && __ds_out > __ds_l) )) && __ds_int_overflow '+' ;;\n"
        "    '-') __ds_out=$((__ds_l - __ds_r)); (( (__ds_r < 0 && __ds_out < __ds_l) || (__ds_r > 0 && __ds_out > __ds_l) )) && __ds_int_overflow '-' ;;\n"
        "    '*') __ds_out=$(__ds_int_mul_checked '*' \"$__ds_l\" \"$__ds_r\") || exit $? ;;\n"
        "    '/') (( __ds_r != 0 )) || __ds_error 'division or modulo by zero'; (( __ds_l == (-9223372036854775807 - 1) && __ds_r == -1 )) && __ds_int_overflow '/'; __ds_out=$((__ds_l / __ds_r)) ;;\n"
        "    '%') (( __ds_r != 0 )) || __ds_error 'division or modulo by zero'; (( __ds_l == (-9223372036854775807 - 1) && __ds_r == -1 )) && __ds_int_overflow '%'; __ds_out=$((__ds_l % __ds_r)) ;;\n"
        "    '**') (( __ds_r >= 0 )) || __ds_error 'negative exponent runtime value is rejected in v0.21.0'; local __ds_base=\"$__ds_l\"; __ds_out=1; while (( __ds_r > 0 )); do if (( __ds_r & 1 )); then __ds_out=$(__ds_int_mul_checked '**' \"$__ds_out\" \"$__ds_base\") || exit $?; fi; __ds_r=$((__ds_r / 2)); if (( __ds_r > 0 )); then __ds_base=$(__ds_int_mul_checked '**' \"$__ds_base\" \"$__ds_base\") || exit $?; fi; done ;;\n"
        "    *) __ds_error \"internal Bash helper invariant failed: unknown integer operator '$__ds_op' after lowering\" ;;\n"
        "  esac\n"
        "  printf '%s' \"$__ds_out\"\n"
        "}\n"
        "__ds_int_neg() { __ds_int_bin '-' 0 \"$1\"; }\n\n";
}

const char *ds_bash_function_value_capture_helpers_source(void) {
    /*
     * Function return kind is validated by lowering and carried in HIR. These
     * helper diagnostics are emitted-script ABI invariants: they catch corrupted
     * private payloads or impossible backend mismatches, not source-language
     * return-kind errors.
     */
    return
        "__ds_call_value_capture() {\n"
        "  local __ds_cv_expected=\"$1\" __ds_cv_fn=\"$2\" __ds_cv_tmpdir __ds_cv_stdout __ds_cv_stderr __ds_cv_code\n"
        "  shift 2\n"
        "  __ds_return_type=\n"
        "  __ds_return_value=\n"
        "  unset -v __ds_return_array __ds_return_elem_type __ds_return_map __ds_return_value_type __ds_return_stdout __ds_return_stderr __ds_return_code __ds_return_status __ds_return_ok __ds_return_failed 2>/dev/null || true\n"
        "  __ds_cv_tmpdir=$(mktemp -d) || __ds_error 'failed to create function capture temp dir'\n"
        "  __ds_cv_stdout=\"$__ds_cv_tmpdir/stdout\"\n"
        "  __ds_cv_stderr=\"$__ds_cv_tmpdir/stderr\"\n"
        "  set +e\n"
        "  \"$__ds_cv_fn\" \"$@\" >\"$__ds_cv_stdout\" 2>\"$__ds_cv_stderr\"\n"
        "  __ds_cv_code=$?\n"
        "  set -e\n"
        "  if (( __ds_cv_code != 0 )); then cat \"$__ds_cv_stderr\" >&2; rm -rf \"$__ds_cv_tmpdir\"; exit \"$__ds_cv_code\"; fi\n"
        "  if [[ -s \"$__ds_cv_stdout\" ]]; then cat \"$__ds_cv_stdout\" >&2; rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'value-returning function produced stdout during expression capture'; fi\n"
        "  if [[ -z \"$__ds_return_type\" ]]; then rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'value-returning function did not set an internal return payload'; fi\n"
        "  case \"$__ds_return_type\" in string|int|bool|null|array|map|command_result) ;; *) rm -rf \"$__ds_cv_tmpdir\"; __ds_error \"invalid internal function return type '$__ds_return_type'\" ;; esac\n"
        "  if [[ \"$__ds_cv_expected\" != unknown && \"$__ds_cv_expected\" != \"$__ds_return_type\" ]]; then rm -rf \"$__ds_cv_tmpdir\"; __ds_error \"internal function return kind mismatch: got $__ds_return_type but expected $__ds_cv_expected\"; fi\n"
        "  if [[ \"$__ds_return_type\" == int ]]; then __ds_int_check \"$__ds_return_value\" || { rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'invalid internal int function return payload'; }; fi\n"
        "  if [[ \"$__ds_return_type\" == bool && \"$__ds_return_value\" != true && \"$__ds_return_value\" != false ]]; then rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'invalid internal bool function return payload'; fi\n"
        "  if [[ \"$__ds_return_type\" == array ]]; then declare -p __ds_return_array >/dev/null 2>&1 || { rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'invalid internal array function return payload'; }; declare -p __ds_return_elem_type >/dev/null 2>&1 || { rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'invalid internal array element-type function return payload'; }; fi\n"
        "  if [[ \"$__ds_return_type\" == map ]]; then declare -p __ds_return_map >/dev/null 2>&1 || { rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'invalid internal map function return payload'; }; declare -p __ds_return_value_type >/dev/null 2>&1 || { rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'invalid internal map value-type function return payload'; }; fi\n"
        "  if [[ \"$__ds_return_type\" == command_result ]]; then declare -p __ds_return_stdout __ds_return_stderr __ds_return_code __ds_return_ok __ds_return_failed >/dev/null 2>&1 || { rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'invalid internal command-result function return payload'; }; __ds_int_check \"$__ds_return_code\" || { rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'invalid internal command-result status function return payload'; }; [[ \"$__ds_return_ok\" == true || \"$__ds_return_ok\" == false ]] || { rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'invalid internal command-result ok function return payload'; }; [[ \"$__ds_return_failed\" == true || \"$__ds_return_failed\" == false ]] || { rm -rf \"$__ds_cv_tmpdir\"; __ds_error 'invalid internal command-result failed function return payload'; }; fi\n"
        "  rm -rf \"$__ds_cv_tmpdir\"\n"
        "}\n";
}

const char *ds_bash_function_value_materialize_helpers_source(void) {
    return
        "__ds_call_value_into() {\n"
        "  __ds_call_value_capture \"$2\" \"$3\" \"${@:4}\"\n"
        "  local __ds_cv_target=\"$1\" __ds_cv_meta __ds_cv_key\n"
        "  __ds_cv_meta=\"${__ds_cv_target#__ds_}\"\n"
        "  case \"$__ds_return_type\" in\n"
        "    array) eval \"$__ds_cv_target=()\"; eval \"__ds_elem_type_${__ds_cv_meta}=()\"; for __ds_cv_key in \"${!__ds_return_array[@]}\"; do eval \"$__ds_cv_target+=(\\\"\\${__ds_return_array[\\$__ds_cv_key]}\\\")\"; eval \"__ds_elem_type_${__ds_cv_meta}[\\$__ds_cv_key]=\\\"\\${__ds_return_elem_type[\\$__ds_cv_key]:-unknown}\\\"\"; done; printf -v \"__ds_type_${__ds_cv_meta}\" '%s' array ;;\n"
        "    map) eval \"$__ds_cv_target=()\"; eval \"__ds_value_type_${__ds_cv_meta}=()\"; for __ds_cv_key in \"${!__ds_return_map[@]}\"; do eval \"$__ds_cv_target[\\$__ds_cv_key]=\\\"\\${__ds_return_map[\\$__ds_cv_key]}\\\"\"; eval \"__ds_value_type_${__ds_cv_meta}[\\$__ds_cv_key]=\\\"\\${__ds_return_value_type[\\$__ds_cv_key]:-unknown}\\\"\"; done; printf -v \"__ds_type_${__ds_cv_meta}\" '%s' map ;;\n"
        "    command_result) printf -v \"${__ds_cv_target}_stdout\" '%s' \"$__ds_return_stdout\"; printf -v \"${__ds_cv_target}_stderr\" '%s' \"$__ds_return_stderr\"; printf -v \"${__ds_cv_target}_code\" '%s' \"$__ds_return_code\"; printf -v \"${__ds_cv_target}_status\" '%s' \"$__ds_return_code\"; printf -v \"${__ds_cv_target}_ok\" '%s' \"$__ds_return_ok\"; printf -v \"${__ds_cv_target}_failed\" '%s' \"$__ds_return_failed\"; printf -v \"__ds_type_${__ds_cv_meta}\" '%s' command_result ;;\n"
        "    *) printf -v \"$__ds_cv_target\" '%s' \"$__ds_return_value\" ;;\n"
        "  esac\n"
        "}\n"
        "__ds_call_value() {\n"
        "  local __ds_cv_expected=\"$1\" __ds_cv_fn=\"$2\"\n"
        "  shift 2\n"
        "  __ds_call_value_capture \"$__ds_cv_expected\" \"$__ds_cv_fn\" \"$@\"\n"
        "  printf '%s' \"$__ds_return_value\"\n"
        "}\n\n";
}

const char *ds_bash_command_result_helpers_source(void) {
    return
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
        "  printf -v \"${__ds_prefix}_status\" '%s' \"$__ds_code\"\n"
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
    /* Dynamic collection access failures mirror VM runtime data diagnostics. */
    return
        "__ds_array_get() {\n"
        "  local __ds_name=$1 __ds_index=$2 __ds_len\n"
        "  [[ \"$__ds_index\" =~ ^[0-9]+$ ]] || __ds_error \"runtime array index $__ds_index is not an int\"\n"
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
        "}\n"
        "__ds_map_sorted_keys() {\n"
        "  local __ds_source=$1 __ds_target=$2 __ds_len __ds_i __ds_j __ds_tmp __ds_prev\n"
        "  local LC_ALL=C\n"
        "  eval \"$__ds_target=(\\\"\\${!${__ds_source}[@]}\\\")\"\n"
        "  eval \"__ds_len=\\${#${__ds_target}[@]}\"\n"
        "  for (( __ds_i = 1; __ds_i < __ds_len; __ds_i++ )); do\n"
        "    eval \"__ds_tmp=\\\"\\${${__ds_target}[\\$__ds_i]}\\\"\"\n"
        "    __ds_j=$__ds_i\n"
        "    while (( __ds_j > 0 )); do\n"
        "      eval \"__ds_prev=\\\"\\${${__ds_target}[\\$((__ds_j - 1))]}\\\"\"\n"
        "      if [[ \"$__ds_prev\" < \"$__ds_tmp\" || \"$__ds_prev\" == \"$__ds_tmp\" ]]; then break; fi\n"
        "      eval \"${__ds_target}[\\$__ds_j]=\\\"\\$__ds_prev\\\"\"\n"
        "      __ds_j=$((__ds_j - 1))\n"
        "    done\n"
        "    eval \"${__ds_target}[\\$__ds_j]=\\\"\\$__ds_tmp\\\"\"\n"
        "  done\n"
        "}\n\n";
}

const char *ds_bash_stdlib_helpers_source(void) {
    /* Static helper validation is lowerer-owned; these helpers validate dynamic
     * runtime strings and OS/environment state in emitted Bash. */
    return
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
        "__ds_stdlib_env_valid() { [[ \"$1\" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || __ds_error \"invalid environment variable name '$1' at runtime in v0.11.0\"; }\n"
        "__ds_stdlib_env_get() { local n=\"$1\"; __ds_stdlib_env_valid \"$n\"; if [[ ${!n+x} ]]; then printf '%s' \"${!n}\"; elif [[ $# -ge 2 ]]; then printf '%s' \"$2\"; fi; }\n"
        "__ds_stdlib_env_set() { __ds_stdlib_env_valid \"$1\"; export \"$1=$2\"; }\n"
        "__ds_stdlib_env_unset() { __ds_stdlib_env_valid \"$1\"; unset \"$1\"; }\n"
        "__ds_stdlib_capture() { local __ds_var=\"$1\" __ds_data __ds_status; shift; set +e; __ds_data=\"$(\"$@\"; printf x)\"; __ds_status=$?; set -e; if (( __ds_status != 0 )); then exit \"$__ds_status\"; fi; __ds_data=\"${__ds_data%x}\"; printf -v \"$__ds_var\" '%s' \"$__ds_data\"; }\n"
        "__ds_stdlib_reject_recursive_glob() { [[ \"$1\" != *'**'* ]] || __ds_error \"runtime glob pattern contains recursive **; recursive glob patterns are deferred in v0.11.0\"; }\n"
        "__ds_stdlib_glob() { __ds_stdlib_reject_recursive_glob \"$1\"; { compgen -G \"$1\" || true; } | sort; }\n"
        "__ds_stdlib_glob_required() { local out; out=$(__ds_stdlib_glob \"$1\"); [[ -n \"$out\" ]] || __ds_error \"required glob '$1' had no matches\"; printf '%s\n' \"$out\"; }\n"
        "__ds_stdlib_lines() { [[ -f \"$1\" ]] || __ds_error \"failed to read lines from '$1'\"; __ds_stdlib_reject_nul \"$1\" \"lines from\"; while IFS= read -r line || [[ -n \"$line\" ]]; do printf '%s\\n' \"$line\"; done <\"$1\"; }\n\n";
}

const char *ds_bash_string_helpers_source(void) {
    return
        "__ds_string_trim() { local s=\"$1\"; s=\"${s#${s%%[!$' \\t\\r\\n']*}}\"; s=\"${s%${s##*[!$' \\t\\r\\n']}}\"; printf '%s' \"$s\"; }\n"
        "__ds_string_upper() { printf '%s' \"$1\" | LC_ALL=C tr '[:lower:]' '[:upper:]'; }\n"
        "__ds_string_lower() { printf '%s' \"$1\" | LC_ALL=C tr '[:upper:]' '[:lower:]'; }\n"
        "__ds_string_replace() { [[ -n \"$2\" ]] || __ds_error 'replace with an empty runtime source is rejected in v0.19.0'; local s=\"$1\" from=\"$2\" to=\"$3\" out= i=0 flen=${#2}; while (( i < ${#s} )); do if [[ \"${s:i:flen}\" == \"$from\" ]]; then out+=\"$to\"; i=$((i + flen)); else out+=\"${s:i:1}\"; i=$((i + 1)); fi; done; printf '%s' \"$out\"; }\n"
        "__ds_string_contains() { local s=\"$1\" sub=\"$2\" i=0 slen=${#2}; if [[ -z \"$sub\" ]]; then printf true; return; fi; while (( i + slen <= ${#s} )); do [[ \"${s:i:slen}\" == \"$sub\" ]] && { printf true; return; }; i=$((i + 1)); done; printf false; }\n"
        "__ds_string_starts_with() { local s=\"$1\" pre=\"$2\"; [[ \"${s:0:${#pre}}\" == \"$pre\" ]] && printf true || printf false; }\n"
        "__ds_string_ends_with() { local s=\"$1\" suf=\"$2\"; if [[ -z \"$suf\" ]]; then printf true; elif [[ \"${s: -${#suf}}\" == \"$suf\" ]]; then printf true; else printf false; fi; }\n"
        "__ds_string_split() { [[ -n \"$2\" ]] || __ds_error 'split with an empty runtime separator is rejected in v0.19.0'; local s=\"$1\" sep=\"$2\" start=0 i=0 slen=${#2}; while (( i + slen <= ${#s} )); do if [[ \"${s:i:slen}\" == \"$sep\" ]]; then printf '%s\\n' \"${s:start:i-start}\"; i=$((i + slen)); start=$i; else i=$((i + 1)); fi; done; printf '%s\\n' \"${s:start}\"; }\n"
        "__ds_format_center() { local width=\"$1\" s=\"$2\" pad left right; pad=$((width - ${#s})); (( pad > 0 )) || pad=0; left=$((pad / 2)); right=$((pad - left)); printf '%*s%s%*s' \"$left\" '' \"$s\" \"$right\" ''; }\n\n";
}

