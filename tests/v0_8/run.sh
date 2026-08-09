#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_8_tests.$$"
FIX="$ROOT/tests/v0_8/fixtures"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"
# shellcheck source=tests/lib/build_sources.sh
source "$ROOT/tests/lib/build_sources.sh"

if [[ "${DS_SKIP_BUILD:-0}" != 1 ]]; then
  make -C "$ROOT" clean all >/dev/null
fi

cd "$ROOT"

# Unit coverage for the v0.8.0 cleanup helpers that are intentionally public to internal tests.
ds_compile_unit "$ROOT" command_model "$ROOT/tests/v0_8/unit/command_model.c" "$TMP/test_v0_8_command_model"
run_ok command_model_unit "$TMP/test_v0_8_command_model"
ds_compile_unit "$ROOT" command_result "$ROOT/tests/v0_8/unit/command_result_fields.c" "$TMP/test_v0_8_command_result_fields"
run_ok command_result_fields_unit "$TMP/test_v0_8_command_result_fields"

# Public command smoke matrix on representative supported fixtures.
for fixture in \
  examples/basic.ds \
  tests/v0_5/fixtures/args_basic.ds \
  tests/v0_6/fixtures/imports_basic/main.ds \
  examples/command-result.ds \
  examples/redirection.ds \
  tests/v0_8/fixtures/parity/script_args_capture.ds \
  tests/v0_8/fixtures/parity/import_capture.ds; do
  base="$(basename "$fixture" .ds | tr '-' '_')"
  run_ok "smoke_tokens_$base" "$DS" tokens "$fixture"
  assert_not_contains "$TMP/smoke_tokens_$base.out" "0x" "tokens pointer-free: $base"
  run_ok "smoke_ast_$base" "$DS" ast "$fixture"
  assert_not_contains "$TMP/smoke_ast_$base.out" "0x" "AST pointer-free: $base"
  run_ok "smoke_check_$base" "$DS" check "$fixture"
  run_ok "smoke_bytecode_$base" "$DS" bytecode "$fixture"
  assert_not_contains "$TMP/smoke_bytecode_$base.out" "0x" "bytecode pointer-free: $base"
done

capture_status direct_basic "$DS" examples/basic.ds
assert_status direct_basic 0
capture_status run_basic "$DS" run examples/basic.ds
assert_status run_basic 0
assert_same "$TMP/direct_basic.out" "$TMP/run_basic.out" "direct and ds run match for basic script"

# Shared VM/Bash parity helper coverage, including non-zero plain-command parity and file comparisons.
assert_vm_bash_parity v0_8_capture_success "$FIX/parity/capture_success.ds" 0 ""
assert_vm_bash_parity v0_8_capture_failure "$FIX/parity/capture_failure.ds" 0 ""
assert_vm_bash_parity v0_8_capture_stderr "$FIX/parity/capture_stderr.ds" 0 ""
assert_vm_bash_parity v0_8_capture_args "$FIX/parity/capture_args.ds" 0 ""
assert_vm_bash_parity v0_8_field_interpolation "$FIX/parity/field_interpolation.ds" 0 ""
assert_vm_bash_parity v0_8_redirect_stdout "$FIX/parity/redirect_stdout.ds" 0 "out.txt"
assert_vm_bash_parity v0_8_redirect_stderr "$FIX/parity/redirect_stderr.ds" 0 "err.txt"
assert_vm_bash_parity v0_8_redirect_combined "$FIX/parity/redirect_combined.ds" 0 "all.txt"
assert_vm_bash_parity v0_8_import_capture "$FIX/parity/import_capture.ds" 0 ""
assert_vm_bash_parity v0_8_script_args_capture "$FIX/parity/script_args_capture.ds" 0 "" 'hello world; $(echo nope) {brace}'
run_ok v0_8_plain_fail_fast_emit "$DS" emit bash "$FIX/parity/plain_fail_fast.ds" -o "$TMP/v0_8_plain_fail_fast.sh"
run_ok v0_8_plain_fail_fast_bash_syntax bash -n "$TMP/v0_8_plain_fail_fast.sh"
capture_status v0_8_plain_fail_fast_vm "$DS" run "$FIX/parity/plain_fail_fast.ds"
assert_status v0_8_plain_fail_fast_vm 6
capture_status v0_8_plain_fail_fast_bash bash "$TMP/v0_8_plain_fail_fast.sh"
assert_status v0_8_plain_fail_fast_bash 6
assert_same "$TMP/v0_8_plain_fail_fast_vm.out" "$TMP/v0_8_plain_fail_fast_bash.out" "VM/Bash stdout parity: v0_8_plain_fail_fast"
assert_contains "$TMP/v0_8_plain_fail_fast_bash.err" "$FIX/parity/plain_fail_fast.ds:1:1: error: command failed with exit 6" "Bash fail-fast reports source marker"
assert_not_contains "$TMP/v0_8_plain_fail_fast_vm.out" "unreachable" "plain fail-fast VM stops subsequent statements"
assert_not_contains "$TMP/v0_8_plain_fail_fast_bash.out" "unreachable" "plain fail-fast Bash stops subsequent statements"

