#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
FIX="tests/v0_3/fixtures"
GOLD="tests/v0_3/golden"
TMP="${TMPDIR:-/tmp}/ds_v0_3_tests.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

pass_count=0

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

pass() {
  pass_count=$((pass_count + 1))
  echo "ok $pass_count - $*"
}

run_ok() {
  local name="$1"; shift
  "$@" >"$TMP/$name.out" 2>"$TMP/$name.err" || {
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected success"
  }
  pass "$name"
}

run_fail() {
  local name="$1"; shift
  if "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"; then
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected failure"
  fi
  pass "$name"
}

capture_status() {
  local name="$1"; shift
  set +e
  "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/$name.rc"
}

assert_status() {
  local name="$1"
  local expected="$2"
  local actual
  actual="$(cat "$TMP/$name.rc")"
  [ "$actual" = "$expected" ] || {
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected exit $expected, got $actual"
  }
  pass "$name exit $expected"
}

assert_nonzero_status() {
  local name="$1"
  local actual
  actual="$(cat "$TMP/$name.rc")"
  [ "$actual" != "0" ] || {
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected non-zero exit"
  }
  pass "$name exit non-zero"
}

assert_contains() {
  local file="$1"
  local text="$2"
  local name="$3"
  grep -F -- "$text" "$file" >/dev/null || {
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected to contain [$text]"
  }
  pass "$name"
}

assert_not_contains() {
  local file="$1"
  local text="$2"
  local name="$3"
  if grep -F -- "$text" "$file" >/dev/null; then
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected not to contain [$text]"
  fi
  pass "$name"
}

assert_same() {
  local expected="$1"
  local actual="$2"
  local name="$3"
  if ! diff -u "$expected" "$actual"; then
    fail "$name: output mismatch"
  fi
  pass "$name"
}

assert_same_text() {
  local expected="$1"
  local actual_file="$2"
  local name="$3"
  printf '%s' "$expected" >"$TMP/$name.expected"
  assert_same "$TMP/$name.expected" "$actual_file" "$name"
}

emit_bash() {
  local input="$1"
  local output="$2"
  (cd "$ROOT" && "$DS" emit bash "$input" -o "$output") >"$TMP/emit.out" 2>"$TMP/emit.err" || {
    cat "$TMP/emit.out" >&2 || true
    cat "$TMP/emit.err" >&2 || true
    fail "emit bash failed for $input"
  }
}

parity_ok() {
  local name="$1"
  local input="$2"
  capture_status "${name}_vm" "$DS" "$input"
  emit_bash "$input" "$TMP/$name.sh"
  run_ok "${name}_bash_syntax" bash -n "$TMP/$name.sh"
  capture_status "${name}_bash" bash "$TMP/$name.sh"
  assert_status "${name}_vm" "0"
  assert_status "${name}_bash" "0"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "parity stdout: $name"
}

parity_fail() {
  local name="$1"
  local input="$2"
  capture_status "${name}_vm" "$DS" "$input"
  emit_bash "$input" "$TMP/$name.sh"
  run_ok "${name}_bash_syntax" bash -n "$TMP/$name.sh"
  capture_status "${name}_bash" bash "$TMP/$name.sh"
  assert_nonzero_status "${name}_vm"
  assert_nonzero_status "${name}_bash"
}

# Build first so all tests exercise the local executable.
if [[ "${DS_SKIP_BUILD:-0}" != 1 ]]; then
  make -C "$ROOT" clean all >/dev/null
fi

# Runtime unit tests for strings, values, arrays, and map wrapper.
cc -std=c99 -Wall -Wextra -Wpedantic -I"$ROOT/include" \
  "$ROOT/tests/v0_3/unit/runtime.c" "$ROOT/src/runtime.c" "$ROOT/src/source.c" "$ROOT/src/diag.c" "$ROOT/src/runtime/hashmap.c" \
  -o "$TMP/test_v0_3_runtime"
run_ok runtime_unit "$TMP/test_v0_3_runtime"

