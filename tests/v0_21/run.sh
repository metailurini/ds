#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_21_tests.$$"
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

assert_file_equals() {
  local file="$1" expected="$2" name="$3"
  local expected_file="$TMP/${name//[^A-Za-z0-9_]/_}.expected"
  printf '%s' "$expected" >"$expected_file"
  assert_same "$expected_file" "$file" "$name"
}

assert_vm_bash_parity_args() {
  local name="$1" fixture="$2" expected_status="$3" output_files="$4"; shift 4
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/$name.sh"
  mkdir -p "$vm_work" "$bash_work"

  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  else
    assert_contains "$TMP/${name}_vm.err" ': error:' "$name VM error marker"
    assert_contains "$TMP/${name}_bash.err" ': error:' "$name Bash error marker"
  fi
  assert_contains "$script" '#!/usr/bin/env bash' "$name emitted Bash shebang"
  assert_contains "$script" 'set -euo pipefail' "$name emitted Bash strict mode"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"

  local rel
  for rel in $output_files; do
    [ -f "$vm_work/$rel" ] || fail "$name VM missing side-effect file $rel"
    [ -f "$bash_work/$rel" ] || fail "$name Bash missing side-effect file $rel"
    assert_same "$vm_work/$rel" "$bash_work/$rel" "$name side-effect parity $rel"
  done
}

assert_emit_fails() {
  local name="$1" fixture="$2" fragment="$3"
  run_fail "${name}_emit" "$DS" emit bash "$fixture" -o "$TMP/${name}.sh"
  assert_diag "$TMP/${name}_emit.err" "$fragment" "$name emit diagnostic"
  [ ! -s "$TMP/${name}.sh" ] || fail "$name: emit failure should not leave a non-empty script"
  pass "$name no partial Bash"
}

assert_check_fails() {
  local name="$1" fixture="$2" fragment="$3"
  run_fail "${name}_check" "$DS" check "$fixture"
  assert_diag "$TMP/${name}_check.err" "$fragment" "$name check diagnostic"
}

FIX="$TMP/fixtures with spaces"
mkdir -p "$FIX"

# Static and build wiring tests.
assert_contains Makefile '0-21' 'TEST_VERSIONS contains v0.21'
assert_matches Makefile '^TEST_VERSIONS := .*0-20 0-21($| )' 'v0.21 follows v0.20 in TEST_VERSIONS'
assert_contains Makefile '$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address" test' 'asan includes aggregate suites'
assert_contains Makefile '$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined" test' 'ubsan includes aggregate suites'
assert_not_matches src/lexer.c 'TOKEN_(LAMBDA|YIELD|DEFER|TRAP)' 'no deferred keyword tokens added'
assert_not_matches src/parser.c 'TOKEN_(LAMBDA|YIELD|DEFER|TRAP)' 'parser has no deferred keyword handling'
assert_not_matches 'docs/language.ds' 'typed .*parameter.*implemented|return annotation.*implemented|closure.*implemented|float.*implemented' 'docs do not claim out-of-scope features'
assert_contains docs/runtime.md 'standalone Bash' 'runtime docs keep standalone Bash contract'

# Documentation and milestone consistency.
[ -f docs/milestones/v0.21.0-spec.md ] || fail 'v0.21 spec file exists'
pass 'v0.21 spec file exists'
[ -f docs/milestones/v0.21.0-test-plan.md ] || fail 'v0.21 test plan file exists'
pass 'v0.21 test plan file exists'
assert_contains docs/milestones/v0.21.0-spec.md 'Function Values' 'v0.21 spec identifies feature-foundation scope'
for non_goal in 'typed function parameters' 'closures' 'multiple return values' 'floating-point arithmetic' 'arrays, maps' 'signal handling'; do
  assert_contains docs/milestones/v0.21.0-spec.md "$non_goal" "v0.21 spec non-goal mentions $non_goal"
done
assert_contains docs/milestones/v0.21.0-spec.md 'VM/Bash parity' 'v0.21 spec acceptance mentions parity'
assert_contains docs/milestones/v0.21.0-spec.md 'standalone Bash' 'v0.21 spec acceptance mentions standalone Bash'
assert_contains docs/status.md 'v0.21.0' 'status mentions v0.21'
assert_contains docs/language.ds 'return expr' 'language catalog documents return expr'
assert_contains CHANGELOG.md 'v0.21.0' 'changelog has v0.21 entry'
assert_contains README.md 'v0.21.0' 'README mentions v0.21 status'

