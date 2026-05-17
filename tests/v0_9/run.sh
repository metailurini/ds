#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_9_tests.$$"
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

assert_diag() {
  local name="$1"
  local fixture="$2"
  local message="$3"
  run_fail "${name}_check" "$DS" check "$fixture"
  assert_contains "$TMP/${name}_check.err" "$fixture:" "$name diagnostic path"
  assert_contains "$TMP/${name}_check.err" ': error:' "$name diagnostic shape"
  assert_contains "$TMP/${name}_check.err" "$message" "$name diagnostic message"
  run_fail "${name}_emit" "$DS" emit bash "$fixture" -o "$TMP/${name}.sh"
  assert_file_missing_or_empty "$TMP/${name}.sh" "$name failed emit leaves no artifact"
}

FIX="$TMP/fixtures"
mkdir -p "$FIX"

write_fixture "$FIX/basic.ds" <<'DS'
fn hello() {
  echo "hello"
}

hello()
DS

write_fixture "$FIX/positional.ds" <<'DS'
fn deploy(app, target) {
  echo "{app}:{target}"
}

deploy("api", "prod")
deploy("worker service", "staging")
DS

write_fixture "$FIX/defaults.ds" <<'DS'
fn show(a, b = "x", c = 0, d = false) {
  echo "{a}:{b}:{c}:{d}"
}

show("req")
show("req", "override")
show("req", "override", 4)
show("req", "override", 4, true)
DS

write_fixture "$FIX/default_metachar.ds" <<'DS'
fn show(value = "spaces $HOME `echo bad` ; [brace] \"quoted\"") {
  echo "value={value}"
}

show()
DS

write_fixture "$FIX/all_defaults.ds" <<'DS'
fn greet(name = "world", suffix = "!") {
  echo "hello {name}{suffix}"
}

greet()
greet("ds", "?")
DS

write_fixture "$FIX/call_before_declaration.ds" <<'DS'
run_later("ok")

fn run_later(value) {
  echo value
}
DS

write_fixture "$FIX/scopes.ds" <<'DS'
let outer_target = "outer"

fn show(target) {
  let value = "local"
  echo "inner={target}:{value}"
  if true {
    let block_value = "block"
    echo "block={block_value}"
  }
}

show("first")
show("second")
echo "outer={outer_target}"
DS

write_fixture "$FIX/script_args.ds" <<'DS'
script {
  arg app: string
  option target: string = "staging"
  flag force: bool = false
}

fn deploy() {
  echo "{app}:{target}:{force}"
}

deploy()
DS

write_fixture "$FIX/capture.ds" <<'DS'
fn capture() {
  let result = run sh -c "printf out; printf err >&2; exit 7"
  echo "stdout={result.stdout}"
  echo "stderr={result.stderr}"
  echo "code={result.code}"
  echo "ok={result.ok}"
  echo "failed={result.failed}"
}

capture()
DS

write_fixture "$FIX/handled_failure.ds" <<'DS'
fn nonfatal() {
  let result = run sh -c "exit 9"
  if result.failed {
    echo "handled {result.code}"
  }
}

nonfatal()
echo "after"
DS

write_fixture "$FIX/plain_fail.ds" <<'DS'
fn fail() {
  sh -c "exit 6"
  echo "function unreachable"
}

fail()
echo "caller unreachable"
DS

write_fixture "$FIX/redirect_stdout.ds" <<'DS'
fn write_out() {
  printf "hello" |> "out.txt"
}

write_out()
DS

write_fixture "$FIX/redirect_stderr.ds" <<'DS'
fn write_err() {
  sh -c "printf err >&2" !> "err.txt"
}

write_err()
DS

write_fixture "$FIX/redirect_combined.ds" <<'DS'
fn write_all() {
  sh -c "printf out; printf err >&2" &> "all.txt"
}

write_all()
DS

write_fixture "$FIX/metachar_args.ds" <<'DS'
script {
  arg value: string
}

fn show(input, empty = "", zero = 0, off = false) {
  echo "value={input}"
  echo "empty={empty} zero={zero} off={off}"
}

show(value)
DS

write_fixture "$FIX/declaration_only.ds" <<'DS'
fn dangerous() {
  sh -c "exit 99"
}
DS

write_fixture "$FIX/import_lib.ds" <<'DS'
echo "import body"
fn greet(name) {
  echo "hello {name}"
}
fn call_root() {
  announce("from import")
}
DS
write_fixture "$FIX/import_main.ds" <<DS
import "$FIX/import_lib.ds"

fn announce(message) {
  echo "announce {message}"
}