# Direct lowering tests prove the shared lowered representation shape instead
# of only exercising it through the VM and Bash backends.
cc -std=c99 -Wall -Wextra -Wpedantic -I"$ROOT/include" \
  "$ROOT/tests/v0_3/unit/lower.c" \
  "$ROOT/src/lexer.c" "$ROOT/src/parser.c" "$ROOT/src/parse_expr.c" "$ROOT/src/parse_command.c" "$ROOT/src/parse_script.c" "$ROOT/src/parse_function.c" "$ROOT/src/parse_stmt.c" "$ROOT/src/ast.c" "$ROOT/src/lower.c" "$ROOT/src/lower_symbols.c" "$ROOT/src/lower_expr.c" "$ROOT/src/lower_command.c" "$ROOT/src/lower_stmt.c" "$ROOT/src/lower_stdlib.c" "$ROOT/src/lower_functions.c" "$ROOT/src/lower_tests.c" "$ROOT/src/lower_free.c" \
  "$ROOT/src/command.c" "$ROOT/src/runtime.c" "$ROOT/src/stdlib.c" "$ROOT/src/source.c" "$ROOT/src/diag.c" "$ROOT/src/runtime/hashmap.c" \
  -o "$TMP/test_v0_3_lower"
run_ok lowering_unit "$TMP/test_v0_3_lower"

# Bytecode dump golden tests and stable dump shape.
for name in empty comments_only simple_variable interpolation if_true if_false nested_if mixed; do
  (cd "$ROOT" && "$DS" bytecode "$FIX/$name.ds") >"$TMP/$name.bytecode" 2>"$TMP/$name.bytecode.err" || {
    cat "$TMP/$name.bytecode.err" >&2 || true
    fail "bytecode dump failed: $name"
  }
  assert_same "$ROOT/$GOLD/$name.bytecode" "$TMP/$name.bytecode" "bytecode golden: $name"
  assert_contains "$TMP/$name.bytecode" "constants:" "bytecode constants header: $name"
  assert_contains "$TMP/$name.bytecode" "instructions:" "bytecode instructions header: $name"
  assert_contains "$TMP/$name.bytecode" "RETURN" "bytecode return: $name"
  assert_not_contains "$TMP/$name.bytecode" "0x" "bytecode dump has no pointer addresses: $name"
done

assert_contains "$TMP/mixed.bytecode" "LOAD_CONST" "bytecode emits LOAD_CONST"
assert_contains "$TMP/mixed.bytecode" "STORE_VAR" "bytecode emits STORE_VAR"
assert_contains "$TMP/mixed.bytecode" "LOAD_VAR" "bytecode emits LOAD_VAR"
assert_contains "$TMP/mixed.bytecode" "COMPARE" "bytecode emits COMPARE"
assert_contains "$TMP/mixed.bytecode" "JUMP_IF_FALSE" "bytecode emits conditional jump"
assert_contains "$TMP/mixed.bytecode" "PUSH_SCOPE" "bytecode emits scope push"
assert_contains "$TMP/mixed.bytecode" "POP_SCOPE" "bytecode emits scope pop"
assert_contains "$TMP/mixed.bytecode" "RUN_CMD" "bytecode emits command instruction"
assert_contains "$TMP/interpolation.bytecode" "string \"api\"" "bytecode string constant formatting"
assert_contains "$TMP/if_true.bytecode" "bool true" "bytecode bool constant formatting"
assert_contains "$TMP/mixed.bytecode" "int 2" "bytecode int constant formatting"
assert_contains "$TMP/nested_if.bytecode" "tests/v0_3/fixtures/nested_if.ds:4:6" "bytecode source location inside nested block"

cat >"$TMP/source_map_comments.ds" <<'DS'
# leading comment

let name = "Danh"

# comment before command
echo $name
DS
run_ok bytecode_source_map_comments "$DS" bytecode "$TMP/source_map_comments.ds"
assert_contains "$TMP/bytecode_source_map_comments.out" "$TMP/source_map_comments.ds:3:12" "bytecode source mapping after blank lines for let value"
assert_contains "$TMP/bytecode_source_map_comments.out" "$TMP/source_map_comments.ds:6:1" "bytecode source mapping after comments for command"

cat >"$TMP/empty_block.ds" <<'DS'
if false {
}
echo "after"
DS
run_ok bytecode_empty_block "$DS" bytecode "$TMP/empty_block.ds"
assert_contains "$TMP/bytecode_empty_block.out" "JUMP_IF_FALSE" "bytecode empty block has conditional jump"
assert_contains "$TMP/bytecode_empty_block.out" "PUSH_SCOPE" "bytecode empty block has scope push"
assert_contains "$TMP/bytecode_empty_block.out" "POP_SCOPE" "bytecode empty block has scope pop"
run_ok vm_empty_block "$DS" run "$TMP/empty_block.ds"
assert_same_text $'after\n' "$TMP/vm_empty_block.out" "VM jumps over empty block"

