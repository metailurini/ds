#!/usr/bin/env bash
set -euo pipefail

__ds_error() { echo "${0##*/}: error: $1" >&2; exit 1; }

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
__ds_fail() {
  local __ds_loc=$1 __ds_code=$2
  echo "$__ds_loc: error: command failed with exit $__ds_code" >&2
  exit "$__ds_code"
}

__ds_capture() {
  local __ds_prefix=$1
  shift
  local __ds_loc=$1
  shift
  local __ds_tmpdir
  __ds_tmpdir=$(mktemp -d) || __ds_error 'failed to create command capture temp dir'
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
  rm -rf "$__ds_tmpdir"
}

# ds: tests/v0_7/fixtures/helpers/bash_capture.ds:1
__ds_word="hello world"

# ds: tests/v0_7/fixtures/helpers/bash_capture.ds:2
__ds_capture __ds_r 'tests/v0_7/fixtures/helpers/bash_capture.ds':2:9 printf "$__ds_word"

# ds: tests/v0_7/fixtures/helpers/bash_capture.ds:3
__ds_trace_cmd 'tests/v0_7/fixtures/helpers/bash_capture.ds':3:1 echo "$__ds_r_stdout"
echo "$__ds_r_stdout" || __ds_fail 'tests/v0_7/fixtures/helpers/bash_capture.ds':3:1 "$?"

