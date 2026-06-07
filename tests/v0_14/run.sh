#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_14_tests.$$"
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

make_fakebin() {
  local bin="$TMP/fakebin"
  mkdir -p "$bin"
  cat >"$bin/fake-ok" <<'SH'
#!/usr/bin/env bash
printf 'ok:%s\n' "$*"
SH
  cat >"$bin/fake-fail" <<'SH'
#!/usr/bin/env bash
printf 'out:%s\n' "$*"
printf 'err:%s\n' "$*" >&2
exit 7
SH
  chmod +x "$bin"/*
  printf '%s' "$bin"
}

assert_matches() {
  local file="$1"
  local regex="$2"
  local name="$3"
  grep -E -- "$regex" "$file" >/dev/null || {
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected to match /$regex/"
  }
  pass "$name"
}

assert_not_matches() {
  local file="$1"
  local regex="$2"
  local name="$3"
  if grep -E -- "$regex" "$file" >/dev/null; then
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected not to match /$regex/"
  fi
  pass "$name"
}

assert_line_count() {
  local expected="$1" file="$2" pattern="$3" name="$4"
  local count
  count="$(grep -E -c -- "$pattern" "$file" || true)"
  [ "$count" = "$expected" ] || {
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected $expected matching lines, got $count"
  }
  pass "$name"
}

assert_diag_span() {
  local file="$1" fixture="$2" message="$3" name="$4"
  assert_contains "$file" "$fixture:" "$name path"
  assert_contains "$file" ': error:' "$name error shape"
  assert_contains "$file" "$message" "$name message"
  assert_contains "$file" '^' "$name caret"
}

assert_test_summary() {
  local file="$1" total="$2" passed="$3" failed="$4" name="$5"
  assert_contains "$file" "$total tests, $passed passed, $failed failed" "$name summary"
}

FAKEBIN="$(make_fakebin)"
FIX="$TMP/fixtures"
mkdir -p "$FIX"

# Static wiring and CLI usage.
count_014="$(grep -E '^TEST_VERSIONS :=' Makefile | grep -o '0-14' | wc -l | tr -d ' ')"
[ "$count_014" = 1 ] || fail "TEST_VERSIONS should contain 0-14 exactly once, got $count_014"
pass 'TEST_VERSIONS contains 0-14 exactly once'
assert_matches Makefile '^TEST_VERSIONS := .*0-13 0-14($| )' 'v0.14 follows v0.13 in TEST_VERSIONS'
assert_contains Makefile '$(TEST_TARGETS): $(BIN)' 'pattern target drives version suites'
assert_contains Makefile 'asan:' 'asan target exists'
assert_contains Makefile 'ubsan:' 'ubsan target exists'

run_ok help_top "$DS" --help
assert_contains "$TMP/help_top.out" 'ds test <file.ds>' 'top-level help lists ds test'
assert_contains "$TMP/help_top.out" 'ds run [--trace-cmd] [--trace-vm] <file.ds> [args...]' 'top-level help keeps trace usage'
capture_status test_no_args "$DS" test
assert_nonzero_status test_no_args
assert_contains "$TMP/test_no_args.err" 'expected `ds test <file.ds>`' 'ds test without path is explicit usage error'
assert_same_text '' "$TMP/test_no_args.out" 'ds test without path does not print stdout'

# Parser, AST, and HIR support.
write_fixture "$FIX/simple.ds" <<'DS'
test "simple" {
  assert true
}
DS
run_ok simple_check "$DS" check "$FIX/simple.ds"
run_ok simple_ast "$DS" ast "$FIX/simple.ds"
assert_contains "$TMP/simple_ast.out" 'TestStmt "simple"' 'AST includes test declaration'
assert_contains "$TMP/simple_ast.out" 'AssertStmt' 'AST includes assert statement'
run_ok simple_hir_a "$DS" hir "$FIX/simple.ds"
run_ok simple_hir_b "$DS" hir "$FIX/simple.ds"
assert_same "$TMP/simple_hir_a.out" "$TMP/simple_hir_b.out" 'HIR output with tests is deterministic'
assert_contains "$TMP/simple_hir_a.out" 'Tests' 'HIR includes Tests section'
assert_contains "$TMP/simple_hir_a.out" 'Test simple @' 'HIR includes test name and span'
assert_contains "$TMP/simple_hir_a.out" 'Assert @' 'HIR includes assert span'

write_fixture "$FIX/names.ds" <<'DS'
test "quote \" inside" {
  assert true
}

test "unicode-ish café" {
  assert true
}
DS
run_ok names_test "$DS" test "$FIX/names.ds"
assert_contains "$TMP/names_test.out" 'ok   quote " inside' 'escaped quote test name is decoded in report'
assert_contains "$TMP/names_test.out" 'ok   unicode-ish café' 'unicode-ish test name is reported'
assert_test_summary "$TMP/names_test.out" 2 2 0 'decoded names'

for case in missing_name bare_name empty_name missing_body; do
  file="$FIX/invalid_${case}.ds"
  case "$case" in
    missing_name) printf 'test {\n  assert true\n}\n' >"$file" ;;
    bare_name) printf 'test name {\n  assert true\n}\n' >"$file" ;;
    empty_name) printf 'test "" {\n  assert true\n}\n' >"$file" ;;
    missing_body) printf 'test "missing body"\n' >"$file" ;;
  esac
  capture_status "invalid_${case}_check" "$DS" check "$file"
  assert_nonzero_status "invalid_${case}_check"
  assert_contains "$TMP/invalid_${case}_check.err" "$file:" "invalid test $case has file diagnostic"
  assert_contains "$TMP/invalid_${case}_check.err" ': error:' "invalid test $case has error diagnostic"
  assert_contains "$TMP/invalid_${case}_check.err" '^' "invalid test $case has caret diagnostic"
  capture_status "invalid_${case}_test" "$DS" test "$file"
  assert_nonzero_status "invalid_${case}_test"
done

write_fixture "$FIX/assert_outside.ds" <<'DS'
assert true
DS
capture_status assert_outside "$DS" check "$FIX/assert_outside.ds"
assert_nonzero_status assert_outside
assert_diag_span "$TMP/assert_outside.err" "$FIX/assert_outside.ds" '`assert` is only allowed inside a test block in v0.14.0' 'assert outside test'

write_fixture "$FIX/assert_missing.ds" <<'DS'
test "missing expression" {
  assert
}
DS
capture_status assert_missing "$DS" check "$FIX/assert_missing.ds"
assert_nonzero_status assert_missing
assert_diag_span "$TMP/assert_missing.err" "$FIX/assert_missing.ds" 'expected expression' 'missing assert expression'

write_fixture "$FIX/nested_function.ds" <<'DS'
fn f() {
  test "inside function" {
    assert true
  }
}
DS
capture_status nested_function "$DS" check "$FIX/nested_function.ds"
assert_nonzero_status nested_function
assert_contains "$TMP/nested_function.err" "$FIX/nested_function.ds:" 'nested test in function has file diagnostic'
assert_contains "$TMP/nested_function.err" ': error:' 'nested test in function has error diagnostic'

write_fixture "$FIX/nested_if.ds" <<'DS'
if true {
  test "inside if" {
    assert true
  }
}
DS
capture_status nested_if "$DS" check "$FIX/nested_if.ds"
assert_nonzero_status nested_if
assert_diag_span "$TMP/nested_if.err" "$FIX/nested_if.ds" '`test` declarations are only allowed at top level in v0.14.0' 'nested test in if'

write_fixture "$FIX/nested_loop.ds" <<'DS'
for item in ["one"] {
  test "inside loop" {
    assert true
  }
}
DS
capture_status nested_loop "$DS" check "$FIX/nested_loop.ds"
assert_nonzero_status nested_loop
assert_diag_span "$TMP/nested_loop.err" "$FIX/nested_loop.ds" '`test` declarations are only allowed at top level in v0.14.0' 'nested test in loop'

write_fixture "$FIX/nested_test.ds" <<'DS'
test "outer" {
  test "inner" {
    assert true
  }
}
DS
capture_status nested_test "$DS" check "$FIX/nested_test.ds"
assert_nonzero_status nested_test
assert_contains "$TMP/nested_test.err" "$FIX/nested_test.ds:" 'nested test in test has file diagnostic'
assert_contains "$TMP/nested_test.err" ': error:' 'nested test in test has error diagnostic'

# Semantic/lowering behavior.
write_fixture "$FIX/duplicate.ds" <<'DS'
test "same" {
  assert true
}

test "same" {
  assert true
}
DS
capture_status duplicate_check "$DS" check "$FIX/duplicate.ds"
assert_nonzero_status duplicate_check
assert_diag_span "$TMP/duplicate_check.err" "$FIX/duplicate.ds" 'duplicate test `same`' 'duplicate test names'
capture_status duplicate_test "$DS" test "$FIX/duplicate.ds"
assert_nonzero_status duplicate_test
assert_same_text '' "$TMP/duplicate_test.out" 'duplicate tests do not execute'

write_fixture "$FIX/duplicate_function.ds" <<'DS'
fn helper() {
  echo "first"
}

fn helper() {
  echo "second"
}

test "never" {
  assert true
}
DS
capture_status duplicate_function "$DS" test "$FIX/duplicate_function.ds"
assert_nonzero_status duplicate_function
assert_diag_span "$TMP/duplicate_function.err" "$FIX/duplicate_function.ds" 'duplicate function `helper`' 'duplicate functions still fail before tests'
assert_same_text '' "$TMP/duplicate_function.out" 'duplicate functions run no tests'

write_fixture "$FIX/test_keyword_function.ds" <<'DS'
fn test() {
  echo "reserved"
}

test "never" {
  assert true
}
DS
capture_status test_keyword_function "$DS" check "$FIX/test_keyword_function.ds"
assert_nonzero_status test_keyword_function
assert_diag_span "$TMP/test_keyword_function.err" "$FIX/test_keyword_function.ds" 'expected function name after `fn`' 'test keyword cannot be function name'

write_fixture "$FIX/fail_exit_user_functions.ds" <<'DS'
fn fail() {
  echo "user fail"
}

fn exit() {
  echo "user exit"
}

test "test helpers take precedence over user functions" {
  fail "boom"
}

test "exit helper takes precedence over user function" {
  exit 0
  assert false
}
DS
capture_status fail_exit_user_functions "$DS" test "$FIX/fail_exit_user_functions.ds"
assert_nonzero_status fail_exit_user_functions
assert_contains "$TMP/fail_exit_user_functions.out" 'fail test helpers take precedence over user functions' 'fail helper wins over user function'
assert_contains "$TMP/fail_exit_user_functions.out" 'ok   exit helper takes precedence over user function' 'exit helper wins over user function'
assert_not_contains "$TMP/fail_exit_user_functions.out" 'user fail' 'test fail helper does not call user fail function'
assert_not_contains "$TMP/fail_exit_user_functions.out" 'user exit' 'test exit helper does not call user exit function'
assert_test_summary "$TMP/fail_exit_user_functions.out" 2 1 1 'test helper/user function collision summary'

write_fixture "$FIX/dup_lib.ds" <<'DS'
test "shared" {
  assert true
}
DS
write_fixture "$FIX/dup_main.ds" <<'DS'
import "./dup_lib.ds"

test "shared" {
  assert true
}
DS
capture_status duplicate_import "$DS" test "$FIX/dup_main.ds"
assert_nonzero_status duplicate_import
assert_contains "$TMP/duplicate_import.err" 'duplicate test `shared`' 'duplicate imported test detected'
assert_contains "$TMP/duplicate_import.err" 'dup_main.ds:' 'duplicate import diagnostic points at duplicate declaration'

write_fixture "$FIX/function.ds" <<'DS'
fn is_prod(target = "staging") {
  if target == "production" {
    echo "yes"
  } else {
    echo "no"
  }
}

test "function can be called" {
  is_prod("production")
  assert true
}
DS
run_ok function_test "$DS" test "$FIX/function.ds"
assert_contains "$TMP/function_test.out" 'yes' 'command output inside tests is visible'
assert_contains "$TMP/function_test.out" 'ok   function can be called' 'function-call test passes'
assert_test_summary "$TMP/function_test.out" 1 1 0 'function-call test'

write_fixture "$FIX/scope.ds" <<'DS'
test "first" {
  let value = "one"
  assert value == "one"
}

test "second" {
  let value = "two"
  assert value == "two"
}
DS
run_ok scope_test "$DS" test "$FIX/scope.ds"
assert_contains "$TMP/scope_test.out" 'ok   first' 'first scoped test passes'
assert_contains "$TMP/scope_test.out" 'ok   second' 'second scoped test passes'
assert_test_summary "$TMP/scope_test.out" 2 2 0 'test-local scopes'

write_fixture "$FIX/no_prod_side_effect.ds" <<'DS'
file.write("should-not-exist.txt", "bad")

test "does not run production top level" {
  assert true
}
DS
work="$TMP/no_prod_work"
mkdir -p "$work"
run_ok no_prod_test bash -c "cd '$work' && '$DS' test '$FIX/no_prod_side_effect.ds'"
[ ! -e "$work/should-not-exist.txt" ] || fail 'ds test must not execute production top-level file.write'
pass 'ds test skips production side effects'
run_ok no_prod_run bash -c "cd '$work' && '$DS' run '$FIX/no_prod_side_effect.ds'"
[ -f "$work/should-not-exist.txt" ] || fail 'ds run should execute production file.write'
pass 'ds run still executes production side effects'

write_fixture "$FIX/script_block.ds" <<'DS'
script {
  arg app: string
  option target: string = "staging"
}

test "script block does not require cli args" {
  assert true
}
DS
run_ok script_block_test "$DS" test "$FIX/script_block.ds"
assert_contains "$TMP/script_block_test.out" 'ok   script block does not require cli args' 'ds test accepts script block without args'
capture_status script_block_run "$DS" run "$FIX/script_block.ds"
assert_nonzero_status script_block_run
assert_contains "$TMP/script_block_run.err" 'missing required argument `app`' 'normal run still requires script arg'

# VM runner behavior.
run_ok passing_test "$DS" test "$FIX/simple.ds"
assert_same_text '' "$TMP/passing_test.err" 'passing ds test stderr empty'
assert_contains "$TMP/passing_test.out" 'ok   simple' 'passing test reports ok line'
assert_test_summary "$TMP/passing_test.out" 1 1 0 'passing test'

write_fixture "$FIX/failing_assert.ds" <<'DS'
test "false fails" {
  assert false
}
DS
capture_status failing_assert "$DS" test "$FIX/failing_assert.ds"
assert_nonzero_status failing_assert
assert_contains "$TMP/failing_assert.out" 'fail false fails' 'failing assertion reports fail line'
assert_test_summary "$TMP/failing_assert.out" 1 0 1 'failing assertion'
assert_diag_span "$TMP/failing_assert.err" "$FIX/failing_assert.ds" 'test `false fails`: assertion failed' 'failing assertion span'

write_fixture "$FIX/continue.ds" <<'DS'
test "first fails" {
  assert false
}

test "second passes" {
  assert true
}
DS
capture_status continue_test "$DS" test "$FIX/continue.ds"
assert_nonzero_status continue_test
assert_contains "$TMP/continue_test.out" 'fail first fails' 'first test fails'
assert_contains "$TMP/continue_test.out" 'ok   second passes' 'second test still runs'
assert_test_summary "$TMP/continue_test.out" 2 1 1 'continue after failure'

write_fixture "$FIX/conditions.ds" <<'DS'
test "conditions" {
  assert true
  assert 1
  assert "nonempty"
  assert !false
  assert 3 > 2
  assert "api" == "api"
}

test "empty string condition" {
  assert ""
}
DS
capture_status conditions "$DS" test "$FIX/conditions.ds"
assert_nonzero_status conditions
assert_contains "$TMP/conditions.out" 'ok   conditions' 'truthy conditions pass'
assert_contains "$TMP/conditions.out" 'fail empty string condition' 'empty string assertion follows condition semantics and fails'
assert_test_summary "$TMP/conditions.out" 2 1 1 'condition semantics summary'

write_fixture "$FIX/function_defaults.ds" <<'DS'
fn greet(name = "world") {
  echo "hello {name}"
}

test "default function call" {
  greet()
  assert true
}

test "argument function call" {
  greet("ds")
  assert true
}
DS
run_ok function_defaults "$DS" test "$FIX/function_defaults.ds"
assert_contains "$TMP/function_defaults.out" 'hello world' 'test uses default function arg'
assert_contains "$TMP/function_defaults.out" 'hello ds' 'test uses explicit function arg'
assert_test_summary "$TMP/function_defaults.out" 2 2 0 'function defaults in tests'

write_fixture "$FIX/collections.ds" <<'DS'
test "arrays and maps" {
  let xs = ["a", "b"]
  xs.push("c")
  assert xs[0] == "a"

  let ports = { api: 3000, web: 5173 }
  assert ports.api == 3000

  for x in xs {
    echo $x
  }
  assert true
}

test "missing map key" {
  let ports = { api: 3000 }
  let key = "web"
  assert ports[key] == 5173
}
DS
capture_status collections "$DS" test "$FIX/collections.ds"
assert_nonzero_status collections
assert_contains "$TMP/collections.out" 'a' 'array loop output includes a'
assert_contains "$TMP/collections.out" 'b' 'array loop output includes b'
assert_contains "$TMP/collections.out" 'c' 'array loop output includes c'
assert_contains "$TMP/collections.out" 'ok   arrays and maps' 'collections test passes'
assert_contains "$TMP/collections.out" 'fail missing map key' 'missing map key fails current test'
assert_test_summary "$TMP/collections.out" 2 1 1 'collections summary'
assert_diag_span "$TMP/collections.err" "$FIX/collections.ds" 'missing map key `web`' 'missing map key diagnostic'

write_fixture "$FIX/commands.ds" <<'DS'
test "plain command" {
  fake-ok hello
  assert true
}

test "captured failure is inspectable" {
  let result = run fake-fail bad
  assert result.failed
  assert result.code == 7
}

test "direct failure fails" {
  fake-fail direct
  assert true
}
DS
capture_status commands env PATH="$FAKEBIN:$PATH" "$DS" test "$FIX/commands.ds"
assert_nonzero_status commands
assert_contains "$TMP/commands.out" 'ok:hello' 'plain command stdout is visible'
assert_contains "$TMP/commands.out" 'ok   plain command' 'plain command test passes'
assert_contains "$TMP/commands.out" 'ok   captured failure is inspectable' 'captured command failure can be inspected'
assert_contains "$TMP/commands.out" 'fail direct failure fails' 'direct command failure fails test'
assert_contains "$TMP/commands.err" 'err:direct' 'direct failing command stderr visible'
assert_test_summary "$TMP/commands.out" 3 2 1 'command behavior summary'

write_fixture "$FIX/stdlib.ds" <<'DS'
test "stdlib file env path glob lines" {
  file.write("input.txt", "a\nb\n")
  assert file.exists("input.txt")
  assert file.is_file("input.txt")
  assert dir.exists(".")
  assert path.basename("a/b.txt") == "b.txt"
  assert path.ext("a/b.txt") == ".txt"
  env.set("DS_V014_TEST", "ok")
  assert env.get("DS_V014_TEST", "") == "ok"
  env.unset("DS_V014_TEST")
  for line in lines("input.txt") {
    echo $line
  }
  assert true
}
DS
stdlib_work="$TMP/stdlib_work"
mkdir -p "$stdlib_work"
run_ok stdlib_test bash -c "cd '$stdlib_work' && '$DS' test '$FIX/stdlib.ds'"
assert_contains "$TMP/stdlib_test.out" 'a' 'lines helper output includes first line'
assert_contains "$TMP/stdlib_test.out" 'b' 'lines helper output includes second line'
assert_contains "$TMP/stdlib_test.out" 'ok   stdlib file env path glob lines' 'stdlib helper test passes'
[ -f "$stdlib_work/input.txt" ] || fail 'stdlib file side effects remain visible in test working directory'
pass 'stdlib file side effect visible after test'

write_fixture "$FIX/fail_exit.ds" <<'DS'
test "fail helper" {
  fail "boom"
}

test "exit nonzero" {
  exit 3
}

test "exit zero" {
  exit 0
  assert false
}

test "after failures" {
  assert true
}
DS
capture_status fail_exit "$DS" test "$FIX/fail_exit.ds"
assert_nonzero_status fail_exit
assert_contains "$TMP/fail_exit.out" 'fail fail helper' 'fail helper marks test failed'
assert_contains "$TMP/fail_exit.out" 'fail exit nonzero' 'exit nonzero marks test failed'
assert_contains "$TMP/fail_exit.out" 'ok   exit zero' 'exit zero stops active test as passed'
assert_contains "$TMP/fail_exit.out" 'ok   after failures' 'runner continues after fail/exit failures'
assert_test_summary "$TMP/fail_exit.out" 4 2 2 'fail and exit summary'
assert_diag_span "$TMP/fail_exit.err" "$FIX/fail_exit.ds" 'test `fail helper`: fail: boom' 'fail helper diagnostic'
assert_diag_span "$TMP/fail_exit.err" "$FIX/fail_exit.ds" 'test `exit nonzero`: exit 3' 'exit nonzero diagnostic'

write_fixture "$FIX/exit_bad.ds" <<'DS'
test "bad exit" {
  exit "nope"
}
DS
capture_status exit_bad "$DS" test "$FIX/exit_bad.ds"
assert_nonzero_status exit_bad
assert_contains "$TMP/exit_bad.out" 'fail bad exit' 'invalid exit argument fails test'
assert_contains "$TMP/exit_bad.err" '`exit` code must be an integer from 0 to 255' 'invalid exit status diagnostic'

# Import behavior.
mkdir -p "$FIX/imports/sub"
write_fixture "$FIX/imports/sub/lib.ds" <<'DS'
fn service_name() {
  echo "api"
}

test "lib test" {
  assert true
}
DS
write_fixture "$FIX/imports/main.ds" <<'DS'
import "./sub/lib.ds"

test "main test" {
  service_name()
  assert true
}
DS
run_ok imports_test "$DS" test "$FIX/imports/main.ds"
assert_matches "$TMP/imports_test.out" '^ok   lib test$' 'imported test runs'
assert_matches "$TMP/imports_test.out" '^api$' 'imported helper function runs relative import'
assert_matches "$TMP/imports_test.out" '^ok   main test$' 'main test runs after imported test'
assert_test_summary "$TMP/imports_test.out" 2 2 0 'imported tests summary'

write_fixture "$FIX/missing_import.ds" <<'DS'
import "./does-not-exist.ds"

test "never" {
  assert true
}
DS
capture_status missing_import "$DS" test "$FIX/missing_import.ds"
assert_nonzero_status missing_import
assert_contains "$TMP/missing_import.err" 'does-not-exist.ds' 'missing import diagnostic mentions missing file'
assert_same_text '' "$TMP/missing_import.out" 'missing import runs no tests'

write_fixture "$FIX/cycle_a.ds" <<'DS'
import "./cycle_b.ds"

test "a" {
  assert true
}
DS
write_fixture "$FIX/cycle_b.ds" <<'DS'
import "./cycle_a.ds"

test "b" {
  assert true
}
DS
capture_status cyclic_import "$DS" test "$FIX/cycle_a.ds"
assert_nonzero_status cyclic_import
assert_contains "$TMP/cyclic_import.err" 'import cycle' 'cyclic import diagnostic'
assert_same_text '' "$TMP/cyclic_import.out" 'cyclic import runs no tests'

# Normal execution and Bash emission ignore tests.
write_fixture "$FIX/ignore_tests.ds" <<'DS'
echo "production"

test "should not run in emitted bash" {
  echo "test output"
  assert false
}
DS
run_ok ignore_vm_run "$DS" run "$FIX/ignore_tests.ds"
assert_same_text 'production
' "$TMP/ignore_vm_run.out" 'ds run ignores test block output'
assert_same_text '' "$TMP/ignore_vm_run.err" 'ds run ignores failing test stderr'
run_ok ignore_direct "$DS" "$FIX/ignore_tests.ds"
assert_same_text 'production
' "$TMP/ignore_direct.out" 'direct execution ignores test block output'
script="$TMP/ignore_tests.sh"
run_ok emit_ignore "$DS" emit bash "$FIX/ignore_tests.ds" -o "$script"
run_ok bash_ignore_syntax bash -n "$script"
run_ok bash_ignore bash "$script"
assert_same_text 'production
' "$TMP/bash_ignore.out" 'emitted Bash ignores test block output'
assert_not_contains "$script" 'should not run in emitted bash' 'normal Bash emission omits test names'
assert_not_contains "$script" 'assert false' 'normal Bash emission omits assert statements'
assert_not_contains "$script" 'ds test' 'normal Bash emission does not shell out to ds test'

write_fixture "$FIX/test_only.ds" <<'DS'
test "only" {
  assert true
}
DS
run_ok emit_test_only "$DS" emit bash "$FIX/test_only.ds" -o "$TMP/test_only.sh"
run_ok test_only_syntax bash -n "$TMP/test_only.sh"
run_ok test_only_bash bash "$TMP/test_only.sh"
assert_same_text '' "$TMP/test_only_bash.out" 'test-only emitted Bash is no-op stdout'
assert_same_text '' "$TMP/test_only_bash.err" 'test-only emitted Bash is no-op stderr'
assert_not_contains "$TMP/test_only.sh" 'ds test' 'test-only emitted Bash remains standalone'

# Diagnostics and stable output.
write_fixture "$FIX/no_tests.ds" <<'DS'
echo "production only"
DS
capture_status no_tests "$DS" test "$FIX/no_tests.ds"
assert_nonzero_status no_tests
assert_contains "$TMP/no_tests.err" 'no tests found' 'no-tests diagnostic'
assert_same_text '' "$TMP/no_tests.out" 'no-tests does not execute production statement'

write_fixture "$FIX/span.ds" <<'DS'
test "span" {
  let target = "dev"
  assert target == "prod"
}
DS
capture_status span_failure "$DS" test "$FIX/span.ds"
assert_nonzero_status span_failure
assert_contains "$TMP/span_failure.out" 'fail span' 'assertion failure output names test'
assert_diag_span "$TMP/span_failure.err" "$FIX/span.ds" 'test `span`: assertion failed' 'assertion failure source span'

write_fixture "$FIX/order.ds" <<'DS'
test "first" {
  assert true
}

test "second" {
  assert false
}

test "third" {
  assert true
}
DS
capture_status order_a "$DS" test "$FIX/order.ds"
capture_status order_b "$DS" test "$FIX/order.ds"
assert_nonzero_status order_a
assert_nonzero_status order_b
assert_same "$TMP/order_a.out" "$TMP/order_b.out" 'test report stdout is deterministic'
assert_same "$TMP/order_a.err" "$TMP/order_b.err" 'test report stderr is deterministic'
assert_matches "$TMP/order_a.out" '^ok   first$' 'first report line appears'
assert_matches "$TMP/order_a.out" '^fail second$' 'second report line appears'
assert_matches "$TMP/order_a.out" '^ok   third$' 'third report line appears'
assert_test_summary "$TMP/order_a.out" 3 2 1 'mixed order summary'

# CLI edge cases.
capture_status missing_file "$DS" test "$FIX/missing-file.ds"
assert_nonzero_status missing_file
assert_contains "$TMP/missing_file.err" 'missing-file.ds' 'missing file diagnostic includes path'
mkdir -p "$FIX/dir_input"
capture_status directory_input "$DS" test "$FIX/dir_input"
assert_nonzero_status directory_input
assert_contains "$TMP/directory_input.err" 'failed to read source file' 'directory discovery is deferred with clear load diagnostic'
write_fixture "$FIX/unknown_side_effect.ds" <<'DS'
file.write("should-not-exist.txt", "bad")

test "never" {
  assert true
}
DS
unknown_work="$TMP/unknown_work"
mkdir -p "$unknown_work"
capture_status unknown_flag bash -c "cd '$unknown_work' && '$DS' test --unknown '$FIX/unknown_side_effect.ds'"
assert_nonzero_status unknown_flag
assert_contains "$TMP/unknown_flag.err" 'expected `ds test <file.ds>`' 'unknown ds test flag fails usage'
[ ! -e "$unknown_work/should-not-exist.txt" ] || fail 'unknown ds test flag must not execute source'
pass 'unknown ds test flag does not execute source'
capture_status extra_arg "$DS" test "$FIX/simple.ds" extra
assert_nonzero_status extra_arg
assert_contains "$TMP/extra_arg.err" 'expected `ds test <file.ds>`' 'extra ds test arg fails usage'
write_fixture "$FIX/invalid_production_syntax.ds" <<'DS'
let x =

test "never" {
  assert true
}
DS
capture_status invalid_prod "$DS" test "$FIX/invalid_production_syntax.ds"
assert_nonzero_status invalid_prod
assert_contains "$TMP/invalid_prod.err" ': error:' 'invalid production syntax fails before tests'
assert_same_text '' "$TMP/invalid_prod.out" 'invalid production syntax runs no tests'

# Regression checks for debug commands with test files.
run_ok bytecode_test_file "$DS" bytecode "$FIX/simple.ds"
assert_contains "$TMP/bytecode_test_file.out" 'instructions:' 'bytecode still works with test files'
assert_not_matches "$TMP/bytecode_test_file.out" '0x[0-9a-fA-F]+' 'bytecode output remains pointer-free'
run_ok hir_no_test "$DS" hir "$FIX/no_tests.ds"
assert_contains "$TMP/hir_no_test.out" 'Program' 'HIR still works on files without tests'

# Optional Bash test backend is intentionally not implemented in v0.14.
capture_status backend_flag "$DS" test --backend bash "$FIX/simple.ds"
assert_nonzero_status backend_flag
assert_contains "$TMP/backend_flag.err" 'expected `ds test <file.ds>`' 'Bash test backend remains deferred and rejected clearly'

printf 'v0.14 assertions: %s\n' "$pass_count"
