#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
DS="$ROOT/ds"
CASE_TIMEOUT=${DS_TEST_CASE_TIMEOUT:-25}
PIPE_TIMEOUT=${DS_TEST_PIPE_TIMEOUT:-8}
TMP=${TMPDIR:-/tmp}/ds_v0_34_tests.$$
FIX="$TMP/fixtures"
mkdir -p "$FIX"
if [[ "${DS_KEEP_TMP:-0}" != "1" ]]; then
  trap 'rm -rf "$TMP"' EXIT
fi

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != "1" ]]; then
  make -C "$ROOT" >/dev/null
fi

write_fixture() {
  local name="$1"
  local path="$FIX/$name.ds"
  mkdir -p "$(dirname "$path")"
  cat >"$path"
  printf '%s' "$path"
}

write_expected() {
  local name="$1" text="$2" path
  path="$TMP/$name.expected"
  printf '%s' "$text" >"$path"
  printf '%s' "$path"
}

assert_text() {
  local name="$1" expected="$2" actual="$3" exp
  exp=$(write_expected "$name" "$expected")
  assert_same "$exp" "$actual" "$name"
}

capture_cmd() {
  local name="$1"
  shift
  set +e
  "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/$name.rc"
}

emit_checked() {
  local name="$1" file="$2" script="$3"
  run_ok "${name}_emit" "$DS" emit bash "$file" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_no_ds_call "$script" "$name emitted Bash standalone"
  assert_no_duplicate_helpers "$script" "$name emitted Bash"
}

assert_no_ds_call() {
  local script="$1" name="$2"
  assert_not_contains "$script" "$ROOT/ds" "$name omits repo ds path"
  assert_not_contains "$script" './ds ' "$name omits ./ds invocation"
  assert_not_contains "$script" ' ds run ' "$name omits ds run invocation"
  assert_not_contains "$script" ' ds emit ' "$name omits ds emit invocation"
}

assert_no_duplicate_helpers() {
  local script="$1" name="$2" defs dups
  defs="$TMP/${name//[^A-Za-z0-9_]/_}_helper_defs.txt"
  dups="$TMP/${name//[^A-Za-z0-9_]/_}_helper_dups.txt"
  grep -E '^__ds_[A-Za-z0-9_]+\(\)' "$script" | sed 's/(.*//' | sort >"$defs" || true
  uniq -d "$defs" >"$dups"
  [ ! -s "$dups" ] || { cat "$dups" >&2; fail "$name has duplicate helper definitions"; }
  pass "$name has no duplicate helper definitions"
}

run_parity() {
  local name="$1" file="$2" expected_stdout="$3" expected_status="${4:-0}"
  if [ "$#" -ge 4 ]; then
    shift 4
  else
    shift "$#"
  fi
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  rm -rf "$vm_work" "$bash_work"
  mkdir -p "$vm_work" "$bash_work"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"
  set +e
  (cd "$vm_work" && timeout "$CASE_TIMEOUT" "$DS" run "$file" "$@") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && timeout "$CASE_TIMEOUT" bash "$script" "$@") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"
  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name VM/Bash stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name VM/Bash stderr parity"
  else
    assert_contains "$TMP/${name}_vm.err" ': error:' "$name VM diagnostic shape"
    assert_contains "$TMP/${name}_bash.err" ': error:' "$name Bash diagnostic shape"
  fi
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
}

run_parity_hostile() {
  local name="$1" file="$2" expected_stdout="$3"
  local script="$TMP/$name.sh" bash_work="$TMP/${name}_bash_work"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"
  rm -rf "$bash_work"; mkdir -p "$bash_work"
  set +e
  (cd "$bash_work" && set -f && IFS=':' && shopt -s failglob nullglob dotglob globstar 2>/dev/null || true; timeout "$CASE_TIMEOUT" bash "$script") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"
  assert_status "${name}_bash" 0
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_bash.out"
  assert_text "${name}_stderr" '' "$TMP/${name}_bash.err"
}

