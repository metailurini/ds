#!/usr/bin/env bash
set -euo pipefail

__ds_error() { echo "${0##*/}: error: $1" >&2; exit 1; }

__ds_usage() {
  cat <<'__DS_USAGE__'
Usage: args_basic.ds <app> [options]

Arguments:
  app string

Options:
  --target string    default: staging
  --retries int    default: 3
  --force            boolean flag
  --help             show this help
__DS_USAGE__
}

__ds_parse_int() {
  [[ "$1" =~ ^[+-]?[0-9]+$ ]] || return 1
  [[ "$1" != "+" && "$1" != "-" ]] || return 1
  local __ds_abs="$1" __ds_limit=9223372036854775807
  if [[ "$__ds_abs" == -* ]]; then __ds_abs="${__ds_abs#-}"; __ds_limit=9223372036854775808; elif [[ "$__ds_abs" == +* ]]; then __ds_abs="${__ds_abs#+}"; fi
  while [[ ${#__ds_abs} -gt 1 && "$__ds_abs" == 0* ]]; do __ds_abs="${__ds_abs#0}"; done
  [[ ${#__ds_abs} -lt ${#__ds_limit} ]] && return 0
  [[ ${#__ds_abs} -gt ${#__ds_limit} ]] && return 1
  [[ "$__ds_abs" < "$__ds_limit" || "$__ds_abs" == "$__ds_limit" ]]
}

__ds_app=
__ds_type_app='string'
__ds_target='staging'
__ds_type_target='string'
__ds_seen_target=false
__ds_retries=3
__ds_type_retries='int'
__ds_seen_retries=false
__ds_force=false
__ds_type_force='bool'
__ds_seen_force=false
__ds_positionals=()
while (($#)); do
  case "$1" in
    --help|-h) __ds_usage; exit 0 ;;
    --) shift; while (($#)); do __ds_positionals+=("$1"); shift; done; break ;;
    --target)
      $__ds_seen_target && __ds_error 'duplicate option `--target`'
      __ds_seen_target=true
      shift
      [[ $# -gt 0 && "$1" != --* ]] || __ds_error 'option `--target` requires a value'
__ds_target="$1"
      ;;
    --retries)
      $__ds_seen_retries && __ds_error 'duplicate option `--retries`'
      __ds_seen_retries=true
      shift
      [[ $# -gt 0 && "$1" != --* ]] || __ds_error 'option `--retries` requires a value'
      __ds_parse_int "$1" || __ds_error 'invalid int value `'"$1"'` for `retries`'
__ds_retries="$1"
      ;;
    --force)
      $__ds_seen_force && __ds_error 'duplicate option `--force`'
      __ds_seen_force=true
__ds_force=true
      ;;
    --*) __ds_error 'unknown option `'"$1"'`' ;;
    *) __ds_positionals+=("$1") ;;
  esac
  shift
done

[[ ${#__ds_positionals[@]} -gt 0 ]] || __ds_error 'missing required argument `app`'
__ds_app="${__ds_positionals[0]}"
[[ ${#__ds_positionals[@]} -eq 1 ]] || __ds_error 'unexpected extra positional argument `'"${__ds_positionals[1]}"'`'

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

# ds: tests/v0_5/fixtures/args_basic.ds:8
__ds_trace_cmd 'tests/v0_5/fixtures/args_basic.ds':8:1 echo "Deploying ${__ds_app} to ${__ds_target}"
( echo "Deploying ${__ds_app} to ${__ds_target}" ) || __ds_fail 'tests/v0_5/fixtures/args_basic.ds':8:1 "$?" 1

# ds: tests/v0_5/fixtures/args_basic.ds:9
__ds_trace_cmd 'tests/v0_5/fixtures/args_basic.ds':9:1 echo "retries=${__ds_retries}"
( echo "retries=${__ds_retries}" ) || __ds_fail 'tests/v0_5/fixtures/args_basic.ds':9:1 "$?" 1

# ds: tests/v0_5/fixtures/args_basic.ds:11
if [[ ( "${__ds_type_force:-unknown}" == bool && "$__ds_force" == true ) || ( "${__ds_type_force:-unknown}" == int && "$__ds_force" != 0 ) || ( "${__ds_type_force:-unknown}" != bool && "${__ds_type_force:-unknown}" != int && -n "$__ds_force" ) ]]; then
  # ds: tests/v0_5/fixtures/args_basic.ds:12
  __ds_trace_cmd 'tests/v0_5/fixtures/args_basic.ds':12:3 echo "force enabled"
  ( echo "force enabled" ) || __ds_fail 'tests/v0_5/fixtures/args_basic.ds':12:3 "$?" 1

else
  # ds: tests/v0_5/fixtures/args_basic.ds:14
  __ds_trace_cmd 'tests/v0_5/fixtures/args_basic.ds':14:3 echo "force disabled"
  ( echo "force disabled" ) || __ds_fail 'tests/v0_5/fixtures/args_basic.ds':14:3 "$?" 1

fi

