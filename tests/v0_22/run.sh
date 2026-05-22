#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_22_tests.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != 1 ]]; then
  make -C "$ROOT" clean all >/dev/null
fi

cd "$ROOT"

write_fixture() {
  local path="$1"
  mkdir -p "$(dirname "$path")"
  cat >"$path"
}

capture_in_dir() {
  local name="$1" dir="$2"; shift 2
  mkdir -p "$dir"
  set +e
  (cd "$dir" && "$@") >"$TMP/$name.out" 2>"$TMP/$name.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/$name.rc"
}

assert_matches() {
  local file="$1" regex="$2" name="$3"
  grep -E -- "$regex" "$file" >/dev/null || {
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected to match /$regex/"
  }
  pass "$name"
}

assert_not_matches() {
  local file="$1" regex="$2" name="$3"
  if grep -E -- "$regex" "$file" >/dev/null; then
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected not to match /$regex/"
  fi
  pass "$name"
}

assert_diag() {
  local file="$1" fragment="$2" name="$3"
  assert_contains "$file" ': error:' "$name severity"
  assert_contains "$file" "$fragment" "$name text"
  assert_contains "$file" '^' "$name caret"
}

assert_check_fails() {
  local name="$1" fixture="$2" fragment="$3"
  run_fail "${name}_check" "$DS" check "$fixture"
  assert_diag "$TMP/${name}_check.err" "$fragment" "$name check diagnostic"
}

assert_emit_fails() {
  local name="$1" fixture="$2" fragment="$3"
  run_fail "${name}_emit" "$DS" emit bash "$fixture" -o "$TMP/${name}.sh"
  assert_diag "$TMP/${name}_emit.err" "$fragment" "$name emit diagnostic"
  assert_file_missing_or_empty "$TMP/${name}.sh" "$name no partial Bash"
}

assert_exact_stdout() {
  local name="$1" expected="$2"
  assert_same_text "$expected" "$TMP/${name}.out" "$name stdout"
}

assert_parity() {
  local name="$1" fixture="$2" expected_status="$3" expected_stdout="$4"; shift 4
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"

  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_exact_stdout "${name}_vm" "$expected_stdout"
  assert_exact_stdout "${name}_bash" "$expected_stdout"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  else
    pass "$name non-zero stderr checked by focused assertions when required"
  fi
}

FIX="$TMP/fixtures with spaces"
mkdir -p "$FIX"

# Build wiring and docs for the v0.22.1 deterministic slice.
assert_contains Makefile '0-22' 'TEST_VERSIONS contains v0.22'
assert_matches Makefile '^TEST_VERSIONS := .*0-21 0-22($| )' 'v0.22 follows v0.21 in TEST_VERSIONS'
assert_contains docs/roadmap.md 'v0.22.1 — Cleanup Core Test Stabilization' 'roadmap names v0.22.1 slice'
assert_contains docs/roadmap.md 'No real `SIGINT`/`SIGTERM` delivery tests' 'roadmap excludes real signals for v0.22.1'
assert_contains docs/runtime.md 'Handler registration is process-scope' 'runtime docs document process-scope handlers'
assert_contains docs/runtime.md 'Cleanup runs for normal completion' 'runtime docs document cleanup triggers'
assert_contains docs/language.ds 'defer {' 'language catalog documents defer'
assert_contains docs/language.ds 'trap "EXIT"' 'language catalog documents trap EXIT'
assert_contains docs/language.ds 'function-local variables' 'language catalog documents local capture rejection'

# Syntax shape and formatter checks without exercising real OS signals.
write_fixture "$FIX/shape.ds" <<'DS'
defer {
  echo cleanup
}

defer {
  echo explicit
}

trap "EXIT" {
  echo trap
}
DS
run_ok shape_tokens "$DS" tokens "$FIX/shape.ds"
assert_contains "$TMP/shape_tokens.out" 'DEFER' 'tokens include defer keyword'
assert_contains "$TMP/shape_tokens.out" 'TRAP' 'tokens include trap keyword'
run_ok shape_ast "$DS" ast "$FIX/shape.ds"
assert_contains "$TMP/shape_ast.out" 'DeferStmt' 'AST includes defer handler'
assert_contains "$TMP/shape_ast.out" 'TrapStmt' 'AST includes trap handler'
run_ok shape_hir "$DS" hir "$FIX/shape.ds"
assert_contains "$TMP/shape_hir.out" 'Defer EXIT' 'HIR includes defer handler'
assert_contains "$TMP/shape_hir.out" 'Trap EXIT' 'HIR includes trap handler'
run_ok shape_bytecode "$DS" bytecode "$FIX/shape.ds"
assert_matches "$TMP/shape_bytecode.out" 'REGISTER_HANDLER|handler' 'bytecode exposes handler registration'
run_ok shape_fmt_check "$DS" fmt --check "$FIX/shape.ds"

