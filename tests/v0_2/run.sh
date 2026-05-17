#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
FIX="tests/v0_2/fixtures"
GOLD="tests/v0_2/golden"
TMP="${TMPDIR:-/tmp}/ds_v0_2_tests.$$"
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

emit_fixture() {
  local name="$1"
  (cd "$ROOT" && "$DS" emit bash "$FIX/$name.ds" -o "$TMP/$name.sh") >"$TMP/$name.emit.out" 2>"$TMP/$name.emit.err" || {
    cat "$TMP/$name.emit.out" >&2 || true
    cat "$TMP/$name.emit.err" >&2 || true
    fail "$name: emit failed"
  }
}

# Build first so all tests exercise the local executable.
make -C "$ROOT" clean all >/dev/null

# Golden emitter tests and generated Bash syntax validity.
for name in empty comments_only simple_string int_bool simple_command interpolation if_without_else if_with_else nested_if mixed; do
  emit_fixture "$name"
  assert_same "$ROOT/$GOLD/$name.sh" "$TMP/$name.sh" "bash golden: $name"
  run_ok "bash_syntax_$name" bash -n "$TMP/$name.sh"
done

# Header tests.
head -n 1 "$TMP/mixed.sh" >"$TMP/header_1.out"
printf '#!/usr/bin/env bash\n' >"$TMP/header_1.expected"
assert_same "$TMP/header_1.expected" "$TMP/header_1.out" "bash header shebang"
assert_contains "$TMP/mixed.sh" "set -euo pipefail" "bash header strict mode"
line3="$(sed -n '3p' "$TMP/mixed.sh")"
[ -z "$line3" ] || fail "bash header should include blank line after strict mode"
pass "bash header blank line after strict mode"

# Variable emission safety and quoting.
cat >"$TMP/strings.ds" <<'EOF_DS'
let empty = ""
let spaced = "hello world"
let quoted = "hello \"world\""
let single = "it's ok"
let dollar = "$HOME"
let command_sub = "$(echo bad)"
let backticks = "`echo bad`"
let backslash = "C:\\tmp"
echo $empty
echo $spaced
echo $quoted
echo $single
echo $dollar
echo $command_sub
echo $backticks
echo $backslash
EOF_DS
run_ok emit_strings "$DS" emit bash "$TMP/strings.ds" -o "$TMP/strings.sh"
run_ok bash_syntax_strings bash -n "$TMP/strings.sh"
assert_contains "$TMP/strings.sh" '__ds_empty=""' "string empty quoted"
assert_contains "$TMP/strings.sh" '__ds_spaced="hello world"' "string spaces quoted"
assert_contains "$TMP/strings.sh" '__ds_quoted="hello \"world\""' "string double quote escaped"
assert_contains "$TMP/strings.sh" '__ds_dollar="\$HOME"' "string dollar escaped"
assert_contains "$TMP/strings.sh" '__ds_command_sub="\$(echo bad)"' "string command substitution escaped"
assert_contains "$TMP/strings.sh" '__ds_backticks="\`echo bad\`"' "string backticks escaped"
run_ok run_strings bash "$TMP/strings.sh"
assert_contains "$TMP/run_strings.out" '$HOME' "runtime literal dollar preserved"
assert_contains "$TMP/run_strings.out" '$(echo bad)' "runtime command substitution preserved as text"
assert_contains "$TMP/run_strings.out" '`echo bad`' "runtime backticks preserved as text"
assert_not_contains "$TMP/run_strings.out" '^bad$' "runtime did not execute command-looking string"

# Integer variable edge cases.
cat >"$TMP/ints.ds" <<'EOF_DS'
let zero = 0
let positive = 42
let large = 12345678901234567890
EOF_DS
run_ok emit_ints "$DS" emit bash "$TMP/ints.ds" -o "$TMP/ints.sh"
assert_contains "$TMP/ints.sh" '__ds_zero=0' "integer zero emitted"
assert_contains "$TMP/ints.sh" '__ds_positive=42' "integer positive emitted"
assert_contains "$TMP/ints.sh" '__ds_large=12345678901234567890' "integer large emitted"
run_ok bash_syntax_ints bash -n "$TMP/ints.sh"

