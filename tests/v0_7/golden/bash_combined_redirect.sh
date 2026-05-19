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

# ds: tests/v0_7/fixtures/helpers/bash_combined_redirect.ds:1
__ds_trace_cmd 'tests/v0_7/fixtures/helpers/bash_combined_redirect.ds':1:1 sh -c "printf out; printf err >&2"
sh -c "printf out; printf err >&2" > "all.txt" 2>&1 || __ds_fail 'tests/v0_7/fixtures/helpers/bash_combined_redirect.ds':1:1 "$?"