# Examples remain coherent, including the new v0.21 example.
for example in basic args import-main command-result redirection functions collections control-flow pipeline strings stdlib vm function-values; do
  path="examples/$example.ds"
  run_ok "example_${example}_check" "$DS" check "$path"
  run_ok "example_${example}_emit" "$DS" emit bash "$path" -o "$TMP/example_${example}.sh"
  run_ok "example_${example}_bash_n" bash -n "$TMP/example_${example}.sh"
  assert_not_matches "$TMP/example_${example}.sh" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$example emitted Bash is standalone"
done
run_fail example_bad_check "$DS" check examples/bad.ds
assert_diag "$TMP/example_bad_check.err" 'expected expression' 'bad example stays invalid'
run_ok function_values_example_fmt "$DS" fmt --check examples/function-values.ds
assert_vm_bash_parity_args function_values_example "$ROOT/examples/function-values.ds" 0 ''

# Parser/checker tests for return expressions and diagnostics.
write_fixture "$FIX/return_scalars.ds" <<'DS'
fn answer() {
  return 42
}

fn label() {
  return "api"
}

fn ok() {
  return true
}
DS
run_ok return_scalars_check "$DS" check "$FIX/return_scalars.ds"
run_ok return_scalars_hir "$DS" hir "$FIX/return_scalars.ds"
assert_contains "$TMP/return_scalars_hir.out" 'return' 'HIR includes return nodes'
run_ok return_scalars_fmt "$DS" fmt --check "$FIX/return_scalars.ds"

write_fixture "$FIX/return_top.ds" <<'DS'
return 1
DS
assert_check_fails return_top "$FIX/return_top.ds" '`return` is only allowed inside a function'

write_fixture "$FIX/return_test.ds" <<'DS'
test "bad" {
  return 1
}
DS
assert_check_fails return_test "$FIX/return_test.ds" '`return` is only allowed inside a function'

write_fixture "$FIX/return_if.ds" <<'DS'
if true {
  return 1
}
DS
assert_check_fails return_if "$FIX/return_if.ds" '`return` is only allowed inside a function'

write_fixture "$FIX/bare_return.ds" <<'DS'
fn bad() {
  return
}
DS
assert_check_fails bare_return "$FIX/bare_return.ds" 'expected expression after `return`'

write_fixture "$FIX/return_array.ds" <<'DS'
fn values() {
  return [1, 2]
}
let xs = values()
let second = xs[1]
echo "second={second}"
DS
assert_vm_bash_parity_args return_array "$FIX/return_array.ds" 0 ''

write_fixture "$FIX/return_command_result.ds" <<'DS'
fn captured() {
  let r = run printf "x"
  return r
}
let r = captured()
echo "out={r.stdout}"
echo "status={r.status}"
DS
assert_vm_bash_parity_args return_command_result "$FIX/return_command_result.ds" 0 ''

write_fixture "$FIX/return_map.ds" <<'DS'
fn service() {
  return { name: "api" }
}
let app = service()
let name = app.name
echo "name={name}"
DS
assert_vm_bash_parity_args return_map "$FIX/return_map.ds" 0 ''

write_fixture "$FIX/mixed_return.ds" <<'DS'
fn maybe(flag = true) {
  if flag {
    return 1
  }
  return "one"
}

let x = maybe()
DS
assert_check_fails mixed_return "$FIX/mixed_return.ds" 'all return statements in a function must have the same value kind'

write_fixture "$FIX/missing_return_value.ds" <<'DS'
fn maybe(flag = true) {
  if flag {
    return 1
  }
}

let x = maybe()
DS
assert_check_fails missing_return_value "$FIX/missing_return_value.ds" 'not all control paths return'

write_fixture "$FIX/procedure_value.ds" <<'DS'
fn log(name = "api") {
  echo "{name}"
}

let x = log()
DS
assert_check_fails procedure_value "$FIX/procedure_value.ds" 'does not return a value'