greet("world")
call_root()
DS
write_fixture "$FIX/import_once_lib.ds" <<'DS'
echo "loaded once"
fn once(message) {
  echo "once {message}"
}
DS
write_fixture "$FIX/import_once_main.ds" <<DS
import "$FIX/import_once_lib.ds"
import "$FIX/import_once_lib.ds"

once("call")
DS
write_fixture "$FIX/import_dup_a.ds" <<'DS'
fn conflict() {}
DS
write_fixture "$FIX/import_dup_b.ds" <<'DS'
fn conflict() {}
DS
write_fixture "$FIX/import_dup_main.ds" <<DS
import "$FIX/import_dup_a.ds"
import "$FIX/import_dup_b.ds"
DS
write_fixture "$FIX/import_cycle_a.ds" <<DS
import "$FIX/import_cycle_b.ds"
fn cycle_a() {}
DS
write_fixture "$FIX/import_cycle_b.ds" <<DS
import "$FIX/import_cycle_a.ds"
fn cycle_b() {}
DS
write_fixture "$FIX/import_cycle_main.ds" <<DS
import "$FIX/import_cycle_a.ds"
cycle_a()
DS

# Lexer, parser, AST, bytecode, and check smoke coverage.
run_ok tokens_defaults "$DS" tokens "$FIX/defaults.ds"
assert_contains "$TMP/tokens_defaults.out" 'FN' "lexer emits fn token"
assert_contains "$TMP/tokens_defaults.out" 'COMMA' "lexer emits comma token"
assert_contains "$TMP/tokens_defaults.out" 'EQUAL' "lexer emits default equal token"
assert_not_contains "$TMP/tokens_defaults.out" '0x' "tokens pointer-free"
run_ok ast_defaults "$DS" ast "$FIX/defaults.ds"
assert_contains "$TMP/ast_defaults.out" 'FnStmt show' "AST prints function declaration"
assert_contains "$TMP/ast_defaults.out" 'Param b =' "AST prints default parameter"
assert_contains "$TMP/ast_defaults.out" 'CallStmt show' "AST prints function call"
assert_not_contains "$TMP/ast_defaults.out" '0x' "AST pointer-free"
run_ok bytecode_defaults "$DS" bytecode "$FIX/defaults.ds"
assert_contains "$TMP/bytecode_defaults.out" 'functions:' "bytecode prints function metadata section"
assert_contains "$TMP/bytecode_defaults.out" 'fn0 show(required=1, params=4)' "bytecode prints function name and arity metadata"
assert_contains "$TMP/bytecode_defaults.out" 'param 1 b = string "x"' "bytecode prints string default metadata"
assert_contains "$TMP/bytecode_defaults.out" 'param 2 c = int 0' "bytecode prints int default metadata"
assert_contains "$TMP/bytecode_defaults.out" 'param 3 d = bool false' "bytecode prints bool default metadata"
assert_contains "$TMP/bytecode_defaults.out" 'CALL' "bytecode prints function call instruction"
assert_not_contains "$TMP/bytecode_defaults.out" '0x' "bytecode pointer-free"
run_ok check_declaration_only "$DS" check "$FIX/declaration_only.ds"
assert_same_text '' "$TMP/check_declaration_only.out" "check does not execute declaration-only function"
assert_same_text '' "$TMP/check_declaration_only.err" "check declaration-only stderr empty"

