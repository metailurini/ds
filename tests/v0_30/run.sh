#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
DS="$ROOT/ds"
TMP=${TMPDIR:-/tmp}/ds_v0_30_tests.$$
FIX="$TMP/fixtures"
mkdir -p "$FIX"
trap 'rm -rf "$TMP"' EXIT

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != "1" ]]; then
  make -C "$ROOT" >/dev/null
fi

assert_text() {
  local name="$1" expected="$2" actual_file="$3"
  local expected_file="$TMP/${name}.expected"
  printf '%s' "$expected" >"$expected_file"
  assert_same "$expected_file" "$actual_file" "$name"
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
    assert_contains "$TMP/${name}_vm.err" 'error:' "$name VM diagnostic has error marker"
    assert_contains "$TMP/${name}_bash.err" 'error:' "$name Bash diagnostic has error marker"
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
  if [ -n "$marker" ]; then
    [ ! -e "$marker" ] || fail "$name created side-effect marker during check"
    pass "$name no side effects during check"
  fi
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

# 2. Basic array assignment parity.
array_first=$(write_fixture array_replace_first <<'DS'
let items = ["old", "keep"]
items[0] = "new"
let first = items[0]
let second = items[1]
echo $first
echo $second
DS
)
run_parity array_replace_first "$array_first" $'new\nkeep\n'

array_last=$(write_fixture array_replace_last <<'DS'
let items = ["a", "b", "c"]
items[2] = "z"
for item in items {
  echo $item
}
DS
)
run_parity array_replace_last "$array_last" $'a\nb\nz\n'

array_named_index=$(write_fixture array_named_index <<'DS'
let items = ["a", "b", "c"]
let i = 1
items[i] = "B"
let got = items[1]
echo $got
DS
)
run_parity array_named_index "$array_named_index" $'B\n'

array_int_rhs=$(write_fixture array_int_rhs <<'DS'
let nums = [1, 2, 3]
nums[1] = 40 + 2
if nums[1] == 42 {
  echo "{nums[1]}"
} else {
  echo "wrong"
}
DS
)
run_parity array_int_rhs "$array_int_rhs" $'42\n'

array_bool_rhs=$(write_fixture array_bool_rhs <<'DS'
let flags = [false, false]
flags[1] = true
if flags[1] {
  echo "enabled"
} else {
  echo "disabled"
}
DS
)
run_parity array_bool_rhs "$array_bool_rhs" $'enabled\n'

# 3. Basic map assignment parity.
map_replace=$(write_fixture map_replace <<'DS'
let ports = { api: 3000, web: 5173 }
ports["api"] = 3001
let api = ports["api"]
let web = ports["web"]
echo $api
echo $web
DS
)
run_parity map_replace "$map_replace" $'3001\n5173\n'

map_insert=$(write_fixture map_insert <<'DS'
let ports = { api: 3000 }
ports["web"] = 5173
let api = ports["api"]
let web = ports["web"]
echo $api
echo $web
DS
)
run_parity map_insert "$map_insert" $'3000\n5173\n'
assert_contains "$TMP/map_insert.sh" 'declare -A' 'map insertion emits associative array support'
assert_contains "$TMP/map_insert.sh" '__ds_value_type_ports' 'map insertion emits value sidecar metadata'

map_named_key=$(write_fixture map_named_key <<'DS'
let ports = { api: 3000 }
let name = "worker"
ports[name] = 9000
let worker = ports["worker"]
echo $worker
DS
)
run_parity map_named_key "$map_named_key" $'9000\n'

map_iteration_after_mutation=$(write_fixture map_iteration_after_mutation <<'DS'
let ports = { web: 5173 }
ports["api"] = 3000
ports["worker"] = 9000
for name, port in ports {
  echo "{name}:{port}"
}
DS
)
run_parity map_iteration_after_mutation "$map_iteration_after_mutation" $'api:3000\nweb:5173\nworker:9000\n'

map_bool_replace=$(write_fixture map_bool_replace <<'DS'
let flags = { fast: false }
flags["fast"] = true
if flags["fast"] {
  echo "on"
}
DS
)
run_parity map_bool_replace "$map_bool_replace" $'on\n'

# 4. Mutation composition.
while_assignment=$(write_fixture while_assignment <<'DS'
let items = ["a", "b", "c"]
let i = 0
while i < 3 {
  items[i] = "x"
  i += 1
}
for item in items {
  echo $item
}
DS
)
run_parity while_assignment "$while_assignment" $'x\nx\nx\n'

loop_separate_target=$(write_fixture loop_separate_target <<'DS'
let items = ["a", "b"]
let out = ["", ""]
let i = 0
for item in items {
  out[i] = item.upper()
  i += 1
}
for value in out {
  echo $value
}
DS
)
run_parity loop_separate_target "$loop_separate_target" $'A\nB\n'

function_return_mutation=$(write_fixture function_return_mutation <<'DS'
fn build() {
  let xs = ["a", "b"]
  xs[1] = "B"
  return xs
}

let result = build()
let second = result[1]
echo $second
DS
)
run_parity function_return_mutation "$function_return_mutation" $'B\n'

no_aliasing=$(write_fixture no_aliasing <<'DS'
let a = ["x", "y"]
let b = ["x", "y"]
a[0] = "A"
let first_a = a[0]
let first_b = b[0]
echo $first_a
echo $first_b
DS
)
run_parity no_aliasing "$no_aliasing" $'A\nx\n'

array_copy_no_aliasing=$(write_fixture array_copy_no_aliasing <<'DS'
let a = ["x", "y"]
let b = a
b[0] = "B"
echo "{a[0]}"
echo "{b[0]}"
DS
)
run_parity array_copy_no_aliasing "$array_copy_no_aliasing" $'x\nB\n'

map_copy_no_aliasing=$(write_fixture map_copy_no_aliasing <<'DS'
let a = { x: 1 }
let b = a
b["y"] = 2
for key, value in a {
  echo "a:{key}:{value}"
}
for key, value in b {
  echo "b:{key}:{value}"
}
DS
)
run_parity map_copy_no_aliasing "$map_copy_no_aliasing" $'a:x:1\nb:x:1\nb:y:2\n'

collection_copy_sidecars=$(write_fixture collection_copy_sidecars <<'DS'
let a = [1, false]
let b = a
b[1] = true
if a[1] {
  echo "wrong-a"
} else {
  echo "a-false"
}
if b[1] {
  echo "b-true"
}
DS
)
run_parity collection_copy_sidecars "$collection_copy_sidecars" $'a-false\nb-true\n'

interpolation_after_mutation=$(write_fixture interpolation_after_mutation <<'DS'
let xs = ["old"]
xs[0] = "new"
echo "value={xs[0]}"
DS
)
run_parity interpolation_after_mutation "$interpolation_after_mutation" $'value=new\n'

case_after_dynamic_mutation=$(write_fixture case_after_dynamic_mutation <<'DS'
let values = ["seed"]
let flags = { yes: true }
values[0] = flags["yes"]
case values[0] {
  true { echo "bool" }
  _ { echo "wrong" }
}
DS
)
run_parity case_after_dynamic_mutation "$case_after_dynamic_mutation" $'bool\n'

# 5. Bounds and runtime-data failures.
oob_positive=$(write_fixture oob_positive <<'DS'
let xs = ["a", "b"]
xs[2] = "c"
echo "unreachable"
DS
)
run_parity oob_positive "$oob_positive" '' 1
assert_not_contains "$TMP/oob_positive_vm.out" 'unreachable' 'VM positive OOB stops before later command'
assert_not_contains "$TMP/oob_positive_bash.out" 'unreachable' 'Bash positive OOB stops before later command'
assert_contains "$TMP/oob_positive_vm.err" 'array index 2 out of range' 'VM positive OOB diagnostic'
assert_contains "$TMP/oob_positive_bash.err" 'array index 2 out of range' 'Bash positive OOB diagnostic'

negative_index=$(write_fixture negative_index <<'DS'
let xs = ["a"]
xs[-1] = "x"
DS
)
assert_rejected negative_index "$negative_index" 'array index assignment requires a non-negative index'

append_by_assignment=$(write_fixture append_by_assignment <<'DS'
let xs = ["a", "b"]
xs[2] = "c"
DS
)
run_parity append_by_assignment "$append_by_assignment" '' 1
assert_contains "$TMP/append_by_assignment_vm.err" 'array index 2 out of range' 'append by assignment VM diagnostic'
assert_contains "$TMP/append_by_assignment_bash.err" 'array index 2 out of range' 'append by assignment Bash diagnostic'

dynamic_negative_index=$(write_fixture dynamic_negative_index <<'DS'
let xs = ["a"]
let i = -1
xs[i] = "x"
echo "unreachable"
DS
)
run_parity dynamic_negative_index "$dynamic_negative_index" '' 1
assert_contains "$TMP/dynamic_negative_index_vm.err" 'array index -1 out of range' 'dynamic negative index VM diagnostic'
assert_contains "$TMP/dynamic_negative_index_bash.err" 'array index -1 out of range' 'dynamic negative index Bash diagnostic'
assert_not_contains "$TMP/dynamic_negative_index_vm.out" 'unreachable' 'VM dynamic negative index stops before later command'
assert_not_contains "$TMP/dynamic_negative_index_bash.out" 'unreachable' 'Bash dynamic negative index stops before later command'

dynamic_empty_key=$(write_fixture dynamic_empty_key <<'DS'
let m = { a: 1 }
let key = ""
m[key] = 2
echo "unreachable"
DS
)
run_parity dynamic_empty_key "$dynamic_empty_key" '' 1
assert_contains "$TMP/dynamic_empty_key_vm.err" 'map key must be non-empty' 'dynamic empty key VM diagnostic'
assert_contains "$TMP/dynamic_empty_key_bash.err" 'map key must be non-empty' 'dynamic empty key Bash diagnostic'
assert_not_contains "$TMP/dynamic_empty_key_vm.out" 'unreachable' 'VM dynamic empty key stops before later command'
assert_not_contains "$TMP/dynamic_empty_key_bash.out" 'unreachable' 'Bash dynamic empty key stops before later command'

failed_assignment_no_side_effect=$(write_fixture failed_assignment_no_side_effect <<'DS'
let marker = "marker.txt"
let xs = ["a"]
xs[5] = "bad"
touch $marker
DS
)
run_parity failed_assignment_no_side_effect "$failed_assignment_no_side_effect" '' 1
[ ! -e "$TMP/failed_assignment_no_side_effect_vm_work/marker.txt" ] || fail 'VM created marker after failed assignment'
pass 'VM failed assignment stops before side effect'
[ ! -e "$TMP/failed_assignment_no_side_effect_bash_work/marker.txt" ] || fail 'Bash created marker after failed assignment'
pass 'Bash failed assignment stops before side effect'

# 6. Rejected targets and value kinds.
temp_target=$(write_fixture temp_target <<'DS'
["a", "b"][0] = "x"
DS
)
assert_rejected temp_target "$temp_target" 'index assignment target must be a named flat array or map'

function_result_target=$(write_fixture function_result_target <<'DS'
fn xs() {
  return ["a", "b"]
}
xs()[0] = "x"
DS
)
assert_rejected function_result_target "$function_result_target" 'function-result index assignment is deferred'

command_result_field_target=$(write_fixture command_result_field_target <<'DS'
let r = run echo hi
r.stdout[0] = "x"
DS
)
assert_rejected command_result_field_target "$command_result_field_target" 'command-result fields are not mutable collection targets'

env_field_target=$(write_fixture env_field_target <<'DS'
env.PATH[0] = "x"
DS
)
assert_rejected env_field_target "$env_field_target" 'environment values are scalar strings, not mutable arrays'

nested_target=$(write_fixture nested_target <<'DS'
let xs = [["a"]]
xs[0][0] = "x"
DS
)
assert_rejected nested_target "$nested_target" 'nested collections are deferred'

field_style_map=$(write_fixture field_style_map <<'DS'
let ports = { api: 3000 }
ports.api = 3001
DS
)
assert_rejected field_style_map "$field_style_map" 'field-style map assignment is deferred'

compound_index=$(write_fixture compound_index <<'DS'
let xs = [1]
xs[0] += 1
DS
)
assert_rejected compound_index "$compound_index" 'compound index assignment is unsupported'

rhs_array=$(write_fixture rhs_array <<'DS'
let xs = ["a"]
xs[0] = ["nested"]
DS
)
assert_rejected rhs_array "$rhs_array" 'index assignment value must be a flat scalar'

rhs_map=$(write_fixture rhs_map <<'DS'
let m = { a: 1 }
m["a"] = { nested: 1 }
DS
)
assert_rejected rhs_map "$rhs_map" 'index assignment value must be a flat scalar'

rhs_command_result=$(write_fixture rhs_command_result <<'DS'
let xs = ["a"]
let r = run echo hi
xs[0] = r
DS
)
assert_rejected rhs_command_result "$rhs_command_result" 'nested collections and command results are deferred'

wrong_array_index_kind=$(write_fixture wrong_array_index_kind <<'DS'
let xs = ["a"]
xs["name"] = "x"
DS
)
assert_rejected wrong_array_index_kind "$wrong_array_index_kind" 'array index assignment requires an int index'

wrong_map_key_kind=$(write_fixture wrong_map_key_kind <<'DS'
let m = { a: 1 }
m[0] = 2
DS
)
assert_rejected wrong_map_key_kind "$wrong_map_key_kind" 'map index assignment requires a string key'

missing_target=$(write_fixture missing_target <<'DS'
missing[0] = "x"
DS
)
assert_rejected missing_target "$missing_target" 'index assignment target `missing` is not defined'

param_target=$(write_fixture param_target <<'DS'
fn f(param) {
  param[0] = "x"
}
DS
)
assert_rejected param_target "$param_target" 'index assignment target kind must be a known array or map'

empty_map_key=$(write_fixture empty_map_key <<'DS'
let m = { a: 1 }
m[""] = 2
DS
)
assert_rejected empty_map_key "$empty_map_key" 'empty map keys are deferred'

# 7. Same-map mutation during iteration.
same_map_mutation=$(write_fixture same_map_mutation <<'DS'
let ports = { api: 3000 }
for name, port in ports {
  ports["web"] = 5173
}
DS
)
assert_rejected same_map_mutation "$same_map_mutation" 'mutating the map currently being iterated is unsupported'

shadowed_same_name_map_mutation=$(write_fixture shadowed_same_name_map_mutation <<'DS'
let ports = { api: 3000 }
for name, port in ports {
  let ports = { seed: 1 }
  ports["web"] = 2
  echo "{ports["web"]}"
}
DS
)
run_parity shadowed_same_name_map_mutation "$shadowed_same_name_map_mutation" $'2\n'

different_map_mutation=$(write_fixture different_map_mutation <<'DS'
let src = { api: 3000, web: 5173 }
let dst = { seed: 1 }
for name, port in src {
  dst[name] = port
}
for name, port in dst {
  echo "{name}:{port}"
}
DS
)
run_parity different_map_mutation "$different_map_mutation" $'api:3000\nseed:1\nweb:5173\n'

# 7.1 Control transfer after successful mutation.
mutation_then_break=$(write_fixture mutation_then_break <<'DS'
let xs = ["a", "b"]
for item in xs {
  xs[0] = "B"
  break
}
echo "{xs[0]}"
DS
)
run_parity mutation_then_break "$mutation_then_break" $'B\n'

mutation_then_continue=$(write_fixture mutation_then_continue <<'DS'
let xs = ["a", "b"]
let out = ["", ""]
let i = 0
for item in xs {
  out[i] = item.upper()
  i += 1
  continue
  echo "unreachable"
}
for value in out {
  echo $value
}
DS
)
run_parity mutation_then_continue "$mutation_then_continue" $'A\nB\n'

mutation_then_return=$(write_fixture mutation_then_return <<'DS'
fn pick() {
  let xs = ["a"]
  xs[0] = "A"
  return xs[0]
}

echo "{pick()}"
DS
)
run_parity mutation_then_return "$mutation_then_return" $'A\n'

mutation_then_fail=$(write_fixture mutation_then_fail <<'DS'
let xs = ["a"]
xs[0] = "A"
echo "{xs[0]}"
fail "stop"
echo "unreachable"
DS
)
run_parity mutation_then_fail "$mutation_then_fail" $'A\n' 1
assert_contains "$TMP/mutation_then_fail_vm.err" 'stop' 'VM fail after mutation reports stop'
assert_contains "$TMP/mutation_then_fail_bash.err" 'stop' 'Bash fail after mutation reports stop'
assert_not_contains "$TMP/mutation_then_fail_vm.out" 'unreachable' 'VM fail after mutation stops later command'
assert_not_contains "$TMP/mutation_then_fail_bash.out" 'unreachable' 'Bash fail after mutation stops later command'

mutation_then_exit=$(write_fixture mutation_then_exit <<'DS'
let xs = ["a"]
xs[0] = "A"
echo "{xs[0]}"
exit 7
echo "unreachable"
DS
)
mutation_then_exit_script="$TMP/mutation_then_exit.sh"
run_ok mutation_then_exit_check "$DS" check "$mutation_then_exit"
run_ok mutation_then_exit_emit "$DS" emit bash "$mutation_then_exit" -o "$mutation_then_exit_script"
run_ok mutation_then_exit_bash_n bash -n "$mutation_then_exit_script"
assert_no_ds_call "$mutation_then_exit_script" 'mutation_then_exit emitted Bash standalone'
set +e
"$DS" run "$mutation_then_exit" >"$TMP/mutation_then_exit_vm.out" 2>"$TMP/mutation_then_exit_vm.err"
mutation_then_exit_vm_rc=$?
bash "$mutation_then_exit_script" >"$TMP/mutation_then_exit_bash.out" 2>"$TMP/mutation_then_exit_bash.err"
mutation_then_exit_bash_rc=$?
set -e
printf '%s' "$mutation_then_exit_vm_rc" >"$TMP/mutation_then_exit_vm.rc"
printf '%s' "$mutation_then_exit_bash_rc" >"$TMP/mutation_then_exit_bash.rc"
assert_status mutation_then_exit_vm 7
assert_status mutation_then_exit_bash 7
assert_same "$TMP/mutation_then_exit_vm.out" "$TMP/mutation_then_exit_bash.out" 'mutation_then_exit VM/Bash stdout parity'
assert_text mutation_then_exit_expected_stdout $'A\n' "$TMP/mutation_then_exit_vm.out"
assert_same "$TMP/mutation_then_exit_vm.err" "$TMP/mutation_then_exit_bash.err" 'mutation_then_exit VM/Bash stderr parity'
assert_not_contains "$TMP/mutation_then_exit_vm.out" 'unreachable' 'VM exit after mutation stops later command'
assert_not_contains "$TMP/mutation_then_exit_bash.out" 'unreachable' 'Bash exit after mutation stops later command'

# 8. Formatter and debug-output coverage.
fmt_assignment=$(write_fixture fmt_assignment <<'DS'
let items = ["a"]
items [ 0 ] = "x"
let ports = { api: 3000 }
ports [ "api" ] = 3001
DS
)
run_ok fmt_assignment "$DS" fmt "$fmt_assignment"
assert_contains "$TMP/fmt_assignment.out" 'items[0] = "x"' 'fmt normalizes array index assignment spacing'
assert_contains "$TMP/fmt_assignment.out" 'ports["api"] = 3001' 'fmt normalizes map index assignment spacing'
printf '%s' "$(cat "$TMP/fmt_assignment.out")" >"$TMP/fmt_assignment_once.ds"
run_ok fmt_assignment_idempotent "$DS" fmt "$TMP/fmt_assignment_once.ds"
assert_same "$TMP/fmt_assignment.out" "$TMP/fmt_assignment_idempotent.out" 'fmt index assignment idempotent'

run_ok ast_index_assignment "$DS" ast "$array_first"
assert_contains "$TMP/ast_index_assignment.out" 'IndexAssignStmt' 'AST shows index assignment statement'
assert_contains "$TMP/ast_index_assignment.out" 'IdentExpr items' 'AST shows assignment target base'
assert_contains "$TMP/ast_index_assignment.out" 'IntExpr 0' 'AST shows assignment index'
run_ok hir_index_assignment "$DS" hir "$array_first"
assert_contains "$TMP/hir_index_assignment.out" 'IndexAssign items array[]' 'HIR shows explicit collection mutation statement'
run_ok bytecode_index_assignment "$DS" bytecode "$array_first"
assert_contains "$TMP/bytecode_index_assignment.out" 'SET_INDEX' 'bytecode shows stable index assignment op'

malformed_target=$(write_fixture malformed_target <<'DS'
let xs = ["a"]
xs[] = "x"
DS
)
assert_check_fails malformed_target "$malformed_target" 'expected index expression after `[`'

# 9. Generated Bash helper hygiene.
array_helper_hygiene=$(write_fixture array_helper_hygiene <<'DS'
let xs = ["a"]
xs[0] = "b"
let got = xs[0]
echo $got
DS
)
run_parity array_helper_hygiene "$array_helper_hygiene" $'b\n'
assert_contains "$TMP/array_helper_hygiene.sh" '__ds_array_set()' 'array-only helper emits array setter'
assert_contains "$TMP/array_helper_hygiene.sh" '__ds_array_get()' 'array-only helper emits array getter'
assert_not_contains "$TMP/array_helper_hygiene.sh" 'BASH_VERSINFO' 'array-only helper omits Bash associative-array guard'
assert_not_contains "$TMP/array_helper_hygiene.sh" '__ds_map_set()' 'array-only helper omits map setter'
assert_not_contains "$TMP/array_helper_hygiene.sh" '__ds_map_get()' 'array-only helper omits map getter'
assert_not_contains "$TMP/array_helper_hygiene.sh" '__ds_map_sorted_keys()' 'array-only helper omits map sort helper'

map_helper_hygiene=$(write_fixture map_helper_hygiene <<'DS'
let m = { a: 1 }
m["b"] = 2
let b = m["b"]
echo $b
DS
)
run_parity map_helper_hygiene "$map_helper_hygiene" $'2\n'
assert_contains "$TMP/map_helper_hygiene.sh" 'BASH_VERSINFO' 'map assignment emits Bash associative-array guard'
assert_contains "$TMP/map_helper_hygiene.sh" '__ds_map_set()' 'map assignment emits map setter'
assert_contains "$TMP/map_helper_hygiene.sh" '__ds_map_get()' 'map assignment emits map getter'
assert_not_contains "$TMP/map_helper_hygiene.sh" '__ds_array_set()' 'map-only helper omits array setter'
assert_not_contains "$TMP/map_helper_hygiene.sh" '__ds_array_get()' 'map-only helper omits array getter'

helper_count=$(grep -c '^__ds_map_set()' "$TMP/map_helper_hygiene.sh" || true)
[ "$helper_count" = 1 ] || fail "map helper emitted $helper_count map setters"
pass 'map helper definitions are not duplicated'
assert_contains "$TMP/map_helper_hygiene.sh" '__ds_value_type_m' 'map helper sidecar uses reserved __ds_ name'

interpolation_helpers=$(write_fixture interpolation_helpers <<'DS'
let xs = ["old"]
xs[0] = "new"
echo "{xs[0]}"
DS
)
run_parity interpolation_helpers "$interpolation_helpers" $'new\n'
assert_contains "$TMP/interpolation_helpers.sh" '__ds_array_get()' 'interpolation helper scanning emits array getter'
assert_contains "$TMP/interpolation_helpers.sh" '__ds_array_set()' 'interpolation helper scanning emits array setter'

mkdir -p "$FIX/imported"
cat >"$FIX/imported/lib.ds" <<'DS'
fn build() {
  let xs = ["a"]
  xs[0] = "A"
  return xs
}
DS
imported_assignment=$(write_fixture imported/main <<'DS'
import "./lib.ds"
let out = build()
let first = out[0]
echo $first
DS
)
run_parity imported_assignment "$imported_assignment" $'A\n'
assert_no_ds_call "$TMP/imported_assignment.sh" 'imported assignment bundled Bash'

# 10. Regression target metadata.
assert_contains "$ROOT/Makefile" '0-30' 'Makefile includes v0.30 suite in TEST_VERSIONS'

printf 'v0.30.0 tests passed: %d assertions\n' "$pass_count"