write_fixture "$FIX/unformatted.ds" <<'DS'
defer{echo cleanup}

defer on:"EXIT"{echo explicit}

trap "EXIT"{echo trap}
DS
run_fail unformatted_fmt_check "$DS" fmt --check "$FIX/unformatted.ds"
run_ok unformatted_fmt_write "$DS" fmt --write "$FIX/unformatted.ds"
assert_same "$FIX/shape.ds" "$FIX/unformatted.ds" 'formatter normalizes cleanup handler syntax'

# Deterministic VM/Bash cleanup core parity.
write_fixture "$FIX/plain_exit.ds" <<'DS'
echo start

defer {
  echo cleanup
}

echo end
DS
assert_parity plain_exit "$FIX/plain_exit.ds" 0 $'start\nend\ncleanup\n'

write_fixture "$FIX/lifo.ds" <<'DS'
defer { echo first }
defer { echo second }
defer { echo third }
echo body
DS
assert_parity lifo "$FIX/lifo.ds" 0 $'body\nthird\nsecond\nfirst\n'

write_fixture "$FIX/defer_exit_equivalence.ds" <<'DS'
defer { echo plain }
defer on: "EXIT" { echo explicit }
echo body
DS
assert_parity defer_exit_equivalence "$FIX/defer_exit_equivalence.ds" 0 $'body\nexplicit\nplain\n'

write_fixture "$FIX/trap_exit_order.ds" <<'DS'
trap "EXIT" { echo trap }
defer { echo plain }
echo body
DS
assert_parity trap_exit_order "$FIX/trap_exit_order.ds" 0 $'body\ntrap\nplain\n'

write_fixture "$FIX/trap_exit_replacement.ds" <<'DS'
trap "EXIT" { echo first }
trap "EXIT" { echo second }
echo body
DS
assert_parity trap_exit_replacement "$FIX/trap_exit_replacement.ds" 0 $'body\nsecond\n'

write_fixture "$FIX/explicit_exit.ds" <<'DS'
defer { echo cleanup }
echo before
exit 7
echo after
DS
assert_parity explicit_exit "$FIX/explicit_exit.ds" 7 $'before\ncleanup\n'
assert_not_contains "$TMP/explicit_exit_vm.out" 'after' 'VM exit skips following statements'
assert_not_contains "$TMP/explicit_exit_bash.out" 'after' 'Bash exit skips following statements'

write_fixture "$FIX/explicit_fail.ds" <<'DS'
defer { echo cleanup }
echo before
fail "boom"
echo after
DS
assert_parity explicit_fail "$FIX/explicit_fail.ds" 1 $'before\ncleanup\n'
assert_contains "$TMP/explicit_fail_vm.err" 'boom' 'VM fail reports message'
assert_contains "$TMP/explicit_fail_bash.err" 'boom' 'Bash fail reports message'
assert_not_contains "$TMP/explicit_fail_vm.out" 'after' 'VM fail skips following statements'
assert_not_contains "$TMP/explicit_fail_bash.out" 'after' 'Bash fail skips following statements'

write_fixture "$FIX/command_failure.ds" <<'DS'
defer { echo cleanup }
echo before
__definitely_missing_ds_command_v0_22__
echo after
DS
assert_parity command_failure "$FIX/command_failure.ds" 127 $'before\ncleanup\n'
assert_contains "$TMP/command_failure_vm.err" '__definitely_missing_ds_command_v0_22__' 'VM command failure names command'
assert_contains "$TMP/command_failure_bash.err" '__definitely_missing_ds_command_v0_22__' 'Bash command failure names command'
assert_not_contains "$TMP/command_failure_vm.out" 'after' 'VM command failure skips following statements'
assert_not_contains "$TMP/command_failure_bash.out" 'after' 'Bash command failure skips following statements'

write_fixture "$FIX/captured_failure.ds" <<'DS'
defer { echo cleanup }
let result = run __definitely_missing_ds_command_v0_22__
echo "failed={result.failed}"
DS
assert_parity captured_failure "$FIX/captured_failure.ds" 0 $'failed=true\ncleanup\n'

write_fixture "$FIX/handler_failure_continues.ds" <<'DS'
defer { echo older }
defer { __definitely_missing_cleanup_command_v0_22__ }
defer { echo newer }
echo body
DS
assert_parity handler_failure_continues "$FIX/handler_failure_continues.ds" 127 $'body\nnewer\nolder\n'
assert_contains "$TMP/handler_failure_continues_vm.err" '__definitely_missing_cleanup_command_v0_22__' 'VM handler failure names command'
assert_contains "$TMP/handler_failure_continues_bash.err" '__definitely_missing_cleanup_command_v0_22__' 'Bash handler failure names command'

