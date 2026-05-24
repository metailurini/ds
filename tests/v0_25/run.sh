#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_25_tests.$$"
FIX="$TMP/fixtures"
mkdir -p "$TMP" "$FIX"
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

assert_parity() {
  local name="$1" fixture="$2" expected_status="$3" expected_stdout="$4"; shift 4
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"

  run_ok "${name}_check" "$DS" check "$fixture"
  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same_text "$expected_stdout" "$TMP/${name}_vm.out" "$name VM stdout"
  assert_same_text "$expected_stdout" "$TMP/${name}_bash.out" "$name Bash stdout"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  else
    pass "$name non-zero stderr checked by focused assertions when required"
  fi
}

assert_parity_same() {
  local name="$1" fixture="$2" expected_status="$3"; shift 3
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"

  run_ok "${name}_check" "$DS" check "$fixture"
  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  else
    pass "$name non-zero stderr checked by focused assertions when required"
  fi
}

assert_parity_file() {
  local name="$1" fixture="$2" expected_status="$3" expected_stdout="$4" rel_file="$5" expected_file_text="$6"; shift 6
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"

  run_ok "${name}_check" "$DS" check "$fixture"
  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same_text "$expected_stdout" "$TMP/${name}_vm.out" "$name VM stdout"
  assert_same_text "$expected_stdout" "$TMP/${name}_bash.out" "$name Bash stdout"
  assert_same_text "$expected_file_text" "$vm_work/$rel_file" "$name VM side effect"
  assert_same_text "$expected_file_text" "$bash_work/$rel_file" "$name Bash side effect"
  assert_same "$vm_work/$rel_file" "$bash_work/$rel_file" "$name side-effect parity"
}

# 1. Scalar return preservation.
write_fixture "$FIX/string_return.ds" <<'DS'
fn name() {
  return "api"
}

let value = name()
echo "value={value}"
DS
assert_parity string_return "$FIX/string_return.ds" 0 $'value=api\n'
grep -F '__ds_call_value' "$TMP/string_return.sh" >/dev/null || fail 'string_return helper missing'
[ "$(grep -c '^__ds_call_value_capture()' "$TMP/string_return.sh")" = 1 ] || fail 'string_return emits capture helper once'
pass 'string_return value-return helper emitted once'
assert_not_contains "$TMP/string_return.sh" '$(ds ' 'string_return emitted Bash has no ds command substitution'

write_fixture "$FIX/empty_string_return.ds" <<'DS'
fn empty() {
  return ""
}

let value = empty()
echo "before[{value}]after"
DS
assert_parity empty_string_return "$FIX/empty_string_return.ds" 0 $'before[]after\n'

write_fixture "$FIX/whitespace_string_return.ds" <<'DS'
fn padded() {
  return "  api  "
}

let value = padded()
echo "[{value}]"
DS
assert_parity whitespace_string_return "$FIX/whitespace_string_return.ds" 0 $'[  api  ]\n'

write_fixture "$FIX/shell_metachar_string_return.ds" <<'DS'
fn weird() {
  return "a b; rm -rf nope; $(echo bad) * ? [x]"
}

let value = weird()
echo "{value}"
DS
assert_parity shell_metachar_string_return "$FIX/shell_metachar_string_return.ds" 0 $'a b; rm -rf nope; $(echo bad) * ? [x]\n'
[ ! -e "$TMP/shell_metachar_string_return_vm_work/nope" ] || fail 'VM should not create nope from metachar return'
[ ! -e "$TMP/shell_metachar_string_return_bash_work/nope" ] || fail 'Bash should not create nope from metachar return'
pass 'shell metacharacters are returned literally'

write_fixture "$FIX/newline_string_return.ds" <<'DS'
fn block() {
  return """one
two
"""
}