assert_check_fails() {
  local name="$1" file="$2" needle="$3"
  capture_cmd "${name}_check" "$DS" check "$file"
  assert_nonzero_status "${name}_check"
  assert_contains "$TMP/${name}_check.err" ': error:' "$name check diagnostic shape"
  assert_contains "$TMP/${name}_check.err" "$needle" "$name check diagnostic message"
}

assert_emit_fails() {
  local name="$1" file="$2" needle="$3" out="$TMP/$name.sh"
  capture_cmd "${name}_emit" "$DS" emit bash "$file" -o "$out"
  assert_nonzero_status "${name}_emit"
  assert_contains "$TMP/${name}_emit.err" ': error:' "$name emit diagnostic shape"
  assert_contains "$TMP/${name}_emit.err" "$needle" "$name emit diagnostic message"
  assert_file_missing_or_empty "$out" "$name failed emit leaves no valid artifact"
}

assert_rejected() {
  local name="$1" file="$2" needle="$3"
  assert_check_fails "$name" "$file" "$needle"
  assert_emit_fails "$name" "$file" "$needle"
}

assert_runtime_failure() {
  local name="$1" file="$2" needle="$3"
  local bash_needle="${4:-$needle}"
  local script="$TMP/$name.sh"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"
  capture_cmd "${name}_vm" "$DS" run "$file"
  capture_cmd "${name}_bash" bash "$script"
  assert_nonzero_status "${name}_vm"
  assert_nonzero_status "${name}_bash"
  assert_contains "$TMP/${name}_vm.err" "$needle" "$name VM diagnostic"
  assert_contains "$TMP/${name}_bash.err" "$bash_needle" "$name Bash diagnostic"
}

run_head_vm() {
  local name="$1" file="$2" expected="$3"
  set +e
  timeout "$PIPE_TIMEOUT" bash -c 'set -o pipefail; "$1" run "$2" | head -n 1' bash "$DS" "$file" >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/${name}_vm.rc"
  assert_status "${name}_vm" 0
  assert_text "${name}_vm_stdout" "$expected" "$TMP/${name}_vm.out"
  assert_not_contains "$TMP/${name}_vm.err" 'failed with exit 141' "$name VM no noisy exit-141 diagnostic"
  assert_not_contains "$TMP/${name}_vm.err" 'Broken pipe' "$name VM no Broken pipe diagnostic"
}

run_head_bash() {
  local name="$1" file="$2" expected="$3"
  local script="$TMP/$name.sh"
  emit_checked "$name" "$file" "$script"
  set +e
  timeout "$PIPE_TIMEOUT" bash -c 'set -o pipefail; bash "$1" | head -n 1' bash "$script" >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/${name}_bash.rc"
  assert_status "${name}_bash" 0
  assert_text "${name}_bash_stdout" "$expected" "$TMP/${name}_bash.out"
  assert_not_contains "$TMP/${name}_bash.err" 'failed with exit 141' "$name Bash no noisy exit-141 diagnostic"
  assert_not_contains "$TMP/${name}_bash.err" 'Broken pipe' "$name Bash no Broken pipe diagnostic"
}

run_head_both() {
  local name="$1" file="$2" expected="$3"
  run_ok "${name}_check" "$DS" check "$file"
  run_head_vm "$name" "$file" "$expected"
  run_head_bash "$name" "$file" "$expected"
}

