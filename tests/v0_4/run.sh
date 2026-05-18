#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_4_tests.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != 1 ]]; then
  make -C "$ROOT" clean all >/dev/null
fi

cc -std=c99 -Wall -Wextra -Wpedantic -I"$ROOT/include" \
  "$ROOT/tests/v0_4/unit/runtime.c" "$ROOT/src/runtime.c" "$ROOT/src/source.c" "$ROOT/src/diag.c" "$ROOT/src/runtime/hashmap.c" \
  -o "$TMP/test_v0_4_runtime"
run_ok runtime_ownership_unit "$TMP/test_v0_4_runtime"

cat >"$TMP/pipeline_valid.ds" <<'DS'
let name = "Danh"
let count = 2
if count >= 2 {
  echo "Hello {name}!"
} else {
  echo "no"
}
DS
run_ok pipeline_check "$DS" check "$TMP/pipeline_valid.ds"
run_ok pipeline_emit "$DS" emit bash "$TMP/pipeline_valid.ds" -o "$TMP/pipeline_valid.sh"
run_ok pipeline_emit_syntax bash -n "$TMP/pipeline_valid.sh"
run_ok pipeline_bytecode "$DS" bytecode "$TMP/pipeline_valid.ds"
run_ok pipeline_run "$DS" run "$TMP/pipeline_valid.ds"
run_ok pipeline_direct "$DS" "$TMP/pipeline_valid.ds"
assert_same_text $'Hello Danh!\n' "$TMP/pipeline_run.out" "pipeline run stdout"
assert_same "$TMP/pipeline_run.out" "$TMP/pipeline_direct.out" "direct and explicit run stdout match"
assert_contains "$TMP/pipeline_bytecode.out" "$TMP/pipeline_valid.ds:3:4" "bytecode preserves condition source span"
assert_contains "$TMP/pipeline_valid.sh" "# ds: $TMP/pipeline_valid.ds:4" "bash preserves command source comment"
assert_not_contains "$TMP/pipeline_valid.sh" "$DS" "generated bash does not reference ds binary path"
assert_not_contains "$TMP/pipeline_valid.sh" "source " "generated bash does not source runtime"
(
  cd "$TMP"
  PATH="/usr/bin:/bin" bash "$TMP/pipeline_valid.sh"
) >"$TMP/pipeline_bash_outside_repo.out" 2>"$TMP/pipeline_bash_outside_repo.err" || {
  cat "$TMP/pipeline_bash_outside_repo.err" >&2 || true
  fail "generated bash should run outside repository"
}
pass "generated bash runs outside repository"
assert_same "$TMP/pipeline_run.out" "$TMP/pipeline_bash_outside_repo.out" "VM/Bash stdout parity after cleanup"

cat >"$TMP/lexer_only.ds" <<'DS'
echo "unterminated
DS
run_fail lexer_tokens_error "$DS" tokens "$TMP/lexer_only.ds"
assert_contains "$TMP/lexer_tokens_error.err" "$TMP/lexer_only.ds:1:6: error:" "tokens reports lexer diagnostic"
run_fail lexer_check_error "$DS" check "$TMP/lexer_only.ds"
assert_contains "$TMP/lexer_check_error.err" "$TMP/lexer_only.ds:1:6: error:" "check reports same lexer diagnostic"

cat >"$TMP/parser_error.ds" <<'DS'
if true {
  echo "open"
DS
run_fail parser_check_error "$DS" check "$TMP/parser_error.ds"
run_fail parser_emit_error "$DS" emit bash "$TMP/parser_error.ds" -o "$TMP/parser_error.sh"
run_fail parser_bytecode_error "$DS" bytecode "$TMP/parser_error.ds"
run_fail parser_run_error "$DS" run "$TMP/parser_error.ds"
run_fail parser_direct_error "$DS" "$TMP/parser_error.ds"
for name in parser_check_error parser_emit_error parser_bytecode_error parser_run_error parser_direct_error; do
  assert_contains "$TMP/$name.err" "$TMP/parser_error.ds:3:1: error:" "$name source diagnostic shape"
done
assert_file_missing_or_empty "$TMP/parser_error.sh" "emit bash leaves no artifact on parse failure"

cat >"$TMP/lowering_error.ds" <<'DS'
let name = "Danh"
echo "Hello {missing}"
DS
run_fail lower_check_error "$DS" check "$TMP/lowering_error.ds"
run_fail lower_emit_error "$DS" emit bash "$TMP/lowering_error.ds" -o "$TMP/lowering_error.sh"
run_fail lower_bytecode_error "$DS" bytecode "$TMP/lowering_error.ds"
run_fail lower_run_error "$DS" run "$TMP/lowering_error.ds"
run_fail lower_direct_error "$DS" "$TMP/lowering_error.ds"
for name in lower_check_error lower_emit_error lower_bytecode_error lower_run_error lower_direct_error; do
  assert_contains "$TMP/$name.err" "$TMP/lowering_error.ds:2:6: error: unknown interpolation variable" "$name lowering source diagnostic shape"
  assert_contains "$TMP/$name.err" '^' "$name keeps caret rendering"
done
assert_file_missing_or_empty "$TMP/lowering_error.sh" "emit bash leaves no artifact on lowering failure"

cat >"$TMP/first_column_error.ds" <<'DS'
}
DS
run_fail first_column_error "$DS" check "$TMP/first_column_error.ds"
assert_contains "$TMP/first_column_error.err" "$TMP/first_column_error.ds:1:1: error:" "first-column diagnostic shape"

