#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
DS="$ROOT/ds"
TMP=${TMPDIR:-/tmp}/ds_v0_29_tests.$$
FIX="$TMP/fixtures"
mkdir -p "$FIX"
trap 'rm -rf "$TMP"' EXIT

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"
# shellcheck source=tests/lib/build_sources.sh
source "$ROOT/tests/lib/build_sources.sh"

if [[ "${DS_SKIP_BUILD:-0}" != "1" ]]; then
  make -C "$ROOT" >/dev/null
fi

write_fixture() {
  local name="$1"
  local path="$FIX/$name.ds"
  cat >"$path"
  printf '%s' "$path"
}

assert_text() {
  local name="$1" expected="$2" actual_file="$3"
  local expected_file="$TMP/${name}.expected"
  printf '%s' "$expected" >"$expected_file"
  assert_same "$expected_file" "$actual_file" "$name"
}

assert_file_equals() {
  local path="$1" expected="$2" name="$3"
  local expected_file="$TMP/${name//[^A-Za-z0-9_]/_}.expected"
  printf '%s' "$expected" >"$expected_file"
  assert_same "$expected_file" "$path" "$name"
}

assert_no_ds_call() {
  local script="$1" name="$2"
  assert_not_contains "$script" "$ROOT/ds" "$name omits repo ds path"
  assert_not_contains "$script" './ds ' "$name omits ./ds invocation"
  assert_not_contains "$script" ' ds run ' "$name omits ds run invocation"
  assert_not_contains "$script" ' ds emit ' "$name omits ds emit invocation"
}

assert_helper_present() {
  local script="$1" helper="$2" name="$3"
  assert_contains "$script" "$helper" "$name helper present"
}

assert_helper_absent() {
  local script="$1" helper="$2" name="$3"
  assert_not_contains "$script" "$helper" "$name helper absent"
}

assert_helper_count() {
  local script="$1" pattern="$2" expected="$3" name="$4"
  local count
  count=$(grep -c -- "$pattern" "$script" || true)
  [ "$count" = "$expected" ] || fail "$name: expected $expected, got $count"
  pass "$name"
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

run_parity() {
  local name="$1" file="$2" expected_stdout="$3" expected_status="${4:-0}" side_effects="${5:-}"
  local script="$TMP/$name.sh"
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  mkdir -p "$vm_work" "$bash_work"

  run_ok "${name}_check" "$DS" check "$file"
  run_ok "${name}_emit" "$DS" emit bash "$file" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_no_ds_call "$script" "$name emitted Bash standalone"

  set +e
  (cd "$vm_work" && "$DS" run "$file") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && bash "$script") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name VM/Bash stdout parity"
  assert_text "${name}_expected_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name VM/Bash stderr parity"
  else
    assert_contains "$TMP/${name}_vm.err" ': error:' "$name VM diagnostic has source marker"
    assert_contains "$TMP/${name}_bash.err" ': error:' "$name Bash diagnostic has source marker"
  fi

  local rel
  for rel in $side_effects; do
    [ -f "$vm_work/$rel" ] || fail "$name VM missing side-effect $rel"
    [ -f "$bash_work/$rel" ] || fail "$name Bash missing side-effect $rel"
    assert_same "$vm_work/$rel" "$bash_work/$rel" "$name side-effect parity $rel"
  done
}

assert_check_fails() {
  local name="$1" file="$2" needle="$3" marker="${4:-}"
  capture_cmd "${name}_check" "$DS" check "$file"
  assert_nonzero_status "${name}_check"
  assert_contains "$TMP/${name}_check.err" "$needle" "$name check diagnostic"
  if [ -n "$marker" ]; then [ ! -e "$marker" ] || fail "$name created side-effect marker during check"; pass "$name no side effects during check"; fi
}

