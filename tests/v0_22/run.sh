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

run_and_signal() {
  local name="$1" mode="$2" signal="$3" fixture="$4" expected_status="$5" expected_stdout="$6"
  local out="$TMP/${name}.out"
  local err="$TMP/${name}.err"
  local rc_file="$TMP/${name}.rc"
  local script="$TMP/${name}.sh"
  local run_dir
  local pid=""
  local ready=0
  local done=0
  local stat=""

  : >"$out"
  : >"$err"
  run_dir="$(dirname "$fixture")"

  case "$mode" in
    vm)
      setsid bash -c 'cd "$1" && exec "$2" run "$3"' _ "$run_dir" "$DS" "$fixture" >"$out" 2>"$err" &
      pid=$!
      ;;
    bash)
      run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
      run_ok "${name}_bash_n" bash -n "$script"
      assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
      setsid bash -c 'cd "$1" && exec bash "$2"' _ "$run_dir" "$script" >"$out" 2>"$err" &
      pid=$!
      ;;
    *)
      fail "$name unknown signal harness mode: $mode"
      ;;
  esac

  # Signal tests never use self-signaling fixtures: the harness owns process
  # lifetime. It waits for an explicit marker, then signals the isolated process
  # group so the foreground child and the ds/Bash runner observe the same event.
  for _ in $(seq 1 200); do
    if grep -qx 'ready' "$out"; then
      ready=1
      break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      break
    fi
    sleep 0.05
  done

  if [ "$ready" -ne 1 ]; then
    kill -KILL -- "-$pid" 2>/dev/null || true
    set +e
    wait "$pid" 2>/dev/null
    set -e
    echo "--- $out" >&2
    cat "$out" >&2 || true
    echo "--- $err" >&2
    cat "$err" >&2 || true
    fail "$name did not print ready before signal"
  fi

  if ! kill -"$signal" -- "-$pid" 2>/dev/null; then
    echo "--- $out" >&2
    cat "$out" >&2 || true
    echo "--- $err" >&2
    cat "$err" >&2 || true
    fail "$name could not signal process group"
  fi

  for _ in $(seq 1 200); do
    if ! stat=$(ps -p "$pid" -o stat= 2>/dev/null); then
      done=1
      break
    fi
    case "$stat" in
      *Z*) done=1; break ;;
    esac
    sleep 0.05
  done

  if [ "$done" -ne 1 ]; then
    kill -KILL -- "-$pid" 2>/dev/null || true
    set +e
    wait "$pid" 2>/dev/null
    set -e
    echo "--- $out" >&2
    cat "$out" >&2 || true
    echo "--- $err" >&2
    cat "$err" >&2 || true
    fail "$name did not exit after $signal"
  fi

  set +e
  wait "$pid"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$rc_file"

  assert_status "$name" "$expected_status"
  assert_same_text "$expected_stdout" "$out" "$name stdout"
  assert_same_text '' "$err" "$name stderr"
}

FIX="$TMP/fixtures with spaces"
mkdir -p "$FIX"

# Build wiring and docs for the staged v0.22 slices completed so far.
assert_contains Makefile '0-22' 'TEST_VERSIONS contains v0.22'
assert_matches Makefile '^TEST_VERSIONS := .*0-21 0-22($| )' 'v0.22 follows v0.21 in TEST_VERSIONS'
assert_contains docs/roadmap.md 'v0.22.1 — Cleanup Core Test Stabilization' 'roadmap names v0.22.1 slice'
assert_contains docs/roadmap.md 'No real `SIGINT`/`SIGTERM` delivery tests' 'roadmap excludes real signals for v0.22.1'
assert_contains docs/roadmap.md 'v0.22.2 — Signal Syntax and Diagnostic Surface' 'roadmap names v0.22.2 slice'
assert_contains docs/roadmap.md 'No claim that foreground child commands are interrupted reliably yet' 'roadmap bounds v0.22.2 runtime claims'
assert_contains docs/runtime.md 'Handler registration is process-scope' 'runtime docs document process-scope handlers'
assert_contains docs/runtime.md 'Cleanup runs for normal completion' 'runtime docs document cleanup triggers'
assert_contains docs/runtime.md 'Supported signal names are the string literals `"EXIT"`, `"INT"`, and `"TERM"`' 'runtime docs list supported signal literals'
assert_contains docs/language.ds 'defer {' 'language catalog documents defer'
assert_contains docs/language.ds 'trap "EXIT"' 'language catalog documents trap EXIT'
assert_contains docs/language.ds 'trap "INT"' 'language catalog documents trap INT'
assert_contains docs/language.ds 'trap "TERM"' 'language catalog documents trap TERM'
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