printf 'let name = "Danh"\necho $missing' >"$TMP/eof_error.ds"
run_fail eof_error "$DS" check "$TMP/eof_error.ds"
assert_contains "$TMP/eof_error.err" "$TMP/eof_error.ds:2:6: error: unknown command variable" "EOF without newline diagnostic shape"

cat >"$TMP/blank_nested_error.ds" <<'DS'
let ok = true


if ok {
  if ok {
    echo "{missing}"
  }
}
DS
run_fail blank_nested_error "$DS" check "$TMP/blank_nested_error.ds"
assert_contains "$TMP/blank_nested_error.err" "$TMP/blank_nested_error.ds:6:10: error: unknown interpolation variable" "nested diagnostic after blank lines"

cat >"$TMP/else_error.ds" <<'DS'
if false {
  echo "no"
} else {
  echo $missing
}
DS
run_fail else_error "$DS" check "$TMP/else_error.ds"
assert_contains "$TMP/else_error.err" "$TMP/else_error.ds:4:8: error: unknown command variable" "else branch source span"

run_fail missing_file "$DS" check "$TMP/does_not_exist.ds"
assert_contains "$TMP/missing_file.err" "$TMP/does_not_exist.ds:1:1: error: failed to open source file" "missing file diagnostic shape"
run_fail usage_tokens_missing "$DS" tokens
assert_contains "$TMP/usage_tokens_missing.err" "error: expected a command and <file.ds>" "tokens missing input usage"
assert_not_contains "$TMP/usage_tokens_missing.err" ":1:1: error:" "usage error is not source-tied"
run_fail usage_ast_missing "$DS" ast
assert_contains "$TMP/usage_ast_missing.err" "error: expected a command and <file.ds>" "ast missing input usage"
run_fail usage_check_missing "$DS" check
assert_contains "$TMP/usage_check_missing.err" "error: expected a command and <file.ds>" "check missing input usage"
run_fail usage_run_missing "$DS" run
assert_contains "$TMP/usage_run_missing.err" 'error: expected `ds run <file.ds> [args...]`' "run missing input usage"
run_fail usage_bytecode_missing "$DS" bytecode
assert_contains "$TMP/usage_bytecode_missing.err" "error: expected a command and <file.ds>" "bytecode missing input usage"
run_fail usage_emit_missing_input "$DS" emit bash
assert_contains "$TMP/usage_emit_missing_input.err" 'expected `ds emit bash <file.ds> -o <file.sh>`' "emit missing input usage"
run_fail usage_emit_missing_output "$DS" emit bash "$TMP/pipeline_valid.ds" -o
assert_contains "$TMP/usage_emit_missing_output.err" 'expected `ds emit bash <file.ds> -o <file.sh>`' "emit missing output usage"
run_fail usage_emit_extra_arg "$DS" emit bash "$TMP/pipeline_valid.ds" -o "$TMP/extra.sh" extra
assert_contains "$TMP/usage_emit_extra_arg.err" 'expected `ds emit bash <file.ds> -o <file.sh>`' "emit extra arg rejected"
for cmd in tokens ast check bytecode; do
  run_fail "usage_${cmd}_extra_arg" "$DS" "$cmd" "$TMP/pipeline_valid.ds" extra
  assert_contains "$TMP/usage_${cmd}_extra_arg.err" "error: expected a command and <file.ds>" "$cmd extra arg rejected"