# Process behavior edge cases after the VM wrapper cleanup.
capture_status plain_success "$DS" run "$FIX/process/plain_success.ds"
assert_status plain_success 0
assert_same_text 'plain-ok' "$TMP/plain_success.out" "plain command streams stdout"
capture_status plain_fail_fast "$DS" run "$FIX/process/plain_fail_fast.ds"
assert_status plain_fail_fast 7
assert_same_text 'before' "$TMP/plain_fail_fast.out" "plain failure streams before exiting"
assert_not_contains "$TMP/plain_fail_fast.out" "after" "plain failure does not run following statements"
capture_status plain_command_not_found "$DS" run "$FIX/process/plain_command_not_found.ds"
assert_status plain_command_not_found 127
assert_contains "$TMP/plain_command_not_found.err" "$FIX/process/plain_command_not_found.ds:1:1: error:" "plain command-not-found diagnostic is source-located"
assert_contains "$TMP/plain_command_not_found.err" "failed to launch command" "plain command-not-found diagnostic"
run_ok capture_command_not_found "$DS" run "$FIX/process/capture_command_not_found.ds"
assert_contains "$TMP/capture_command_not_found.out" "127" "captured command-not-found records status"
assert_contains "$TMP/capture_command_not_found.out" "true" "captured command-not-found records failed bool"
assert_contains "$TMP/capture_command_not_found.out" "failed to launch command" "captured command-not-found stores launch message in stderr field"
assert_same_text '' "$TMP/capture_command_not_found.err" "captured command-not-found does not stream stderr"
run_ok capture_empty_output "$DS" run "$FIX/process/capture_empty_output.ds"
assert_same_text $'stdout= stderr= code=0\n' "$TMP/capture_empty_output.out" "empty capture fields are stable"
run_ok capture_large_output "$DS" run "$FIX/process/capture_large_output.ds"
if [ "$(wc -c < "$TMP/capture_large_output.out")" -ne 4097 ]; then
  fail "large captured output should contain 4096 bytes plus echo newline"
fi
pass "large captured output survives wrapper"
run_ok capture_many_repeated "$DS" run "$FIX/process/capture_many_repeated.ds"
assert_same_text $'one\ntwo\n\nerr\n\n5\n' "$TMP/capture_many_repeated.out" "repeated captures do not reuse stale values"

# Executable path with spaces is still passed as one argv word.
mkdir -p "$TMP/bin space"
cat >"$TMP/bin space/tool with spaces" <<'SH'
#!/usr/bin/env sh
printf 'tool:%s\n' "$1"
SH
chmod +x "$TMP/bin space/tool with spaces"
cat >"$TMP/executable_path_with_spaces.ds" <<DS
let tool = "$TMP/bin space/tool with spaces"
let arg = "value with spaces"
let r = run \$tool \$arg
echo r.stdout
DS
run_ok executable_path_with_spaces "$DS" run "$TMP/executable_path_with_spaces.ds"
assert_same_text $'tool:value with spaces\n\n' "$TMP/executable_path_with_spaces.out" "executable path with spaces works"

