#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_26_tests.$$"
FIX="$TMP/fixtures"
mkdir -p "$TMP" "$FIX"
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

assert_diag() {
  local file="$1" fragment="$2" name="$3"
  assert_contains "$file" ': error:' "$name severity"
  assert_contains "$file" "$fragment" "$name text"
  assert_contains "$file" '^' "$name caret"
}

assert_check_fails() {
  local name="$1" fixture="$2" fragment="$3"
  run_fail "${name}_check" "$DS" check "$fixture"
  assert_diag "$TMP/${name}_check.err" "$fragment" "$name check diagnostic"
}

assert_emit_fails() {
  local name="$1" fixture="$2" fragment="$3"
  run_fail "${name}_emit" "$DS" emit bash "$fixture" -o "$TMP/${name}.sh"
  assert_diag "$TMP/${name}_emit.err" "$fragment" "$name emit diagnostic"
  assert_file_missing_or_empty "$TMP/${name}.sh" "$name no partial Bash"
}

assert_parity() {
  local name="$1" fixture="$2" expected_status="$3" expected_stdout="$4"; shift 4
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"

  run_ok "${name}_check" "$DS" check "$fixture"
  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same_text "$expected_stdout" "$TMP/${name}_vm.out" "$name VM stdout"
  assert_same_text "$expected_stdout" "$TMP/${name}_bash.out" "$name Bash stdout"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  else
    pass "$name non-zero stderr checked by focused assertions when required"
  fi
}

assert_parity_same() {
  local name="$1" fixture="$2" expected_status="$3"; shift 3
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"

  run_ok "${name}_check" "$DS" check "$fixture"
  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  else
    pass "$name non-zero stderr checked by focused assertions when required"
  fi
}

assert_parity_file() {
  local name="$1" fixture="$2" expected_status="$3" expected_stdout="$4" rel_file="$5" expected_file_text="$6"; shift 6
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"

  run_ok "${name}_check" "$DS" check "$fixture"
  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same_text "$expected_stdout" "$TMP/${name}_vm.out" "$name VM stdout"
  assert_same_text "$expected_stdout" "$TMP/${name}_bash.out" "$name Bash stdout"
  assert_same_text "$expected_file_text" "$vm_work/$rel_file" "$name VM side effect"
  assert_same_text "$expected_file_text" "$bash_work/$rel_file" "$name Bash side effect"
  assert_same "$vm_work/$rel_file" "$bash_work/$rel_file" "$name side-effect parity"
}

# 1. Flat array function returns.
write_fixture "$FIX/string_array_return.ds" <<'DS'
fn apps() {
  return ["api", "web", "worker"]
}

let names = apps()
let first = names[0]
let second = names[1]
let third = names[2]
echo "{first}"
echo "{second}"
echo "{third}"
DS
assert_parity string_array_return "$FIX/string_array_return.ds" 0 $'api\nweb\nworker\n'
[ "$(grep -c '^__ds_call_value_into()' "$TMP/string_array_return.sh")" = 1 ] || fail 'string_array_return emits call-value helper once'
pass 'string_array_return value helper emitted once'

write_fixture "$FIX/empty_array_return.ds" <<'DS'
fn empty() {
  return []
}

let xs = empty()
echo "ok"
DS
assert_parity empty_array_return "$FIX/empty_array_return.ds" 0 $'ok\n'

write_fixture "$FIX/string_array_tricky.ds" <<'DS'
fn values() {
  return ["", "  api  ", "a b c", "semi;colon", "$(echo bad)", "*?[x]"]
}

let xs = values()
let first = xs[0]
let second = xs[1]
let third = xs[2]
let fourth = xs[3]
let fifth = xs[4]
let sixth = xs[5]
echo "[{first}]"
echo "[{second}]"
echo "[{third}]"
echo "{fourth}"
echo "{fifth}"
echo "{sixth}"
DS
assert_parity string_array_tricky "$FIX/string_array_tricky.ds" 0 $'[]\n[  api  ]\n[a b c]\nsemi;colon\n$(echo bad)\n*?[x]\n'
[ ! -e "$TMP/string_array_tricky_bash_work/bad" ] || fail 'string array metacharacters should not execute'
pass 'string_array_tricky shell metacharacters are literal'