# Interpolation positions and multiple variables.
cat >"$TMP/interp_positions.ds" <<'EOF_DS'
let name = "Danh"
let app = "api"
let a = "A"
let b = "B"
echo "{name} starts"
echo "middle {name} value"
echo "ends {name}"
echo "{a}{b}"
let greeting = "Hello {name} from {app}"
echo $greeting
EOF_DS
run_ok emit_interp_positions "$DS" emit bash "$TMP/interp_positions.ds" -o "$TMP/interp_positions.sh"
assert_contains "$TMP/interp_positions.sh" 'echo "${__ds_name} starts"' "interpolation beginning"
assert_contains "$TMP/interp_positions.sh" 'echo "middle ${__ds_name} value"' "interpolation middle"
assert_contains "$TMP/interp_positions.sh" 'echo "ends ${__ds_name}"' "interpolation end"
assert_contains "$TMP/interp_positions.sh" 'echo "${__ds_a}${__ds_b}"' "adjacent interpolations"
assert_contains "$TMP/interp_positions.sh" '__ds_greeting="Hello ${__ds_name} from ${__ds_app}"' "let string interpolation"
run_ok run_interp_positions bash "$TMP/interp_positions.sh"
assert_contains "$TMP/run_interp_positions.out" 'Hello Danh from api' "runtime let interpolation behavior"

# Literal braces and invalid interpolation are rejected clearly in v0.2.0.
cat >"$TMP/unknown_interp.ds" <<'EOF_DS'
echo "Hello {missing}"
EOF_DS
run_fail unknown_interp "$DS" emit bash "$TMP/unknown_interp.ds" -o "$TMP/unknown_interp.sh"
assert_contains "$TMP/unknown_interp.err" 'unknown interpolation variable `missing`' "unknown interpolation diagnostic"
[ ! -e "$TMP/unknown_interp.sh" ] || fail "unknown interpolation should not write output"
pass "unknown interpolation leaves no output file"

cat >"$TMP/literal_brace.ds" <<'EOF_DS'
echo "literal { brace"
EOF_DS
run_fail literal_brace "$DS" emit bash "$TMP/literal_brace.ds" -o "$TMP/literal_brace.sh"
assert_contains "$TMP/literal_brace.err" 'unsupported string interpolation; expected `{name}`' "literal brace rejected clearly"

# Command statement behavior.
cat >"$TMP/commands.ds" <<'EOF_DS'
echo "one"
echo "two words"
let arg = "three"
echo $arg
if true {
  echo "inside"
}
echo "after"
EOF_DS
run_ok emit_commands "$DS" emit bash "$TMP/commands.ds" -o "$TMP/commands.sh"
run_ok bash_syntax_commands bash -n "$TMP/commands.sh"
run_ok run_commands bash "$TMP/commands.sh"
cat >"$TMP/commands.expected" <<'EOF_EXPECT'
one
two words
three
inside
after
EOF_EXPECT
assert_same "$TMP/commands.expected" "$TMP/run_commands.out" "runtime command order and blocks"

# Branch behavior and strict mode failure behavior.
cat >"$TMP/true_branch.ds" <<'EOF_DS'
let ok = true
if ok {
  echo "yes"
} else {
  echo "no"
}
EOF_DS
run_ok emit_true_branch "$DS" emit bash "$TMP/true_branch.ds" -o "$TMP/true_branch.sh"
run_ok run_true_branch bash "$TMP/true_branch.sh"
printf 'yes\n' >"$TMP/true_branch.expected"
assert_same "$TMP/true_branch.expected" "$TMP/run_true_branch.out" "runtime true branch"