# Signal syntax and diagnostic surface for v0.22.2. These tests intentionally
# avoid real OS signal delivery; they prove the user-facing INT/TERM language
# surface, diagnostics, and emitted-helper structure only.
write_fixture "$FIX/signal_shape.ds" <<'DS'
defer on: "INT" {
  echo int-defer
}

defer on: "TERM" {
  echo term-defer
}

trap "INT" {
  echo int-trap-1
}

trap "INT" {
  echo int-trap-2
}

trap "TERM" {
  echo term-trap
}

trap "EXIT" {
  echo exit-trap
}

echo body
DS
run_ok signal_shape_tokens "$DS" tokens "$FIX/signal_shape.ds"
assert_contains "$TMP/signal_shape_tokens.out" 'STRING         "\"INT\""' 'tokens include INT signal literal'
assert_contains "$TMP/signal_shape_tokens.out" 'STRING         "\"TERM\""' 'tokens include TERM signal literal'
run_ok signal_shape_ast "$DS" ast "$FIX/signal_shape.ds"
assert_contains "$TMP/signal_shape_ast.out" 'DeferStmt INT' 'AST includes INT defer handler'
assert_contains "$TMP/signal_shape_ast.out" 'DeferStmt TERM' 'AST includes TERM defer handler'
assert_contains "$TMP/signal_shape_ast.out" 'TrapStmt INT' 'AST includes INT trap handler'
assert_contains "$TMP/signal_shape_ast.out" 'TrapStmt TERM' 'AST includes TERM trap handler'
run_ok signal_shape_hir "$DS" hir "$FIX/signal_shape.ds"
assert_contains "$TMP/signal_shape_hir.out" 'Defer INT' 'HIR includes INT defer handler'
assert_contains "$TMP/signal_shape_hir.out" 'Defer TERM' 'HIR includes TERM defer handler'
assert_contains "$TMP/signal_shape_hir.out" 'Trap INT' 'HIR includes INT trap handler'
assert_contains "$TMP/signal_shape_hir.out" 'Trap TERM' 'HIR includes TERM trap handler'
run_ok signal_shape_bytecode "$DS" bytecode "$FIX/signal_shape.ds"
assert_contains "$TMP/signal_shape_bytecode.out" 'REGISTER_HANDLER defer INT' 'bytecode registers INT defer handler'
assert_contains "$TMP/signal_shape_bytecode.out" 'REGISTER_HANDLER defer TERM' 'bytecode registers TERM defer handler'
assert_contains "$TMP/signal_shape_bytecode.out" 'REGISTER_HANDLER trap INT' 'bytecode registers INT trap handler'
assert_contains "$TMP/signal_shape_bytecode.out" 'REGISTER_HANDLER trap TERM' 'bytecode registers TERM trap handler'
run_ok signal_shape_fmt_check "$DS" fmt --check "$FIX/signal_shape.ds"

write_fixture "$FIX/unformatted_signal.ds" <<'DS'
defer on:"INT"{echo int-defer}

defer on:"TERM"{echo term-defer}

trap "INT"{echo int-trap-1}

trap "INT"{echo int-trap-2}

trap "TERM"{echo term-trap}

trap "EXIT"{echo exit-trap}

echo body
DS
run_fail unformatted_signal_fmt_check "$DS" fmt --check "$FIX/unformatted_signal.ds"
run_ok unformatted_signal_fmt_write "$DS" fmt --write "$FIX/unformatted_signal.ds"
assert_same "$FIX/signal_shape.ds" "$FIX/unformatted_signal.ds" 'formatter normalizes INT/TERM handler syntax'