write_fixture "$FIX/string_array_newline.ds" <<'DS'
fn values() {
  return ["head", """one
two""", "tail"]
}

let xs = values()
let first = xs[0]
let second = xs[1]
let third = xs[2]
echo "{first}"
echo "{second}"
echo "{third}"
DS
assert_parity string_array_newline "$FIX/string_array_newline.ds" 0 $'head\none\ntwo\ntail\n'

write_fixture "$FIX/int_array_return.ds" <<'DS'
fn nums() {
  return [1, 2, 3]
}

let xs = nums()
let first = xs[0]
let second = xs[1]
let third = xs[2]
let total = first + second + third
echo "{total}"
DS
assert_parity int_array_return "$FIX/int_array_return.ds" 0 $'6\n'

write_fixture "$FIX/bool_array_return.ds" <<'DS'
fn flags() {
  return [true, false, true]
}

let xs = flags()
let first = xs[0]
let second = xs[1]
let third = xs[2]
if first {
  echo "first"
}
if !second {
  echo "second-false"
}
if third {
  echo "third"
}
DS
assert_parity bool_array_return "$FIX/bool_array_return.ds" 0 $'first\nsecond-false\nthird\n'

write_fixture "$FIX/array_loop_return.ds" <<'DS'
fn apps() {
  return ["api", "web", "worker"]
}

let names = apps()
for name in names {
  echo "app={name}"
}
DS
assert_parity array_loop_return "$FIX/array_loop_return.ds" 0 $'app=api\napp=web\napp=worker\n'

write_fixture "$FIX/array_membership_return.ds" <<'DS'
fn allowed() {
  return ["api", "web"]
}

let names = allowed()
if "api" in names {
  echo "api-ok"
}
if !("db" in names) {
  echo "db-missing"
}
DS
assert_parity array_membership_return "$FIX/array_membership_return.ds" 0 $'api-ok\ndb-missing\n'

write_fixture "$FIX/array_variable_return.ds" <<'DS'
fn apps() {
  let local = ["api", "web"]
  return local
}

let names = apps()
let second = names[1]
echo "{second}"
DS
assert_parity array_variable_return "$FIX/array_variable_return.ds" 0 $'web\n'

write_fixture "$FIX/array_forward_return.ds" <<'DS'
fn base() {
  return ["api", "web"]
}

fn wrapper() {
  return base()
}

let names = wrapper()
let first = names[0]
let second = names[1]
echo "{first}"
echo "{second}"
DS
assert_parity array_forward_return "$FIX/array_forward_return.ds" 0 $'api\nweb\n'

# 2. Flat map/object function returns.
write_fixture "$FIX/map_return.ds" <<'DS'
fn service() {
  return { name: "api", port: 8080, enabled: true }
}

let app = service()
let name = app.name
let port = app.port
let enabled = app.enabled
echo "{name}"
echo "{port}"
if enabled {
  echo "enabled"
}
DS
assert_parity map_return "$FIX/map_return.ds" 0 $'api\n8080\nenabled\n'

write_fixture "$FIX/map_tricky_strings.ds" <<'DS'
fn labels() {
  return { empty: "", padded: "  api  ", shell: "$(echo bad); *" }
}

let data = labels()
let empty = data.empty
let padded = data.padded
let shell = data.shell
echo "[{empty}]"
echo "[{padded}]"
echo "{shell}"
DS
assert_parity map_tricky_strings "$FIX/map_tricky_strings.ds" 0 $'[]\n[  api  ]\n$(echo bad); *\n'

write_fixture "$FIX/map_newline_value.ds" <<'DS'
fn data() {
  return { text: """one
two""" }
}

let value = data()
let text = value.text
echo "{text}"
DS
assert_parity map_newline_value "$FIX/map_newline_value.ds" 0 $'one\ntwo\n'