let value = block()
echo "[{value}]"
DS
assert_parity newline_string_return "$FIX/newline_string_return.ds" 0 $'[one\ntwo\n]\n'
assert_not_matches "$TMP/newline_string_return.sh" '\$\(__ds_call_value '\''string'\''' 'newline let call avoids command substitution'

write_fixture "$FIX/newline_string_equality.ds" <<'DS'
fn block() {
  return """api
"""
}

if block() == """api
""" {
  echo "matched"
} else {
  echo "miss"
}
DS
assert_parity newline_string_equality "$FIX/newline_string_equality.ds" 0 $'matched\n'
assert_not_matches "$TMP/newline_string_equality.sh" '\[\[ "\$\(__ds_call_value '\''string'\''' 'newline equality call avoids condition command substitution'

write_fixture "$FIX/newline_string_case_selector.ds" <<'DS'
fn block() {
  return """api
"""
}

case block() {
  """api
""" { echo "matched" }
  _ { echo "miss" }
}
DS
assert_parity newline_string_case_selector "$FIX/newline_string_case_selector.ds" 0 $'matched\n'
assert_not_matches "$TMP/newline_string_case_selector.sh" '\$\(__ds_call_value '\''string'\''' 'newline case selector avoids command substitution'

write_fixture "$FIX/newline_string_function_argument.ds" <<'DS'
fn block() {
  return """api
"""
}

fn echo_arg(x = "") {
  return x
}

let value = echo_arg(block())
echo "[{value}]"
DS
assert_parity newline_string_function_argument "$FIX/newline_string_function_argument.ds" 0 $'[api\n]\n'
assert_not_matches "$TMP/newline_string_function_argument.sh" 'echo_arg "\$\(__ds_call_value '\''string'\''' 'newline user-function argument is pre-materialized'

write_fixture "$FIX/int_return.ds" <<'DS'
fn answer() {
  return 42
}

let value = answer()
echo "n={value}"
DS
assert_parity int_return "$FIX/int_return.ds" 0 $'n=42\n'

write_fixture "$FIX/int_arithmetic_return.ds" <<'DS'
fn two() {
  return 2
}

let value = two() * 5
echo "{value}"
DS
assert_parity int_arithmetic_return "$FIX/int_arithmetic_return.ds" 0 $'10\n'

write_fixture "$FIX/bool_true_return.ds" <<'DS'
fn enabled() {
  return true
}

if enabled() {
  echo "yes"
} else {
  echo "no"
}
DS
assert_parity bool_true_return "$FIX/bool_true_return.ds" 0 $'yes\n'

write_fixture "$FIX/bool_false_return.ds" <<'DS'
fn enabled() {
  return false
}

if enabled() {
  echo "yes"
} else {
  echo "no"
}
DS
assert_parity bool_false_return "$FIX/bool_false_return.ds" 0 $'no\n'

write_fixture "$FIX/null_return_rejected.ds" <<'DS'
fn none() {
  return null
}

let value = none()
echo "{value}"
DS
assert_check_fails null_return_rejected "$FIX/null_return_rejected.ds" 'unknown variable `null`'
assert_emit_fails null_return_rejected "$FIX/null_return_rejected.ds" 'unknown variable `null`'

# 2. Return kind preservation.
write_fixture "$FIX/string_int_kinds.ds" <<'DS'
fn as_string() {
  return "2"
}

fn as_int() {
  return 2
}

let s = as_string()
let n = as_int()

if s == "2" {
  echo "string-ok"
}

if n == 2 {
  echo "int-ok"
}
DS
assert_parity string_int_kinds "$FIX/string_int_kinds.ds" 0 $'string-ok\nint-ok\n'

write_fixture "$FIX/string_not_int.ds" <<'DS'
fn as_string() {
  return "2"
}

let n = as_string() + 1
echo "{n}"
DS
assert_check_fails string_not_int "$FIX/string_not_int.ds" 'operator `+` supports integer operands'

write_fixture "$FIX/bool_string_kinds.ds" <<'DS'
fn as_bool() {
  return true
}

