#!/usr/bin/env bash
set -euo pipefail

__ds_error() { echo "${0##*/}: error: $1" >&2; exit 1; }
__ds_capture() {
  local __ds_prefix=$1
  shift
  local __ds_tmpdir
  __ds_tmpdir=$(mktemp -d) || __ds_error 'failed to create command capture temp dir'
  local __ds_stdout="$__ds_tmpdir/stdout"
  local __ds_stderr="$__ds_tmpdir/stderr"
  set +e
  "$@" >"$__ds_stdout" 2>"$__ds_stderr"
  local __ds_code=$?
  set -e
  local __ds_data
  __ds_data=$(cat "$__ds_stdout"; printf x)
  printf -v "${__ds_prefix}_stdout" '%s' "${__ds_data%x}"
  __ds_data=$(cat "$__ds_stderr"; printf x)
  printf -v "${__ds_prefix}_stderr" '%s' "${__ds_data%x}"
  printf -v "${__ds_prefix}_code" '%s' "$__ds_code"
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
__ds_capture __ds_r printf "$__ds_word"

# ds: tests/v0_7/fixtures/helpers/bash_capture.ds:3
echo "$__ds_r_stdout"

