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

# ds: tests/v0_2/fixtures/if_with_else.ds:1
__ds_ok=false
__ds_type_ok='bool'

# ds: tests/v0_2/fixtures/if_with_else.ds:2
if [[ ( "${__ds_type_ok:-unknown}" == bool && "$__ds_ok" == true ) || ( "${__ds_type_ok:-unknown}" == int && "$__ds_ok" != 0 ) || ( "${__ds_type_ok:-unknown}" != bool && "${__ds_type_ok:-unknown}" != int && -n "$__ds_ok" ) ]]; then
  # ds: tests/v0_2/fixtures/if_with_else.ds:3
  __ds_trace_cmd 'tests/v0_2/fixtures/if_with_else.ds':3:3 echo "yes"
  ( echo "yes" ) || __ds_fail 'tests/v0_2/fixtures/if_with_else.ds':3:3 "$?" 1

else
  # ds: tests/v0_2/fixtures/if_with_else.ds:5
  __ds_trace_cmd 'tests/v0_2/fixtures/if_with_else.ds':5:3 echo "no"
  ( echo "no" ) || __ds_fail 'tests/v0_2/fixtures/if_with_else.ds':5:3 "$?" 1

fi