done
run_fail usage_run_extra_arg "$DS" run "$TMP/pipeline_valid.ds" extra
assert_contains "$TMP/usage_run_extra_arg.err" 'unexpected script arguments' "run extra arg rejected for scripts without arg contract"
run_fail usage_unknown_command "$DS" frobnicate "$TMP/pipeline_valid.ds"
assert_contains "$TMP/usage_unknown_command.err" 'error: unknown command `frobnicate`' "unknown command usage"
run_fail usage_direct_extra_arg "$DS" "$TMP/pipeline_valid.ds" extra
assert_contains "$TMP/usage_direct_extra_arg.err" 'unexpected script arguments' "direct script extra arg rejected for scripts without arg contract"

cat >"$TMP/future_assignment.ds" <<'DS'
let total = 1 + 2
DS
run_fail unsupported_expr "$DS" check "$TMP/future_assignment.ds"
assert_contains "$TMP/unsupported_expr.err" "$TMP/future_assignment.ds:1:13: error:" "unsupported expression diagnostic shape"

cat >"$TMP/runtime_command_not_found.ds" <<'DS'
__ds_command_that_should_not_exist_040
DS
capture_status runtime_command_not_found "$DS" run "$TMP/runtime_command_not_found.ds"
assert_status runtime_command_not_found 127
assert_contains "$TMP/runtime_command_not_found.err" "failed to launch command" "command launch failure reported"

cat >"$TMP/exit_7.ds" <<'DS'
sh -c "exit 7"
DS
capture_status command_exit_7 "$DS" run "$TMP/exit_7.ds"
assert_status command_exit_7 7
emit_bash "$TMP/exit_7.ds" "$TMP/exit_7.sh"
capture_status command_exit_7_bash bash "$TMP/exit_7.sh"
assert_status command_exit_7_bash 7

cat >"$TMP/long_output.ds" <<'DS'
sh -c "i=0; while [ $i -lt 300 ]; do printf '0123456789'; i=$((i+1)); done; printf '\n'"
DS
run_ok long_output_vm "$DS" run "$TMP/long_output.ds"
emit_bash "$TMP/long_output.ds" "$TMP/long_output.sh"
run_ok long_output_bash bash "$TMP/long_output.sh"
assert_same "$TMP/long_output_vm.out" "$TMP/long_output_bash.out" "long output VM/Bash parity"

cat >"$TMP/no_output_branch.ds" <<'DS'
if false {
  echo "hidden"
}
DS
run_ok no_output_branch "$DS" run "$TMP/no_output_branch.ds"
assert_same_text '' "$TMP/no_output_branch.out" "branch with no output"

cat >"$TMP/bash_literals.ds" <<'DS'
echo "$HOME $(echo bad) `bad` stays literal"
DS
emit_bash "$TMP/bash_literals.ds" "$TMP/bash_literals.sh"
run_ok bash_literals_syntax bash -n "$TMP/bash_literals.sh"
run_ok bash_literals bash "$TMP/bash_literals.sh"
assert_same_text '$HOME $(echo bad) `bad` stays literal
' "$TMP/bash_literals.out" "bash syntax-looking strings stay literal"

cat >"$TMP/source_map.ds" <<'DS'
let name = "Danh"

if true {
  echo "Hello {name}"
} else {
  echo "no"
}
DS
run_ok source_map_bytecode "$DS" bytecode "$TMP/source_map.ds"
assert_contains "$TMP/source_map_bytecode.out" "$TMP/source_map.ds:4:3" "bytecode command source after blank line"
emit_bash "$TMP/source_map.ds" "$TMP/source_map.sh"
assert_contains "$TMP/source_map.sh" "# ds: $TMP/source_map.ds:4" "bash command source after blank line"

# Direct boundary/static checks: backends should not switch over AST node kinds for language semantics.
if grep -E 'DS_STMT_|DS_EXPR_' "$ROOT/src/vm.c" "$ROOT/src/bash_emit.c" >/dev/null; then
  grep -nE 'DS_STMT_|DS_EXPR_' "$ROOT/src/vm.c" "$ROOT/src/bash_emit.c" >&2 || true
  fail "backend source should consume lowered nodes, not AST enum cases"
fi
pass "VM and Bash backends do not switch over AST enum cases"

if grep -R -nE '#include[[:space:]]+[<"].*hashmap|struct hashmap|hashmap_' "$ROOT/src" "$ROOT/include" | grep -v 'src/runtime.c' | grep -v 'src/runtime/hashmap\.[ch]' >/dev/null; then
  grep -R -nE '#include[[:space:]]+[<"].*hashmap|struct hashmap|hashmap_' "$ROOT/src" "$ROOT/include" | grep -v 'src/runtime.c' | grep -v 'src/runtime/hashmap\.[ch]' >&2 || true
  fail "production src/include should not leak staged hashmap API outside DsMap runtime wrapper"
