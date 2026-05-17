#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_7_tests.$$"
FIX="$ROOT/tests/v0_7/fixtures"
GOLD="$ROOT/tests/v0_7/golden"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != 1 ]]; then
  make -C "$ROOT" clean all >/dev/null
fi

# Runtime ownership unit tests for command-result values.
cc -std=c99 -Wall -Wextra -Wpedantic -I"$ROOT/include" \
  "$ROOT/tests/v0_7/unit/process_result.c" "$ROOT/src/runtime.c" "$ROOT/src/source.c" "$ROOT/src/diag.c" \
  -o "$TMP/test_v0_7_process_result"
run_ok process_result_unit "$TMP/test_v0_7_process_result"

# Direct lowering tests verify the shared backend-facing command-result/redirection shape.
cc -std=c99 -Wall -Wextra -Wpedantic -I"$ROOT/include" \
  "$ROOT/tests/v0_7/unit/lower_command_result.c" \
  "$ROOT/src/lexer.c" "$ROOT/src/parser.c" "$ROOT/src/ast.c" "$ROOT/src/lower.c" \
  "$ROOT/src/runtime.c" "$ROOT/src/source.c" "$ROOT/src/diag.c" \
  -o "$TMP/test_v0_7_lower_command_result"
run_ok lower_command_result_unit "$TMP/test_v0_7_lower_command_result"

cd "$ROOT"

# Lexer/parser/AST golden coverage for new syntax.
run_ok tokens_v0_7 "$DS" tokens tests/v0_7/fixtures/helpers/lex_all.ds
assert_golden "$GOLD/lex_all.tokens" "$TMP/tokens_v0_7.out" "v0.7 token golden"
for token in RUN DOT REDIRECT_OUT REDIRECT_OUT_APPEND REDIRECT_ERR REDIRECT_ERR_APPEND REDIRECT_ALL REDIRECT_ALL_APPEND; do
  assert_contains "$TMP/tokens_v0_7.out" "$token" "token output includes $token"
done
run_ok ast_v0_7 "$DS" ast tests/v0_7/fixtures/helpers/ast_all.ds
assert_golden "$GOLD/ast_all.ast" "$TMP/ast_v0_7.out" "v0.7 AST golden"
assert_contains "$TMP/ast_v0_7.out" "RunExpr" "AST prints captured command"
assert_contains "$TMP/ast_v0_7.out" "FieldExpr stdout" "AST prints command-result field"
assert_contains "$TMP/ast_v0_7.out" "Redirect &>" "AST prints combined redirect"
assert_not_contains "$TMP/ast_v0_7.out" "0x" "AST output is pointer-free"

# Bytecode golden coverage for captures, fields, conditions, and redirections.
run_ok bytecode_fields "$DS" bytecode tests/v0_7/fixtures/helpers/bytecode_fields.ds
assert_golden "$GOLD/bytecode_fields.bytecode" "$TMP/bytecode_fields.out" "command-result bytecode golden"
for op in RUN_CAPTURE GET_FIELD RUN_CMD JUMP_IF_FALSE RETURN; do
  assert_contains "$TMP/bytecode_fields.out" "$op" "bytecode includes $op"
done
assert_contains "$TMP/bytecode_fields.out" ".stdout" "bytecode includes stdout field"
assert_contains "$TMP/bytecode_fields.out" ".stderr" "bytecode includes stderr field"
assert_contains "$TMP/bytecode_fields.out" ".code" "bytecode includes code field"
assert_contains "$TMP/bytecode_fields.out" ".ok" "bytecode includes ok field"
assert_contains "$TMP/bytecode_fields.out" ".failed" "bytecode includes failed field"
run_ok bytecode_combined_append "$DS" bytecode tests/v0_7/fixtures/redirection/combined_append.ds
assert_golden "$GOLD/combined_append.bytecode" "$TMP/bytecode_combined_append.out" "combined redirect bytecode golden"
assert_contains "$TMP/bytecode_combined_append.out" "&>" "bytecode marks combined truncate"
assert_contains "$TMP/bytecode_combined_append.out" "&>>" "bytecode marks combined append"