fn as_string() {
  return "true"
}

if as_bool() {
  echo "bool-ok"
}

let value = as_string()
echo "string={value}"
DS
assert_parity bool_string_kinds "$FIX/bool_string_kinds.ds" 0 $'bool-ok\nstring=true\n'

write_fixture "$FIX/sidecar_membership.ds" <<'DS'
fn app() {
  return "api"
}

fn port() {
  return 8080
}

fn ok() {
  return true
}

let known = app() in ["api", "web"]
let port_ok = port() in [8080, 9090]
let bool_ok = ok() in [true]
let string_int_mismatch = app() in [8080]

if known && port_ok && bool_ok && !string_int_mismatch {
  echo "all-ok"
}
DS
assert_parity sidecar_membership "$FIX/sidecar_membership.ds" 0 $'all-ok\n'

# 3. Function return composition.
write_fixture "$FIX/nested_return.ds" <<'DS'
fn base() {
  return "api"
}

fn wrapper() {
  return base()
}

let value = wrapper()
echo "{value}"
DS
assert_parity nested_return "$FIX/nested_return.ds" 0 $'api\n'
assert_not_matches "$TMP/nested_return.sh" 'return base' 'nested return lowered to helper protocol'

write_fixture "$FIX/conditional_returns.ds" <<'DS'
fn classify(name = "api") {
  if name == "api" {
    return "service"
  }
  return "worker"
}

let a = classify("api")
let b = classify("job")
echo "{a}"
echo "{b}"
DS
assert_parity conditional_returns "$FIX/conditional_returns.ds" 0 $'service\nworker\n'

write_fixture "$FIX/loop_early_return.ds" <<'DS'
fn first_match() {
  let names = ["web", "api", "job"]
  for name in names {
    if name == "api" {
      return name
    }
  }
  return "missing"
}

let value = first_match()
echo "{value}"
DS
assert_parity loop_early_return "$FIX/loop_early_return.ds" 0 $'api\n'

write_fixture "$FIX/imports/lib.ds" <<'DS'
fn service_name() {
  return "api"
}
DS
write_fixture "$FIX/imports/main.ds" <<'DS'
import "./lib.ds"

let value = service_name()
echo "service={value}"
DS
assert_parity import_scalar_return "$FIX/imports/main.ds" 0 $'service=api\n'
[ "$(grep -c '^__ds_call_value_capture()' "$TMP/import_scalar_return.sh")" = 1 ] || fail 'import scalar return emits helper once'
pass 'import scalar return helper emitted once'

write_fixture "$FIX/test_block_scalar_return.ds" <<'DS'
fn value() {
  return 3
}

test "scalar return" {
  assert value() == 3
}
DS
run_ok test_block_scalar_return "$DS" test "$FIX/test_block_scalar_return.ds"
assert_contains "$TMP/test_block_scalar_return.out" 'ok' 'test block scalar return passes'
run_ok test_block_scalar_return_check "$DS" check "$FIX/test_block_scalar_return.ds"

# 4. Output/effect function behavior.
write_fixture "$FIX/output_statement_function.ds" <<'DS'
fn show(name = "api") {
  echo "deploying {name}"
}

show("web")
DS
assert_parity output_statement_function "$FIX/output_statement_function.ds" 0 $'deploying web\n'
assert_not_contains "$TMP/output_statement_function_bash.out" '__ds_return_' 'output statement function leaks no payload'

write_fixture "$FIX/output_function_as_value.ds" <<'DS'
fn show(name = "api") {
  echo "deploying {name}"
}

let value = show("web")
DS
assert_check_fails output_function_as_value "$FIX/output_function_as_value.ds" 'does not return a value'
assert_emit_fails output_function_as_value "$FIX/output_function_as_value.ds" 'does not return a value'

write_fixture "$FIX/stdout_plus_return_rejected.ds" <<'DS'
fn bad(name = "api") {
  echo "debug"
  return name
}