write_fixture "$FIX/map_variable_return.ds" <<'DS'
fn service() {
  let app = { name: "api", tier: "frontend" }
  return app
}

let value = service()
let name = value.name
let tier = value.tier
echo "{name}:{tier}"
DS
assert_parity map_variable_return "$FIX/map_variable_return.ds" 0 $'api:frontend\n'

write_fixture "$FIX/map_forward_return.ds" <<'DS'
fn base() {
  return { name: "api", port: 8080 }
}

fn wrapper() {
  return base()
}

let value = wrapper()
let name = value.name
let port = value.port
echo "{name}:{port}"
DS
assert_parity map_forward_return "$FIX/map_forward_return.ds" 0 $'api:8080\n'

write_fixture "$FIX/map_duplicate_key_rejected.ds" <<'DS'
fn data() {
  return { name: "old", name: "new" }
}

let value = data()
DS
assert_check_fails map_duplicate_key_rejected "$FIX/map_duplicate_key_rejected.ds" 'duplicate map key'
assert_emit_fails map_duplicate_key_rejected "$FIX/map_duplicate_key_rejected.ds" 'duplicate map key'

write_fixture "$FIX/map_missing_field.ds" <<'DS'
fn data() {
  return { name: "api" }
}

let value = data()
let missing = value.missing
echo "{missing}"
DS
assert_parity_same map_missing_field "$FIX/map_missing_field.ds" 1
assert_contains "$TMP/map_missing_field_vm.err" 'missing map key' 'missing field VM diagnostic'
assert_contains "$TMP/map_missing_field_bash.err" 'missing map key' 'missing field Bash diagnostic'

# 3. Command-result function returns. The roadmap field is .status; .code remains
# as an existing compatibility alias.
write_fixture "$FIX/command_result_success.ds" <<'DS'
fn probe() {
  return run printf "hello"
}

let result = probe()
echo "stdout={result.stdout}"
echo "status={result.status}"
echo "code={result.code}"
DS
assert_parity command_result_success "$FIX/command_result_success.ds" 0 $'stdout=hello\nstatus=0\ncode=0\n'

write_fixture "$FIX/command_result_stderr.ds" <<'DS'
fn probe() {
  return run sh -c "printf err >&2"
}

let result = probe()
echo "stdout=[{result.stdout}]"
echo "stderr=[{result.stderr}]"
echo "code={result.code}"
DS
assert_parity command_result_stderr "$FIX/command_result_stderr.ds" 0 $'stdout=[]\nstderr=[err]\ncode=0\n'

write_fixture "$FIX/command_result_nonzero.ds" <<'DS'
fn missing() {
  return run sh -c "printf nope >&2; exit 7"
}

let result = missing()
echo "status={result.status}"
echo "code={result.code}"
if result.failed {
  echo "failed"
}
echo "stderr={result.stderr}"
DS
assert_parity command_result_nonzero "$FIX/command_result_nonzero.ds" 0 $'status=7\ncode=7\nfailed\nstderr=nope\n'

write_fixture "$FIX/command_result_multiline.ds" <<'DS'
fn probe() {
  return run sh -c "printf 'one\ntwo\n'; printf 'err1\nerr2\n' >&2"
}

let result = probe()
echo result.stdout
echo result.stderr
DS
assert_parity command_result_multiline "$FIX/command_result_multiline.ds" 0 $'one\ntwo\n\nerr1\nerr2\n\n'

write_fixture "$FIX/command_result_variable.ds" <<'DS'
fn probe() {
  let result = run printf "api"
  return result
}

let value = probe()
let stdout = value.stdout
echo "{stdout}"
DS
assert_parity command_result_variable "$FIX/command_result_variable.ds" 0 $'api\n'

write_fixture "$FIX/command_result_forward.ds" <<'DS'
fn base() {
  return run printf "api"
}

fn wrapper() {
  return base()
}