assert_piped_runtime_failure() {
  local name="$1" file="$2" needle="$3"
  local marker="${4:-}"
  local script="$TMP/$name.sh"
  local vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  rm -rf "$vm_work" "$bash_work"
  mkdir -p "$vm_work" "$bash_work"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"
  set +e
  (cd "$vm_work" && timeout "$PIPE_TIMEOUT" bash -c 'set -o pipefail; "$1" run "$2" | head -n 1' bash "$DS" "$file") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && timeout "$PIPE_TIMEOUT" bash -c 'set -o pipefail; bash "$1" | head -n 1' bash "$script") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"
  assert_nonzero_status "${name}_vm"
  assert_nonzero_status "${name}_bash"
  assert_contains "$TMP/${name}_vm.err" "$needle" "$name VM piped failure remains visible"
  assert_contains "$TMP/${name}_bash.err" "$needle" "$name Bash piped failure remains visible"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name piped failure stdout parity"
  if [[ -n "$marker" ]]; then
    [ -f "$vm_work/$marker" ] || fail "$name VM cleanup marker missing"
    pass "$name VM cleanup marker exists"
    [ -f "$bash_work/$marker" ] || fail "$name Bash cleanup marker missing"
    pass "$name Bash cleanup marker exists"
  fi
}

run_ok cli_help "$DS" --help
assert_contains "$TMP/cli_help.out" 'ds v0.38.0' 'CLI help reports current v0.38.0 identity'

# 2. Literal braces in ordinary strings.
ordinary=$(write_fixture ordinary_braces <<'DS'
let name = "api"
let value = "x y"
echo "{{"
echo "}}"
echo "{{}}"
echo "{{ \"service\": \"{name}\" }}"
echo "{{name}}"
echo "{name}}}"
echo "{{name"
echo "{{{{"
echo "}}}}"
echo "{{{{}}}}"
echo "{{ path: \"{value}\", chars: \"$*?[]\" }}"
DS
)
run_parity ordinary_braces "$ordinary" $'{
}
{}
{ "service": "api" }
{name}
api}
{name
{{
}}
{{}}
{ path: "x y", chars: "$*?[]" }
'

# 3. Triple-quoted strings.
triple=$(write_fixture triple_braces <<'DS'
let name = "api"
let body = """
server {{
  name = "{name}"
}}
"""
printf "%s" "{body}"
let literal = """
{{}}
{{name}}
"""
printf "%s" "{literal}"
DS
)
run_parity triple_braces "$triple" $'
server {
  name = "api"
}

{}
{name}
'

triple_bad=$(write_fixture triple_unclosed <<'DS'
let bad = """
hello {name
"""
touch should-not-exist
DS
)
assert_rejected triple_unclosed "$triple_bad" 'unclosed interpolation'

# 4. Literal braces in expression positions.
function_return=$(write_fixture function_return_braces <<'DS'
fn render(name = "") {
  return "function {name}() {{ return 0; }}"
}
let rendered = render("main")
echo "{rendered}"
DS
)
run_parity function_return_braces "$function_return" $'function main() { return 0; }
'

collections=$(write_fixture collection_braces <<'DS'
let items = ["{{", "}}", "{{x}}"]
let map = { open: "{{", close: "}}", expr: "{{name}}" }
echo "{items[0]}"
echo "{items[1]}"
echo "{items[2]}"
echo "{map["open"]}"
echo "{map["close"]}"
echo "{map["expr"]}"
DS
)
run_parity collection_braces "$collections" $'{
}
{x}
{
}
{name}
'

cat >"$FIX/brace_lib.ds" <<'DS'
fn make_block(name = "") {
  return "block {name} {{ ok }}"
}
DS
import_boundary=$(write_fixture import_boundary_braces <<DS
import "$FIX/brace_lib.ds"
let rendered = make_block("api")
echo "{rendered}"
DS
)
run_parity import_boundary_braces "$import_boundary" $'block api { ok }
'

command_words=$(write_fixture command_words_braces <<'DS'
let name = "api"
printf "%s\n" "{{{name}}}"
DS
)
run_parity command_words_braces "$command_words" $'{api}
'

# 5. Diagnostics and parser/lowerer behavior.
lone_open=$(write_fixture lone_open <<'DS'
echo "hello {"
touch should-not-exist
DS
)
assert_rejected lone_open "$lone_open" 'unclosed interpolation'