# VM capture behavior.
run_ok capture_stdout "$DS" run "$FIX/capture/stdout.ds"
assert_same_text $'hello\n\n0\nok\nnot failed\n' "$TMP/capture_stdout.out" "captured stdout fields"
assert_same_text '' "$TMP/capture_stdout.err" "captured stdout leaves stderr quiet"
run_ok capture_stderr "$DS" run "$FIX/capture/stderr.ds"
assert_same_text $'\nerr\n0\nok\n' "$TMP/capture_stderr.out" "captured stderr field"
assert_same_text '' "$TMP/capture_stderr.err" "captured stderr is not streamed"
run_ok capture_mixed "$DS" run "$FIX/capture/mixed.ds"
assert_same_text $'out\nerr\n0\n' "$TMP/capture_mixed.out" "captured mixed stdout/stderr"
run_ok capture_failed "$DS" run "$FIX/capture/failed.ds"
assert_same_text $'out\nerr\n7\nfailed\nnot ok\n' "$TMP/capture_failed.out" "captured failure remains inspectable"
run_ok capture_fields "$DS" run "$FIX/capture/fields.ds"
assert_same_text $'\n\n2\nnot ok\nfailed\n' "$TMP/capture_fields.out" "all result fields work"
run_ok capture_command_args "$DS" run "$FIX/capture/command_args.ds"
assert_same_text $'hello world\n$(echo hacked)\n`echo hacked`\nsemi;colon\n' "$TMP/capture_command_args.out" "command variable arguments preserve data"
assert_not_contains "$TMP/capture_command_args.out" "hacked\nhacked" "command substitution-looking values are not executed"
run_ok capture_large "$DS" run "$FIX/capture/large.ds"
if [ "$(wc -c < "$TMP/capture_large.out")" -ne 5001 ]; then
  fail "large captured output should contain 5000 bytes plus newline"
fi
pass "large captured output is not truncated"
run_ok capture_many "$DS" run "$FIX/capture/many.ds"
assert_same_text $'one\ntwo\nerr\n1\n' "$TMP/capture_many.out" "repeated captures do not reuse stale fields"
run_ok capture_field_interpolation "$DS" run "$FIX/capture/field_interpolation.ds"
assert_same_text $'stdout=out stderr=err code=3 ok=false failed=true\n' "$TMP/capture_field_interpolation.out" "command-result fields interpolate in strings"
parity_field_interpolation_vm="$TMP/capture_field_interpolation.out"
run_ok capture_field_interpolation_emit "$DS" emit bash "$FIX/capture/field_interpolation.ds" -o "$TMP/field_interpolation.sh"
run_ok capture_field_interpolation_syntax bash -n "$TMP/field_interpolation.sh"
run_ok capture_field_interpolation_bash bash "$TMP/field_interpolation.sh"
assert_same "$parity_field_interpolation_vm" "$TMP/capture_field_interpolation_bash.out" "field interpolation VM/Bash parity"
run_ok capture_command_not_found "$DS" run "$FIX/capture/command_not_found.ds"
assert_contains "$TMP/capture_command_not_found.out" '127' "command-not-found capture records status"
assert_contains "$TMP/capture_command_not_found.out" 'captured' "command-not-found capture remains inspectable"
assert_contains "$TMP/capture_command_not_found.out" 'failed to launch command' "command-not-found stderr is captured"
assert_same_text '' "$TMP/capture_command_not_found.err" "command-not-found capture does not stream stderr"
run_ok capture_block_scope "$DS" run "$FIX/capture/block_scope.ds"
assert_same_text $'scoped\ndone\n' "$TMP/capture_block_scope.out" "command-result variables follow block scope"

# Executable paths containing spaces are valid command words when invoked through variables.
mkdir -p "$TMP/bin space"
cat >"$TMP/bin space/tool with spaces" <<'SH'
#!/usr/bin/env sh
printf 'space:%s\n' "$1"
SH
chmod +x "$TMP/bin space/tool with spaces"
cat >"$TMP/executable_space.ds" <<DS
let tool = "$TMP/bin space/tool with spaces"
let arg = "ok value"
let r = run \$tool \$arg
echo r.stdout
DS
run_ok capture_executable_path_with_spaces "$DS" run "$TMP/executable_space.ds"
assert_same_text $'space:ok value\n\n' "$TMP/capture_executable_path_with_spaces.out" "captured executable path with spaces works"

# Plain command fail-fast behavior is preserved.
capture_status plain_fail_fast "$DS" run "$FIX/redirection/fail_fast.ds"
assert_status plain_fail_fast 7
assert_not_contains "$TMP/plain_fail_fast.out" "unreachable" "plain redirected command stays fail-fast"