write_fixture "$FIX/handler_exit_continues.ds" <<'DS'
defer { echo first }
defer { exit 8 }
defer { echo third }
echo body
DS
assert_parity handler_exit_continues "$FIX/handler_exit_continues.ds" 8 $'body\nthird\nfirst\n'

write_fixture "$FIX/handler_exit_zero_override.ds" <<'DS'
defer { exit 0 }
echo body
exit 7
DS
assert_parity handler_exit_zero_override "$FIX/handler_exit_zero_override.ds" 0 $'body\n'

write_fixture "$FIX/normal_ignores_int_term.ds" <<'DS'
defer on: "INT" { echo int }
defer on: "TERM" { echo term }
defer { echo exit }
echo body
DS
assert_parity normal_ignores_int_term "$FIX/normal_ignores_int_term.ds" 0 $'body\nexit\n'
assert_not_contains "$TMP/normal_ignores_int_term_vm.out" 'int' 'VM normal exit skips INT handler'
assert_not_contains "$TMP/normal_ignores_int_term_bash.out" 'term' 'Bash normal exit skips TERM handler'

write_fixture "$FIX/function_call_from_handler.ds" <<'DS'
fn cleanup(label = "done") {
  echo "cleanup={label}"
}

defer { cleanup("ok") }
echo body
DS
assert_parity function_call_from_handler "$FIX/function_call_from_handler.ds" 0 $'body\ncleanup=ok\n'

write_fixture "$FIX/script_arg_cleanup.ds" <<'DS'
script {
  arg name: string
}

defer { echo "cleanup={name}" }
echo "body={name}"
DS
assert_parity script_arg_cleanup "$FIX/script_arg_cleanup.ds" 0 $'body=api\ncleanup=api\n' api

write_fixture "$FIX/import_lib.ds" <<'DS'
defer { echo lib-cleanup }
DS
write_fixture "$FIX/import_main.ds" <<'DS'
import "./import_lib.ds"
defer { echo main-cleanup }
echo body
DS
assert_parity import_cleanup "$FIX/import_main.ds" 0 $'body\nmain-cleanup\nlib-cleanup\n'

# Unsupported deterministic diagnostics.
write_fixture "$FIX/unsupported_signal.ds" <<'DS'
defer on: "HUP" { echo nope }
DS
assert_check_fails unsupported_signal "$FIX/unsupported_signal.ds" 'unsupported defer on: signal `HUP`'
assert_emit_fails unsupported_signal "$FIX/unsupported_signal.ds" 'unsupported defer on: signal `HUP`'

write_fixture "$FIX/return_in_handler.ds" <<'DS'
fn setup() {
  defer {
    return 1
  }
}

setup()
DS
assert_check_fails return_in_handler "$FIX/return_in_handler.ds" '`return` from a cleanup handler is not supported'
assert_emit_fails return_in_handler "$FIX/return_in_handler.ds" '`return` from a cleanup handler is not supported'

write_fixture "$FIX/function_local_capture.ds" <<'DS'
fn setup() {
  let msg = "local"
  defer { echo "cleanup={msg}" }
}

setup()
echo body
DS
assert_check_fails function_local_capture "$FIX/function_local_capture.ds" 'cleanup handler captures function-local variable `msg`'
assert_emit_fails function_local_capture "$FIX/function_local_capture.ds" 'cleanup handler captures function-local variable `msg`'

# Emission internals: helpers are emitted only when needed, and control commands
# route through the cleanup-aware Bash helpers when handlers exist.
write_fixture "$FIX/no_handlers.ds" <<'DS'
echo body
DS
run_ok no_handlers_emit "$DS" emit bash "$FIX/no_handlers.ds" -o "$TMP/no_handlers.sh"
assert_not_contains "$TMP/no_handlers.sh" '__ds_run_cleanup' 'no cleanup helper emitted without handlers'
assert_not_matches "$TMP/no_handlers.sh" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' 'no-handler emitted Bash does not call ds'

run_ok explicit_fail_emit_again "$DS" emit bash "$FIX/explicit_fail.ds" -o "$TMP/explicit_fail_again.sh"
assert_contains "$TMP/explicit_fail_again.sh" '__ds_control_fail' 'fail emits cleanup-aware helper'
assert_not_matches "$TMP/explicit_fail_again.sh" '^[[:space:]]*fail([[:space:]]|$)' 'fail is not emitted as external shell command'
assert_contains "$TMP/explicit_fail_again.sh" '__ds_run_cleanup' 'cleanup helper emitted with handlers'

printf 'v0.22 deterministic cleanup tests passed (%d checks)\n' "$pass_count"