# CLI behavior: implicit run, explicit run, bytecode dump, existing commands, and errors.
run_ok cli_implicit "$DS" "tests/v0_3/fixtures/simple_variable.ds"
assert_same_text $'Danh\n' "$TMP/cli_implicit.out" "implicit run stdout"
run_ok cli_explicit "$DS" run "tests/v0_3/fixtures/simple_variable.ds"
assert_same_text $'Danh\n' "$TMP/cli_explicit.out" "explicit run stdout"
assert_same "$TMP/cli_implicit.out" "$TMP/cli_explicit.out" "implicit and explicit run match"
run_ok cli_bytecode "$DS" bytecode "tests/v0_3/fixtures/simple_variable.ds"
assert_contains "$TMP/cli_bytecode.out" "RUN_CMD" "bytecode command does not execute script stdout"
run_ok cli_tokens "$DS" tokens examples/basic.ds
run_ok cli_ast "$DS" ast examples/basic.ds
run_ok cli_check "$DS" check examples/basic.ds
run_ok cli_emit "$DS" emit bash examples/basic.ds -o "$TMP/basic.sh"
run_ok cli_emit_bash_syntax bash -n "$TMP/basic.sh"
run_ok vm_example "$DS" examples/vm.ds
assert_same_text $'Hello Danh from vm\n' "$TMP/vm_example.out" "VM example script runs"
run_fail cli_no_args "$DS"
assert_contains "$TMP/cli_no_args.err" "Usage:" "CLI no args usage"
run_fail cli_run_no_input "$DS" run
assert_contains "$TMP/cli_run_no_input.err" "Usage:" "CLI run missing input usage"
run_fail cli_bytecode_no_input "$DS" bytecode
assert_contains "$TMP/cli_bytecode_no_input.err" "Usage:" "CLI bytecode missing input usage"
run_fail cli_run_missing "$DS" run "$TMP/missing.ds"
assert_contains "$TMP/cli_run_missing.err" "failed to open source file" "CLI run missing file diagnostic"
run_fail cli_bytecode_missing "$DS" bytecode "$TMP/missing.ds"
assert_contains "$TMP/cli_bytecode_missing.err" "failed to open source file" "CLI bytecode missing file diagnostic"
run_fail cli_unknown "$DS" unknown-command examples/basic.ds
assert_contains "$TMP/cli_unknown.err" 'unknown command `unknown-command`' "CLI unknown command diagnostic"

# VM execution values, expressions, conditionals, interpolation, command behavior, and source shapes.
run_ok vm_empty "$DS" "tests/v0_3/fixtures/empty.ds"
assert_same_text "" "$TMP/vm_empty.out" "VM empty stdout"
run_ok vm_comments "$DS" "tests/v0_3/fixtures/comments_only.ds"
assert_same_text "" "$TMP/vm_comments.out" "VM comments-only stdout"

cat >"$TMP/values.ds" <<'DS'
let empty = ""
let spaced = "hello world"
let quoted = "hello \"world\""
let backslash = "C:\\tmp"
let dollar = "$HOME"
let command_sub = "$(echo bad)"
let backticks = "`echo bad`"
let braces = "{spaced}"
let zero = 0
let one = 1
let large = 1234567890123
let truth = true
let lie = false
echo "[{empty}]"
echo $spaced
echo $quoted
echo $backslash
echo $dollar
echo $command_sub
echo $backticks
echo $braces
echo $zero
echo $one
echo $large
echo $truth
echo $lie
DS
run_ok vm_values "$DS" "$TMP/values.ds"
cat >"$TMP/values.expected" <<'EOF_EXPECT'
[]
hello world
hello "world"
C:\tmp
$HOME
$(echo bad)
`echo bad`
hello world
0
1
1234567890123
true
false
EOF_EXPECT
assert_same "$TMP/values.expected" "$TMP/vm_values.out" "VM value rendering and safe literals"
if grep -Fx -- 'bad' "$TMP/vm_values.out" >/dev/null; then
  fail "VM command-looking data should not execute as a standalone command"
fi
pass "VM command-looking data not executed"