fi
pass "staged hashmap API is contained to the DsMap runtime wrapper"

# Syntax that remains future/deferred after the current milestone must be rejected.
declare -A future_syntax
future_syntax[import_block]='import "other.ds" { }'
future_syntax[capture]='let out = $(echo hi)'
future_syntax[redirect_capture]='let redirected = echo "hi" > out.txt'
future_syntax[env_direct]='let home = env.HOME'
for name in "${!future_syntax[@]}"; do
  printf '%s\n' "${future_syntax[$name]}" >"$TMP/future_$name.ds"
  run_fail "future_$name" "$DS" check "$TMP/future_$name.ds"
  assert_contains "$TMP/future_$name.err" "$TMP/future_$name.ds:" "future syntax $name reports source path"
  assert_contains "$TMP/future_$name.err" "error:" "future syntax $name reports error shape"
done

# Fixture/test-runner robustness for v0.4 layout.
[ -d "$ROOT/tests/v0_4/fixtures" ] || fail "v0.4 fixture directory missing"
pass "v0.4 fixture directory exists"
[ -d "$ROOT/tests/v0_4/golden" ] || fail "v0.4 golden directory missing"
pass "v0.4 golden directory exists"
[ -x "$ROOT/tests/lib/testlib.sh" ] || fail "shared test helper must be executable"
pass "shared shell test helper is executable"

printf 'same\n' >"$TMP/golden.expected"
printf 'same\n' >"$TMP/golden.actual"
assert_golden "$TMP/golden.expected" "$TMP/golden.actual" "shared golden helper accepts matching files"
if (TMP="$TMP/golden_missing_tmp"; mkdir -p "$TMP"; source "$ROOT/tests/lib/testlib.sh"; assert_golden "$TMP/missing.golden" "$TMP/golden.actual" missing_golden_case) \
  >"$TMP/golden_missing.out" 2>"$TMP/golden_missing.err"; then
  fail "shared golden helper should fail on missing golden"
fi
pass "shared golden helper rejects missing golden"
assert_contains "$TMP/golden_missing.err" "missing golden file" "missing golden failure explains cause"
printf 'different\n' >"$TMP/golden.actual"
if (TMP="$TMP/golden_mismatch_tmp"; mkdir -p "$TMP"; source "$ROOT/tests/lib/testlib.sh"; assert_golden "$TMP/../golden.expected" "$TMP/../golden.actual" mismatch_case) \
  >"$TMP/golden_mismatch.out" 2>"$TMP/golden_mismatch.err"; then
  fail "shared golden helper should fail on mismatch"
fi
pass "shared golden helper rejects mismatch"
assert_contains "$TMP/golden_mismatch.err" "golden mismatch" "golden mismatch failure explains cause"
assert_not_contains "$TMP/pipeline_valid.sh" "0x" "generated bash has no pointer addresses"
assert_not_contains "$TMP/pipeline_bytecode.out" "0x" "bytecode has no pointer addresses"

run_ok help_output "$DS" --help
for line in \
  "ds <file.ds> [args...]" \
  "ds run <file.ds> [args...]" \
  "ds tokens <file.ds>" \
  "ds ast <file.ds>" \
  "ds check <file.ds>" \
  "ds bytecode <file.ds>" \
  "ds emit bash <file.ds> -o <file.sh>"; do
  assert_contains "$TMP/help_output.out" "$line" "help lists $line"
done
assert_contains "$ROOT/README.md" "./ds examples/basic.ds" "README documents direct execution"
assert_contains "$ROOT/README.md" "./ds run examples/basic.ds" "README documents run command"
assert_contains "$ROOT/README.md" "./ds tokens examples/basic.ds" "README documents tokens command"
assert_contains "$ROOT/README.md" "./ds ast examples/basic.ds" "README documents ast command"
assert_contains "$ROOT/README.md" "./ds check examples/basic.ds" "README documents check command"
assert_contains "$ROOT/README.md" "./ds bytecode examples/basic.ds" "README documents bytecode command"
assert_contains "$ROOT/README.md" "./ds emit bash examples/basic.ds -o /tmp/basic.sh" "README documents emit bash command"
assert_contains "$ROOT/docs/architecture.md" "lowered" "architecture documents lowered pipeline"
assert_contains "$ROOT/docs/runtime.md" "ownership" "runtime docs mention ownership"
assert_contains "$ROOT/docs/language.ds" "future" "language catalog marks future syntax"
assert_contains "$ROOT/README.md" "array/map literals" "README documents implemented arrays/maps"

printf 'v0.4.0 tests passed: %d checks\n' "$pass_count"