assert_emit_fails() {
  local name="$1" file="$2" needle="$3"
  local out="$TMP/${name}.sh"
  capture_cmd "${name}_emit" "$DS" emit bash "$file" -o "$out"
  assert_nonzero_status "${name}_emit"
  assert_contains "$TMP/${name}_emit.err" "$needle" "$name emit diagnostic"
  assert_file_missing_or_empty "$out" "$name no valid emitted artifact"
}

assert_rejected() {
  local name="$1" file="$2" needle="$3" marker="${4:-}"
  assert_check_fails "$name" "$file" "$needle" "$marker"
  assert_emit_fails "$name" "$file" "$needle"
}

assert_repo_doc_contains() {
  local needle="$1" name="$2"
  grep -R -F -- "$needle" docs README.md >/dev/null || fail "$name: docs should contain [$needle]"
  pass "$name"
}

# 1. Planning and docs checks.
for doc in \
  docs/milestones/v0.29.0-spec.md \
  docs/milestones/v0.29.0-test-plan.md \
  docs/roadmap.md \
  docs/status.md \
  docs/language.ds \
  docs/runtime.md \
  docs/parity-contracts.md \
  docs/diagnostics.md \
  docs/concept-map.md \
  docs/source-map.md; do
  [ -f "$doc" ] || fail "missing required doc $doc"
  pass "required doc exists: $doc"
done
assert_repo_doc_contains 'for key, value in map' 'docs document map-loop syntax'
assert_repo_doc_contains 'v0.29.0 map iteration' 'docs document v0.29 map iteration support'
assert_repo_doc_contains 'ascending bytewise/ASCII' 'docs document bytewise ASCII map-loop order'
assert_repo_doc_contains 'scoped to the loop body' 'docs document map loop body scoping'
assert_repo_doc_contains 'scalar value kind' 'docs document map value-kind preservation'
assert_repo_doc_contains 'zero iterations' 'docs document empty-map zero-iteration behavior'
assert_repo_doc_contains 'generated Bash is standalone' 'docs document standalone generated Bash'
assert_repo_doc_contains 'index assignment' 'docs keep index assignment deferred'
assert_repo_doc_contains 'nested collections' 'docs keep nested collections deferred'
assert_repo_doc_contains 'custom iterator' 'docs keep custom iterators deferred'
assert_repo_doc_contains 'command-result values are not maps' 'docs keep command-result map iteration deferred'
assert_repo_doc_contains 'environment iteration' 'docs keep environment iteration deferred'
assert_repo_doc_contains 'recursive glob' 'docs keep recursive glob deferred'
assert_repo_doc_contains 'regex captures' 'docs keep advanced regex deferred'

# Low-level empty-map and bytewise-sort coverage where source-level empty map literals remain deferred.
# The unit also builds HIR-level VM and emitted Bash map loops over an empty map
# whose bodies contain unreachable `break`/`continue` control flow.
ds_compile_unit "$ROOT" library "$ROOT/tests/v0_29/unit/map_iteration.c" "$TMP/test_v0_29_map_iteration"
run_ok runtime_empty_and_sorted_keys_unit "$TMP/test_v0_29_map_iteration" "$TMP"
for empty_script in "$TMP/empty_map_break.sh" "$TMP/empty_map_continue.sh"; do
  run_ok "$(basename "$empty_script" .sh)_bash_n" bash -n "$empty_script"
  assert_no_ds_call "$empty_script" "$(basename "$empty_script" .sh) emitted Bash standalone"
  run_ok "$(basename "$empty_script" .sh)_run" bash "$empty_script"
  assert_text "$(basename "$empty_script" .sh)_stdout" '' "$TMP/$(basename "$empty_script" .sh)_run.out"
  assert_text "$(basename "$empty_script" .sh)_stderr" '' "$TMP/$(basename "$empty_script" .sh)_run.err"
done

