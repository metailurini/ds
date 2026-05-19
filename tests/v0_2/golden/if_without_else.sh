#!/usr/bin/env bash
set -euo pipefail

__ds_trace_cmd() {
  [[ "${DS_TRACE_CMD:-}" == 1 ]] || return 0
  local __ds_loc=$1
  shift
  printf 'trace: cmd %s:' "$__ds_loc" >&2
  local __ds_arg
  for __ds_arg in "$@"; do printf ' %q' "$__ds_arg" >&2; done
  printf '\n' >&2
}
__ds_fail() {
  local __ds_loc=$1 __ds_code=$2
  echo "$__ds_loc: error: command failed with exit $__ds_code" >&2
  exit "$__ds_code"
}

# ds: tests/v0_2/fixtures/if_without_else.ds:1
__ds_ok=true

# ds: tests/v0_2/fixtures/if_without_else.ds:2
if [[ "$__ds_ok" == true ]]; then
  # ds: tests/v0_2/fixtures/if_without_else.ds:3
  __ds_trace_cmd 'tests/v0_2/fixtures/if_without_else.ds':3:3 echo "ok"
  echo "ok" || __ds_fail 'tests/v0_2/fixtures/if_without_else.ds':3:3 "$?"

fi