cat >"$TMP/expressions.ds" <<'DS'
if true { echo "true" }
if false { echo "bad-false" }
if !true { echo "bad-not-true" } else { echo "not-true-ok" }
if !false { echo "not-false-ok" }
if 1 == 1 { echo "eq-int" }
if 1 != 2 { echo "ne-int" }
if 2 > 1 { echo "gt-int" }
if 2 >= 2 { echo "ge-int" }
if 1 < 2 { echo "lt-int" }
if 2 <= 2 { echo "le-int" }
if "a" == "a" { echo "eq-str" }
if "a" != "b" { echo "ne-str" }
if 10 < 2 { echo "string-style" }
DS
run_ok vm_expressions "$DS" "$TMP/expressions.ds"
cat >"$TMP/expressions.expected" <<'EOF_EXPECT'
true
not-true-ok
not-false-ok
eq-int
ne-int
gt-int
ge-int
lt-int
le-int
eq-str
ne-str
string-style
EOF_EXPECT
assert_same "$TMP/expressions.expected" "$TMP/vm_expressions.out" "VM expression execution"

cat >"$TMP/conditionals.ds" <<'DS'
let outer = true
let inner = false
if outer {
  echo "outer"
  if inner {
    echo "bad-inner"
  } else {
    echo "inner-else"
  }
}
if false {
  echo "bad-no-else"
}
echo "after"
DS
run_ok vm_conditionals "$DS" "$TMP/conditionals.ds"
cat >"$TMP/conditionals.expected" <<'EOF_EXPECT'
outer
inner-else
after
EOF_EXPECT
assert_same "$TMP/conditionals.expected" "$TMP/vm_conditionals.out" "VM conditional flow"

cat >"$TMP/interp.ds" <<'DS'
let app = "api"
let target = "staging"
let n = 3
let ok = true
echo "{app}-start"
echo "middle:{target}:value"
echo "end-{app}"
echo "{app}{target}"
echo "deploy:{app}:{target}!"
echo "n={n}, ok={ok}."
DS
run_ok vm_interpolation "$DS" "$TMP/interp.ds"
cat >"$TMP/interp.expected" <<'EOF_EXPECT'
api-start
middle:staging:value
end-api
apistaging
deploy:api:staging!
n=3, ok=true.
EOF_EXPECT
assert_same "$TMP/interp.expected" "$TMP/vm_interpolation.out" "VM interpolation positions and types"

cat >"$TMP/commands.ds" <<'DS'
echo "one"
printf "[%s]\n" "two words"
if true { echo "inside" }
echo "after"
DS
run_ok vm_commands "$DS" "$TMP/commands.ds"
cat >"$TMP/commands.expected" <<'EOF_EXPECT'
one
[two words]
inside
after
EOF_EXPECT
assert_same "$TMP/commands.expected" "$TMP/vm_commands.out" "VM command execution and argument boundaries"

cat >"$TMP/stderr.ds" <<'DS'
sh "-c" "printf err >&2"
DS
run_ok vm_command_stderr "$DS" "$TMP/stderr.ds"
assert_same_text "err" "$TMP/vm_command_stderr.err" "VM command stderr passthrough"

printf 'let name = "Danh"\necho "hi {name}"' >"$TMP/no_trailing_newline.ds"
run_ok vm_no_trailing_newline "$DS" "$TMP/no_trailing_newline.ds"
assert_same_text $'hi Danh\n' "$TMP/vm_no_trailing_newline.out" "VM no trailing newline"

{
  printf 'let gate = true\n'
  for i in $(seq 1 8); do printf 'if gate {\n'; done
  printf 'echo "deep"\n'
  for i in $(seq 1 8); do printf '}\n'; done
} >"$TMP/deep_if.ds"
run_ok vm_deep_if "$DS" "$TMP/deep_if.ds"
assert_same_text $'deep\n' "$TMP/vm_deep_if.out" "VM deep nested if"

long_value="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
cat >"$TMP/long_many.ds" <<DS
let text = "$long_value"
echo \$text
DS
for i in $(seq 1 60); do printf 'let v%s = "%s"\n' "$i" "$i" >>"$TMP/long_many.ds"; done
for i in $(seq 1 60); do printf 'echo $v%s\n' "$i" >>"$TMP/long_many.ds"; done
run_ok vm_long_many "$DS" "$TMP/long_many.ds"
{
  printf '%s\n' "$long_value"
  seq 1 60
} >"$TMP/long_many.expected"
assert_same "$TMP/long_many.expected" "$TMP/vm_long_many.out" "VM long strings and many variables"

