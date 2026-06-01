#!/usr/bin/env bash
set -euo pipefail

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
__ds_stdout_is_pipe_like() { [[ -p /dev/stdout || -S /dev/stdout ]]; }
__ds_is_quiet_broken_pipe() { local __ds_code=$1 __ds_allow=${2:-0}; (( __ds_allow == 1 && __ds_code == 141 )) && __ds_stdout_is_pipe_like; }
__ds_fail() {
  local __ds_loc=$1 __ds_code=$2 __ds_allow=${3:-0}
  if __ds_is_quiet_broken_pipe "$__ds_code" "$__ds_allow"; then exit 0; fi
  echo "$__ds_loc: error: command failed with exit $__ds_code" >&2
  exit "$__ds_code"
}

# ds: tests/v0_2/fixtures/mixed.ds:1
__ds_name="Danh"

# ds: tests/v0_2/fixtures/mixed.ds:2
__ds_count=3

# ds: tests/v0_2/fixtures/mixed.ds:3
__ds_enabled=true

# ds: tests/v0_2/fixtures/mixed.ds:4
__ds_trace_cmd 'tests/v0_2/fixtures/mixed.ds':4:1 echo "Deploying ${__ds_name}"
( echo "Deploying ${__ds_name}" ) || __ds_fail 'tests/v0_2/fixtures/mixed.ds':4:1 "$?" 1

# ds: tests/v0_2/fixtures/mixed.ds:5
if [[ "$__ds_name" == "Danh" ]]; then
  # ds: tests/v0_2/fixtures/mixed.ds:6
  __ds_trace_cmd 'tests/v0_2/fixtures/mixed.ds':6:3 echo "matched"
  ( echo "matched" ) || __ds_fail 'tests/v0_2/fixtures/mixed.ds':6:3 "$?" 1

  # ds: tests/v0_2/fixtures/mixed.ds:7
  if ! [[ "$__ds_count" < 3 ]]; then
    # ds: tests/v0_2/fixtures/mixed.ds:8
    __ds_trace_cmd 'tests/v0_2/fixtures/mixed.ds':8:5 echo "$__ds_name"
    ( echo "$__ds_name" ) || __ds_fail 'tests/v0_2/fixtures/mixed.ds':8:5 "$?" 1

  fi

else
  # ds: tests/v0_2/fixtures/mixed.ds:11
  __ds_trace_cmd 'tests/v0_2/fixtures/mixed.ds':11:3 echo "no"
  ( echo "no" ) || __ds_fail 'tests/v0_2/fixtures/mixed.ds':11:3 "$?" 1

fi

# ds: tests/v0_2/fixtures/mixed.ds:13
__ds_trace_cmd 'tests/v0_2/fixtures/mixed.ds':13:1 echo "done"
( echo "done" ) || __ds_fail 'tests/v0_2/fixtures/mixed.ds':13:1 "$?" 1