# Bash helper generation remains standalone and deterministic enough for cleanup guarantees.
run_ok emit_no_capture "$DS" emit bash "$FIX/helpers/no_capture.ds" -o "$TMP/no_capture.sh"
assert_not_contains "$TMP/no_capture.sh" "__ds_capture" "no-capture script does not emit capture helper"
assert_contains "$TMP/no_capture.sh" "set -euo pipefail" "strict mode remains in emitted Bash"
run_ok emit_capture_only "$DS" emit bash "$FIX/helpers/capture_only.ds" -o "$TMP/capture_only.sh"
count_capture_helpers="$(grep -c '^__ds_capture()' "$TMP/capture_only.sh" || true)"
[ "$count_capture_helpers" = 1 ] || fail "capture helper should be emitted exactly once"
pass "capture helper emitted exactly once"
assert_contains "$TMP/capture_only.sh" "__ds_" "Bash helper uses __ds_ prefix"
assert_not_contains "$TMP/capture_only.sh" "$DS" "generated Bash does not reference local ds binary"
run_ok emit_redirect_only "$DS" emit bash "$FIX/helpers/redirect_only.ds" -o "$TMP/redirect_only.sh"
assert_not_contains "$TMP/redirect_only.sh" "__ds_capture" "redirect-only script does not emit capture helper"
run_ok emit_capture_and_redirect "$DS" emit bash "$FIX/helpers/capture_and_redirect.ds" -o "$TMP/capture_and_redirect.sh"
run_ok capture_and_redirect_syntax bash -n "$TMP/capture_and_redirect.sh"
mkdir -p "$TMP/copied"
cp "$TMP/capture_and_redirect.sh" "$TMP/copied/script.sh"
run_ok copied_standalone bash -c "cd '$TMP/copied' && bash script.sh"
assert_same_text 'redir' "$TMP/copied/out.txt" "copied generated Bash remains standalone"

# Diagnostics must be source-located and consistent across command families where applicable.
declare -A diag_messages=(
  [run_missing_command.ds]='expected command after `run`'
  [run_with_stdout_redirect.ds]='captured `run` commands do not support redirection'
  [run_with_combined_redirect.ds]='captured `run` commands do not support redirection'
  [legacy_stdout_redirect.ds]='unsupported command operator'
  [duplicate_redirection.ds]='duplicate redirection suffix'
  [redirect_missing_target.ds]='expected string redirection target after `&>`'
  [redirect_bad_target.ds]='redirection target must be a string literal'
  [unknown_command_variable.ds]='unknown command variable `missing`'
  [command_result_missing_field_name.ds]='expected field name after `.`'
  [command_result_unknown_field.ds]='unsupported command result field `missing`'
  [field_on_string.ds]='field access is only supported on command results'
  [interpolation_unknown_result_field.ds]='unknown command result field `missing`'
)
for file in "${!diag_messages[@]}"; do
  base="${file%.ds}"
  for cmd in check bytecode run direct emit; do
    case "$cmd" in
      check) run_fail "${base}_check" "$DS" check "$FIX/diagnostics/$file" ;;
      bytecode) run_fail "${base}_bytecode" "$DS" bytecode "$FIX/diagnostics/$file" ;;
      run) run_fail "${base}_run" "$DS" run "$FIX/diagnostics/$file" ;;
      direct) run_fail "${base}_direct" "$DS" "$FIX/diagnostics/$file" ;;
      emit) run_fail "${base}_emit" "$DS" emit bash "$FIX/diagnostics/$file" -o "$TMP/${base}_emit.sh" ;;
    esac
    assert_contains "$TMP/${base}_${cmd}.err" "$FIX/diagnostics/$file:" "$base $cmd diagnostic path"
    assert_contains "$TMP/${base}_${cmd}.err" ': error:' "$base $cmd diagnostic shape"
    assert_contains "$TMP/${base}_${cmd}.err" "${diag_messages[$file]}" "$base $cmd diagnostic message"
    if [ "$cmd" = emit ]; then
      assert_file_missing_or_empty "$TMP/${base}_emit.sh" "$base failed emit leaves no artifact"
    fi
  done