# VM/Bash parity for supported function behavior.
assert_vm_bash_parity v0_9_basic "$FIX/basic.ds" 0 ""
assert_same_text $'hello\n' "$TMP/v0_9_basic_vm.out" "basic function output"
assert_vm_bash_parity v0_9_positional "$FIX/positional.ds" 0 ""
assert_vm_bash_parity v0_9_defaults "$FIX/defaults.ds" 0 ""
assert_same_text $'req:x:0:false\nreq:override:0:false\nreq:override:4:false\nreq:override:4:true\n' "$TMP/v0_9_defaults_vm.out" "default parameter binding output"
assert_vm_bash_parity v0_9_default_metachar "$FIX/default_metachar.ds" 0 ""
assert_contains "$TMP/v0_9_default_metachar_vm.out" 'spaces $HOME `echo bad` ; [brace] "quoted"' "default string metacharacters preserved"
assert_vm_bash_parity v0_9_all_defaults "$FIX/all_defaults.ds" 0 ""
assert_vm_bash_parity v0_9_call_before_declaration "$FIX/call_before_declaration.ds" 0 ""
assert_vm_bash_parity v0_9_scopes "$FIX/scopes.ds" 0 ""
assert_contains "$TMP/v0_9_scopes_vm.out" 'inner=first:local' "function first call has fresh locals"
assert_contains "$TMP/v0_9_scopes_vm.out" 'inner=second:local' "function second call has fresh locals"
assert_contains "$TMP/v0_9_scopes_vm.out" 'outer=outer' "top-level binding survives function calls"
assert_vm_bash_parity v0_9_script_args "$FIX/script_args.ds" 0 "" api --target prod --force
assert_same_text $'api:prod:true\n' "$TMP/v0_9_script_args_vm.out" "script args visible inside function"
assert_vm_bash_parity v0_9_capture "$FIX/capture.ds" 0 ""
assert_contains "$TMP/v0_9_capture_vm.out" 'stdout=out' "captured stdout in function"
assert_contains "$TMP/v0_9_capture_vm.out" 'stderr=err' "captured stderr in function"
assert_contains "$TMP/v0_9_capture_vm.out" 'code=7' "captured status in function"
assert_vm_bash_parity v0_9_handled_failure "$FIX/handled_failure.ds" 0 ""
assert_same_text $'handled 9\nafter\n' "$TMP/v0_9_handled_failure_vm.out" "captured failure handled inside function"
assert_vm_bash_parity v0_9_plain_fail "$FIX/plain_fail.ds" 6 ""
assert_not_contains "$TMP/v0_9_plain_fail_vm.out" 'unreachable' "VM plain command failure aborts function and caller"
assert_not_contains "$TMP/v0_9_plain_fail_bash.out" 'unreachable' "Bash plain command failure aborts function and caller"
assert_vm_bash_parity v0_9_redirect_stdout "$FIX/redirect_stdout.ds" 0 "out.txt"
assert_same_text 'hello' "$TMP/parity_v0_9_redirect_stdout_vm_work/out.txt" "stdout redirection from function"
assert_vm_bash_parity v0_9_redirect_stderr "$FIX/redirect_stderr.ds" 0 "err.txt"
assert_same_text 'err' "$TMP/parity_v0_9_redirect_stderr_vm_work/err.txt" "stderr redirection from function"
assert_vm_bash_parity v0_9_redirect_combined "$FIX/redirect_combined.ds" 0 "all.txt"
assert_contains "$TMP/parity_v0_9_redirect_combined_vm_work/all.txt" 'out' "combined redirection captures stdout"
assert_contains "$TMP/parity_v0_9_redirect_combined_vm_work/all.txt" 'err' "combined redirection captures stderr"
assert_vm_bash_parity v0_9_metachar_args "$FIX/metachar_args.ds" 0 "" 'spaces $HOME `echo bad` ; {brace} "quoted"'
assert_contains "$TMP/v0_9_metachar_args_vm.out" 'spaces $HOME `echo bad` ; {brace} "quoted"' "metacharacter argument preserved"
assert_vm_bash_parity v0_9_import "$FIX/import_main.ds" 0 ""
assert_same_text $'import body\nhello world\nannounce from import\n' "$TMP/v0_9_import_vm.out" "imported and root functions are whole-program visible"
assert_vm_bash_parity v0_9_import_once "$FIX/import_once_main.ds" 0 ""
assert_same_text $'loaded once\nonce call\n' "$TMP/v0_9_import_once_vm.out" "repeated import with functions loads once"

# Direct execution matches `ds run`, and script help exits before executing functions.
capture_status direct_functions "$DS" "$FIX/script_args.ds" api --target prod --force
assert_status direct_functions 0
assert_same "$TMP/direct_functions.out" "$TMP/v0_9_script_args_vm.out" "direct function script matches ds run"
run_ok script_help "$DS" run "$FIX/script_args.ds" --help
assert_contains "$TMP/script_help.out" 'Usage:' "script help works with functions"
assert_not_contains "$TMP/script_help.out" 'api:prod:true' "script help does not execute function body"

# Bash emission inspections.
run_ok emit_defaults "$DS" emit bash "$FIX/defaults.ds" -o "$TMP/defaults.sh"
run_ok emit_defaults_syntax bash -n "$TMP/defaults.sh"
assert_contains "$TMP/defaults.sh" '__ds_fn_show()' "emitted Bash uses internal function name"
assert_contains "$TMP/defaults.sh" 'local __ds_a' "emitted Bash binds params as locals"
assert_not_contains "$TMP/defaults.sh" "$DS" "emitted Bash is standalone"
run_ok emit_import "$DS" emit bash "$FIX/import_main.ds" -o "$TMP/import.sh"
mkdir -p "$TMP/copied"
cp "$TMP/import.sh" "$TMP/copied/import.sh"
rm -f "$FIX/import_lib.ds" "$FIX/import_main.ds"
run_ok copied_import_bash bash -c "cd '$TMP/copied' && bash import.sh"
assert_same_text $'import body\nhello world\nannounce from import\n' "$TMP/copied_import_bash.out" "emitted import function script remains standalone after sources move away"