# Lowering and diagnostics.
cat >"$TMP/unknown_var.ds" <<'DS'
echo $missing
DS
run_fail diag_unknown_cmd_var "$DS" run "$TMP/unknown_var.ds"
assert_contains "$TMP/diag_unknown_cmd_var.err" 'unknown command variable `missing`' "lowering unknown command variable diagnostic"
assert_contains "$TMP/diag_unknown_cmd_var.err" ':1:6: error:' "diagnostic location points at command variable"

cat >"$TMP/unknown_interp.ds" <<'DS'
echo "Hello {missing}"
DS
run_fail diag_unknown_interp "$DS" run "$TMP/unknown_interp.ds"
assert_contains "$TMP/diag_unknown_interp.err" 'unknown interpolation variable `missing`' "lowering unknown interpolation diagnostic"

cat >"$TMP/duplicate.ds" <<'DS'
let name = "one"
let name = "two"
DS
run_fail diag_duplicate "$DS" check "$TMP/duplicate.ds"
assert_contains "$TMP/diag_duplicate.err" 'duplicate variable `name`' "lowering duplicate declaration diagnostic"

cat >"$TMP/block_scope.ds" <<'DS'
if true {
  let hidden = "x"
}
echo $hidden
DS
run_fail diag_block_scope "$DS" run "$TMP/block_scope.ds"
assert_contains "$TMP/diag_block_scope.err" 'unknown command variable `hidden`' "lowering branch-local variable rejected after block"
assert_contains "$TMP/diag_block_scope.err" ':4:6: error:' "diagnostic after block points at command variable"

cat >"$TMP/inner_scope_ok.ds" <<'DS'
if true {
  let inside = "ok"
  echo $inside
}
DS
run_ok lower_inner_scope_ok "$DS" check "$TMP/inner_scope_ok.ds"
run_ok vm_inner_scope_ok "$DS" run "$TMP/inner_scope_ok.ds"
assert_same_text $'ok\n' "$TMP/vm_inner_scope_ok.out" "branch-local variable works inside block"

cat >"$TMP/runtime_scope_shadow.ds" <<'DS'
let name = "outer"
if true {
  let inner = "inner"
  echo $inner
}
echo $name
if true {
  let left = "left"
  echo $left
}
if true {
  let left = "right"
  echo $left
}
echo $name
DS
run_ok vm_runtime_scope_shadow "$DS" run "$TMP/runtime_scope_shadow.ds"
cat >"$TMP/runtime_scope_shadow.expected" <<'EOF_EXPECT'
inner
outer
left
right
outer
EOF_EXPECT
assert_same "$TMP/runtime_scope_shadow.expected" "$TMP/vm_runtime_scope_shadow.out" "VM runtime scopes isolate nested declarations"

cat >"$TMP/lower_shadow_rejected.ds" <<'DS'
let name = "outer"
if true {
  let name = "inner"
}
DS
run_ok lower_shadow_rejected "$DS" check "$TMP/lower_shadow_rejected.ds"
assert_contains "$TMP/lower_shadow_rejected.err" 'shadows an outer declaration' "checker warns for nested shadowing consistently"

cat >"$TMP/unsupported_expr.ds" <<'DS'
let total = "a" + "b"
DS
run_fail diag_unsupported_expr "$DS" run "$TMP/unsupported_expr.ds"
assert_contains "$TMP/diag_unsupported_expr.err" 'string binary `+` cannot be emitted to standalone Bash with parity' "unsupported expression diagnostic"

