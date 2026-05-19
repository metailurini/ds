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

# ds: tests/v0_6/fixtures/imports_basic/lib.ds:1
__ds_app="api"

# ds: tests/v0_6/fixtures/imports_basic/constants.ds:1
__ds_target="production"

# ds: tests/v0_6/fixtures/imports_basic/constants.ds:2
__ds_retries=3

# ds: tests/v0_6/fixtures/imports_basic/constants.ds:3
__ds_enabled=true

# ds: tests/v0_6/fixtures/imports_basic/main.ds:3
__ds_trace_cmd 'tests/v0_6/fixtures/imports_basic/main.ds':3:1 echo "Deploying ${__ds_app} to ${__ds_target}"
echo "Deploying ${__ds_app} to ${__ds_target}" || __ds_fail 'tests/v0_6/fixtures/imports_basic/main.ds':3:1 "$?"

# ds: tests/v0_6/fixtures/imports_basic/main.ds:4
__ds_trace_cmd 'tests/v0_6/fixtures/imports_basic/main.ds':4:1 echo "$__ds_retries"
echo "$__ds_retries" || __ds_fail 'tests/v0_6/fixtures/imports_basic/main.ds':4:1 "$?"

# ds: tests/v0_6/fixtures/imports_basic/main.ds:5
if [[ "$__ds_enabled" == true ]]; then
  # ds: tests/v0_6/fixtures/imports_basic/main.ds:6
  __ds_trace_cmd 'tests/v0_6/fixtures/imports_basic/main.ds':6:3 echo "enabled"
  echo "enabled" || __ds_fail 'tests/v0_6/fixtures/imports_basic/main.ds':6:3 "$?"

fi

