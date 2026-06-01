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

# ds: tests/v0_6/fixtures/imports_basic/lib.ds:1
__ds_app="api"
__ds_type_app='string'

# ds: tests/v0_6/fixtures/imports_basic/constants.ds:1
__ds_target="production"
__ds_type_target='string'

# ds: tests/v0_6/fixtures/imports_basic/constants.ds:2
__ds_retries=3
__ds_type_retries='int'

# ds: tests/v0_6/fixtures/imports_basic/constants.ds:3
__ds_enabled=true
__ds_type_enabled='bool'

# ds: tests/v0_6/fixtures/imports_basic/main.ds:3
__ds_trace_cmd 'tests/v0_6/fixtures/imports_basic/main.ds':3:1 echo "Deploying ${__ds_app} to ${__ds_target}"
( echo "Deploying ${__ds_app} to ${__ds_target}" ) || __ds_fail 'tests/v0_6/fixtures/imports_basic/main.ds':3:1 "$?" 1

# ds: tests/v0_6/fixtures/imports_basic/main.ds:4
__ds_trace_cmd 'tests/v0_6/fixtures/imports_basic/main.ds':4:1 echo "$__ds_retries"
( echo "$__ds_retries" ) || __ds_fail 'tests/v0_6/fixtures/imports_basic/main.ds':4:1 "$?" 1

# ds: tests/v0_6/fixtures/imports_basic/main.ds:5
if [[ ( "${__ds_type_enabled:-unknown}" == bool && "$__ds_enabled" == true ) || ( "${__ds_type_enabled:-unknown}" == int && "$__ds_enabled" != 0 ) || ( "${__ds_type_enabled:-unknown}" != bool && "${__ds_type_enabled:-unknown}" != int && -n "$__ds_enabled" ) ]]; then
  # ds: tests/v0_6/fixtures/imports_basic/main.ds:6
  __ds_trace_cmd 'tests/v0_6/fixtures/imports_basic/main.ds':6:3 echo "enabled"
  ( echo "enabled" ) || __ds_fail 'tests/v0_6/fixtures/imports_basic/main.ds':6:3 "$?" 1

fi