# 2. Basic map iteration parity.
basic=$(write_fixture basic_literal <<'DS'
let ports = {
  web: 5173,
  api: 3000,
  worker: 9000
}

for name, port in ports {
  echo "{name}:{port}"
}
DS
)
run_parity basic_literal "$basic" $'api:3000\nweb:5173\nworker:9000\n'
assert_helper_present "$TMP/basic_literal.sh" '__ds_map_sorted_keys()' 'basic literal'

strings=$(write_fixture string_values <<'DS'
let labels = {
  b: "beta",
  a: "alpha"
}

for key, label in labels {
  let upper = label.upper()
  echo "{key}={upper}"
}
DS
)
run_parity string_values "$strings" $'a=ALPHA\nb=BETA\n'
assert_helper_present "$TMP/string_values.sh" '__ds_string_upper()' 'string value loop'

booleans=$(write_fixture bool_values <<'DS'
let flags = {
  slow: false,
  fast: true
}

for name, enabled in flags {
  if enabled {
    echo "{name}=on"
  } else {
    echo "{name}=off"
  }
}
DS
)
run_parity bool_values "$booleans" $'fast=on\nslow=off\n'

ints=$(write_fixture int_values <<'DS'
let ports = {
  web: 5173,
  api: 3000
}

for name, port in ports {
  let next = port + 1
  echo "{name}:{next}"
}
DS
)
run_parity int_values "$ints" $'api:3001\nweb:5174\n'

# 3. Ordering edge cases.
numeric_keys=$(write_fixture numeric_keys <<'DS'
let values = {
  "2": "two",
  "10": "ten",
  "1": "one"
}

for key, value in values {
  echo "{key}:{value}"
}
DS
)
run_parity numeric_keys "$numeric_keys" $'1:one\n10:ten\n2:two\n'

case_keys=$(write_fixture case_keys <<'DS'
let values = {
  b: "lower-b",
  A: "upper-a",
  a: "lower-a",
  B: "upper-b"
}

for key, value in values {
  echo "{key}:{value}"
}
DS
)
run_parity case_keys "$case_keys" $'A:upper-a\nB:upper-b\na:lower-a\nb:lower-b\n'

punct_keys=$(write_fixture punct_keys <<'DS'
let values = {
  "z": "zed",
  "a-b": "dash",
  "a_b": "underscore",
  "a.b": "dot"
}

for key, value in values {
  echo "{key}:{value}"
}
DS
)
run_parity punct_keys "$punct_keys" $'a-b:dash\na.b:dot\na_b:underscore\nz:zed\n'

shell_sensitive_keys=$(write_fixture shell_sensitive_keys <<'DS'
let values = {
  "a b": "space",
  "a\"q": "quote",
  "a$d": "dollar",
  "a\\b": "slash",
  "a[0]": "bracket"
}

for key, value in values {
  echo "{key}:{value}"
}
DS
)
run_parity shell_sensitive_keys "$shell_sensitive_keys" $'a b:space\na"q:quote\na$d:dollar\na[0]:bracket\na\\b:slash\n'
assert_contains "$TMP/shell_sensitive_keys.sh" "['a b']=\"space\"" 'shell-sensitive space key remains quoted in Bash'
assert_contains "$TMP/shell_sensitive_keys.sh" "['a\$d']=\"dollar\"" 'shell-sensitive dollar key remains quoted in Bash'
assert_contains "$TMP/shell_sensitive_keys.sh" "['a[0]']=\"bracket\"" 'shell-sensitive bracket key remains quoted in Bash'

duplicate_key=$(write_fixture duplicate_key <<'DS'
let values = {
  api: 1,
  api: 2
}

for key, value in values {
  echo "{key}:{value}"
}
DS
)
assert_rejected duplicate_key "$duplicate_key" 'duplicate map key `api`'

# 4. Function-returned maps.
direct_return=$(write_fixture direct_return <<'DS'
fn ports() {
  return { web: 5173, api: 3000 }
}

for name, port in ports() {
  echo "{name}:{port}"
}
DS
)
run_parity direct_return "$direct_return" $'api:3000\nweb:5173\n'
assert_helper_present "$TMP/direct_return.sh" '__ds_call_value_into' 'direct return map loop'