# Function return VM/Bash parity.
write_fixture "$FIX/scalar_returns.ds" <<'DS'
fn answer() {
  return 42
}

fn label() {
  return "api"
}

fn ok() {
  return true
}

let a = "{answer()}"
let l = "{label()}"
echo "{a}"
echo "{l}"
if ok() {
  echo yes
}
DS
assert_vm_bash_parity_args scalar_returns "$FIX/scalar_returns.ds" 0 ''
assert_file_equals "$TMP/scalar_returns_vm.out" $'42\napi\nyes\n' 'scalar returns stdout'

write_fixture "$FIX/early_return.ds" <<'DS'
fn choose(name = "api") {
  if name == "api" {
    return "backend"
  }
  return "other"
}

let result = "{choose()}"
echo "{result}"
DS
assert_vm_bash_parity_args early_return "$FIX/early_return.ds" 0 ''
assert_file_equals "$TMP/early_return_vm.out" $'backend\n' 'early return stdout'

write_fixture "$FIX/return_loop_case.ds" <<'DS'
fn first_web(items = "api,web,worker") {
  let parts = items.split(",")
  let i = 0
  while i < 3 {
    let part = parts[i]
    case part {
      "web" { return part.upper() }
      _ { i += 1 }
    }
  }
  return "NONE"
}

let result = "{first_web()}"
echo "{result}"
DS
assert_vm_bash_parity_args return_loop_case "$FIX/return_loop_case.ds" 0 ''
assert_file_equals "$TMP/return_loop_case_vm.out" $'WEB\n' 'return exits loop/case stdout'

write_fixture "$FIX/statement_call_ignores_return.ds" <<'DS'
fn compute() {
  echo "body"
  return 7
}

compute()
echo done
DS
assert_vm_bash_parity_args statement_call_ignores_return "$FIX/statement_call_ignores_return.ds" 0 ''
assert_file_equals "$TMP/statement_call_ignores_return_vm.out" $'body\ndone\n' 'statement call ignores return stdout'

write_fixture "$FIX/lib.ds" <<'DS'
fn normalize(name = " api ") {
  return name.trim().upper()
}
DS
write_fixture "$FIX/imported_value.ds" <<DS
import "./lib.ds"

let result = "{normalize()}"
echo "{result}"
DS
assert_vm_bash_parity_args imported_value "$FIX/imported_value.ds" 0 ''
assert_file_equals "$TMP/imported_value_vm.out" $'API\n' 'imported value stdout'

write_fixture "$FIX/command_word_bind_first.ds" <<'DS'
fn file_name() {
  return "README.md"
}

let name = "{file_name()}"
printf "%s\n" "{name}"
DS
assert_vm_bash_parity_args command_word_bind_first "$FIX/command_word_bind_first.ds" 0 ''
assert_file_equals "$TMP/command_word_bind_first_vm.out" $'README.md\n' 'command word bind-first stdout'

write_fixture "$FIX/command_word_call_interp.ds" <<'DS'
fn file_name() {
  return "README.md"
}

printf "%s\n" "{file_name()}"
DS
assert_vm_bash_parity_args command_word_call_interp "$FIX/command_word_call_interp.ds" 0 ''
assert_file_equals "$TMP/command_word_call_interp_vm.out" $'README.md\n' 'command word direct call interpolation stdout'

# Recursive functions remain rejected by the existing cycle guard rather than hanging.
write_fixture "$FIX/fact_recursion.ds" <<'DS'
fn fact(n = 5) {
  if n <= 1 {
    return 1
  }
  return n * fact(n - 1)
}

let result = "{fact()}"
echo "{result}"
DS
assert_check_fails fact_recursion "$FIX/fact_recursion.ds" 'recursion cycle'

write_fixture "$FIX/fib_recursion.ds" <<'DS'
fn fib(n = 7) {
  if n <= 1 {
    return n
  }
  return fib(n - 1) + fib(n - 2)
}

let result = "{fib()}"
echo "{result}"
DS
assert_check_fails fib_recursion "$FIX/fib_recursion.ds" 'recursion cycle'

write_fixture "$FIX/loop_recursion.ds" <<'DS'
fn loop(n = 0) {
  return loop(n + 1)
}