unclosed_interp=$(write_fixture unclosed_interp <<'DS'
let name = "api"
echo "hello {name"
touch should-not-exist
DS
)
assert_rejected unclosed_interp "$unclosed_interp" 'unclosed interpolation'

lone_close=$(write_fixture lone_close <<'DS'
echo "hello }"
touch should-not-exist
DS
)
assert_rejected lone_close "$lone_close" 'unmatched `}`'

ambiguous_close_after_open=$(write_fixture ambiguous_close_after_open <<'DS'
echo "{{name}"
touch should-not-exist
DS
)
assert_rejected ambiguous_close_after_open "$ambiguous_close_after_open" 'unmatched `}`'

single_close_after_interp=$(write_fixture single_close_after_interp <<'DS'
let name = "api"
echo "{name}}"
touch should-not-exist
DS
)
assert_rejected single_close_after_interp "$single_close_after_interp" 'unmatched `}`'

malformed_expr=$(write_fixture malformed_expr <<'DS'
echo "value {1 + }"
touch should-not-exist
DS
)
assert_rejected malformed_expr "$malformed_expr" 'invalid arithmetic interpolation'

structured_array=$(write_fixture structured_array <<'DS'
let xs = ["a"]
echo "xs={xs}"
DS
)
assert_rejected structured_array "$structured_array" 'cannot interpolate array value'
structured_map=$(write_fixture structured_map <<'DS'
let m = { a: 1 }
echo "m={m}"
DS
)
assert_rejected structured_map "$structured_map" 'cannot interpolate map value'
structured_command=$(write_fixture structured_command <<'DS'
let result = run printf ok
echo "r={result}"
DS
)
assert_rejected structured_command "$structured_command" 'cannot interpolate command-result value'

# 6. Formatter and debug output.
fmt_fixture=$(write_fixture fmt_braces <<'DS'
let name = "api"
echo "{{ \"service\": \"{name}\" }}"
echo "{{name}}"
DS
)
run_ok fmt_braces "$DS" fmt "$fmt_fixture"
assert_contains "$TMP/fmt_braces.out" '{{' 'formatter preserves doubled open brace spelling'
assert_contains "$TMP/fmt_braces.out" '}}' 'formatter preserves doubled close brace spelling'
for cmd in tokens ast hir bytecode; do
  run_ok "debug_${cmd}" "$DS" "$cmd" "$fmt_fixture"
  assert_contains "$TMP/debug_${cmd}.out" '{{' "debug $cmd keeps literal-brace spelling visible"
done