cat >"$TMP/false_branch.ds" <<'EOF_DS'
let ok = false
if ok {
  echo "yes"
} else {
  echo "no"
}
EOF_DS
run_ok emit_false_branch "$DS" emit bash "$TMP/false_branch.ds" -o "$TMP/false_branch.sh"
run_ok run_false_branch bash "$TMP/false_branch.sh"
printf 'no\n' >"$TMP/false_branch.expected"
assert_same "$TMP/false_branch.expected" "$TMP/run_false_branch.out" "runtime false branch"

cat >"$TMP/failing_command.ds" <<'EOF_DS'
false
echo "after"
EOF_DS
run_ok emit_failing_command "$DS" emit bash "$TMP/failing_command.ds" -o "$TMP/failing_command.sh"
run_fail run_failing_command bash "$TMP/failing_command.sh"
assert_not_contains "$TMP/run_failing_command.out" 'after' "strict mode stops after failing command"

# Comparison operator emission coverage. Numeric-vs-string dispatch is intentionally documented as deferred.
cat >"$TMP/comparisons.ds" <<'EOF_DS'
let a = "a"
let b = "b"
let n = 3
if a == "a" { echo "eq" }
if a != b { echo "ne" }
if b > a { echo "gt" }
if b >= a { echo "ge" }
if a < b { echo "lt" }
if a <= b { echo "le" }
if n >= 3 { echo "num-limited" }
EOF_DS
run_ok emit_comparisons "$DS" emit bash "$TMP/comparisons.ds" -o "$TMP/comparisons.sh"
assert_contains "$TMP/comparisons.sh" ' == ' "comparison emits =="
assert_contains "$TMP/comparisons.sh" ' != ' "comparison emits !="
assert_contains "$TMP/comparisons.sh" ' > ' "comparison emits >"
assert_contains "$TMP/comparisons.sh" ' < ' "comparison emits <"
assert_contains "$TMP/comparisons.sh" '! [[ "$__ds_b" < "$__ds_a" ]]' "comparison emits Bash-compatible >= shape"
assert_contains "$TMP/comparisons.sh" '! [[ "$__ds_a" > "$__ds_b" ]]' "comparison emits Bash-compatible <= shape"
assert_contains "$ROOT/docs/milestones/v0.2.0-spec.md" 'does not perform type-aware numeric dispatch yet' "comparison limitation documented in spec"
assert_contains "$ROOT/README.md" 'Known `v0.2.0` Bash-emission limitation' "comparison limitation documented in README"
run_ok bash_syntax_comparisons bash -n "$TMP/comparisons.sh"

# Diagnostics for unsupported/unsafe emission cases and CLI behavior.
cat >"$TMP/unsupported_assignment.ds" <<'EOF_DS'
let total = 1 + 2
EOF_DS
run_fail unsupported_assignment "$DS" emit bash "$TMP/unsupported_assignment.ds" -o "$TMP/unsupported_assignment.sh"
assert_contains "$TMP/unsupported_assignment.err" 'this expression cannot be emitted as a Bash assignment in v0.2.0' "unsupported assignment diagnostic"

cat >"$TMP/unknown_command_var.ds" <<'EOF_DS'
echo $missing
EOF_DS
run_fail unknown_command_var "$DS" emit bash "$TMP/unknown_command_var.ds" -o "$TMP/unknown_command_var.sh"
assert_contains "$TMP/unknown_command_var.err" 'unknown command variable `missing`' "unknown command variable diagnostic"

cat >"$TMP/unknown_condition_var.ds" <<'EOF_DS'
if missing {
  echo "bad"
}
EOF_DS
run_fail unknown_condition_var "$DS" emit bash "$TMP/unknown_condition_var.ds" -o "$TMP/unknown_condition_var.sh"
assert_contains "$TMP/unknown_condition_var.err" 'unknown variable `missing`' "unknown condition variable diagnostic"