# Redirection VM behavior in isolated working directories.
run_redirection_fixture() {
  local name="$1"
  local fixture="$2"
  local expected_file="$3"
  local expected_text="$4"
  local work="$TMP/redir_$name"
  mkdir -p "$work"
  capture_status "redir_${name}_vm" bash -c "cd '$work' && '$DS' run '$FIX/redirection/$fixture'"
  assert_status "redir_${name}_vm" 0
  assert_same_text "$expected_text" "$work/$expected_file" "redirection file content: $name"
  assert_same_text '' "$TMP/redir_${name}_vm.out" "redirection suppresses stdout: $name"
  assert_same_text '' "$TMP/redir_${name}_vm.err" "redirection suppresses stderr: $name"
}
run_redirection_fixture stdout_truncate stdout_truncate.ds out.txt 'two'
run_redirection_fixture stdout_append stdout_append.ds out.txt 'onetwo'
run_redirection_fixture stderr_truncate stderr_truncate.ds err.txt 'two'
run_redirection_fixture stderr_append stderr_append.ds err.txt 'onetwo'
run_redirection_fixture combined_truncate combined_truncate.ds all.txt 'outerr'
run_redirection_fixture combined_append combined_append.ds all.txt 'onetwothreefour'

# Redirection open failures are clear and non-zero.
cat >"$TMP/redir_missing_parent.ds" <<'DS'
printf "x" |> "missing/out.txt"
DS
capture_status redir_open_missing "$DS" run "$TMP/redir_missing_parent.ds"
assert_nonzero_status redir_open_missing
assert_contains "$TMP/redir_open_missing.err" "failed to open redirection target" "redirection missing parent diagnostic"
assert_contains "$TMP/redir_open_missing.err" "$TMP/redir_missing_parent.ds:1:15: error:" "redirection missing parent diagnostic has source location"
cat >"$TMP/redir_target_directory.ds" <<'DS'
printf "x" |> "target"
DS
mkdir -p "$TMP/target"
capture_status redir_open_dir bash -c "cd '$TMP' && '$DS' run '$TMP/redir_target_directory.ds'"
assert_nonzero_status redir_open_dir
assert_contains "$TMP/redir_open_dir.err" "failed to open redirection target" "redirection directory diagnostic"
assert_contains "$TMP/redir_open_dir.err" "$TMP/redir_target_directory.ds:1:15: error:" "redirection directory diagnostic has source location"

# Bash emission goldens and standalone helper checks.
run_ok emit_bash_capture "$DS" emit bash tests/v0_7/fixtures/helpers/bash_capture.ds -o "$TMP/bash_capture.sh"
assert_golden "$GOLD/bash_capture.sh" "$TMP/bash_capture.sh" "captured Bash golden"
run_ok bash_capture_syntax bash -n "$TMP/bash_capture.sh"
run_ok bash_capture bash "$TMP/bash_capture.sh"
assert_same_text $'hello world\n' "$TMP/bash_capture.out" "captured Bash runs"
assert_contains "$TMP/bash_capture.sh" "__ds_capture" "Bash helper uses __ds prefix"
assert_not_contains "$TMP/bash_capture.sh" "$DS" "generated capture Bash does not reference ds"
run_ok emit_bash_failure "$DS" emit bash tests/v0_7/fixtures/helpers/bash_failure.ds -o "$TMP/bash_failure.sh"
assert_golden "$GOLD/bash_failure.sh" "$TMP/bash_failure.sh" "captured failure Bash golden"
run_ok bash_failure_syntax bash -n "$TMP/bash_failure.sh"
run_ok bash_failure bash "$TMP/bash_failure.sh"
assert_same_text $'err\n' "$TMP/bash_failure.out" "captured failure Bash does not abort before inspection"
run_ok emit_bash_combined "$DS" emit bash tests/v0_7/fixtures/helpers/bash_combined_redirect.ds -o "$TMP/bash_combined_redirect.sh"
assert_golden "$GOLD/bash_combined_redirect.sh" "$TMP/bash_combined_redirect.sh" "combined redirect Bash golden"
assert_contains "$TMP/bash_combined_redirect.sh" " 2>&1" "combined redirect emits 2>&1"
run_ok bash_combined_syntax bash -n "$TMP/bash_combined_redirect.sh"
mkdir -p "$TMP/bash_combined_work"
run_ok bash_combined bash -c "cd '$TMP/bash_combined_work' && bash '$TMP/bash_combined_redirect.sh'"
assert_same_text 'outerr' "$TMP/bash_combined_work/all.txt" "combined Bash redirect writes both streams"

