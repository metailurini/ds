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
__ds_fail() {
  local __ds_loc=$1 __ds_code=$2
  echo "$__ds_loc: error: command failed with exit $__ds_code" >&2
  exit "$__ds_code"
}

# ds: tests/v0_2/fixtures/nested_if.ds:1
__ds_ok=true

# ds: tests/v0_2/fixtures/nested_if.ds:2
__ds_ready=true

# ds: tests/v0_2/fixtures/nested_if.ds:3
if [[ "$__ds_ok" == true ]]; then
  # ds: tests/v0_2/fixtures/nested_if.ds:4
  if [[ "$__ds_ready" == true ]]; then
    # ds: tests/v0_2/fixtures/nested_if.ds:5
    __ds_trace_cmd 'tests/v0_2/fixtures/nested_if.ds':5:5 echo "ready"
    echo "ready" || __ds_fail 'tests/v0_2/fixtures/nested_if.ds':5:5 "$?"

  fi

fi