# Diagnostics: parser, semantic, runtime, and failed emission cleanup.
write_fixture "$FIX/bad_missing_name.ds" <<'DS'
fn { }
DS
assert_diag missing_name "$FIX/bad_missing_name.ds" 'expected function name after `fn`'

write_fixture "$FIX/bad_param_list.ds" <<'DS'
fn deploy(target,, other) { }
DS
assert_diag malformed_param_list "$FIX/bad_param_list.ds" 'expected parameter name'

write_fixture "$FIX/bad_trailing_param_comma.ds" <<'DS'
fn deploy(target,) { }
DS
assert_diag trailing_param_comma "$FIX/bad_trailing_param_comma.ds" 'expected parameter name after `,`'

write_fixture "$FIX/bad_trailing_call_comma.ds" <<'DS'
fn deploy(target) { }
deploy("x",)
DS
assert_diag trailing_call_comma "$FIX/bad_trailing_call_comma.ds" 'expected function call argument after `,`'

write_fixture "$FIX/bad_duplicate_function.ds" <<'DS'
fn deploy() {}
fn deploy() {}
DS
assert_diag duplicate_function "$FIX/bad_duplicate_function.ds" 'duplicate function `deploy`'
run_fail duplicate_imported_function_check "$DS" check "$FIX/import_dup_main.ds"
assert_contains "$TMP/duplicate_imported_function_check.err" 'import_dup_b.ds:' "duplicate imported function reports imported source path"
assert_contains "$TMP/duplicate_imported_function_check.err" 'duplicate function `conflict`' "duplicate imported function diagnostic message"
run_fail duplicate_imported_function_emit "$DS" emit bash "$FIX/import_dup_main.ds" -o "$TMP/duplicate_imported_function.sh"
assert_file_missing_or_empty "$TMP/duplicate_imported_function.sh" "duplicate imported function failed emit leaves no artifact"
run_fail import_cycle_functions_check "$DS" check "$FIX/import_cycle_main.ds"
assert_contains "$TMP/import_cycle_functions_check.err" 'import cycle detected' "function import cycle diagnostic message"
assert_contains "$TMP/import_cycle_functions_check.err" 'import_cycle_a.ds' "function import cycle includes first file"
assert_contains "$TMP/import_cycle_functions_check.err" 'import_cycle_b.ds' "function import cycle includes second file"
run_fail import_cycle_functions_emit "$DS" emit bash "$FIX/import_cycle_main.ds" -o "$TMP/import_cycle_functions.sh"
assert_file_missing_or_empty "$TMP/import_cycle_functions.sh" "function import cycle failed emit leaves no artifact"

write_fixture "$FIX/bad_duplicate_param.ds" <<'DS'
fn deploy(target, target) {}
DS
assert_diag duplicate_param "$FIX/bad_duplicate_param.ds" 'duplicate parameter `target`'

write_fixture "$FIX/bad_unknown_call.ds" <<'DS'
missing()
DS
assert_diag unknown_call "$FIX/bad_unknown_call.ds" 'unknown function `missing`'

write_fixture "$FIX/bad_too_few.ds" <<'DS'
fn deploy(target) {}
deploy()
DS
assert_diag too_few_args "$FIX/bad_too_few.ds" 'expects 1 arguments but got 0'

write_fixture "$FIX/bad_too_many.ds" <<'DS'
fn deploy(target) {}
deploy("a", "b")
DS
assert_diag too_many_args "$FIX/bad_too_many.ds" 'expects 1 arguments but got 2'

write_fixture "$FIX/bad_default_order.ds" <<'DS'
fn deploy(target = "staging", app) {}
DS
assert_diag default_before_required "$FIX/bad_default_order.ds" 'required parameter `app` cannot follow a default parameter'

write_fixture "$FIX/bad_default_expr.ds" <<'DS'
fn deploy(target = other) {}
DS
assert_diag bad_default_expr "$FIX/bad_default_expr.ds" 'function parameter defaults must be string, int, or bool literals in v0.9.0'

write_fixture "$FIX/bad_typed_param.ds" <<'DS'
fn deploy(target: string) {}
DS
assert_diag typed_param "$FIX/bad_typed_param.ds" 'typed function parameters are deferred in v0.9.0'