let result = "{loop()}"
echo "{result}"
DS
assert_check_fails loop_recursion "$FIX/loop_recursion.ds" 'recursion cycle'

# Arithmetic parser, precedence, compound assignment, and runtime errors.
write_fixture "$FIX/arithmetic_precedence.ds" <<'DS'
let a = "{1 + 2 * 3}"
let b = "{(1 + 2) * 3}"
let c = "{2 ** 3 ** 2}"
let d = "{10 - 6 / 2}"
let e = "{10 % 4 + 1}"
echo "{a}"
echo "{b}"
echo "{c}"
echo "{d}"
echo "{e}"
DS
assert_vm_bash_parity_args arithmetic_precedence "$FIX/arithmetic_precedence.ds" 0 ''
assert_file_equals "$TMP/arithmetic_precedence_vm.out" $'7\n9\n512\n7\n3\n' 'arithmetic precedence stdout'

write_fixture "$FIX/arithmetic_calls.ds" <<'DS'
fn scale(n = 3) {
  return n * 10 + 2
}

fn add(a = 1, b = 2) {
  return a + b
}

let a = "{scale()}"
let b = "{scale(add(2, 3))}"
echo "{a}"
echo "{b}"
DS
assert_vm_bash_parity_args arithmetic_calls "$FIX/arithmetic_calls.ds" 0 ''
assert_file_equals "$TMP/arithmetic_calls_vm.out" $'32\n52\n' 'arithmetic calls stdout'

write_fixture "$FIX/arithmetic_conditions_case.ds" <<'DS'
let n = 3
if n * 2 == 6 {
  echo yes
}

case n + 1 {
  4 { echo four }
  _ { echo other }
}
DS
assert_vm_bash_parity_args arithmetic_conditions_case "$FIX/arithmetic_conditions_case.ds" 0 ''
assert_file_equals "$TMP/arithmetic_conditions_case_vm.out" $'yes\nfour\n' 'arithmetic condition/case stdout'

write_fixture "$FIX/command_word_arithmetic_interp.ds" <<'DS'
let n = 4
echo "sum={1 + 2 * 3}"
echo "pow={2 ** 3}"
echo "n={n * 2}"
DS
assert_vm_bash_parity_args command_word_arithmetic_interp "$FIX/command_word_arithmetic_interp.ds" 0 ''
assert_file_equals "$TMP/command_word_arithmetic_interp_vm.out" $'sum=7\npow=8\nn=8\n' 'command-word arithmetic interpolation stdout'

write_fixture "$FIX/evaluate_call_args_once.ds" <<'DS'
fn mark1() {
  let r = run sh -c "printf 1 >> order.txt"
  return 1
}

fn mark2() {
  let r = run sh -c "printf 2 >> order.txt"
  return 2
}

let result = "{mark1() + mark2()}"
echo "{result}"
DS
assert_vm_bash_parity_args evaluate_call_args_once "$FIX/evaluate_call_args_once.ds" 0 'order.txt'
assert_file_equals "$TMP/evaluate_call_args_once_vm.out" $'3\n' 'evaluate-once call result stdout'
assert_file_equals "$TMP/evaluate_call_args_once_vm_work/order.txt" '12' 'evaluate-once left-to-right side effect order'

write_fixture "$FIX/negative_exponent_edges.ds" <<'DS'
let a = "{-2 * 3}"
let b = "{(-2) ** 3}"
let c = "{2 ** 0}"
echo "{a}"
echo "{b}"
echo "{c}"
DS
assert_vm_bash_parity_args negative_exponent_edges "$FIX/negative_exponent_edges.ds" 0 ''
assert_file_equals "$TMP/negative_exponent_edges_vm.out" $'-6\n-8\n1\n' 'negative/exponent edge stdout'

write_fixture "$FIX/compound_basic.ds" <<'DS'
let n = 6
n *= 7
echo "{n}"
n /= 3
echo "{n}"
n %= 5
echo "{n}"
DS
assert_vm_bash_parity_args compound_basic "$FIX/compound_basic.ds" 0 ''
assert_file_equals "$TMP/compound_basic_vm.out" $'42\n14\n4\n' 'compound assignment stdout'