# 7. Emitted Bash quoting and standalone behavior.
run_parity_hostile hostile_brace_output "$ordinary" $'{
}
{}
{ "service": "api" }
{name}
api}
{name
{{
}}
{{}}
{ path: "x y", chars: "$*?[]" }
'
plain_no_helper=$(write_fixture no_broken_pipe_helper_needed <<'DS'
echo "{{ok}}"
DS
)
plain_script="$TMP/no_broken_pipe_helper_needed.sh"
emit_checked no_broken_pipe_helper_needed "$plain_no_helper" "$plain_script"
assert_contains "$plain_script" '__ds_is_quiet_broken_pipe' 'command Bash includes tiny shared broken-pipe helper'
assert_contains "$plain_script" '__ds_stdout_is_pipe_like' 'command Bash includes pipe-detection helper'
helper_script="$TMP/broken_pipe_helper_needed.sh"
emit_checked broken_pipe_helper_needed "$ordinary" "$helper_script"
assert_no_ds_call "$helper_script" 'brace script helper body'

# 8. Broken-pipe quieting: common top-level output.
loop_lines=$(write_fixture pipe_loop_lines <<'DS'
for n in 1..1000 {
  echo "line {n}"
}
DS
)
run_head_both pipe_loop_lines "$loop_lines" $'line 1
'

large_output=$(write_fixture pipe_large_output <<'DS'
for n in 1..2000 {
  printf "%s\n" "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx {n}"
}
DS
)
run_head_both pipe_large_output "$large_output" $'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx 1
'

function_output=$(write_fixture pipe_function_output <<'DS'
fn write_lines() {
  for n in 1..1000 {
    echo "line {n}"
  }
}
write_lines()
DS
)
run_head_both pipe_function_output "$function_output" $'line 1
'

cat >"$FIX/pipe_import_lib.ds" <<'DS'
fn write_lines() {
  for n in 1..1000 {
    echo "line {n}"
  }
}
DS
imported_output=$(write_fixture pipe_imported_output <<DS
import "$FIX/pipe_import_lib.ds"
write_lines()
DS
)
run_head_both pipe_imported_output "$imported_output" $'line 1
'

cleanup_output=$(write_fixture pipe_cleanup <<'DS'
defer {
  file.write("cleaned", "yes")
}
for n in 1..1000 {
  echo "line {n}"
}
DS
)
run_ok pipe_cleanup_check "$DS" check "$cleanup_output"
cleanup_script="$TMP/pipe_cleanup.sh"
emit_checked pipe_cleanup "$cleanup_output" "$cleanup_script"
for mode in vm bash; do
  work="$TMP/pipe_cleanup_${mode}_work"
  rm -rf "$work"; mkdir -p "$work"
  set +e
  if [ "$mode" = vm ]; then
    (cd "$work" && timeout "$PIPE_TIMEOUT" bash -c 'set -o pipefail; "$1" run "$2" | head -n 1' bash "$DS" "$cleanup_output") >"$TMP/pipe_cleanup_${mode}.out" 2>"$TMP/pipe_cleanup_${mode}.err"
  else
    (cd "$work" && timeout "$PIPE_TIMEOUT" bash -c 'set -o pipefail; bash "$1" | head -n 1' bash "$cleanup_script") >"$TMP/pipe_cleanup_${mode}.out" 2>"$TMP/pipe_cleanup_${mode}.err"
  fi
  rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/pipe_cleanup_${mode}.rc"
  assert_status "pipe_cleanup_${mode}" 0
  assert_text "pipe_cleanup_${mode}_stdout" $'line 1
' "$TMP/pipe_cleanup_${mode}.out"
  [ -f "$work/cleaned" ] || fail "pipe_cleanup_${mode}: cleanup marker missing"
  pass "pipe_cleanup_${mode}: cleanup marker exists"
done

# 9. Broken-pipe quieting: unrelated failures stay visible.
ordinary_false=$(write_fixture ordinary_false <<'DS'
false
echo "after"
DS
)
assert_runtime_failure ordinary_false "$ordinary_false" 'exit 1'

command_not_found=$(write_fixture command_not_found <<'DS'
missing-command-for-v034
DS
)
assert_runtime_failure command_not_found "$command_not_found" 'failed to launch command' 'exit 127'

perm_dir="$TMP/perm"
mkdir -p "$perm_dir"
printf '#!/bin/sh\nexit 0\n' >"$perm_dir/noexec"
chmod 0644 "$perm_dir/noexec"
perm_denied=$(write_fixture permission_denied <<DS
sh -c "$perm_dir/noexec"
DS
)
if [ -x "$perm_dir/noexec" ]; then
  pass 'permission_denied skipped: filesystem treats file as executable'
else
  assert_runtime_failure permission_denied "$perm_denied" 'exit 126'
fi

invalid_regex=$(write_fixture invalid_regex <<'DS'
let m = regex.match("abc", /[/)
echo m.matched
DS
)
assert_rejected invalid_regex "$invalid_regex" 'invalid regex pattern'
invalid_glob=$(write_fixture invalid_glob <<'DS'
let files = glob!("missing/*.ds")
echo files[0]
DS
)
assert_runtime_failure invalid_glob "$invalid_glob" 'glob!' 'required glob'

# 10. Broken-pipe command-result capture.
captured_status=$(write_fixture captured_status <<'DS'
let result = run sh -c "exit 141"
echo result.code
echo result.failed
DS
)
run_parity captured_status "$captured_status" $'141
true
'

captured_false=$(write_fixture captured_false <<'DS'
let result = run false
echo result.code
echo result.failed
echo result.ok
DS
)
run_parity captured_false "$captured_false" $'1
true
false
'

# 11. Pipeline and signal boundary cases.
pipeline_failure=$(write_fixture pipeline_failure <<'DS'
false | cat
echo after
DS
)
assert_runtime_failure pipeline_failure "$pipeline_failure" 'pipeline failed'

explicit_141_file=$(write_fixture explicit_141_file <<'DS'
sh -c "exit 141"
echo after
DS
)
# VM must keep explicit exit 141 visible; emitted Bash must keep it visible when stdout is a regular file.
assert_runtime_failure explicit_141_file "$explicit_141_file" '141'

redirected_141_pipe=$(write_fixture redirected_141_pipe <<'DS'
sh -c "exit 141" |> "out.txt"
echo after
DS
)
assert_piped_runtime_failure redirected_141_pipe "$redirected_141_pipe" '141'

redirected_pipeline_141_pipe=$(write_fixture redirected_pipeline_141_pipe <<'DS'
sh -c "exit 141" | cat |> "out.txt"
echo after
DS
)
assert_piped_runtime_failure redirected_pipeline_141_pipe "$redirected_pipeline_141_pipe" '141'

cleanup_redirected_pipeline_141_pipe=$(write_fixture cleanup_redirected_pipeline_141_pipe <<'DS'
defer {
  file.write("cleaned", "yes")
}
sh -c "exit 141" | cat |> "out.txt"
echo after
DS
)
assert_piped_runtime_failure cleanup_redirected_pipeline_141_pipe "$cleanup_redirected_pipeline_141_pipe" '141' cleaned

# 12. Regression and examples smoke from the v0.34 suite.
examples=(examples/basic.ds)
for example in "${examples[@]}"; do
  example_abs="$ROOT/$example"
  run_ok "example_check_${example//[^A-Za-z0-9_]/_}" "$DS" check "$example"
  run_ok "example_emit_${example//[^A-Za-z0-9_]/_}" "$DS" emit bash "$example" -o "$TMP/${example//[^A-Za-z0-9_]/_}.sh"
  run_ok "example_bash_n_${example//[^A-Za-z0-9_]/_}" bash -n "$TMP/${example//[^A-Za-z0-9_]/_}.sh"
  vm_work="$TMP/example_${example//[^A-Za-z0-9_]/_}_vm_work"
  bash_work="$TMP/example_${example//[^A-Za-z0-9_]/_}_bash_work"
  rm -rf "$vm_work" "$bash_work"
  mkdir -p "$vm_work" "$bash_work"
  run_ok "example_run_${example//[^A-Za-z0-9_]/_}" bash -c 'cd "$1" && "$2" run "$3"' bash "$vm_work" "$DS" "$example_abs"
  run_ok "example_bash_run_${example//[^A-Za-z0-9_]/_}" bash -c 'cd "$1" && bash "$2"' bash "$bash_work" "$TMP/${example//[^A-Za-z0-9_]/_}.sh"
  assert_same "$TMP/example_run_${example//[^A-Za-z0-9_]/_}.out" "$TMP/example_bash_run_${example//[^A-Za-z0-9_]/_}.out" "example $example VM/Bash stdout parity"
  assert_same "$TMP/example_run_${example//[^A-Za-z0-9_]/_}.err" "$TMP/example_bash_run_${example//[^A-Za-z0-9_]/_}.err" "example $example VM/Bash stderr parity"
done

printf 'ok - v0.34.0 suite completed with %s checks\n' "$pass_count"