write_fixture "$FIX/bad_call_value.ds" <<'DS'
fn deploy() {}
let result = deploy()
DS
assert_diag call_value "$FIX/bad_call_value.ds" 'function calls do not produce values in v0.9.0'

write_fixture "$FIX/bad_function_as_variable.ds" <<'DS'
fn deploy() {}
echo $deploy
DS
assert_diag function_as_variable "$FIX/bad_function_as_variable.ds" 'function `deploy` cannot be used as a variable in v0.9.0'
count_function_as_variable="$(grep -c 'function `deploy` cannot be used as a variable in v0.9.0' "$TMP/function_as_variable_check.err" || true)"
[ "$count_function_as_variable" = 1 ] || fail "function-as-variable should be reported once, got $count_function_as_variable"
pass "function-as-variable conflict reports once"

write_fixture "$FIX/bad_nested.ds" <<'DS'
fn outer() {
  fn inner() {}
}
DS
assert_diag nested_function "$FIX/bad_nested.ds" 'function declarations are only allowed at top level in v0.9.0'

write_fixture "$FIX/bad_direct_recursion.ds" <<'DS'
fn self() {
  self()
}
self()
DS
assert_diag direct_recursion "$FIX/bad_direct_recursion.ds" 'recursive function calls are deferred in v0.9.0'

write_fixture "$FIX/bad_mutual_recursion.ds" <<'DS'
fn a() { b() }
fn b() { a() }
a()
DS
assert_diag mutual_recursion "$FIX/bad_mutual_recursion.ds" 'recursive function calls are deferred in v0.9.0'

write_fixture "$FIX/bad_local_leak.ds" <<'DS'
fn leak() {
  let secret = "hidden"
}
leak()
echo $secret
DS
assert_diag local_leak "$FIX/bad_local_leak.ds" 'unknown command variable `secret`'

write_fixture "$FIX/bad_pipeline.ds" <<'DS'
fn pipe() {
  echo hi | cat
}
pipe()
DS
assert_diag pipeline_function "$FIX/bad_pipeline.ds" 'pipelines are not supported in v0.7.0'

write_fixture "$FIX/bad_duplicate_top.ds" <<'DS'
fn same() {}
let same = "value"
DS
run_fail duplicate_top_check "$DS" check "$FIX/bad_duplicate_top.ds"
count_duplicate_top="$(grep -c 'duplicate variable `same`' "$TMP/duplicate_top_check.err" || true)"
[ "$count_duplicate_top" = 1 ] || fail "duplicate top-level name should be reported once, got $count_duplicate_top"
pass "duplicate top-level function/let conflict reports once"

write_fixture "$FIX/bad_redirection_open.ds" <<'DS'
fn write_bad() {
  printf "nope" |> "missing-parent/out.txt"
}
write_bad()
DS
run_ok redirection_open_check "$DS" check "$FIX/bad_redirection_open.ds"
capture_status redirection_open_run "$DS" run "$FIX/bad_redirection_open.ds"
assert_nonzero_status redirection_open_run
assert_contains "$TMP/redirection_open_run.err" "$FIX/bad_redirection_open.ds:" "redirection-open diagnostic path inside function"
assert_contains "$TMP/redirection_open_run.err" 'failed to open redirection target' "redirection-open diagnostic message inside function"
assert_same_text '' "$TMP/redirection_open_run.out" "redirection-open failure does not execute function command"

run_ok stale_emit_seed "$DS" emit bash "$FIX/basic.ds" -o "$TMP/stale.sh"
assert_contains "$TMP/stale.sh" '#!/usr/bin/env bash' "seed emit wrote script"
run_fail stale_emit_invalid "$DS" emit bash "$FIX/bad_unknown_call.ds" -o "$TMP/stale.sh"
assert_file_missing_or_empty "$TMP/stale.sh" "failed function emit removes stale artifact"

# Makefile and completion-review integration.
assert_contains "$ROOT/Makefile" 'test-v0-9:' "Makefile exposes test-v0-9 target"
assert_contains "$ROOT/Makefile" './tests/v0_9/run.sh' "Makefile wires v0.9 suite into test targets"
assert_contains "$ROOT/docs/milestones/v0.9.0-spec.md" 'Tests added' "v0.9 spec completion review records tests"
assert_contains "$ROOT/docs/milestones/v0.9.0-spec.md" 'tests/v0_9/run.sh' "v0.9 spec names test suite"
assert_contains "$ROOT/README.md" 'v0.9.0` implementation and tests are complete' "README status is current for v0.9"

echo "v0.9.0 tests passed: $pass_count checks"