write_fixture "$FIX/compound_loop.ds" <<'DS'
let n = 1
let i = 0
while i < 5 {
  n *= 2
  i += 1
}
echo "{n}"
DS
assert_vm_bash_parity_args compound_loop "$FIX/compound_loop.ds" 0 ''
assert_file_equals "$TMP/compound_loop_vm.out" $'32\n' 'compound loop stdout'

write_fixture "$FIX/compound_string.ds" <<'DS'
let s = "api"
s *= 2
DS
assert_check_fails compound_string "$FIX/compound_string.ds" 'integer'

write_fixture "$FIX/compound_expr_target.ds" <<'DS'
(1 + 2) *= 3
DS
assert_check_fails compound_expr_target "$FIX/compound_expr_target.ds" 'compound assignment target must be a variable'

write_fixture "$FIX/compound_index.ds" <<'DS'
let xs = [1, 2]
xs[0] *= 3
DS
assert_check_fails compound_index "$FIX/compound_index.ds" 'compound index assignment is unsupported in v0.30.0'

write_fixture "$FIX/div_zero.ds" <<'DS'
let x = "{1 / 0}"
echo "{x}"
DS
assert_vm_bash_parity_args div_zero "$FIX/div_zero.ds" 1 ''
assert_contains "$TMP/div_zero_vm.err" 'division or modulo by zero' 'VM division/modulo zero diagnostic'
assert_contains "$TMP/div_zero_bash.err" 'division or modulo by zero' 'Bash division/modulo zero diagnostic'

write_fixture "$FIX/mod_zero.ds" <<'DS'
let x = "{1 % 0}"
echo "{x}"
DS
assert_vm_bash_parity_args mod_zero "$FIX/mod_zero.ds" 1 ''
assert_contains "$TMP/mod_zero_vm.err" 'division or modulo by zero' 'VM division or modulo by zero diagnostic'
assert_contains "$TMP/mod_zero_bash.err" 'division or modulo by zero' 'Bash division or modulo by zero diagnostic'

write_fixture "$FIX/negative_power.ds" <<'DS'
let x = "{2 ** -1}"
echo "{x}"
DS
assert_vm_bash_parity_args negative_power "$FIX/negative_power.ds" 1 ''
assert_contains "$TMP/negative_power_vm.err" 'negative exponent runtime value is rejected' 'VM negative exponent diagnostic'
assert_contains "$TMP/negative_power_bash.err" 'negative exponent runtime value is rejected' 'Bash negative exponent diagnostic'

write_fixture "$FIX/mul_overflow.ds" <<'DS'
let x = "{9223372036854775807 * 2}"
echo "{x}"
DS
assert_vm_bash_parity_args mul_overflow "$FIX/mul_overflow.ds" 1 ''
assert_contains "$TMP/mul_overflow_vm.err" 'overflow' 'VM multiplication overflow diagnostic'
assert_contains "$TMP/mul_overflow_bash.err" 'overflow' 'Bash multiplication overflow diagnostic'

write_fixture "$FIX/add_overflow.ds" <<'DS'
let x = "{9223372036854775807 + 1}"
echo "{x}"
DS
assert_vm_bash_parity_args add_overflow "$FIX/add_overflow.ds" 1 ''
assert_contains "$TMP/add_overflow_vm.err" 'overflow' 'VM addition overflow diagnostic'
assert_contains "$TMP/add_overflow_bash.err" 'overflow' 'Bash addition overflow diagnostic'

write_fixture "$FIX/sub_overflow.ds" <<'DS'
let x = "{-9223372036854775807 - 2}"
echo "{x}"
DS
assert_vm_bash_parity_args sub_overflow "$FIX/sub_overflow.ds" 1 ''
assert_contains "$TMP/sub_overflow_vm.err" 'overflow' 'VM subtraction overflow diagnostic'
assert_contains "$TMP/sub_overflow_bash.err" 'overflow' 'Bash subtraction overflow diagnostic'

write_fixture "$FIX/pow_overflow.ds" <<'DS'
let x = "{2 ** 63}"
echo "{x}"
DS
assert_vm_bash_parity_args pow_overflow "$FIX/pow_overflow.ds" 1 ''
assert_contains "$TMP/pow_overflow_vm.err" 'overflow' 'VM power overflow diagnostic'
assert_contains "$TMP/pow_overflow_bash.err" 'overflow' 'Bash power overflow diagnostic'