forwarded_return=$(write_fixture forwarded_return <<'DS'
fn base() {
  return { web: 5173, api: 3000 }
}

fn forwarded() {
  return base()
}

for name, port in forwarded() {
  echo "{name}:{port}"
}
DS
)
run_parity forwarded_return "$forwarded_return" $'api:3000\nweb:5173\n'

call_once=$(write_fixture call_once <<'DS'
let calls = 0

fn data() {
  calls += 1
  return { b: 2, a: 1 }
}

for key, value in data() {
  echo "{key}:{value}"
}

echo "calls={calls}"
DS
)
run_parity call_once "$call_once" $'a:1\nb:2\ncalls=1\n'

# 5. Scope and shadowing.
leak=$(write_fixture loop_vars_do_not_leak <<'DS'
let ports = { api: 3000 }

for name, port in ports {
  echo "inside={name}:{port}"
}

echo "{name}"
DS
)
assert_rejected loop_vars_do_not_leak "$leak" 'unknown interpolation variable `name`'

shadow=$(write_fixture shadow_outer <<'DS'
let name = "outer"
let port = 1
let ports = { api: 3000 }

for name, port in ports {
  echo "inside={name}:{port}"
}

echo "after={name}:{port}"
DS
)
run_parity shadow_outer "$shadow" $'inside=api:3000\nafter=outer:1\n'

reassign=$(write_fixture reassign_loop_value <<'DS'
let ports = { api: 3000, web: 5173 }

for name, port in ports {
  port += 1
  echo "loop={name}:{port}"
}

let after_api = ports.api
let after_web = ports.web
echo "after={after_api}:{after_web}"
DS
)
run_parity reassign_loop_value "$reassign" $'loop=api:3001\nloop=web:5174\nafter=3000:5173\n'

duplicate_loop_names=$(write_fixture duplicate_loop_names <<'DS'
let ports = { api: 3000 }
for key, key in ports {
  echo key
}
DS
)
assert_rejected duplicate_loop_names "$duplicate_loop_names" 'map loop key and value variables must be different'

# 6. Control flow inside map loops.
continues=$(write_fixture continue_loop <<'DS'
let ports = { api: 3000, web: 5173, worker: 9000 }

for name, port in ports {
  if name == "web" { continue }
  echo "{name}:{port}"
}
DS
)
run_parity continue_loop "$continues" $'api:3000\nworker:9000\n'

breaks=$(write_fixture break_loop <<'DS'
let ports = { api: 3000, web: 5173, worker: 9000 }

for name, port in ports {
  if name == "web" { break }
  echo "{name}:{port}"
}

echo done
DS
)
run_parity break_loop "$breaks" $'api:3000\ndone\n'

nested=$(write_fixture nested_break <<'DS'
let outer = { b: 2, a: 1 }
let inner = { y: 20, x: 10 }

for ok, ov in outer {
  for ik, iv in inner {
    if ik == "y" { break }
    echo "{ok}:{ik}:{ov}:{iv}"
  }
}
DS
)
run_parity nested_break "$nested" $'a:x:1:10\nb:x:2:10\n'

return_loop=$(write_fixture return_from_loop <<'DS'
fn find_port() {
  let ports = { api: 3000, web: 5173 }
  for name, port in ports {
    if name == "web" { return port }
  }
  return 0
}

echo "port={find_port()}"
DS
)
run_parity return_from_loop "$return_loop" $'port=5173\n'

# 7. Interaction with existing expressions.
key_string_ops=$(write_fixture key_string_ops <<'DS'
let ports = { api: 3000 }

for name, port in ports {
  let upper = name.upper()
  echo "{upper}"
}
DS
)
run_parity key_string_ops "$key_string_ops" $'API\n'