run_fail cli_emit_missing_output "$DS" emit bash "$TMP/commands.ds"
assert_contains "$TMP/cli_emit_missing_output.err" 'expected `ds emit bash <file.ds> -o <file.sh>`' "CLI emit missing output diagnostic"

run_fail cli_emit_no_input "$DS" emit bash
assert_contains "$TMP/cli_emit_no_input.err" 'expected `ds emit bash <file.ds> -o <file.sh>`' "CLI emit no input diagnostic"

run_fail cli_emit_wrong_backend "$DS" emit zsh "$TMP/commands.ds" -o "$TMP/bad.sh"
assert_contains "$TMP/cli_emit_wrong_backend.err" 'expected `ds emit bash <file.ds> -o <file.sh>`' "CLI emit wrong backend diagnostic"

run_fail cli_emit_missing_input "$DS" emit bash "$TMP/nope.ds" -o "$TMP/nope.sh"
assert_contains "$TMP/cli_emit_missing_input.err" 'failed to open source file' "CLI emit missing input diagnostic"

run_fail cli_emit_unwritable_output "$DS" emit bash "$TMP/commands.ds" -o "$TMP/no_such_dir/out.sh"
assert_contains "$TMP/cli_emit_unwritable_output.err" 'failed to open output file' "CLI emit unwritable output diagnostic"