let value = bad("web")
echo "{value}"
DS
assert_check_fails stdout_plus_return_rejected "$FIX/stdout_plus_return_rejected.ds" 'plain command statements in v0.25.0'
assert_emit_fails stdout_plus_return_rejected "$FIX/stdout_plus_return_rejected.ds" 'plain command statements in v0.25.0'

write_fixture "$FIX/redirected_stdout_rejected.ds" <<'DS'
fn ok(name = "api") {
  echo "debug" !> "debug.log"
  return name
}

let value = ok("web")
echo "{value}"
DS
assert_check_fails redirected_stdout_rejected "$FIX/redirected_stdout_rejected.ds" 'plain command statements in v0.25.0'

# 5. Failure and status behavior.
write_fixture "$FIX/fail_before_return.ds" <<'DS'
fn bad() {
  fail "no value"
  return "unreachable"
}

let value = bad()
echo "{value}"
DS
assert_check_fails fail_before_return "$FIX/fail_before_return.ds" 'plain command statements in v0.25.0'

write_fixture "$FIX/command_failure_before_return.ds" <<'DS'
fn bad() {
  false
  return "unreachable"
}

let value = bad()
echo "{value}"
DS
assert_check_fails command_failure_before_return "$FIX/command_failure_before_return.ds" 'plain command statements in v0.25.0'

# 6. Unsupported/deferred return values.
write_fixture "$FIX/array_return_rejected.ds" <<'DS'
fn names() {
  return ["api", "web"]
}

let value = names()
DS
assert_check_fails array_return_rejected "$FIX/array_return_rejected.ds" 'deferred to v0.26.0'
assert_emit_fails array_return_rejected "$FIX/array_return_rejected.ds" 'deferred to v0.26.0'

write_fixture "$FIX/map_return_rejected.ds" <<'DS'
fn user() {
  return { name: "Ana" }
}

let value = user()
DS
assert_check_fails map_return_rejected "$FIX/map_return_rejected.ds" 'deferred to v0.26.0'

write_fixture "$FIX/command_result_return_rejected.ds" <<'DS'
fn search() {
  return run grep "x" "file.txt"
}

let result = search()
DS
assert_check_fails command_result_return_rejected "$FIX/command_result_return_rejected.ds" 'deferred to v0.26.0'

write_fixture "$FIX/mixed_scalar_collection_rejected.ds" <<'DS'
fn bad(flag = true) {
  if flag {
    return "api"
  }
  return ["api"]
}

let value = bad(false)
DS
assert_check_fails mixed_scalar_collection_rejected "$FIX/mixed_scalar_collection_rejected.ds" 'deferred to v0.26.0'

# 7. Existing scalar diagnostics stay correct.
write_fixture "$FIX/mixed_scalar_kinds_rejected.ds" <<'DS'
fn bad(flag = true) {
  if flag {
    return "api"
  }
  return 1
}

let value = bad(false)
DS
assert_check_fails mixed_scalar_kinds_rejected "$FIX/mixed_scalar_kinds_rejected.ds" 'same value kind'

write_fixture "$FIX/missing_return_path_rejected.ds" <<'DS'
fn maybe(flag = true) {
  if flag {
    return "api"
  }
}

let value = maybe(false)
DS
assert_check_fails missing_return_path_rejected "$FIX/missing_return_path_rejected.ds" 'not all control paths return'

write_fixture "$FIX/forward_function_call.ds" <<'DS'
fn a() {
  return b()
}

fn b() {
  return 1
}

let value = a()
echo "{value}"
DS
assert_parity forward_function_call "$FIX/forward_function_call.ds" 0 $'1\n'

