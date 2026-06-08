#!/usr/bin/env bash
set -euo pipefail

__ds_error() { echo "${0##*/}: error: $1" >&2; exit 1; }

__ds_tmp_paths=()
__ds_temp_register() { __ds_tmp_paths+=("$1"); }
__ds_temp_remove() { local __ds_p=$1 __ds_i; rm -rf "$__ds_p" 2>/dev/null || true; for __ds_i in "${!__ds_tmp_paths[@]}"; do if [[ "${__ds_tmp_paths[$__ds_i]:-}" == "$__ds_p" ]]; then unset "__ds_tmp_paths[$__ds_i]"; fi; done; }
__ds_temp_cleanup() { local __ds_p; for __ds_p in "${__ds_tmp_paths[@]}"; do [[ -n "$__ds_p" ]] && rm -rf "$__ds_p" 2>/dev/null || true; done; __ds_tmp_paths=(); }
__ds_mktemp_file() { local __ds_var=$1 __ds_msg=$2 __ds_path; __ds_path=$(mktemp) || __ds_error "$__ds_msg"; __ds_temp_register "$__ds_path"; printf -v "$__ds_var" '%s' "$__ds_path"; }
__ds_mktemp_dir() { local __ds_var=$1 __ds_msg=$2 __ds_path; __ds_path=$(mktemp -d) || __ds_error "$__ds_msg"; __ds_temp_register "$__ds_path"; printf -v "$__ds_var" '%s' "$__ds_path"; }

__ds_trace_quote() {
  local __ds_q=$1
  __ds_q=${__ds_q//\\/\\\\}
  __ds_q=${__ds_q//\"/\\\"}
  printf '"%s"' "$__ds_q"
}
__ds_trace_cmd() {
  [[ "${DS_TRACE_CMD:-}" == 1 ]] || return 0
  local __ds_loc=$1
  shift
  printf 'trace: cmd %s:' "$__ds_loc" >&2
  local __ds_arg
  for __ds_arg in "$@"; do
    case "$__ds_arg" in
      '>'|'>>'|'2>'|'2>>'|'&>'|'&>>') printf ' %s' "$__ds_arg" >&2 ;;
      *) printf ' ' >&2; __ds_trace_quote "$__ds_arg" >&2 ;;
    esac
  done
  printf '\n' >&2
}
trap '__ds_rc=$?; __ds_temp_cleanup; exit "$__ds_rc"' EXIT

__ds_stdout_is_pipe_like() { [[ -p /dev/stdout || -S /dev/stdout ]]; }
__ds_is_quiet_broken_pipe() { local __ds_code=$1 __ds_allow=${2:-0}; (( __ds_allow == 1 && __ds_code == 141 )) && __ds_stdout_is_pipe_like; }
__ds_fail() {
  local __ds_loc=$1 __ds_code=$2 __ds_allow=${3:-0}
  if __ds_is_quiet_broken_pipe "$__ds_code" "$__ds_allow"; then exit 0; fi
  echo "$__ds_loc: error: command failed with exit $__ds_code" >&2
  exit "$__ds_code"
}

__ds_capture() {
  local __ds_prefix=$1
  shift
  local __ds_loc=$1
  shift
  local __ds_tmpdir
  __ds_mktemp_dir __ds_tmpdir 'failed to create command capture temp dir'
  local __ds_stdout="$__ds_tmpdir/stdout"
  local __ds_stderr="$__ds_tmpdir/stderr"
  set +e
  __ds_trace_cmd "$__ds_loc" "$@"
  "$@" >"$__ds_stdout" 2>"$__ds_stderr"
  local __ds_code=$?
  set -e
  local __ds_data
  __ds_data=$(cat "$__ds_stdout"; printf x)
  printf -v "${__ds_prefix}_stdout" '%s' "${__ds_data%x}"
  __ds_data=$(cat "$__ds_stderr"; printf x)
  printf -v "${__ds_prefix}_stderr" '%s' "${__ds_data%x}"
  printf -v "${__ds_prefix}_code" '%s' "$__ds_code"
  printf -v "${__ds_prefix}_status" '%s' "$__ds_code"
  if [[ $__ds_code -eq 0 ]]; then
    printf -v "${__ds_prefix}_ok" '%s' true
    printf -v "${__ds_prefix}_failed" '%s' false
  else
    printf -v "${__ds_prefix}_ok" '%s' false
    printf -v "${__ds_prefix}_failed" '%s' true
  fi
  __ds_temp_remove "$__ds_tmpdir"
}

# ds: tests/v0_7/fixtures/helpers/bash_failure.ds:1
__ds_capture __ds_r 'tests/v0_7/fixtures/helpers/bash_failure.ds':1:9 sh -c "printf out; printf err >&2; exit 7"

# ds: tests/v0_7/fixtures/helpers/bash_failure.ds:2
if [[ "$__ds_r_failed" == true ]]; then
  # ds: tests/v0_7/fixtures/helpers/bash_failure.ds:3
  __ds_trace_cmd 'tests/v0_7/fixtures/helpers/bash_failure.ds':3:3 echo "$__ds_r_stderr"
  ( echo "$__ds_r_stderr" ) || __ds_fail 'tests/v0_7/fixtures/helpers/bash_failure.ds':3:3 "$?" 1

fi