# VM/Bash parity helpers.
parity_program() {
  local name="$1"
  local fixture="$2"
  local work_vm="$TMP/parity_${name}_vm_work"
  local work_bash="$TMP/parity_${name}_bash_work"
  mkdir -p "$work_vm" "$work_bash"
  capture_status "${name}_vm" bash -c "cd '$work_vm' && '$DS' run '$fixture'"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$TMP/$name.sh"
  run_ok "${name}_bash_syntax" bash -n "$TMP/$name.sh"
  capture_status "${name}_bash" bash -c "cd '$work_bash' && bash '$TMP/$name.sh'"
  assert_status "${name}_vm" 0
  assert_status "${name}_bash" 0
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "VM/Bash stdout parity: $name"
  assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "VM/Bash stderr parity: $name"
  if [ -f "$work_vm/out.txt" ] || [ -f "$work_bash/out.txt" ]; then
    assert_same "$work_vm/out.txt" "$work_bash/out.txt" "VM/Bash out.txt parity: $name"
  fi
  if [ -f "$work_vm/err.txt" ] || [ -f "$work_bash/err.txt" ]; then
    assert_same "$work_vm/err.txt" "$work_bash/err.txt" "VM/Bash err.txt parity: $name"
  fi
  if [ -f "$work_vm/all.txt" ] || [ -f "$work_bash/all.txt" ]; then
    assert_same "$work_vm/all.txt" "$work_bash/all.txt" "VM/Bash all.txt parity: $name"
  fi
}
parity_program parity_capture_success "$FIX/parity/capture_success.ds"
parity_program parity_capture_failure "$FIX/parity/capture_failure.ds"
parity_program parity_redirect_success "$FIX/parity/redirect_success.ds"
parity_program parity_capture_stderr "$FIX/capture/stderr.ds"
parity_program parity_capture_args "$FIX/capture/command_args.ds"
parity_program parity_import_capture "$FIX/parity/import_capture_main.ds"

# Script args interaction with captured commands.
capture_status args_capture_vm "$DS" run "$FIX/parity/args_capture.ds" "hello world"
assert_status args_capture_vm 0
run_ok args_capture_emit "$DS" emit bash "$FIX/parity/args_capture.ds" -o "$TMP/args_capture.sh"
capture_status args_capture_bash bash "$TMP/args_capture.sh" "hello world"
assert_status args_capture_bash 0
assert_same "$TMP/args_capture_vm.out" "$TMP/args_capture_bash.out" "script arg capture VM/Bash parity"

# Diagnostics cover malformed and unsupported forms across behavior-sensitive commands.
declare -A diag_messages=(
  [run_missing_command.ds]='expected command after `run`'
  [run_with_redirection.ds]='captured `run` commands do not support redirection'
  [run_with_pipeline.ds]='pipelines are not supported in v0.7.0'
  [plain_pipe_unsupported.ds]='pipelines are not supported in v0.7.0'
  [unknown_field.ds]='unsupported command result field `missing`'
  [field_on_string.ds]='field access is only supported on command results'
  [field_missing_name.ds]='expected field name after `.`'
  [redirect_missing_target.ds]='expected string redirection target after `&>`'
  [redirect_bad_target.ds]='redirection target must be a string literal'
  [redirect_duplicate.ds]='duplicate redirection suffix'
  [unknown_command_var.ds]='unknown command variable `missing`'
  [out_of_scope_result.ds]='unknown command variable `inside`'
  [unknown_interpolation_field.ds]='unknown command result field `missing`'
)
for file in "${!diag_messages[@]}"; do
  base="${file%.ds}"
  run_fail "check_$base" "$DS" check "$FIX/diagnostics/$file"
  assert_contains "$TMP/check_$base.err" "$FIX/diagnostics/$file:" "$base diagnostic path"
  assert_contains "$TMP/check_$base.err" ': error:' "$base diagnostic shape"
  assert_contains "$TMP/check_$base.err" "${diag_messages[$file]}" "$base diagnostic message"
  run_fail "bytecode_$base" "$DS" bytecode "$FIX/diagnostics/$file"
  assert_contains "$TMP/bytecode_$base.err" "${diag_messages[$file]}" "$base bytecode diagnostic"
  run_fail "run_$base" "$DS" run "$FIX/diagnostics/$file"
  assert_contains "$TMP/run_$base.err" "${diag_messages[$file]}" "$base run diagnostic"
  run_fail "direct_$base" "$DS" "$FIX/diagnostics/$file"
  assert_contains "$TMP/direct_$base.err" "${diag_messages[$file]}" "$base direct diagnostic"
  run_fail "emit_$base" "$DS" emit bash "$FIX/diagnostics/$file" -o "$TMP/$base.sh"
  assert_file_missing_or_empty "$TMP/$base.sh" "$base invalid emit leaves no artifact"
done