# 8. Bash emission helper tests.
write_fixture "$FIX/no_value_helpers.ds" <<'DS'
echo "hello"
DS
run_ok no_value_helpers_emit "$DS" emit bash "$FIX/no_value_helpers.ds" -o "$TMP/no_value_helpers.sh"
run_ok no_value_helpers_bash_n bash -n "$TMP/no_value_helpers.sh"
assert_not_contains "$TMP/no_value_helpers.sh" '__ds_call_value_capture' 'no value-return helper when no value functions are used'
assert_not_contains "$TMP/no_value_helpers.sh" '__ds_return_type' 'no value-return state when no value functions are used'

write_fixture "$FIX/multiple_value_helpers.ds" <<'DS'
fn a() {
  return "a"
}

fn b() {
  return "b"
}

let x = a()
let y = b()
echo "{x}{y}"
DS
assert_parity multiple_value_helpers "$FIX/multiple_value_helpers.ds" 0 $'ab\n'
[ "$(grep -c '^__ds_call_value_capture()' "$TMP/multiple_value_helpers.sh")" = 1 ] || fail 'multiple value functions emit capture helper once'
[ "$(grep -c '^__ds_call_value_into()' "$TMP/multiple_value_helpers.sh")" = 1 ] || fail 'multiple value functions emit into helper once'
pass 'multiple value helpers emitted once'

write_fixture "$FIX/no_payload_leak.ds" <<'DS'
fn value() {
  return "api"
}

let v = value()
echo "user:{v}"
DS
assert_parity no_payload_leak "$FIX/no_payload_leak.ds" 0 $'user:api\n'
assert_not_contains "$TMP/no_payload_leak_bash.out" '__ds_return_' 'payload marker not visible on stdout'
assert_not_contains "$TMP/no_payload_leak_bash.out" '__ds_call_value' 'helper text not visible on stdout'

cp "$TMP/no_payload_leak.sh" "$TMP/malformed_payload_guard.sh"
cat >>"$TMP/malformed_payload_guard.sh" <<'BASH'
__ds_bad_bool_payload() {
  __ds_return_type=bool
  __ds_return_value=maybe
}
__ds_call_value_into __ds_payload_guard bool __ds_bad_bool_payload
BASH
run_fail malformed_payload_guard bash "$TMP/malformed_payload_guard.sh"
assert_contains "$TMP/malformed_payload_guard.err" 'invalid internal bool function return payload' 'malformed internal bool payload is rejected'

# 9. Parser, HIR, bytecode, and debug views.
run_ok hir_scalar_return "$DS" hir "$FIX/string_return.ds"
assert_contains "$TMP/hir_scalar_return.out" 'Function' 'HIR has scalar function structure'
run_ok bytecode_scalar_return "$DS" bytecode "$FIX/string_return.ds"
assert_contains "$TMP/bytecode_scalar_return.out" 'RETURN_VALUE' 'bytecode has return-value instruction'
run_fail hir_array_return_rejected "$DS" hir "$FIX/array_return_rejected.ds"
assert_diag "$TMP/hir_array_return_rejected.err" 'deferred to v0.26.0' 'HIR array return diagnostic'
run_fail bytecode_command_result_return_rejected "$DS" bytecode "$FIX/command_result_return_rejected.ds"
assert_diag "$TMP/bytecode_command_result_return_rejected.err" 'deferred to v0.26.0' 'bytecode command-result return diagnostic'
assert_not_contains "$TMP/hir_scalar_return.out" '__ds_return_type' 'HIR does not expose Bash ABI details'
assert_not_contains "$TMP/bytecode_scalar_return.out" '__ds_return_type' 'bytecode does not expose Bash ABI details'

# 10. Formatter and checker tests.
run_ok fmt_scalar_return_check "$DS" fmt --check "$FIX/string_return.ds"
cp "$FIX/string_return.ds" "$TMP/fmt_write.ds"
run_ok fmt_scalar_return_write "$DS" fmt --write "$TMP/fmt_write.ds"
run_ok fmt_scalar_return_write_check "$DS" fmt --check "$TMP/fmt_write.ds"