mixed_case=$(write_fixture mixed_case <<'DS'
let values = { a: 1, b: "1", c: true }

for key, value in values {
  case value {
    1 { echo "{key}:int" }
    "1" { echo "{key}:string" }
    true { echo "{key}:bool" }
    _ { echo "{key}:other" }
  }
}
DS
)
run_parity mixed_case "$mixed_case" $'a:int\nb:string\nc:bool\n'

membership=$(write_fixture membership <<'DS'
let groups = { a: "api", w: "web" }
let allowed = ["api", "worker"]

for key, value in groups {
  if value in allowed {
    echo "{key}=allowed"
  } else {
    echo "{key}=blocked"
  }
}
DS
)
run_parity membership "$membership" $'a=allowed\nw=blocked\n'

cmd_interp=$(write_fixture command_interpolation <<'DS'
let envs = { dev: "local", prod: "remote" }

for name, target in envs {
  echo "{name}:{target}"
}
DS
)
run_parity command_interpolation "$cmd_interp" $'dev:local\nprod:remote\n'

# 8. Empty map behavior: public empty map literal remains deferred; runtime unit above covers zero-key maps.
empty_public=$(write_fixture empty_literal_deferred <<'DS'
let values = {}
for key, value in values {
  echo "unreachable"
}
echo done
DS
)
assert_rejected empty_literal_deferred "$empty_public" 'empty map literals are deferred'

# 9. Imports, tests, and functions.
cat >"$FIX/lib.ds" <<'DS'
fn ports() {
  return { web: 5173, api: 3000 }
}
DS
import_main=$(write_fixture imported_map_return <<'DS'
import "./lib.ds"

for name, port in ports() {
  echo "{name}:{port}"
}
DS
)
run_parity imported_map_return "$import_main" $'api:3000\nweb:5173\n'

map_test=$(write_fixture map_loop_test_block <<'DS'
test "map loop" {
  let values = { b: 2, a: 1 }
  for key, value in values {
    echo "{key}:{value}"
  }
}
DS
)
run_ok map_loop_test_block "$DS" test "$map_test"
assert_contains "$TMP/map_loop_test_block.out" 'map loop' 'test output names map loop'
assert_contains "$TMP/map_loop_test_block.out" 'passed' 'test block passes'

function_stmt=$(write_fixture function_statement <<'DS'
fn show() {
  let values = { b: 2, a: 1 }
  for key, value in values {
    echo "{key}:{value}"
  }
}

show()
DS
)
run_parity function_statement "$function_stmt" $'a:1\nb:2\n'

# 10. Generated Bash helper hygiene.
assert_contains "$TMP/basic_literal.sh" 'BASH_VERSINFO[0] < 4' 'map emitted Bash has Bash 4 guard'
assert_helper_count "$TMP/basic_literal.sh" '^__ds_map_sorted_keys()' 1 'map sorting helper emitted exactly once'
assert_contains "$TMP/basic_literal.sh" 'local LC_ALL=C' 'map sorting helper controls locale'
assert_contains "$TMP/basic_literal.sh" '${!__ds_' 'map loop uses associative array key expansion'
array_only=$(write_fixture array_only <<'DS'
let values = ["b", "a"]
for value in values {
  echo "{value}"
}
DS
)
run_ok array_only_emit "$DS" emit bash "$array_only" -o "$TMP/array_only.sh"
run_ok array_only_bash_n bash -n "$TMP/array_only.sh"
assert_helper_absent "$TMP/array_only.sh" '__ds_map_sorted_keys()' 'array-only script'
assert_not_contains "$TMP/array_only.sh" 'BASH_VERSINFO[0] < 4' 'array-only script does not need map guard'
helper_recursion=$(write_fixture helper_recursion <<'DS'
let values = { b: "beta", a: " alpha " }

for key, value in values {
  let cleaned = value.trim().upper()
  echo "{key}:{cleaned}"
}
DS
)
run_parity helper_recursion "$helper_recursion" $'a:ALPHA\nb:BETA\n'
assert_helper_present "$TMP/helper_recursion.sh" '__ds_string_trim()' 'helper recursion'
assert_helper_present "$TMP/helper_recursion.sh" '__ds_string_upper()' 'helper recursion'
assert_contains "$TMP/nested_break.sh" '__ds___map_iter_0' 'nested map loop first temp unique'
assert_contains "$TMP/nested_break.sh" '__ds___map_iter_1' 'nested map loop second temp unique'

