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

# ds: tests/v0_6/fixtures/imports_nested/shared/common.ds:1
__ds_common="COMMON"

# ds: tests/v0_6/fixtures/imports_nested/shared/b.ds:2
__ds_b="B"

# ds: tests/v0_6/fixtures/imports_nested/shared/b.ds:3
__ds_trace_cmd 'tests/v0_6/fixtures/imports_nested/shared/b.ds':3:1 echo "b sees ${__ds_common}"
echo "b sees ${__ds_common}" || __ds_fail 'tests/v0_6/fixtures/imports_nested/shared/b.ds':3:1 "$?"

# ds: tests/v0_6/fixtures/imports_nested/shared/a.ds:2
__ds_a="A"

# ds: tests/v0_6/fixtures/imports_nested/shared/a.ds:3
__ds_trace_cmd 'tests/v0_6/fixtures/imports_nested/shared/a.ds':3:1 echo "a sees ${__ds_b} and ${__ds_common}"
echo "a sees ${__ds_b} and ${__ds_common}" || __ds_fail 'tests/v0_6/fixtures/imports_nested/shared/a.ds':3:1 "$?"

# ds: tests/v0_6/fixtures/imports_nested/main.ds:2
__ds_trace_cmd 'tests/v0_6/fixtures/imports_nested/main.ds':2:1 echo "root sees ${__ds_a} ${__ds_b} ${__ds_common}"
echo "root sees ${__ds_a} ${__ds_b} ${__ds_common}" || __ds_fail 'tests/v0_6/fixtures/imports_nested/main.ds':2:1 "$?"