write_fixture "$FIX/warnings_value_function.ds" <<'DS'
fn label(name = "api", unused = "x") {
  return name
}

let value = label("web")
echo "{value}"
DS
run_ok check_warning_value_function "$DS" check "$FIX/warnings_value_function.ds"
assert_contains "$TMP/check_warning_value_function.err" 'warning:' 'value function warning remains visible'
capture_status warnings_as_errors_value_function "$DS" check --warnings-as-errors "$FIX/warnings_value_function.ds"
assert_status warnings_as_errors_value_function 1
assert_contains "$TMP/warnings_as_errors_value_function.err" 'warning:' 'warnings-as-errors keeps warning text'
run_ok no_warnings_value_function "$DS" check --no-warnings "$FIX/warnings_value_function.ds"
assert_same_text '' "$TMP/no_warnings_value_function.err" 'no-warnings suppresses value function warning'

# 11. Cross-feature parity cases.
write_fixture "$FIX/args_scalar_return.ds" <<'DS'
script {
  arg app: string
  flag prod: bool = false
}

fn target_name() {
  if prod {
    return "production"
  }
  return "staging"
}

let target = target_name()
echo "{app}:{target}"
DS
assert_parity args_scalar_return "$FIX/args_scalar_return.ds" 0 $'api:production\n' api --prod

write_fixture "$FIX/regex_range_membership.ds" <<'DS'
fn app() {
  return "api"
}

fn count() {
  return 3
}

let name = app()

if name in ["api", "web"] && name matches /^a/ {
  for n in 1..count() {
    echo "{name}:{n}"
  }
}
DS
assert_parity regex_range_membership "$FIX/regex_range_membership.ds" 0 $'api:1\napi:2\napi:3\n'

write_fixture "$FIX/cleanup_scalar_return.ds" <<'DS'
fn label() {
  return "api"
}

defer {
  echo "cleanup"
}

let value = label()
echo "{value}"
DS
assert_parity cleanup_scalar_return "$FIX/cleanup_scalar_return.ds" 0 $'api\ncleanup\n'

# 13. Edge numeric/string cases.
write_fixture "$FIX/edge_scalars.ds" <<'DS'
fn zero() {
  return 0
}

fn negative() {
  return -7
}

fn large() {
  return 9223372036854775807
}

fn quoted() {
  return "quotes: \" and slash: \\"
}

let a = zero()
let b = negative()
let c = large()
let q = quoted()
echo "{a}|{b}|{c}"
echo "{q}"
DS
assert_parity edge_scalars "$FIX/edge_scalars.ds" 0 $'0|-7|9223372036854775807\nquotes: " and slash: \\\n'

# 14. Manual-smoke equivalent: emitted Bash runs away from the source directory.
away="$TMP/away"
mkdir -p "$away"
run_ok away_emit "$DS" emit bash "$FIX/newline_string_return.ds" -o "$TMP/away_newline.sh"
run_ok away_bash_n bash -n "$TMP/away_newline.sh"
capture_in_dir away_bash_run "$away" bash "$TMP/away_newline.sh"
assert_status away_bash_run 0
assert_same_text $'[one\ntwo\n]\n' "$TMP/away_bash_run.out" 'emitted Bash runs away from source directory'

capture_in_dir restricted_path_bash_run "$away" env PATH=/usr/bin:/bin bash "$TMP/away_newline.sh"
assert_status restricted_path_bash_run 0
assert_same_text $'[one\ntwo\n]\n' "$TMP/restricted_path_bash_run.out" 'emitted Bash runs without ds on PATH'

assert_contains Makefile '0-25' 'TEST_VERSIONS contains v0.25'
assert_matches Makefile '^TEST_VERSIONS := .*0-24 0-25($| )' 'v0.25 follows v0.24 in TEST_VERSIONS'

printf 'v0.25.0 tests completed: %s assertions\n' "$pass_count"