# 11. Formatter/debug output.
fmt_fixture="$FIX/fmt_map_loop.ds"
cat >"$fmt_fixture" <<'DS'
let ports={api:3000,web:5173}
for  name ,  port   in   ports{echo "{name}:{port}"}
DS
capture_cmd fmt_check "$DS" fmt "$fmt_fixture" --check
assert_nonzero_status fmt_check
assert_contains "$TMP/fmt_check.err" 'needs formatting' 'fmt --check reports needed formatting'
run_ok fmt_write "$DS" fmt "$fmt_fixture" --write
assert_contains "$fmt_fixture" 'for name, port in ports {' 'fmt preserves two-name map loop header'
cp "$fmt_fixture" "$TMP/fmt_once.ds"
run_ok fmt_second "$DS" fmt "$fmt_fixture" --check
assert_same "$TMP/fmt_once.ds" "$fmt_fixture" 'fmt idempotent after write'

debug_fixture=$(write_fixture debug_outputs <<'DS'
let ports = { api: 3000 }
for name, port in ports { echo "{name}:{port}" }
DS
)
run_ok debug_ast "$DS" ast "$debug_fixture"
assert_contains "$TMP/debug_ast.out" 'ForStmt name, port' 'AST shows both map-loop variables'
run_ok debug_hir "$DS" hir "$debug_fixture"
assert_contains "$TMP/debug_hir.out" 'ForMap name, port in' 'HIR shows map loop shape'
assert_contains "$TMP/debug_hir.out" 'Ident ports' 'HIR shows iterable'
run_ok debug_bytecode "$DS" bytecode "$debug_fixture"
assert_contains "$TMP/debug_bytecode.out" 'FOR_MAP        name, port' 'bytecode shows map loop instruction'
assert_not_contains "$TMP/debug_bytecode.out" 'worker' 'bytecode output does not expose unordered hashmap walk for absent keys'

# 12. Rejection tests: wrong iterable kind.
one_name_map=$(write_fixture reject_one_name_map <<'DS'
let ports = { api: 3000 }
for item in ports {
  echo item
}
DS
)
assert_rejected reject_one_name_map "$one_name_map" 'one-name loop over a map is not supported'

two_name_array=$(write_fixture reject_two_name_array <<'DS'
let items = ["api", "web"]
for index, item in items {
  echo item
}
DS
)
assert_rejected reject_two_name_array "$two_name_array" 'arrays use `for item in array`'

for kind in string int bool; do
  case "$kind" in
    string) value='"not-map"' ;;
    int) value='42' ;;
    bool) value='true' ;;
  esac
  file="$FIX/reject_scalar_${kind}.ds"
  cat >"$file" <<DS
let value = $value
for key, item in value { echo item }
DS
  assert_rejected "reject_scalar_${kind}" "$file" 'two-name map loops require a map iterable'
done

command_result=$(write_fixture reject_command_result <<'DS'
let result = run "printf" "hello"
for key, value in result {
  echo "{key}:{value}"
}
DS
)
assert_rejected reject_command_result "$command_result" 'command-result values are not maps'

env_ns=$(write_fixture reject_env_namespace <<'DS'
for key, value in env {
  echo key
}
DS
)
assert_rejected reject_env_namespace "$env_ns" 'environment iteration is unsupported in v0.29.0; `env` is a namespace, not a map'