write_fixture "$FIX/out_of_range_int.ds" <<'DS'
let x = 9223372036854775808
DS
assert_check_fails out_of_range_int "$FIX/out_of_range_int.ds" 'integer literal is outside the supported int range'

# Function stdout and expression-call tests.
write_fixture "$FIX/clean_value.ds" <<'DS'
fn value() {
  return "clean"
}

let result = "{value()}"
echo "{result}"
DS
assert_vm_bash_parity_args clean_value "$FIX/clean_value.ds" 0 ''
assert_file_equals "$TMP/clean_value_vm.out" $'clean\n' 'clean value stdout'

write_fixture "$FIX/statement_stdout_preserved.ds" <<'DS'
fn log_and_value() {
  echo "log"
  return "value"
}

log_and_value()
echo done
DS
assert_vm_bash_parity_args statement_stdout_preserved "$FIX/statement_stdout_preserved.ds" 0 ''
assert_file_equals "$TMP/statement_stdout_preserved_vm.out" $'log\ndone\n' 'statement stdout preserved'

write_fixture "$FIX/value_stdout_rejected.ds" <<'DS'
fn bad_value() {
  echo "log"
  return "value"
}

let result = bad_value()
DS
assert_check_fails value_stdout_rejected "$FIX/value_stdout_rejected.ds" 'contains plain command statements'

# Function arguments and default-kind behavior.
write_fixture "$FIX/defaults_explicit_args.ds" <<'DS'
fn mul(n = 3, m = 4) {
  return n * m
}

let a = "{mul()}"
let b = "{mul(2, 5)}"
echo "{a}"
echo "{b}"
DS
assert_vm_bash_parity_args defaults_explicit_args "$FIX/defaults_explicit_args.ds" 0 ''
assert_file_equals "$TMP/defaults_explicit_args_vm.out" $'12\n10\n' 'defaults and explicit args stdout'

write_fixture "$FIX/wrong_arg_kind.ds" <<'DS'
fn mul(n = 3) {
  return n * 2
}

let text = "x"
let result = "{mul(text)}"
echo "{result}"
DS
assert_check_fails wrong_arg_kind "$FIX/wrong_arg_kind.ds" 'argument kind must match parameter default kind'

write_fixture "$FIX/explicit_arg_type_tags.ds" <<'DS'
fn classify(n = 0) {
  case n {
    2 { return "int" }
    _ { return "other" }
  }
}

let a = "{classify(2)}"
echo "{a}"
DS
assert_vm_bash_parity_args explicit_arg_type_tags "$FIX/explicit_arg_type_tags.ds" 0 ''
assert_file_equals "$TMP/explicit_arg_type_tags_vm.out" $'int\n' 'explicit argument type tags stdout'

write_fixture "$FIX/explicit_arg_kind_rejected.ds" <<'DS'
fn classify(n = 0) {
  case n {
    2 { return "int" }
    _ { return "other" }
  }
}

let str = "2"
let b = "{classify(str)}"
echo "{b}"
DS
assert_check_fails explicit_arg_kind_rejected "$FIX/explicit_arg_kind_rejected.ds" 'argument kind must match parameter default kind'

# Interpolation and formatting tests in expression-backed strings.
write_fixture "$FIX/interpolation_expr.ds" <<'DS'
fn service() {
  return "api"
}

fn count() {
  return 7
}

let svc_value = service().upper()
let count_value = count()
let svc = "svc={svc_value:upper}"
let count_text = "count={count_value:03d}"
let calc = "calc={count() * 6}"
echo "{svc}"
echo "{count_text}"
echo "{calc}"
DS
assert_vm_bash_parity_args interpolation_expr "$FIX/interpolation_expr.ds" 0 ''
assert_file_equals "$TMP/interpolation_expr_vm.out" $'svc=API\ncount=007\ncalc=42\n' 'interpolation expression stdout'
run_ok interpolation_expr_fmt "$DS" fmt --check "$FIX/interpolation_expr.ds"

write_fixture "$FIX/format_return_blocks.ds" <<'DS'
fn choose(n = 1) {
  if n == 1 {
    return "one"
  }
  return "other"
}
DS
run_ok format_return_blocks "$DS" fmt --check "$FIX/format_return_blocks.ds"