cat >"$TMP/parse_error.ds" <<'DS'
if true {
  echo "missing close"
DS
run_fail diag_parse_error "$DS" run "$TMP/parse_error.ds"
assert_contains "$TMP/diag_parse_error.err" 'expected `}`' "parse error before VM execution"

cat >"$TMP/missing_command.ds" <<'DS'
missing_command_for_ds_v0_3_test
DS
run_fail vm_missing_command "$DS" run "$TMP/missing_command.ds"
assert_contains "$TMP/vm_missing_command.err" 'failed to launch command `missing_command_for_ds_v0_3_test`' "missing command process diagnostic"

cat >"$TMP/failing_command.ds" <<'DS'
false
echo "after"
DS
run_fail vm_failing_command "$DS" run "$TMP/failing_command.ds"
assert_not_contains "$TMP/vm_failing_command.out" 'after' "VM stops after failing command"

cat >"$TMP/exit_7.ds" <<'DS'
sh "-c" "exit 7"
DS
capture_status vm_exit_7 "$DS" run "$TMP/exit_7.ds"
assert_status vm_exit_7 "7"

cat >"$TMP/long_output.ds" <<'DS'
sh "-c" "i=0; while [ $i -lt 300 ]; do printf 'line-%03d abcdefghijklmnopqrstuvwxyz\n' $i; i=$((i + 1)); done"
DS
run_ok vm_long_command_output "$DS" run "$TMP/long_output.ds"
seq 0 299 | awk '{printf "line-%03d abcdefghijklmnopqrstuvwxyz\n", $1}' >"$TMP/long_output.expected"
assert_same "$TMP/long_output.expected" "$TMP/vm_long_command_output.out" "VM preserves long command output across stdio buffering"

cat >"$TMP/failing_branch.ds" <<'DS'
if true {
  false
  echo "after"
}
DS
run_fail vm_failing_branch "$DS" run "$TMP/failing_branch.ds"
assert_not_contains "$TMP/vm_failing_branch.out" 'after' "VM stops after failing command in branch"

cat >"$TMP/array_syntax.ds" <<'DS'
let xs = [1, 2]
let first = xs[0]
echo "{first}"
DS
run_ok array_syntax_supported "$DS" run "$TMP/array_syntax.ds"
assert_same_text $'1\n' "$TMP/array_syntax_supported.out" "array syntax is supported after v0.10"

# VM/Bash parity across supported success and failure cases.
parity_ok parity_empty "tests/v0_3/fixtures/empty.ds"
parity_ok parity_comments "tests/v0_3/fixtures/comments_only.ds"
parity_ok parity_simple "tests/v0_3/fixtures/simple_variable.ds"
parity_ok parity_interpolation "tests/v0_3/fixtures/interpolation.ds"
parity_ok parity_true "tests/v0_3/fixtures/if_true.ds"
parity_ok parity_false "tests/v0_3/fixtures/if_false.ds"
parity_ok parity_nested "tests/v0_3/fixtures/nested_if.ds"
parity_ok parity_mixed "tests/v0_3/fixtures/mixed.ds"
parity_ok parity_values "$TMP/values.ds"
parity_ok parity_expressions "$TMP/expressions.ds"
parity_ok parity_conditionals "$TMP/conditionals.ds"
parity_ok parity_commands "$TMP/commands.ds"
parity_ok parity_no_trailing_newline "$TMP/no_trailing_newline.ds"
parity_fail parity_failing_command "$TMP/failing_command.ds"
parity_fail parity_missing_command "$TMP/missing_command.ds"

# Memory/ownership checks under available sanitizers. These are skipped only when
# the local compiler cannot build sanitizer binaries.
if cc -std=c99 -Wall -Wextra -Wpedantic -fsanitize=address -I"$ROOT/include" \
  "$ROOT/tests/v0_3/unit/runtime.c" "$ROOT/src/runtime.c" "$ROOT/src/source.c" "$ROOT/src/diag.c" "$ROOT/src/runtime/hashmap.c" \
  -o "$TMP/test_v0_3_runtime_asan" >/dev/null 2>"$TMP/asan_build.err"; then
  run_ok runtime_unit_asan "$TMP/test_v0_3_runtime_asan"
else
  pass "runtime_unit_asan skipped: compiler does not support address sanitizer"
fi

if cc -std=c99 -Wall -Wextra -Wpedantic -fsanitize=undefined -I"$ROOT/include" \
  "$ROOT/tests/v0_3/unit/runtime.c" "$ROOT/src/runtime.c" "$ROOT/src/source.c" "$ROOT/src/diag.c" "$ROOT/src/runtime/hashmap.c" \
  -o "$TMP/test_v0_3_runtime_ubsan" >/dev/null 2>"$TMP/ubsan_build.err"; then
  run_ok runtime_unit_ubsan "$TMP/test_v0_3_runtime_ubsan"
else
  pass "runtime_unit_ubsan skipped: compiler does not support undefined-behavior sanitizer"
fi

# Prior milestone regressions still pass as part of the top-level test target; run
# check here too so v0.3 command additions do not break frontend validation.
make -C "$ROOT" check >/dev/null
pass "make check passes"

echo "v0.3.0 tests passed: $pass_count checks"
