#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_18_tests.$$"
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

capture_env_in_dir() {
  local name="$1" dir="$2"; shift 2
  mkdir -p "$dir"
  set +e
  (cd "$dir" && env PATH="$FAKEBIN:$PATH" "$@") >"$TMP/$name.out" 2>"$TMP/$name.err"
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

run_env_ok() {
  local name="$1"; shift
  env PATH="$FAKEBIN:$PATH" "$@" >"$TMP/$name.out" 2>"$TMP/$name.err" || {
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected success"
  }
  pass "$name"
}

run_env_fail() {
  local name="$1"; shift
  if env PATH="$FAKEBIN:$PATH" "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"; then
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected failure"
  fi
  pass "$name"
}

assert_vm_bash_env_parity() {
  local name="$1" fixture="$2" expected_status="$3" output_files="$4"; shift 4
  local script="$TMP/$name.sh"
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  mkdir -p "$vm_work" "$bash_work"

  capture_env_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_env_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  capture_env_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  else
    assert_contains "$TMP/${name}_vm.err" ': error:' "$name VM error diagnostic"
    assert_contains "$TMP/${name}_bash.err" ': error:' "$name Bash error diagnostic"
  fi
  assert_contains "$script" '#!/usr/bin/env bash' "$name emitted Bash shebang"
  assert_contains "$script" 'set -euo pipefail' "$name emitted Bash pipefail mode"
  assert_not_contains "$script" '__ds_capture_eval' "$name emitted Bash avoids eval capture helper"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  local rel
  for rel in $output_files; do
    [ -f "$vm_work/$rel" ] || fail "$name VM missing side-effect file $rel"
    [ -f "$bash_work/$rel" ] || fail "$name Bash missing side-effect file $rel"
    assert_same "$vm_work/$rel" "$bash_work/$rel" "$name side-effect parity $rel"
  done
}

assert_diag() {
  local file="$1" fragment="$2" name="$3"
  assert_contains "$file" ': error:' "$name diagnostic severity"
  assert_contains "$file" "$fragment" "$name diagnostic text"
  assert_contains "$file" '^' "$name diagnostic caret"
}

FIX="$TMP/fixtures"
FAKEBIN="$TMP/fakebin"
mkdir -p "$FIX" "$FAKEBIN"

cat >"$FAKEBIN/emit_lines" <<'SH2'
#!/usr/bin/env bash
printf 'ERROR beta\nINFO skip\nERROR alpha\n'
SH2
cat >"$FAKEBIN/filter_error" <<'SH2'
#!/usr/bin/env bash
while IFS= read -r line; do
  case "$line" in ERROR*) printf '%s\n' "$line" ;; esac