env_value=$(write_fixture reject_env_value <<'DS'
for key, value in env.PATH {
  echo value
}
DS
)
assert_rejected reject_env_value "$env_value" 'environment iteration is unsupported in v0.29.0; `env.NAME` is a string value, not a map'

# 13. Rejection tests: malformed headers.
malformed_cases=(
  'for key, in ports { echo key }|expected value loop variable after `,`'
  'for key value in ports { echo key }|expected `in`'
  'for key, value, extra in ports { echo key }|expected `in`'
  'for , value in ports { echo value }|expected loop variable after `for`'
  'for key, value ports { echo value }|expected `in`'
  'for key, value in ports echo value|expected `{`'
)
idx=0
for entry in "${malformed_cases[@]}"; do
  src=${entry%%|*}
  needle=${entry#*|}
  file="$FIX/malformed_$idx.ds"
  printf 'let ports = { api: 3000 }\n%s\n' "$src" >"$file"
  assert_rejected "malformed_$idx" "$file" "$needle"
  idx=$((idx + 1))
done

# 14. Rejection tests: unsupported sources and values.
temporary_map=$(write_fixture reject_temporary_map <<'DS'
for key, value in { api: 3000 } {
  echo "{key}:{value}"
}
DS
)
assert_rejected reject_temporary_map "$temporary_map" 'bind the map to a variable first'

unknown_iterable=$(write_fixture reject_unknown_iterable <<'DS'
fn show(m) {
  for key, value in m {
    echo key
  }
}
DS
)
assert_rejected reject_unknown_iterable "$unknown_iterable" 'map loop iterable kind must be known'

nested_map=$(write_fixture reject_nested_map <<'DS'
let data = { api: { port: 3000 } }
for key, value in data {
  echo key
}
DS
)
assert_rejected reject_nested_map "$nested_map" 'nested collections are deferred'

nested_array=$(write_fixture reject_nested_array <<'DS'
let data = { api: [3000] }
for key, value in data {
  echo key
}
DS
)
assert_rejected reject_nested_array "$nested_array" 'nested collections are deferred'

collection_param=$(write_fixture reject_collection_param <<'DS'
fn show(items) {
  echo "x"
}
let values = { api: 3000 }
show(values)
DS
)
assert_rejected reject_collection_param "$collection_param" 'passing collection values to functions is deferred'

# 15. Side-effect ordering.
side_effects=$(write_fixture side_effect_order <<'DS'
let values = { c: "third", a: "first", b: "second" }

for key, value in values {
  sh "-c" "printf '%s:%s\n' \"$1\" \"$2\" >> out.txt" "sh" "{key}" "{value}"
}
DS
)
run_parity side_effect_order "$side_effects" '' 0 'out.txt'
assert_file_equals "$TMP/side_effect_order_vm_work/out.txt" $'a:first\nb:second\nc:third\n' 'side effects follow sorted order'

failure_stop=$(write_fixture failure_stops <<'DS'
let values = { a: 1, b: 2, c: 3 }

for key, value in values {
  echo "before={key}"
  if key == "b" { fail "stop" }
  echo "after={key}"
}
DS
)
run_parity failure_stops "$failure_stop" $'before=a\nafter=a\nbefore=b\n' 1
assert_contains "$TMP/failure_stops_vm.err" 'stop' 'VM failure reports stop'
assert_contains "$TMP/failure_stops_bash.err" 'stop' 'Bash failure reports stop'
assert_not_contains "$TMP/failure_stops_vm.out" 'before=c' 'VM stops before later key'
assert_not_contains "$TMP/failure_stops_bash.out" 'before=c' 'Bash stops before later key'

# 16. Regression guard against old deferred behavior is the accepted basic_literal case above.

# 17. Aggregate regression expectations local to this suite.
assert_contains Makefile '0-29' 'Makefile wires v0.29 into TEST_VERSIONS'

printf 'v0.29.0 tests passed (%d assertions)\n' "$pass_count"