run_ok signal_shape_emit "$DS" emit bash "$FIX/signal_shape.ds" -o "$TMP/signal_shape.sh"
run_ok signal_shape_bash_n bash -n "$TMP/signal_shape.sh"
assert_contains "$TMP/signal_shape.sh" 'declare -a __ds_defer_EXIT=() __ds_defer_INT=() __ds_defer_TERM=()' 'Bash helper declares signal defer stacks'
assert_contains "$TMP/signal_shape.sh" 'trap '\''__ds_run_signal INT 130'\'' INT' 'Bash helper installs INT trap'
assert_contains "$TMP/signal_shape.sh" 'trap '\''__ds_run_signal TERM 143'\'' TERM' 'Bash helper installs TERM trap'
assert_contains "$TMP/signal_shape.sh" '__ds_defer_INT+=' 'Bash emission registers INT defer stack entry'
assert_contains "$TMP/signal_shape.sh" '__ds_defer_TERM+=' 'Bash emission registers TERM defer stack entry'
assert_contains "$TMP/signal_shape.sh" '__ds_trap_INT=' 'Bash emission registers INT trap replacement slot'
assert_contains "$TMP/signal_shape.sh" '__ds_trap_TERM=' 'Bash emission registers TERM trap replacement slot'
assert_matches "$TMP/signal_shape.sh" '^__ds_trap_INT=__ds_handler_[0-9]+$' 'Bash emission has INT trap assignment'
if [ "$(grep -c '^__ds_trap_INT=__ds_handler_' "$TMP/signal_shape.sh")" -ne 2 ]; then
  fail 'Bash emission keeps two INT trap assignments so later replacement is visible'
fi
pass 'Bash emission keeps two INT trap assignments so later replacement is visible'
assert_not_matches "$TMP/signal_shape.sh" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' 'signal emitted Bash does not call ds'

# v0.22.3 deterministic signal harness. This slice adds the reusable harness
# and only the smallest direct-command TERM fixture for each backend. Broader
# INT/TERM matrices, pipelines, and process-tree semantics are later slices.
assert_contains docs/roadmap.md 'v0.22.3 — Deterministic Signal Harness' 'roadmap names v0.22.3 slice'
assert_contains docs/roadmap.md 'signal the process group' 'roadmap documents process-group signal harnessing'

write_fixture "$FIX/term_direct_command.ds" <<'DS'
trap "TERM" {
  echo term-trap
}

defer {
  echo exit-defer
}

trap "EXIT" {
  echo exit-trap
}

./ready_sleep
echo after
DS
cat >"$FIX/ready_sleep" <<'SH'
#!/usr/bin/env bash
child=''
trap 'if [ -n "$child" ]; then kill "$child" 2>/dev/null || true; wait "$child" 2>/dev/null || true; fi; exit 143' TERM
echo ready
sleep 30 &
child=$!
wait "$child" 2>/dev/null || exit $?
echo child-after
SH
chmod +x "$FIX/ready_sleep"
run_and_signal signal_vm_term_direct vm TERM "$FIX/term_direct_command.ds" 143 $'ready\nterm-trap\nexit-trap\nexit-defer\n'
run_and_signal signal_bash_term_direct bash TERM "$FIX/term_direct_command.ds" 143 $'ready\nterm-trap\nexit-trap\nexit-defer\n'

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

write_fixture "$FIX/unsupported_trap_signal.ds" <<'DS'
trap "HUP" { echo nope }
DS
assert_check_fails unsupported_trap_signal "$FIX/unsupported_trap_signal.ds" 'unsupported trap signal `HUP`'
assert_emit_fails unsupported_trap_signal "$FIX/unsupported_trap_signal.ds" 'unsupported trap signal `HUP`'

write_fixture "$FIX/lowercase_signal.ds" <<'DS'
defer on: "int" { echo nope }
DS
assert_check_fails lowercase_signal "$FIX/lowercase_signal.ds" 'unsupported defer on: signal `int`'
assert_emit_fails lowercase_signal "$FIX/lowercase_signal.ds" 'unsupported defer on: signal `int`'

write_fixture "$FIX/missing_signal_string.ds" <<'DS'
defer on: INT { echo nope }
DS
run_fail missing_signal_string_check "$DS" check "$FIX/missing_signal_string.ds"
assert_diag "$TMP/missing_signal_string_check.err" 'expected signal string after `defer on:`' 'missing signal string diagnostic'

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