let value = wrapper()
let stdout = value.stdout
let code = value.code
echo "{stdout}"
echo "{code}"
DS
assert_parity command_result_forward "$FIX/command_result_forward.ds" 0 $'api\n0\n'

mkdir -p "$FIX/import_command"
write_fixture "$FIX/import_command/lib.ds" <<'DS'
fn probe(name = "api") {
  return run printf "service=%s" "{name}"
}
DS
write_fixture "$FIX/import_command/main.ds" <<'DS'
import "./lib.ds"

let result = probe("web")
echo result.stdout
echo result.code
DS
assert_parity import_command_result "$FIX/import_command/main.ds" 0 $'service=web\n0\n'

# 4. Return-kind validation and collection shape rules.
write_fixture "$FIX/mixed_array_scalar_rejected.ds" <<'DS'
fn bad(flag = true) {
  if flag {
    return ["api"]
  }
  return "api"
}

let value = bad(false)
DS
assert_check_fails mixed_array_scalar_rejected "$FIX/mixed_array_scalar_rejected.ds" 'same value kind'
assert_emit_fails mixed_array_scalar_rejected "$FIX/mixed_array_scalar_rejected.ds" 'same value kind'

write_fixture "$FIX/mixed_array_map_rejected.ds" <<'DS'
fn bad(flag = true) {
  if flag {
    return ["api"]
  }
  return { name: "api" }
}

let value = bad(false)
DS
assert_check_fails mixed_array_map_rejected "$FIX/mixed_array_map_rejected.ds" 'same value kind'
assert_emit_fails mixed_array_map_rejected "$FIX/mixed_array_map_rejected.ds" 'same value kind'

write_fixture "$FIX/mixed_map_command_rejected.ds" <<'DS'
fn bad(flag = true) {
  if flag {
    return { stdout: "manual", stderr: "", code: 0 }
  }
  return run printf "api"
}

let value = bad(false)
DS
assert_check_fails mixed_map_command_rejected "$FIX/mixed_map_command_rejected.ds" 'same value kind'
assert_emit_fails mixed_map_command_rejected "$FIX/mixed_map_command_rejected.ds" 'same value kind'

write_fixture "$FIX/mixed_array_elements.ds" <<'DS'
fn values() {
  return [1, "two", false]
}

let xs = values()
let first = xs[0]
let second = xs[1]
let third = xs[2]
echo "{first}"
echo "{second}"
if !third {
  echo "false"
}
DS
assert_parity mixed_array_elements "$FIX/mixed_array_elements.ds" 0 $'1\ntwo\nfalse\n'

write_fixture "$FIX/mixed_map_values.ds" <<'DS'
fn values() {
  return { a: 1, b: "two", c: false }
}

let data = values()
let a = data.a
let b = data.b
let c = data.c
echo "{a}"
echo "{b}"
if !c {
  echo "false"
}
DS
assert_parity mixed_map_values "$FIX/mixed_map_values.ds" 0 $'1\ntwo\nfalse\n'

write_fixture "$FIX/missing_structured_return_rejected.ds" <<'DS'
fn bad(flag = true) {
  if flag {
    return ["api"]
  }
}

let value = bad(false)
DS
assert_check_fails missing_structured_return_rejected "$FIX/missing_structured_return_rejected.ds" 'not all control paths return'
assert_emit_fails missing_structured_return_rejected "$FIX/missing_structured_return_rejected.ds" 'not all control paths return'

# 5. Deferred unsupported forms.
write_fixture "$FIX/nested_array_rejected.ds" <<'DS'
fn nested() {
  return [["api"], ["web"]]
}

let value = nested()
DS
assert_check_fails nested_array_rejected "$FIX/nested_array_rejected.ds" 'nested collections are deferred'
assert_emit_fails nested_array_rejected "$FIX/nested_array_rejected.ds" 'nested collections are deferred'

write_fixture "$FIX/nested_map_rejected.ds" <<'DS'
fn nested() {
  return { app: { name: "api" } }
}