done

# Pipelines are supported as of v0.18.0; these former diagnostics now remain as
# stale-expectation coverage in older suites.
run_ok pipeline_plain_now_supported "$DS" check "$FIX/diagnostics/pipeline_plain_command.ds"
run_ok pipeline_captured_now_supported "$DS" check "$FIX/diagnostics/pipeline_captured_command.ds"

# Runtime-only redirection-open diagnostics should not run the child and should include source locations.
run_ok redirection_open_check "$DS" check "$FIX/diagnostics/redirection_open_missing_parent.ds"
capture_status redirection_open_run "$DS" run "$FIX/diagnostics/redirection_open_missing_parent.ds"
assert_nonzero_status redirection_open_run
assert_contains "$TMP/redirection_open_run.err" "$FIX/diagnostics/redirection_open_missing_parent.ds:1:15: error:" "redirection-open diagnostic source location"
assert_contains "$TMP/redirection_open_run.err" "failed to open redirection target" "redirection-open diagnostic message"
assert_same_text '' "$TMP/redirection_open_run.out" "redirection-open failure does not execute command"
run_ok redirection_open_emit "$DS" emit bash "$FIX/diagnostics/redirection_open_missing_parent.ds" -o "$TMP/redirection_open.sh"
run_ok redirection_open_bash_syntax bash -n "$TMP/redirection_open.sh"
capture_status redirection_open_bash bash "$TMP/redirection_open.sh"
assert_nonzero_status redirection_open_bash

# Failed emit after a previous successful emit must not leave a stale successful-looking artifact.
run_ok stale_emit_seed "$DS" emit bash examples/basic.ds -o "$TMP/stale.sh"
assert_contains "$TMP/stale.sh" '#!/usr/bin/env bash' "seed emit wrote script"
run_fail stale_emit_invalid "$DS" emit bash "$FIX/diagnostics/interpolation_unknown_result_field.ds" -o "$TMP/stale.sh"
assert_file_missing_or_empty "$TMP/stale.sh" "failed emit removes stale artifact"