# Command and pipeline composition.
write_fixture "$FIX/return_run_field.ds" <<'DS'
fn status() {
  let r = run false
  return r.code
}

let result = "{status()}"
echo "{result}"
DS
assert_vm_bash_parity_args return_run_field "$FIX/return_run_field.ds" 0 ''
assert_file_equals "$TMP/return_run_field_vm.out" $'1\n' 'return command-result field stdout'

write_fixture "$FIX/arithmetic_controls_commands.ds" <<'DS'
fn repeats(n = 2) {
  return n * 2
}

let i = 0
while i < repeats() {
  printf "x"
  i += 1
}
printf "\n"
DS
assert_vm_bash_parity_args arithmetic_controls_commands "$FIX/arithmetic_controls_commands.ds" 0 ''
assert_file_equals "$TMP/arithmetic_controls_commands_vm.out" $'xxxx\n' 'arithmetic controls command loop stdout'

# Import and cycle diagnostics.
write_fixture "$FIX/math.ds" <<'DS'
fn square(n = 4) {
  return n * n
}
DS
write_fixture "$FIX/import_math.ds" <<DS
import "./math.ds"

let result = "{square()}"
echo "{result}"
DS
assert_vm_bash_parity_args import_math "$FIX/import_math.ds" 0 ''
assert_file_equals "$TMP/import_math_vm.out" $'16\n' 'imported arithmetic stdout'

mkdir -p "$FIX/cycle"
write_fixture "$FIX/cycle/a.ds" <<'DS'
import "./b.ds"

fn a() {
  return 1
}
DS
write_fixture "$FIX/cycle/b.ds" <<'DS'
import "./a.ds"

fn b() {
  return 2
}
DS
assert_check_fails import_cycle_return "$FIX/cycle/a.ds" 'import cycle'

# Test-runner integration.
write_fixture "$FIX/test_runner.ds" <<'DS'
fn double(n = 2) {
  return n * 2
}

test "double" {
  if double(3) != 6 {
    fail "bad double"
  }
}
DS
run_ok test_runner_returns "$DS" test "$FIX/test_runner.ds"

# Generated Bash boundary checks.
write_fixture "$FIX/simple_arithmetic_only.ds" <<'DS'
let x = 2 * 3 + 1
echo "{x}"
DS
run_ok simple_arithmetic_emit_once "$DS" emit bash "$FIX/simple_arithmetic_only.ds" -o "$TMP/simple_arithmetic_one.sh"
run_ok simple_arithmetic_emit_twice "$DS" emit bash "$FIX/simple_arithmetic_only.ds" -o "$TMP/simple_arithmetic_two.sh"
assert_same "$TMP/simple_arithmetic_one.sh" "$TMP/simple_arithmetic_two.sh" 'simple arithmetic Bash emission deterministic'
assert_not_contains "$TMP/simple_arithmetic_one.sh" '__ds_call_value' 'simple arithmetic omits function-return helper'
assert_not_matches "$TMP/simple_arithmetic_one.sh" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' 'simple arithmetic emitted Bash standalone'

run_ok scalar_returns_emit_once "$DS" emit bash "$FIX/scalar_returns.ds" -o "$TMP/scalar_returns_one.sh"
run_ok scalar_returns_emit_twice "$DS" emit bash "$FIX/scalar_returns.ds" -o "$TMP/scalar_returns_two.sh"
assert_same "$TMP/scalar_returns_one.sh" "$TMP/scalar_returns_two.sh" 'function return Bash emission deterministic'
assert_contains "$TMP/scalar_returns_one.sh" '__ds_' 'function return helpers use __ds prefix'
assert_not_matches "$TMP/scalar_returns_one.sh" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' 'function return emitted Bash standalone'

# Formatter and git whitespace checks.
run_ok arithmetic_fmt_check "$DS" fmt --check "$FIX/arithmetic_precedence.ds"
run_ok compound_fmt_check "$DS" fmt --check "$FIX/compound_basic.ds"
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  run_ok diff_check git diff --check
else
  pass "diff_check skipped outside a git work tree"
fi

echo "v0.21 tests passed ($pass_count assertions)"