let value = nested()
DS
assert_check_fails nested_map_rejected "$FIX/nested_map_rejected.ds" 'nested collections are deferred'
assert_emit_fails nested_map_rejected "$FIX/nested_map_rejected.ds" 'nested collections are deferred'

write_fixture "$FIX/array_of_maps_rejected.ds" <<'DS'
fn services() {
  return [{ name: "api" }, { name: "web" }]
}

let value = services()
DS
assert_check_fails array_of_maps_rejected "$FIX/array_of_maps_rejected.ds" 'nested collections are deferred'
assert_emit_fails array_of_maps_rejected "$FIX/array_of_maps_rejected.ds" 'nested collections are deferred'

write_fixture "$FIX/map_containing_array_rejected.ds" <<'DS'
fn data() {
  return { names: ["api", "web"] }
}

let value = data()
DS
assert_check_fails map_containing_array_rejected "$FIX/map_containing_array_rejected.ds" 'nested collections are deferred'
assert_emit_fails map_containing_array_rejected "$FIX/map_containing_array_rejected.ds" 'nested collections are deferred'

write_fixture "$FIX/structured_interpolation_rejected.ds" <<'DS'
fn apps() {
  return ["api", "web"]
}

echo "{apps()}"
DS
assert_check_fails structured_interpolation_rejected "$FIX/structured_interpolation_rejected.ds" 'function-call interpolation in command words'
assert_emit_fails structured_interpolation_rejected "$FIX/structured_interpolation_rejected.ds" 'function-call interpolation in command words'

write_fixture "$FIX/map_iteration_rejected.ds" <<'DS'
fn service() {
  return { name: "api", port: 8080 }
}

let data = service()
for key, value in data {
  echo key
}
DS
assert_check_fails map_iteration_rejected "$FIX/map_iteration_rejected.ds" 'map iteration is deferred'
assert_emit_fails map_iteration_rejected "$FIX/map_iteration_rejected.ds" 'map iteration is deferred'

write_fixture "$FIX/index_assignment_rejected.ds" <<'DS'
fn apps() {
  return ["api", "web"]
}

let names = apps()
names[0] = "worker"
DS
assert_check_fails index_assignment_rejected "$FIX/index_assignment_rejected.ds" 'unsupported assignment target'
assert_emit_fails index_assignment_rejected "$FIX/index_assignment_rejected.ds" 'unsupported assignment target'

# 6. Stdout safety and ABI regression.
write_fixture "$FIX/structured_stdout_rejected.ds" <<'DS'
fn bad() {
  echo "debug"
  return ["api"]
}

let value = bad()
echo value[0]
DS
assert_check_fails structured_stdout_rejected "$FIX/structured_stdout_rejected.ds" 'plain command statements'
assert_emit_fails structured_stdout_rejected "$FIX/structured_stdout_rejected.ds" 'plain command statements'

write_fixture "$FIX/structured_redirected_stdout.ds" <<'DS'
fn ok() {
  echo "debug" !> "debug.log"
  return ["api"]
}

let value = ok()
let first = value[0]
echo "{first}"
DS
assert_check_fails structured_redirected_stdout "$FIX/structured_redirected_stdout.ds" 'plain command statements'
assert_emit_fails structured_redirected_stdout "$FIX/structured_redirected_stdout.ds" 'plain command statements'

write_fixture "$FIX/effect_function_valid.ds" <<'DS'
fn show() {
  echo "api"
}

show()
DS
assert_parity effect_function_valid "$FIX/effect_function_valid.ds" 0 $'api\n'
assert_not_contains "$TMP/effect_function_valid.sh" '__ds_call_value_into' 'effect function emits no value-return helper'

write_fixture "$FIX/effect_function_as_value_rejected.ds" <<'DS'
fn show() {
  echo "api"
}

let value = show()
echo value[0]
DS
assert_check_fails effect_function_as_value_rejected "$FIX/effect_function_as_value_rejected.ds" 'does not return a value'
assert_emit_fails effect_function_as_value_rejected "$FIX/effect_function_as_value_rejected.ds" 'does not return a value'