# Failed Bash emission must remove stale output artifacts from earlier successful emits.
run_ok stale_emit_initial "$DS" emit bash examples/basic.ds -o "$TMP/stale_emit.sh"
assert_contains "$TMP/stale_emit.sh" '#!/usr/bin/env bash' "initial emit creates artifact"
run_fail stale_emit_invalid "$DS" emit bash "$FIX/diagnostics/unknown_interpolation_field.ds" -o "$TMP/stale_emit.sh"
assert_file_missing_or_empty "$TMP/stale_emit.sh" "failed emit removes stale artifact"

# Invalid legacy shell redirection tokens should be rejected clearly.
for src in 'npm test > "out.txt"' 'npm test >> "out.txt"' 'npm test 2> "err.txt"'; do
  printf '%s\n' "$src" >"$TMP/legacy_redirect.ds"
  run_fail legacy_redirect "$DS" check "$TMP/legacy_redirect.ds"
  assert_contains "$TMP/legacy_redirect.err" "unsupported command operator" "legacy redirect rejected: $src"
done

# Operator adjacency is allowed when unambiguous.
cat >"$TMP/adjacent_redirect.ds" <<'DS'
printf "ok"&>"all.txt"
DS
mkdir -p "$TMP/adjacent"
capture_status adjacent_redirect bash -c "cd '$TMP/adjacent' && '$DS' run '$TMP/adjacent_redirect.ds'"
assert_status adjacent_redirect 0
assert_same_text 'ok' "$TMP/adjacent/all.txt" "adjacent redirection operator works"

# Static architecture/documentation/status checks.
assert_contains "$ROOT/README.md" 'let result = run' "README documents command-result syntax"
assert_contains "$ROOT/README.md" '&> "build.log"' "README documents readable redirection"
assert_contains "$ROOT/docs/language.ds" 'let result = run npm test' "language catalog documents run"
assert_contains "$ROOT/docs/language.ds" 'npm run build &> "build.log"' "language catalog documents redirection"
assert_contains "$ROOT/docs/architecture.md" 'command-result HIR' "architecture documents command-result boundary"
assert_contains "$ROOT/docs/runtime.md" 'Command-result ownership' "runtime documents command-result ownership"
assert_contains "$ROOT/CHANGELOG.md" 'Added `tests/v0_7/run.sh`' "changelog records v0.7 tests"
[ -f "$ROOT/examples/command-result.ds" ] || fail "command-result example exists"
[ -f "$ROOT/examples/redirection.ds" ] || fail "redirection example exists"
pass "v0.7 examples exist"
run_ok example_command_result "$DS" run examples/command-result.ds
run_ok example_command_result_emit "$DS" emit bash examples/command-result.ds -o "$TMP/example_command_result.sh"
run_ok example_command_result_bash_syntax bash -n "$TMP/example_command_result.sh"
run_ok example_command_result_bash bash "$TMP/example_command_result.sh"
assert_same "$TMP/example_command_result.out" "$TMP/example_command_result_bash.out" "command-result example VM/Bash parity"
run_ok example_redirection_check "$DS" check examples/redirection.ds
run_ok example_redirection_emit "$DS" emit bash examples/redirection.ds -o "$TMP/example_redirection.sh"
run_ok example_redirection_bash_syntax bash -n "$TMP/example_redirection.sh"

# Prior-version smoke checks stay compatible.
run_ok old_tokens "$DS" tokens examples/basic.ds
run_ok old_ast "$DS" ast examples/basic.ds
run_ok old_check "$DS" check examples/basic.ds
run_ok old_bytecode "$DS" bytecode examples/basic.ds
run_ok old_run "$DS" run examples/basic.ds
run_ok old_direct "$DS" examples/basic.ds
run_ok old_emit "$DS" emit bash examples/basic.ds -o "$TMP/old_basic.sh"
run_ok old_bash_syntax bash -n "$TMP/old_basic.sh"
run_ok old_bash bash "$TMP/old_basic.sh"
assert_same "$TMP/old_run.out" "$TMP/old_direct.out" "old direct/run parity"
assert_same "$TMP/old_run.out" "$TMP/old_bash.out" "old VM/Bash parity"
run_ok old_args "$DS" run tests/v0_5/fixtures/args_basic.ds api --target prod --force
assert_contains "$TMP/old_args.out" 'Deploying api to prod' "v0.5 args still work"
run_ok old_import "$DS" run tests/v0_6/fixtures/imports_basic/main.ds
assert_contains "$TMP/old_import.out" 'Deploying api to production' "v0.6 imports still work"

# Suite-level make target stays runnable.
run_ok make_check make -C "$ROOT" check

echo "v0.7.0 tests passed: $pass_count checks"