# No new syntax was unlocked by cleanup.
for file in "$FIX"/unsupported/*.ds; do
  base="$(basename "$file" .ds)"
  if [ "$base" = pipeline ]; then
    run_ok unsupported_pipeline_now_supported "$DS" check "$file"
    continue
  fi
  run_fail "unsupported_${base}_check" "$DS" check "$file"
  assert_contains "$TMP/unsupported_${base}_check.err" ': error:' "unsupported $base has diagnostic"
  run_fail "unsupported_${base}_emit" "$DS" emit bash "$file" -o "$TMP/unsupported_${base}.sh"
  assert_file_missing_or_empty "$TMP/unsupported_${base}.sh" "unsupported $base emit leaves no artifact"
done

# Import and script-argument interactions remain intact.
assert_vm_bash_parity v0_8_import_capture_again "$FIX/parity/import_capture.ds" 0 ""
assert_vm_bash_parity v0_8_script_arg_metachar "$FIX/parity/script_args_capture.ds" 0 "" 'spaces $HOME `echo bad` ; {x}'
run_ok script_help_unchanged "$DS" run "$FIX/parity/script_args_capture.ds" --help
assert_contains "$TMP/script_help_unchanged.out" 'Usage:' "script help remains available"
run_ok script_help_emit "$DS" emit bash "$FIX/parity/script_args_capture.ds" -o "$TMP/script_help.sh"
run_ok script_help_bash bash "$TMP/script_help.sh" --help
assert_same "$TMP/script_help_unchanged.out" "$TMP/script_help_bash.out" 'VM and emitted Bash share script help behavior'
run_fail imported_script_rejected "$DS" check tests/v0_6/fixtures/imports_errors/imported_script_main.ds
assert_contains "$TMP/imported_script_rejected.err" 'imported files cannot declare `script` blocks' "imported script block remains rejected"
run_fail import_cycle_still_rejected "$DS" check tests/v0_6/fixtures/imports_errors/cycle_a.ds
assert_contains "$TMP/import_cycle_still_rejected.err" 'import cycle detected' "import cycle diagnostic unchanged"

# Static cleanup boundary checks.
run_ok static_bytecode_command "$DS" bytecode "$FIX/parity/capture_success.ds"
assert_not_contains "$TMP/static_bytecode_command.out" "0x" "v0.8 bytecode output is pointer-free"
assert_contains "$ROOT/src/ds_command_facts.c" '"status", "code"' "command-result status storage alias is centralized"
assert_contains "$ROOT/Makefile" "src/ds_signal.c" "shared signal runtime contract is linked into ds"
assert_contains "$ROOT/Makefile" "src/bash_function.c" "Bash function wrapper source is linked into ds"
assert_not_contains "$ROOT/Makefile" "src/bash_return.c" "Bash return emission is folded into statement emitter, not a micro-file"
assert_contains "$ROOT/Makefile" "src/lower_collection.c" "collection portability policy is linked into ds"
assert_contains "$ROOT/src/ds_checker.c" '#include "ds_checker.h"' "checker uses its narrow façade"
assert_not_contains "$ROOT/src/ds_checker.c" '#include "backend.h"' "checker does not include broad backend façade"
assert_contains "$ROOT/src/ds_checker.h" "ds_check_warnings_ast" "checker warning entrypoint has a narrow header"
assert_not_contains "$ROOT/src/backend.h" "ds_check_warnings_ast" "backend façade no longer exposes checker warnings"
assert_contains "$ROOT/tests/lib/build_sources.sh" "ds_project_source_rels" "unit harness source helper reads the Makefile source list"
assert_contains "$ROOT/tests/v0_3/run.sh" "ds_compile_unit" "v0.3 direct unit builds use shared source helper"
assert_contains "$ROOT/tests/v0_5/run.sh" "ds_compile_unit" "v0.5 direct unit builds use shared source helper"
assert_contains "$ROOT/tests/v0_7/run.sh" "ds_compile_unit" "v0.7 direct unit builds use shared source helper"
assert_contains "$ROOT/tests/v0_8/run.sh" "ds_compile_unit" "v0.8 direct unit builds use shared source helper"
assert_not_contains "$ROOT/tests/v0_5/run.sh" '"$ROOT/src/lower.c" "$ROOT/src/lower_symbols.c"' "v0.5 no longer carries an inline lowerer source list"
assert_not_contains "$ROOT/tests/v0_7/run.sh" '"$ROOT/src/lexer.c" "$ROOT/src/parser.c"' "v0.7 no longer carries an inline lowerer source list"
assert_contains "$ROOT/tests/lib/testlib.sh" "assert_vm_bash_parity" "shared parity helper exists"
assert_not_contains "$ROOT/tests/v0_7/run.sh" "run_parity_fixture" "v0.7 no longer carries a duplicate large parity helper"
assert_not_contains "$ROOT/src/bash_emit.c" "hashmap" "Bash backend does not depend on staged hashmap internals"
assert_contains "$ROOT/docs/architecture.md" 'v0.8.0 command cleanup' "architecture documents v0.8 cleanup"
assert_contains "$ROOT/docs/runtime.md" 'process spec/result wrapper' "runtime documents process wrapper"
assert_contains "$ROOT/docs/language.ds" 'let result = run npm test' "language doc keeps v0.7 syntax, not new v0.8 syntax"

# Makefile integration for this dedicated suite. Avoid recursively invoking the
# target from inside itself; the full validation invokes the target directly.
assert_contains "$ROOT/Makefile" 'TEST_VERSIONS := 0-1 0-2 0-3 0-4 0-5 0-6 0-7 0-8 0-9 0-10' "Makefile lists v0.8 in the generated test target set"
assert_contains "$ROOT/Makefile" '$(TEST_TARGETS): $(BIN)' "Makefile exposes generated per-version test targets"
assert_contains "$ROOT/Makefile" './tests/v$$dir/run.sh' "Makefile wires listed suites into the aggregate test target"

echo "v0.8.0 tests passed: $pass_count checks"