# Malformed private structured payloads are not user-program behavior, but the
# generated Bash ABI should fail loudly if a DS compiler/runtime bug corrupts
# the sidecar metadata required to preserve structured value kinds.
write_fixture "$FIX/malformed_array_payload.ds" <<'DS'
fn values() {
  return ["api"]
}

let xs = values()
let first = xs[0]
echo "{first}"
DS
run_ok malformed_array_payload_emit "$DS" emit bash "$FIX/malformed_array_payload.ds" -o "$TMP/malformed_array_payload.sh"
grep -v 'declare -ga __ds_return_elem_type' "$TMP/malformed_array_payload.sh" >"$TMP/malformed_array_payload_corrupt.sh"
capture_status malformed_array_payload_corrupt bash "$TMP/malformed_array_payload_corrupt.sh"
assert_nonzero_status malformed_array_payload_corrupt
assert_contains "$TMP/malformed_array_payload_corrupt.err" 'invalid internal array element-type function return payload' 'malformed array payload diagnostic'

write_fixture "$FIX/malformed_map_payload.ds" <<'DS'
fn service() {
  return { name: "api" }
}

let app = service()
let name = app.name
echo "{name}"
DS
run_ok malformed_map_payload_emit "$DS" emit bash "$FIX/malformed_map_payload.ds" -o "$TMP/malformed_map_payload.sh"
grep -v 'declare -gA __ds_return_value_type' "$TMP/malformed_map_payload.sh" >"$TMP/malformed_map_payload_corrupt.sh"
capture_status malformed_map_payload_corrupt bash "$TMP/malformed_map_payload_corrupt.sh"
assert_nonzero_status malformed_map_payload_corrupt
assert_contains "$TMP/malformed_map_payload_corrupt.err" 'invalid internal map value-type function return payload' 'malformed map payload diagnostic'

# 7. Imports and multi-file parity.
mkdir -p "$FIX/import_all"
write_fixture "$FIX/import_all/lib.ds" <<'DS'
fn apps() {
  return ["api", "web"]
}

fn service() {
  return { name: "api", port: 8080 }
}

fn probe() {
  return run printf "ok"
}
DS
write_fixture "$FIX/import_all/main.ds" <<'DS'
import "./lib.ds"

let names = apps()
let svc = service()
let result = probe()
let second = names[1]
let svc_name = svc.name
let svc_port = svc.port
let result_stdout = result.stdout
let result_code = result.code

echo "{second}"
echo "{svc_name}:{svc_port}"
echo "{result_stdout}:{result_code}"
DS
assert_parity import_all_structured "$FIX/import_all/main.ds" 0 $'web\napi:8080\nok:0\n'
[ "$(grep -c '^__ds_call_value_into()' "$TMP/import_all_structured.sh")" = 1 ] || fail 'import_all emits value helper once'
pass 'import_all value helper emitted once'

# 8. Docs and examples checks.
[ -f docs/milestones/v0.26.0-spec.md ] || fail 'missing v0.26 spec'
pass 'v0.26 spec exists'
[ -f docs/milestones/v0.26.0-test-plan.md ] || fail 'missing v0.26 test plan'
pass 'v0.26 test plan exists'
assert_contains docs/roadmap.md 'v0.26.0 — Flat Collection and Command-Result Function Returns' 'roadmap lists v0.26'
assert_contains docs/status.md 'scalar-array returns' 'status documents array returns'
assert_contains docs/status.md 'command-result returns' 'status documents command-result returns'
assert_contains docs/runtime.md 'private function-value boundary' 'runtime documents structured returns'
assert_contains docs/language.ds 'Nested collections' 'language marks nested collections deferred'
assert_contains docs/language.ds 'function-call interpolation in command words remain deferred' 'language marks function interpolation deferred'
assert_contains docs/status.md 'Generated Bash must not call' 'status documents standalone Bash'

printf 'v0.26.0 tests passed (%d assertions)\n' "$pass_count"