done
SH2
cat >"$FAKEBIN/prefix_stage" <<'SH2'
#!/usr/bin/env bash
prefix="$1"
while IFS= read -r line; do printf '%s%s\n' "$prefix" "$line"; done
SH2
cat >"$FAKEBIN/pass" <<'SH2'
#!/usr/bin/env bash
cat
SH2
cat >"$FAKEBIN/fail5" <<'SH2'
#!/usr/bin/env bash
cat >/dev/null
exit 5
SH2
cat >"$FAKEBIN/fail7" <<'SH2'
#!/usr/bin/env bash
cat >/dev/null
exit 7
SH2
cat >"$FAKEBIN/stderr_first" <<'SH2'
#!/usr/bin/env bash
printf 'first-out\n'
printf 'first-err\n' >&2
SH2
cat >"$FAKEBIN/stderr_stage" <<'SH2'
#!/usr/bin/env bash
while IFS= read -r line; do printf 'seen:%s\n' "$line"; done
printf 'stage-err\n' >&2
SH2
cat >"$FAKEBIN/many_lines" <<'SH2'
#!/usr/bin/env bash
i=0
while [ "$i" -lt 80 ]; do printf 'line-%03d\n' "$i"; i=$((i + 1)); done
SH2
cat >"$FAKEBIN/take_last" <<'SH2'
#!/usr/bin/env bash
last=''
while IFS= read -r line; do last="$line"; done
printf '%s\n' "$last"
SH2
chmod +x "$FAKEBIN"/*

# Static and build wiring tests.
count_018="$(grep -E '^TEST_VERSIONS :=' Makefile | grep -o '0-18' | wc -l | tr -d ' ')"
[ "$count_018" = 1 ] || fail "TEST_VERSIONS should contain 0-18 exactly once, got $count_018"
pass 'TEST_VERSIONS contains 0-18 exactly once'
assert_matches Makefile '^TEST_VERSIONS := .*0-17 0-18($| )' 'v0.18 follows v0.17 in TEST_VERSIONS'
assert_contains Makefile 'DS_SKIP_BUILD=1 ./tests/v$(subst -,_,$(patsubst test-v%,%,$@))/run.sh' 'pattern target invokes version suite'
assert_contains Makefile '$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address" test' 'asan runs aggregate test suite'
assert_contains Makefile '$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined" test' 'ubsan runs aggregate test suite'
assert_contains Makefile 'src/bash_command.c' 'Bash command source is built'
assert_contains Makefile 'src/vm_process.c' 'VM process source is built'
assert_not_contains Makefile 'libs/hashmap' 'build does not reference stale libs/hashmap path'
assert_not_contains include/ds.h 'hashmap' 'public header does not expose hashmap internals'
for file in src/ds_command.c src/parse_command.c src/ast.c src/lower_expr.c src/lower_stmt.c src/hir.c src/format.c src/ds_checker.c src/vm_compile.c src/vm_dump.c src/vm_process.c src/bash_command.c src/bash_stmt.c src/bash_deps.c; do
  [ -f "$file" ] || fail "$file exists"
  pass "$file exists"
  assert_matches "$file" 'stage|Pipeline|pipeline|stages' "$file handles pipeline-aware command data"
done
assert_contains src/ds_command.c 'ds_command_clone' 'command clone handles commands'
assert_contains src/ds_command.c 'ds_command_free' 'command free handles commands'
assert_contains src/bash_command.c 'emit_command_pipeline_stages' 'Bash uses shared stage emission'
assert_contains src/vm_process.c 'process_execute_pipeline' 'VM has pipeline executor'
assert_contains src/vm_process.c 'pipe(' 'VM uses process pipes'
assert_contains src/bash_stmt.c 'pipeline failed with exit' 'Bash failure diagnostic says pipeline'
assert_contains src/bash_stmt.c '{ ' 'Bash groups redirected pipelines'
assert_not_contains src/bash_stmt.c '__ds_capture_eval' 'Bash capture does not use eval helper'

# Documentation and status checks.
assert_contains docs/milestones/v0.18.0-spec.md 'Implementation and tests complete' 'v0.18 spec records implementation completion'
assert_contains docs/milestones/v0.18.0-spec.md 'whole-pipeline redirection' 'v0.18 spec records whole-pipeline redirection'
assert_contains docs/milestones/v0.18.0-test-plan.md 'tests/v0_18/run.sh' 'v0.18 test plan names suite'
assert_contains docs/status.md 'v0.21.0' 'status identifies current pipeline/string state'
assert_contains docs/status.md 'captured `run` pipelines' 'status documents captured pipelines'
assert_contains docs/status.md 'pipefail-style' 'status documents pipefail'
assert_contains docs/status.md 'advanced pipeline forms remain' 'status keeps advanced forms deferred'
assert_contains docs/language.ds 'Plain command pipelines are fail-fast' 'language marks pipelines implemented'
assert_contains docs/language.ds 'Redirection suffixes apply to the whole plain pipeline' 'language documents whole pipeline redirection'
assert_contains README.md 'v0.18.0' 'README mentions v0.18.0'
assert_contains CHANGELOG.md 'v0.18.0' 'changelog mentions v0.18.0'
assert_contains examples/pipeline.ds 'run printf' 'pipeline example has captured run pipeline'

run_ok help_top "$DS" --help
assert_contains "$TMP/help_top.out" 'ds v0.29.0' 'help reports current version'

# Lexer/parser/token tests.
write_fixture "$FIX/tokens.ds" <<'DS'
cat "input.txt" | grep "ERROR" | sort
let result = run cat "input.txt" | grep "ERROR"
cat "input.txt" |> "copy.txt"
case result.code {
  0 | 1 { echo "status" }
  _ { echo "other" }
}
DS
run_ok tokens "$DS" tokens "$FIX/tokens.ds"
assert_contains "$TMP/tokens.out" 'PIPE' 'tokens expose pipeline/case separator pipe'
assert_contains "$TMP/tokens.out" 'REDIRECT_OUT' 'tokens keep |> as redirection token'
assert_contains "$TMP/tokens.out" 'CASE' 'tokens include case keyword'
run_ok parse_tokens_check "$DS" check "$FIX/tokens.ds"

write_fixture "$FIX/parser_accept.ds" <<'DS'
emit_lines | filter_error | sort
let r = run emit_lines | filter_error | sort
echo "ok={r.ok} code={r.code}"
if r.ok {
  emit_lines | filter_error
}
let i = 0
while i < 1 {
  i += 1
  emit_lines | filter_error
}
fn pipe_fn() {
  let got = run emit_lines | filter_error
  echo got.stdout
}
pipe_fn()
DS
run_env_ok parser_accept "$DS" check "$FIX/parser_accept.ds"

write_fixture "$FIX/redir_accept.ds" <<'DS'
emit_lines | filter_error |> "out.txt"
stderr_first | stderr_stage !> "err.txt"
stderr_first | stderr_stage &> "all.txt"
DS
run_env_ok redir_accept "$DS" check "$FIX/redir_accept.ds"

# Parser rejection diagnostics.
declare -A rejects
rejects[leading_pipe]='| grep x'
rejects[trailing_pipe]='echo x |'
rejects[double_pipe]='echo x || grep x'
rejects[and_op]='echo x && grep x'
rejects[empty_stage]='echo x | | grep x'
rejects[expr_pipeline]='let x = echo a | grep a'
rejects[if_run_pipeline]='if run echo a | grep a { echo bad }'
rejects[captured_redir]='let r = run echo a |> "out.txt"'
rejects[mid_redir]='echo a |> "out.txt" | grep a'
for name in leading_pipe trailing_pipe double_pipe and_op empty_stage expr_pipeline if_run_pipeline captured_redir mid_redir; do
  printf '%s\n' "${rejects[$name]}" >"$FIX/reject_$name.ds"
  run_env_fail "reject_$name" "$DS" check "$FIX/reject_$name.ds"
  assert_contains "$TMP/reject_$name.err" ': error:' "reject $name has error"
  assert_contains "$TMP/reject_$name.err" '^' "reject $name has caret"
done
assert_contains "$TMP/reject_leading_pipe.err" 'missing command before `|`' 'leading pipe diagnostic'
assert_contains "$TMP/reject_trailing_pipe.err" 'missing command after `|`' 'trailing pipe diagnostic'
assert_contains "$TMP/reject_double_pipe.err" 'logical OR `||` is not supported' 'logical OR diagnostic'
assert_contains "$TMP/reject_and_op.err" 'logical/background command operators are not supported' 'logical AND diagnostic'
assert_contains "$TMP/reject_empty_stage.err" 'missing command between pipeline separators' 'empty stage diagnostic'
assert_contains "$TMP/reject_captured_redir.err" 'captured `run` commands do not support redirection' 'captured redirection diagnostic'
assert_contains "$TMP/reject_mid_redir.err" 'redirection must apply to the whole pipeline' 'mid-pipeline redirection diagnostic'

# AST, HIR, bytecode coverage.
write_fixture "$FIX/debug.ds" <<'DS'
echo a | cat |> "out.txt"
let r = run emit_lines | filter_error
DS
run_env_ok ast_debug "$DS" ast "$FIX/debug.ds"
assert_contains "$TMP/ast_debug.out" 'Stage 0' 'AST prints first stage'
assert_contains "$TMP/ast_debug.out" 'Stage 1' 'AST prints second stage'
assert_contains "$TMP/ast_debug.out" 'Redirect |>' 'AST prints whole-pipeline redirect once'
assert_not_matches "$TMP/ast_debug.out" '0x[0-9a-fA-F]+' 'AST has no pointer addresses'
run_env_ok hir_debug "$DS" hir "$FIX/debug.ds"
assert_contains "$TMP/hir_debug.out" 'Command ["echo", "a"] | ["cat"]' 'HIR prints plain pipeline'
assert_contains "$TMP/hir_debug.out" 'Run ["emit_lines"] | ["filter_error"]' 'HIR prints captured pipeline'
run_env_ok bytecode_debug "$DS" bytecode "$FIX/debug.ds"
assert_contains "$TMP/bytecode_debug.out" 'RUN_CMD' 'bytecode has command instruction'
assert_contains "$TMP/bytecode_debug.out" 'RUN_CAPTURE' 'bytecode has captured command instruction'
assert_contains "$TMP/bytecode_debug.out" '|' 'bytecode shows stage order'
assert_contains "$TMP/bytecode_debug.out" '|>' 'bytecode shows redirect metadata'

# VM/Bash behavior: success, capture, pipefail, large stream.
write_fixture "$FIX/plain_success.ds" <<'DS'
emit_lines | filter_error | sort
DS
assert_vm_bash_env_parity plain_success "$FIX/plain_success.ds" 0 ''
printf 'ERROR alpha\nERROR beta\n' >"$TMP/expected_plain.out"
assert_same "$TMP/expected_plain.out" "$TMP/plain_success_vm.out" 'plain pipeline sorted stdout'

write_fixture "$FIX/captured_success.ds" <<'DS'
let result = run emit_lines | filter_error | sort
echo "ok={result.ok}"
echo "failed={result.failed}"
echo "code={result.code}"
echo result.stdout
DS
assert_vm_bash_env_parity captured_success "$FIX/captured_success.ds" 0 ''
printf 'ok=true\nfailed=false\ncode=0\nERROR alpha\nERROR beta\n\n' >"$TMP/expected_captured.out"
assert_same "$TMP/expected_captured.out" "$TMP/captured_success_vm.out" 'captured pipeline result fields and stdout'

write_fixture "$FIX/plain_failure.ds" <<'DS'
pass | fail7 | pass
echo after
DS
assert_vm_bash_env_parity plain_failure "$FIX/plain_failure.ds" 7 ''
assert_not_contains "$TMP/plain_failure_vm.out" 'after' 'plain failing pipeline stops later statements in VM'
assert_not_contains "$TMP/plain_failure_bash.out" 'after' 'plain failing pipeline stops later statements in Bash'
assert_contains "$TMP/plain_failure_vm.err" 'pipeline failed with exit 7' 'VM pipefail diagnostic'
assert_contains "$TMP/plain_failure_bash.err" 'pipeline failed with exit 7' 'Bash pipefail diagnostic'

write_fixture "$FIX/rightmost_failure.ds" <<'DS'
let r = run fail5 | fail7
echo "code={r.code} ok={r.ok} failed={r.failed}"
DS
assert_vm_bash_env_parity rightmost_failure "$FIX/rightmost_failure.ds" 0 ''
printf 'code=7 ok=false failed=true\n' >"$TMP/expected_rightmost.out"
assert_same "$TMP/expected_rightmost.out" "$TMP/rightmost_failure_vm.out" 'captured pipeline reports rightmost failure status'

write_fixture "$FIX/captured_stderr.ds" <<'DS'
let r = run stderr_first | stderr_stage
echo "code={r.code}"
echo "stdout={r.stdout}"
echo "stderr={r.stderr}"
DS
assert_vm_bash_env_parity captured_stderr "$FIX/captured_stderr.ds" 0 ''
assert_contains "$TMP/captured_stderr_vm.out" 'stdout=seen:first-out' 'captured stdout comes from final stage'
assert_contains "$TMP/captured_stderr_vm.out" 'first-err' 'captured stderr includes first stage'
assert_contains "$TMP/captured_stderr_vm.out" 'stage-err' 'captured stderr includes later stage'

write_fixture "$FIX/large_stream.ds" <<'DS'
many_lines | take_last
DS
assert_vm_bash_env_parity large_stream "$FIX/large_stream.ds" 0 ''
printf 'line-079\n' >"$TMP/expected_large.out"
assert_same "$TMP/expected_large.out" "$TMP/large_stream_vm.out" 'large stream smoke output'

# Bash emission and whole-pipeline redirection.
write_fixture "$FIX/redir_stdout.ds" <<'DS'
echo hello | cat |> "out.txt"
echo again | cat |>> "out.txt"
DS
assert_vm_bash_env_parity redir_stdout "$FIX/redir_stdout.ds" 0 'out.txt'
printf 'hello\nagain\n' >"$TMP/expected_out_file"
assert_same "$TMP/expected_out_file" "$TMP/redir_stdout_vm_work/out.txt" 'stdout pipeline redirection file content'

write_fixture "$FIX/redir_stderr.ds" <<'DS'
stderr_first | stderr_stage !> "errs.txt"
DS
assert_vm_bash_env_parity redir_stderr "$FIX/redir_stderr.ds" 0 'errs.txt'
assert_contains "$TMP/redir_stderr_vm_work/errs.txt" 'first-err' 'stderr redirect includes first stage'
assert_contains "$TMP/redir_stderr_vm_work/errs.txt" 'stage-err' 'stderr redirect includes later stage'
assert_file_missing_or_empty "$TMP/redir_stderr_vm.err" 'VM terminal stderr empty for stderr redirect'
assert_file_missing_or_empty "$TMP/redir_stderr_bash.err" 'Bash terminal stderr empty for stderr redirect'

write_fixture "$FIX/redir_all.ds" <<'DS'
stderr_first | stderr_stage &> "all.txt"
DS
assert_vm_bash_env_parity redir_all "$FIX/redir_all.ds" 0 'all.txt'
assert_contains "$TMP/redir_all_vm_work/all.txt" 'seen:first-out' 'all redirect includes stdout'
assert_contains "$TMP/redir_all_vm_work/all.txt" 'first-err' 'all redirect includes first stderr'
assert_contains "$TMP/redir_all_vm_work/all.txt" 'stage-err' 'all redirect includes later stderr'
assert_file_missing_or_empty "$TMP/redir_all_vm.out" 'VM terminal stdout empty for all redirect'
assert_file_missing_or_empty "$TMP/redir_all_bash.out" 'Bash terminal stdout empty for all redirect'
assert_contains "$TMP/redir_all.sh" '{ ' 'Bash groups whole-pipeline all redirection'

# Formatter and checker behavior.
write_fixture "$FIX/format.ds" <<'DS'
echo a|cat|sort
let r=run echo a|cat
cat "input.txt"|cat|>"out.txt"
DS
run_ok fmt_pipeline "$DS" fmt "$FIX/format.ds"
assert_contains "$TMP/fmt_pipeline.out" 'echo a | cat | sort' 'formatter spaces plain pipeline'
assert_contains "$TMP/fmt_pipeline.out" 'let r = run echo a | cat' 'formatter spaces captured pipeline'
assert_contains "$TMP/fmt_pipeline.out" 'cat "input.txt" | cat |> "out.txt"' 'formatter spaces pipeline redirection'
printf '%s' "$(cat "$TMP/fmt_pipeline.out")" >"$FIX/formatted.ds"
run_ok fmt_recheck "$DS" check "$FIX/formatted.ds"
run_ok fmt_idempotent "$DS" fmt "$FIX/formatted.ds"
assert_same "$TMP/fmt_pipeline.out" "$TMP/fmt_idempotent.out" 'formatter is idempotent for pipelines'
write_fixture "$FIX/comment_fmt.ds" <<'DS'
# comment must be preserved later
echo a | cat
DS
run_fail fmt_comment "$DS" fmt "$FIX/comment_fmt.ds"
assert_contains "$TMP/fmt_comment.err" 'formatter cannot preserve comments yet' 'formatter still rejects comments safely'
write_fixture "$FIX/noexec_check.ds" <<'DS'
missing_command_that_should_not_run | also_missing
DS
run_ok checker_noexec "$DS" check "$FIX/noexec_check.ds"
[ ! -e "$TMP/should-not-exist" ] || fail 'checker must not run commands'
pass 'checker does not create side effects'

# Import/function/script/test integration.
write_fixture "$FIX/lib_pipe.ds" <<'DS'
emit_lines | filter_error | sort
fn capture_pipe() {
  let r = run emit_lines | filter_error | sort
  echo "fn={r.code}"
  echo r.stdout
}
DS
write_fixture "$FIX/import_main.ds" <<'DS'
import "./lib_pipe.ds"
capture_pipe()
DS
assert_vm_bash_env_parity import_pipeline "$FIX/import_main.ds" 0 ''
assert_contains "$TMP/import_pipeline_vm.out" 'fn=0' 'function captures pipeline from imported file'

write_fixture "$FIX/dup_import_lib.ds" <<'DS'
emit_lines | filter_error | sort
fn dup_import_fn() {
  emit_lines | filter_error | sort
}
DS
write_fixture "$FIX/dup_import_main.ds" <<'DS'
import "./dup_import_lib.ds"
import "./dup_import_lib.ds"
dup_import_fn()
DS
assert_vm_bash_env_parity duplicate_import_pipeline "$FIX/dup_import_main.ds" 0 ''
cat >"$TMP/expected_duplicate_import.out" <<'EOF_EXPECTED_DUP_IMPORT'
ERROR alpha
ERROR beta
ERROR alpha
ERROR beta
EOF_EXPECTED_DUP_IMPORT
assert_same "$TMP/expected_duplicate_import.out" "$TMP/duplicate_import_pipeline_vm.out" 'duplicate import keeps imported pipeline single-instanced'

write_fixture "$FIX/bad_import.ds" <<'DS'
echo a |
DS
write_fixture "$FIX/bad_import_main.ds" <<'DS'
import "./bad_import.ds"
echo never
DS
run_env_fail bad_import_check "$DS" check "$FIX/bad_import_main.ds"
assert_contains "$TMP/bad_import_check.err" 'bad_import.ds:' 'malformed imported pipeline points at imported file'
assert_contains "$TMP/bad_import_check.err" 'missing command after `|`' 'malformed imported pipeline diagnostic'

write_fixture "$FIX/script_args.ds" <<'DS'
script {
  arg needle: string
}
printf "alpha\nbeta\n" | grep $needle
DS
assert_vm_bash_env_parity script_args "$FIX/script_args.ds" 0 '' beta
printf 'beta\n' >"$TMP/expected_script_args.out"
assert_same "$TMP/expected_script_args.out" "$TMP/script_args_vm.out" 'script arg interpolates into pipeline stage'

write_fixture "$FIX/test_blocks.ds" <<'DS'
test "pipeline passes" {
  emit_lines | filter_error | sort
}

test "captured failure continues" {
  let r = run fail7 | pass
  assert r.failed
  assert r.code == 7
}
DS
run_env_ok test_blocks "$DS" test "$FIX/test_blocks.ds"
assert_contains "$TMP/test_blocks.out" 'ok   pipeline passes' 'ds test executes successful pipeline in test block'
assert_contains "$TMP/test_blocks.out" 'ok   captured failure continues' 'ds test captures failing pipeline and continues'
write_fixture "$FIX/test_plain_failure.ds" <<'DS'
test "plain failure fails test" {
  pass | fail7 | pass
}
DS
capture_env_in_dir test_plain_failure "$TMP/test_plain_failure_work" "$DS" test "$FIX/test_plain_failure.ds"
assert_status test_plain_failure 1
assert_contains "$TMP/test_plain_failure.out" 'fail plain failure fails test' 'failing plain pipeline fails test case'
write_fixture "$FIX/test_ignored.ds" <<'DS'
test "ignored by run" {
  echo should-not-print
}
echo prod
DS
assert_vm_bash_env_parity test_ignored "$FIX/test_ignored.ds" 0 ''
printf 'prod\n' >"$TMP/expected_test_ignored.out"
assert_same "$TMP/expected_test_ignored.out" "$TMP/test_ignored_vm.out" 'normal run ignores test block with pipeline capability'

# Case regression: | alternatives must not be parsed as command pipelines.
write_fixture "$FIX/case_pipe.ds" <<'DS'
let lang = "sh"
case lang {
  "bash" | "sh" { echo "shell" }
  _ { echo "other" }
}
DS
assert_vm_bash_env_parity case_pipe "$FIX/case_pipe.ds" 0 ''
printf 'shell\n' >"$TMP/expected_case.out"
assert_same "$TMP/expected_case.out" "$TMP/case_pipe_vm.out" 'case alternatives still work'
run_ok case_ast "$DS" ast "$FIX/case_pipe.ds"
assert_contains "$TMP/case_ast.out" 'CaseStmt' 'AST distinguishes case statement'
assert_contains "$TMP/case_ast.out" 'Arm "bash" "sh"' 'AST prints case alternatives'

# Trace behavior: plain pipeline failures keep source markers in VM and emitted Bash.
write_fixture "$FIX/trace_pipeline.ds" <<'DS'
pass | fail7 | pass
DS
run_env_ok trace_pipeline_emit "$DS" emit bash "$FIX/trace_pipeline.ds" -o "$TMP/trace_pipeline.sh"
capture_env_in_dir trace_pipeline_vm "$TMP/trace_pipeline_vm_work" "$DS" run --trace-cmd "$FIX/trace_pipeline.ds"
capture_in_dir trace_pipeline_bash "$TMP/trace_pipeline_bash_work" env PATH="$FAKEBIN:$PATH" DS_TRACE_CMD=1 bash "$TMP/trace_pipeline.sh"
assert_status trace_pipeline_vm 7
assert_status trace_pipeline_bash 7
assert_contains "$TMP/trace_pipeline_vm.err" 'trace: cmd' 'VM trace emits command trace for pipeline stages'
assert_contains "$TMP/trace_pipeline_vm.err" '"pass"' 'VM trace includes first pipeline stage'
assert_contains "$TMP/trace_pipeline_vm.err" '"fail7"' 'VM trace includes failing pipeline stage'
assert_contains "$TMP/trace_pipeline_vm.err" 'pipeline failed with exit 7' 'VM traced pipeline keeps failure diagnostic'
assert_contains "$TMP/trace_pipeline_bash.err" 'trace: cmd' 'Bash trace emits command trace for pipeline'
assert_contains "$TMP/trace_pipeline_bash.err" '"pass" "|" "fail7" "|" "pass"' 'Bash trace includes full pipeline shape'
assert_contains "$TMP/trace_pipeline_bash.err" 'pipeline failed with exit 7' 'Bash traced pipeline keeps failure diagnostic'

# Examples: check, run deterministic examples, emit Bash.
for example in examples/*.ds; do
  base="$(basename "$example")"
  if [ "$base" = 'bad.ds' ]; then
    run_fail "example_bad_check" "$DS" check "$example"
    assert_contains "$TMP/example_bad_check.err" ': error:' 'bad example remains invalid'
    continue
  fi
  run_ok "example_${base}_check" "$DS" check "$example"
  script="$TMP/example_${base}.sh"
  run_ok "example_${base}_emit" "$DS" emit bash "$example" -o "$script"
  run_ok "example_${base}_bash_n" bash -n "$script"
done
assert_vm_bash_env_parity example_args "$ROOT/examples/args.ds" 0 '' demo --target prod --force
assert_vm_bash_env_parity example_basic "$ROOT/examples/basic.ds" 0 ''
assert_vm_bash_env_parity example_collections "$ROOT/examples/collections.ds" 0 ''
assert_vm_bash_env_parity example_command_result "$ROOT/examples/command-result.ds" 0 ''
assert_vm_bash_env_parity example_control_flow "$ROOT/examples/control-flow.ds" 0 ''
assert_vm_bash_env_parity example_functions "$ROOT/examples/functions.ds" 0 ''
assert_vm_bash_env_parity example_import_main "$ROOT/examples/import-main.ds" 0 ''
assert_vm_bash_env_parity example_redirection "$ROOT/examples/redirection.ds" 0 'build.log'
assert_vm_bash_env_parity example_stdlib "$ROOT/examples/stdlib.ds" 0 ''
assert_vm_bash_env_parity example_vm "$ROOT/examples/vm.ds" 0 ''
assert_vm_bash_env_parity pipeline_example "$ROOT/examples/pipeline.ds" 0 ''

# Sanitizer/resource smoke is covered by make asan/ubsan, but keep focused repeated loops fast.
write_fixture "$FIX/repeated_capture.ds" <<'DS'
let i = 0
while i < 12 {
  let r = run emit_lines | filter_error | sort
  if r.failed { echo "bad" }
  i += 1
}
echo done
DS
assert_vm_bash_env_parity repeated_capture "$FIX/repeated_capture.ds" 0 ''
printf 'done\n' >"$TMP/expected_repeated.out"
assert_same "$TMP/expected_repeated.out" "$TMP/repeated_capture_vm.out" 'repeated captured pipelines complete without visible leaks/deadlock'

# Completion status should be updated by this suite addition.
assert_contains docs/milestones/v0.18.0-spec.md 'Tests complete' 'v0.18 spec completion records tests complete'
assert_contains docs/milestones/v0.18.0-test-plan.md 'Implemented' 'v0.18 test plan status records implementation'

printf 'v0.18.0 tests passed (%d assertions)\n' "$pass_count"