cat >"$TMP/invalid_source.ds" <<'EOF_DS'
if true {
  echo "missing close"
EOF_DS
run_fail invalid_source_before_emission "$DS" emit bash "$TMP/invalid_source.ds" -o "$TMP/invalid_source.sh"
assert_contains "$TMP/invalid_source_before_emission.err" 'expected `}`' "invalid source parse diagnostic"
[ ! -e "$TMP/invalid_source.sh" ] || fail "invalid source should not write output"
pass "invalid source leaves no output file"

# Variable names use the generated prefix consistently, including names that could look similar to helpers.
cat >"$TMP/prefix_collision.ds" <<'EOF_DS'
let __ds_name = "safe"
echo $__ds_name
EOF_DS
run_ok emit_prefix_collision "$DS" emit bash "$TMP/prefix_collision.ds" -o "$TMP/prefix_collision.sh"
assert_contains "$TMP/prefix_collision.sh" '__ds___ds_name="safe"' "generated prefix avoids direct variable collision"
assert_contains "$TMP/prefix_collision.sh" 'echo "$__ds___ds_name"' "command variable uses generated prefix"
run_ok run_prefix_collision bash "$TMP/prefix_collision.sh"
printf 'safe\n' >"$TMP/prefix_collision.expected"
assert_same "$TMP/prefix_collision.expected" "$TMP/run_prefix_collision.out" "runtime prefixed variable behavior"

# Edge cases explicitly listed in the v0.2.0 test plan.
printf 'let name = "Danh"\necho "hi {name}"' >"$TMP/no_trailing_newline.ds"
run_ok emit_no_trailing_newline "$DS" emit bash "$TMP/no_trailing_newline.ds" -o "$TMP/no_trailing_newline.sh"
run_ok bash_syntax_no_trailing_newline bash -n "$TMP/no_trailing_newline.sh"
run_ok run_no_trailing_newline bash "$TMP/no_trailing_newline.sh"
printf 'hi Danh\n' >"$TMP/no_trailing_newline.expected"
assert_same "$TMP/no_trailing_newline.expected" "$TMP/run_no_trailing_newline.out" "file without trailing newline emits valid Bash"

cat >"$TMP/deep_nested_if.ds" <<'EOF_DS'
let a = true
let b = true
let c = true
let d = true
if a {
  if b {
    if c {
      if d {
        echo "deep"
      }
    }
  }
}
EOF_DS
run_ok emit_deep_nested_if "$DS" emit bash "$TMP/deep_nested_if.ds" -o "$TMP/deep_nested_if.sh"
run_ok bash_syntax_deep_nested_if bash -n "$TMP/deep_nested_if.sh"
run_ok run_deep_nested_if bash "$TMP/deep_nested_if.sh"
printf 'deep\n' >"$TMP/deep_nested_if.expected"
assert_same "$TMP/deep_nested_if.expected" "$TMP/run_deep_nested_if.out" "deeply nested if emits valid Bash"

long_value="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
cat >"$TMP/long_string.ds" <<EOF_DS
let text = "$long_value"
echo \$text
EOF_DS
run_ok emit_long_string "$DS" emit bash "$TMP/long_string.ds" -o "$TMP/long_string.sh"
run_ok bash_syntax_long_string bash -n "$TMP/long_string.sh"
run_ok run_long_string bash "$TMP/long_string.sh"
printf '%s\n' "$long_value" >"$TMP/long_string.expected"
assert_same "$TMP/long_string.expected" "$TMP/run_long_string.out" "long string emits valid Bash"

{
  for i in $(seq 1 40); do
    printf 'let v%s = "%s"\n' "$i" "$i"
  done
  for i in $(seq 1 40); do
    printf 'echo $v%s\n' "$i"
  done
} >"$TMP/many_variables.ds"
run_ok emit_many_variables "$DS" emit bash "$TMP/many_variables.ds" -o "$TMP/many_variables.sh"
run_ok bash_syntax_many_variables bash -n "$TMP/many_variables.sh"
run_ok run_many_variables bash "$TMP/many_variables.sh"
seq 1 40 >"$TMP/many_variables.expected"
assert_same "$TMP/many_variables.expected" "$TMP/run_many_variables.out" "many variables emit valid Bash"

cat >"$TMP/interp_punctuation.ds" <<'EOF_DS'
let name = "Danh"
echo "Hello, {name}!"
echo "({name})[{name}]."
EOF_DS
run_ok emit_interp_punctuation "$DS" emit bash "$TMP/interp_punctuation.ds" -o "$TMP/interp_punctuation.sh"
assert_contains "$TMP/interp_punctuation.sh" 'echo "Hello, ${__ds_name}!"' "interpolation next to punctuation emits correctly"
run_ok run_interp_punctuation bash "$TMP/interp_punctuation.sh"
cat >"$TMP/interp_punctuation.expected" <<'EOF_EXPECT'
Hello, Danh!
(Danh)[Danh].
EOF_EXPECT
assert_same "$TMP/interp_punctuation.expected" "$TMP/run_interp_punctuation.out" "interpolation next to punctuation preserves text"

cat >"$TMP/future_syntax.ds" <<'EOF_DS'
script {
  arg app: string
}
EOF_DS
run_fail future_syntax_rejected "$DS" emit bash "$TMP/future_syntax.ds" -o "$TMP/future_syntax.sh"
assert_contains "$TMP/future_syntax_rejected.err" 'unexpected `}`' "future syntax rejected by v0.2 emitter"
[ ! -e "$TMP/future_syntax.sh" ] || fail "future syntax should not write output"
pass "future syntax leaves no output file"

test -f "$ROOT/docs/language.ds" || fail "docs/language.ds should exist"
pass "syntax catalog exists"

# Docs should reflect that v0.2.0 tests now exist.
assert_contains "$ROOT/README.md" 'Current status: `v0.4.0` implementation is complete' "README says v0.4.0 implementation complete"
assert_contains "$ROOT/docs/milestones/v0.2.0-spec.md" 'Implementation and tests complete' "spec status complete"
assert_contains "$ROOT/docs/milestones/v0.2.0-test-plan.md" 'Command failure' "test plan command failure case retained"
assert_contains "$ROOT/docs/milestones/v0.2.0-test-plan.md" '- [x] Edge cases are covered.' "test plan checklist marks edge cases covered"

make -C "$ROOT" check >/dev/null
pass "make check passes"

echo "v0.2.0 tests passed: $pass_count checks"
