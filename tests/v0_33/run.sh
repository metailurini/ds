#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
DS="$ROOT/ds"
CASE_TIMEOUT=${DS_TEST_CASE_TIMEOUT:-30}
TMP=${TMPDIR:-/tmp}/ds_v0_33_tests.$$
FIX="$TMP/fixtures"
SEED="$TMP/seeds"
mkdir -p "$FIX" "$SEED"
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
  local name="$1" text="$2"
  local path="$TMP/$name.expected"
  printf '%s' "$text" >"$path"
  printf '%s' "$path"
}

assert_text() {
  local name="$1" expected="$2" actual="$3"
  local exp
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

assert_no_ds_call() {
  local script="$1" name="$2"
  assert_not_contains "$script" "$ROOT/ds" "$name omits repo ds path"
  assert_not_contains "$script" './ds ' "$name omits ./ds invocation"
  assert_not_contains "$script" ' ds run ' "$name omits ds run invocation"
  assert_not_contains "$script" ' ds emit ' "$name omits ds emit invocation"
}

assert_no_duplicate_helpers() {
  local script="$1" name="$2" defs dups
  defs="$TMP/${name}_helper_defs.txt"
  dups="$TMP/${name}_helper_dups.txt"
  grep -E '^__ds_[A-Za-z0-9_]+\(\)' "$script" | sed 's/(.*//' | sort >"$defs" || true
  uniq -d "$defs" >"$dups"
  [ ! -s "$dups" ] || { cat "$dups" >&2; fail "$name has duplicate helper definitions"; }
  pass "$name has no duplicate helper definitions"
}

assert_helper_count() {
  local script="$1" helper="$2" expected="$3" name="$4" count
  count=$(grep -c -F -- "$helper" "$script" || true)
  [ "$count" = "$expected" ] || fail "$name: expected $helper count $expected, got $count"
  pass "$name"
}

assert_helper_present() { assert_contains "$1" "$2" "$3"; }
assert_helper_absent() { assert_not_contains "$1" "$2" "$3"; }

emit_checked() {
  local name="$1" file="$2" script="$3"
  run_ok "${name}_emit" "$DS" emit bash "$file" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_no_ds_call "$script" "$name emitted Bash standalone"
  assert_no_duplicate_helpers "$script" "$name emitted Bash"
}

copy_seed() {
  local src="$1" dest="$2"
  mkdir -p "$dest"
  (cd "$src" && tar cf - .) | (cd "$dest" && tar xf -)
}

run_parity() {
  local name="$1" file="$2" expected_stdout="$3" expected_status="${4:-0}"
  shift 4 || true
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
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

run_parity_env() {
  local name="$1" file="$2" expected_stdout="$3" expected_status="${4:-0}"
  shift 4 || true
  local script="$TMP/$name.sh"
  local env_args=("PATH=$PATH") script_args=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      *=*) env_args+=("$1") ;;
      *) script_args+=("$1") ;;
    esac
    shift
  done
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"
  set +e
  env -i "${env_args[@]}" timeout "$CASE_TIMEOUT" "$DS" run "$file" "${script_args[@]}" >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  env -i "${env_args[@]}" timeout "$CASE_TIMEOUT" bash "$script" "${script_args[@]}" >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
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

run_parity_in_seed() {
  local name="$1" file="$2" seed="$3" expected_stdout="$4" expected_status="${5:-0}"
  shift 5 || true
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  rm -rf "$vm_work" "$bash_work"
  mkdir -p "$vm_work" "$bash_work"
  copy_seed "$seed" "$vm_work"
  copy_seed "$seed" "$bash_work"
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

run_runtime_failure() {
  local name="$1" file="$2" needle="$3" expected_stdout="$4"
  shift 4 || true
  local script="$TMP/$name.sh"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"
  capture_cmd "${name}_vm" "$DS" run "$file" "$@"
  capture_cmd "${name}_bash" bash "$script" "$@"
  assert_nonzero_status "${name}_vm"
  assert_nonzero_status "${name}_bash"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name runtime stdout parity"
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
  assert_contains "$TMP/${name}_vm.err" "$needle" "$name VM diagnostic message"
  assert_contains "$TMP/${name}_bash.err" "$needle" "$name Bash diagnostic message"
}

run_runtime_failure_in_seed() {
  local name="$1" file="$2" seed="$3" needle="$4" expected_stdout="$5"
  shift 5 || true
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  rm -rf "$vm_work" "$bash_work"
  mkdir -p "$vm_work" "$bash_work"
  copy_seed "$seed" "$vm_work"
  copy_seed "$seed" "$bash_work"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"
  set +e
  (cd "$vm_work" && "$DS" run "$file" "$@") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && bash "$script" "$@") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"
  assert_nonzero_status "${name}_vm"
  assert_nonzero_status "${name}_bash"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name runtime stdout parity"
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
  assert_contains "$TMP/${name}_vm.err" "$needle" "$name VM diagnostic message"
  assert_contains "$TMP/${name}_bash.err" "$needle" "$name Bash diagnostic message"
}

assert_check_fails() {
  local name="$1" file="$2" needle="$3"
  capture_cmd "${name}_check" "$DS" check "$file"
  assert_nonzero_status "${name}_check"
  assert_contains "$TMP/${name}_check.err" ': error:' "$name check diagnostic shape"
  assert_contains "$TMP/${name}_check.err" "$needle" "$name check diagnostic message"
}

assert_emit_fails() {
  local name="$1" file="$2" needle="$3"
  local out="$TMP/$name.sh"
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

assert_doc_contains() { assert_contains "$1" "$2" "$3"; }
assert_doc_not_contains() { assert_not_contains "$1" "$2" "$3"; }

make_glob_seed() {
  local dir="$1"
  mkdir -p "$dir/src/nested" "$dir/src/space dir" "$dir/src/[literal]" "$dir/src/.hidden" "$dir/.config/sub" "$dir/many/a" "$dir/many/b"
  : >"$dir/root.ds"
  : >"$dir/A.ds"
  : >"$dir/b.ds"
  : >"$dir/src/main.ds"
  : >"$dir/src/helper.c"
  : >"$dir/src/nested/deep.ds"
  : >"$dir/src/nested/helper.c"
  : >"$dir/src/space dir/has space.ds"
  : >"$dir/src/[literal]/bracket.ds"
  : >"$dir/src/-leading.ds"
  : >"$dir/src/dollar\$name.ds"
  : >"$dir/src/quote'name.ds"
  : >"$dir/src/.hidden/skip.ds"
  : >"$dir/src/.hidden-file.ds"
  : >"$dir/.config/sub/app.json"
  : >"$dir/src/nested/data.txt"
  : >"$dir/many/a/one.ds"
  : >"$dir/many/b/two.ds"
  local i
  for i in $(seq -w 0 19); do
    mkdir -p "$dir/many/branch$((10#$i % 7))"
    : >"$dir/many/branch$((10#$i % 7))/file_${i}.ds"
  done
}

# 1. Planning, docs, and scope guards.
for doc in \
  docs/milestones/v0.33.0-spec.md \
  docs/milestones/v0.33.0-test-plan.md \
  docs/roadmap.md \
  docs/status.md \
  docs/language.ds \
  docs/runtime.md \
  docs/parity-contracts.md \
  docs/diagnostics.md \
  docs/source-map.md \
  docs/concept-map.md \
  docs/technical-debt.md \
  docs/release-checklist.md; do
  [ -f "$doc" ] || fail "missing required doc $doc"
  pass "required doc exists: $doc"
done
assert_doc_contains docs/milestones/v0.33.0-spec.md 'cleanup/stabilization release' 'spec identifies cleanup/stabilization scope'
assert_doc_contains docs/milestones/v0.33.0-spec.md 'no new syntax' 'spec forbids new syntax surface'
assert_doc_contains docs/roadmap.md 'v0.33.0' 'roadmap lists v0.33.0'
assert_doc_contains docs/roadmap.md 'Collection, Glob, and Regex Stabilization' 'roadmap keeps v0.33 stabilization milestone'
assert_doc_contains docs/roadmap.md 'v0.34.0 — Text Literal and Broken-Pipe DX' 'roadmap keeps deliberate v0.34 after v0.33'
run_ok cli_help "$DS" --help
assert_contains "$TMP/cli_help.out" 'ds v0.36.0' 'CLI help reports current version'
assert_contains "$TMP/cli_help.out" 'emit bash' 'CLI help keeps known command surface'
assert_doc_contains docs/status.md 'v0.33.0' 'status mentions v0.33.0'
assert_doc_contains docs/language.ds 'recursive `**`' 'language docs mention recursive glob support'
assert_doc_contains docs/runtime.md 'regex.match' 'runtime docs mention regex.match support'
assert_doc_contains docs/parity-contracts.md 'ascending bytewise/ASCII order' 'parity docs mention map iteration order'
assert_doc_contains docs/diagnostics.md 'lowerer' 'diagnostics docs mention lowerer ownership'
assert_doc_contains docs/source-map.md 'bash_deps.c' 'source map names Bash helper dependency scanner'
assert_doc_contains docs/concept-map.md 'regex' 'concept map names regex ownership'
assert_doc_contains docs/technical-debt.md 'v0.33 H1' 'technical debt records v0.33 H1 audit'
assert_doc_contains docs/technical-debt.md 'v0.33 H8' 'technical debt records v0.33 H8 audit'
assert_doc_not_contains docs/release-checklist.md 'recursive glob remains deferred' 'release checklist no longer defers recursive glob'
assert_doc_not_contains docs/release-checklist.md 'regex replacement remains deferred' 'release checklist no longer defers regex replacement'

# 2. Collection contract stabilization.
collection_return=$(write_fixture collection_return <<'DS'
fn weird_strings() {
  return ["", "  spaced  ", "semi;colon", "$(echo bad)", "-dash", "one\ntwo"]
}

fn ints() { return [1, 2, 3] }
fn bools() { return [true, false, true] }
fn settings() { return { B: "upper", a: 1, aa: true, b: "bee", z: "last" } }
fn captured() { return run sh -c "printf out; printf err >&2; exit 7" }

let strings = weird_strings()
let strings_copy = strings
strings[1] = "changed"
echo "orig={strings[1]}"
echo "copy={strings_copy[1]}"
echo "empty={strings[0]}"
echo "dash={strings[4]}"
echo "newline={strings[5]}"

let nums = ints()
nums[0] = 10
let i = 1
nums[i] = 20 + 2
nums[2] = 30
if nums[1] == 22 { echo "int-ok" }

let flags = bools()
flags[1] = true
if flags[1] { echo "bool-ok" }

let cfg = settings()
let cfg_copy = cfg
cfg["new key"] = "value with spaces"
cfg["-dash"] = 42
cfg["a"] = 2
let map_copy_a = cfg_copy.a
echo "map-copy-a={map_copy_a}"
for key, value in cfg { echo "{key}={value}" }

let result = captured()
echo "stdout={result.stdout}"
echo "stderr={result.stderr}"
echo "status={result.status}"
if result.failed { printf "%s\n" "cmd={result.stdout}:{result.stderr}:{result.status}" }
DS
)
run_parity collection_return "$collection_return" $'orig=changed
copy=  spaced  
empty=
dash=-dash
newline=one\ntwo
int-ok
bool-ok
map-copy-a=1
-dash=42
B=upper
a=2
aa=true
b=bee
new key=value with spaces
z=last
stdout=out
stderr=err
status=7
cmd=out:err:7
' 0

empty_collections=$(write_fixture empty_collections <<'DS'
fn empty_arr() { return [] }
let seen = "start"
let arr = empty_arr()
for item in arr { seen = "bad-array" }
echo $seen
DS
)
run_parity empty_collections "$empty_collections" $'start
' 0
empty_map_deferred=$(write_fixture empty_map_deferred <<'DS'
let m = {}
DS
)
assert_rejected empty_map_deferred "$empty_map_deferred" 'empty map literals are deferred'

empty_collection_loops=$(write_fixture empty_collection_loops <<'DS'
fn empty_arr() { return [] }
fn single_map() { return { seed: 1 } }
let xs = empty_arr()
for item in xs {
  echo bad-array
  break
}
let m = single_map()
m["seed"] = 2
for key, value in m {
  if key == "seed" { continue }
  echo bad-map
}
echo done
DS
)
run_parity empty_collection_loop_controls "$empty_collection_loops" $'done
' 0

cat >"$FIX/import_collections_lib.ds" <<'DS'
fn imported_names() { return ["api", "web"] }
fn imported_ports() { return { api: 3000, web: 5173 } }
fn imported_match(s) { return regex.match(s, /^([a-z]+)-([0-9]+)$/) }
DS
imported_collections=$(write_fixture imported_collections <<DS
import "$FIX/import_collections_lib.ds"
let names = imported_names()
let ports = imported_ports()
let parsed = imported_match("api-123")
echo "name={names[1]}"
echo "port={ports[\"api\"]}"
echo "match={parsed[\"1\"]}:{parsed[\"2\"]}"
DS
)
run_parity imported_collections "$imported_collections" $'name=web
port=3000
match=api:123
' 0

collection_control=$(write_fixture collection_control <<'DS'
fn names() { return ["api", "skip", "web"] }
fn ports() { return { web: 5173, api: 3000 } }
let ns = names()
let first_name = ns[0]
if first_name == "api" { echo "case-api" }
for name in ns {
  if name == "skip" { continue }
  if name == "web" {
    echo "case-web"
    break
  }
}
let ps = ports()
for key, value in ps {
  if key == "api" { echo "{key}:{value}" }
}
test "returned collections compose" {
  let xs = names()
  assert xs[0] == "api"
}
DS
)
run_parity collection_control "$collection_control" $'case-api
case-web
api:3000
' 0

interp_index=$(write_fixture interp_index <<'DS'
fn double(n) { return n * 2 }
let items = ["api", "web"]
echo "item={items[1]} double={double(21)}"
printf "%s\n" "cmd={items[0]}:{double(2)}"
DS
)
run_parity interp_index "$interp_index" $'item=web double=42
cmd=api:4
' 0

array_failure_negative=$(write_fixture array_failure_negative <<'DS'
script { arg index: int }
let xs = ["a"]
xs[index] = "bad"
file.write("marker.txt", "bad")
DS
)
run_runtime_failure array_failure_negative "$array_failure_negative" 'array index -1 out of range' '' -1
array_failure_len=$(write_fixture array_failure_len <<'DS'
let xs = ["a"]
xs[1] = "append"
file.write("marker.txt", "bad")
DS
)
run_runtime_failure array_failure_len "$array_failure_len" 'array index 1 out of range' ''
array_failure_non_int=$(write_fixture array_failure_non_int <<'DS'
let xs = ["a"]
let index = "0"
xs[index] = "bad"
DS
)
assert_rejected array_failure_non_int "$array_failure_non_int" 'array index assignment requires an int index'
array_failure_rhs=$(write_fixture array_failure_rhs <<'DS'
let xs = ["a"]
xs[0] = ["nested"]
DS
)
assert_rejected array_failure_rhs "$array_failure_rhs" 'index assignment value must be a flat scalar'
array_failure_rhs_map=$(write_fixture array_failure_rhs_map <<'DS'
let xs = ["a"]
xs[0] = { nested: "value" }
DS
)
assert_rejected array_failure_rhs_map "$array_failure_rhs_map" 'index assignment value must be a flat scalar'
array_failure_rhs_command=$(write_fixture array_failure_rhs_command <<'DS'
let xs = ["a"]
let result = run printf ok
xs[0] = result
DS
)
assert_rejected array_failure_rhs_command "$array_failure_rhs_command" 'index assignment value must be a flat scalar'
array_failure_target=$(write_fixture array_failure_target <<'DS'
fn xs() { return ["a"] }
xs()[0] = "bad"
DS
)
assert_rejected array_failure_target "$array_failure_target" 'function-result index assignment is deferred'
array_failure_command_target=$(write_fixture array_failure_command_target <<'DS'
let result = run printf ok
result["stdout"] = "bad"
DS
)
assert_rejected array_failure_command_target "$array_failure_command_target" 'must be a named array or map'
map_failure_empty=$(write_fixture map_failure_empty <<'DS'
let m = { a: 1 }
m[""] = 2
file.write("marker.txt", "bad")
DS
)
assert_rejected map_failure_empty "$map_failure_empty" 'empty map keys are deferred'
map_failure_key_kind=$(write_fixture map_failure_key_kind <<'DS'
let m = { a: 1 }
m[1] = 2
DS
)
assert_rejected map_failure_key_kind "$map_failure_key_kind" 'map index assignment requires a string key'
map_failure_rhs_array=$(write_fixture map_failure_rhs_array <<'DS'
let m = { a: 1 }
m["a"] = ["nested"]
DS
)
assert_rejected map_failure_rhs_array "$map_failure_rhs_array" 'index assignment value must be a flat scalar'
map_failure_rhs_map=$(write_fixture map_failure_rhs_map <<'DS'
let m = { a: 1 }
m["a"] = { nested: "value" }
DS
)
assert_rejected map_failure_rhs_map "$map_failure_rhs_map" 'index assignment value must be a flat scalar'
map_failure_rhs_command=$(write_fixture map_failure_rhs_command <<'DS'
let m = { a: 1 }
let result = run printf ok
m["a"] = result
DS
)
assert_rejected map_failure_rhs_command "$map_failure_rhs_command" 'index assignment value must be a flat scalar'
map_failure_target=$(write_fixture map_failure_target <<'DS'
fn m() { return { a: 1 } }
m()["a"] = 2
DS
)
assert_rejected map_failure_target "$map_failure_target" 'function-result index assignment is deferred'
map_failure_field=$(write_fixture map_failure_field <<'DS'
let m = { a: 1 }
m.a = 2
DS
)
assert_rejected map_failure_field "$map_failure_field" 'field-style map assignment is deferred'
nested_value=$(write_fixture nested_value <<'DS'
let m = { inner: { name: "api" } }
DS
)
assert_rejected nested_value "$nested_value" 'nested collections are deferred'
same_map_mutation=$(write_fixture same_map_mutation <<'DS'
let m = { a: 1 }
for key, value in m { m["b"] = 2 }
DS
)
assert_rejected same_map_mutation "$same_map_mutation" 'mutating the map currently being iterated is unsupported'
different_map_mutation=$(write_fixture different_map_mutation <<'DS'
let source = { a: 1 }
let target = { z: 0 }
for key, value in source { target[key] = value }
let out = target["a"]
echo $out
DS
)
run_parity different_map_mutation "$different_map_mutation" $'1
' 0

large_map=$(write_fixture large_map <<'DS'
let m = { seed: 0 }
let i = 49
while i >= 0 {
  let key = "k{i}"
  m[key] = i
  i = i - 1
}
for key, value in m { echo "{key}={value}" }
DS
)
large_map_expected=$(for i in 0 1 10 11 12 13 14 15 16 17 18 19 2 20 21 22 23 24 25 26 27 28 29 3 30 31 32 33 34 35 36 37 38 39 4 40 41 42 43 44 45 46 47 48 49 5 6 7 8 9; do printf 'k%s=%s\n' "$i" "$i"; done; printf 'seed=0')$'\n'
run_parity large_map "$large_map" "$large_map_expected" 0

# 3. Recursive glob stabilization.
glob_seed="$SEED/glob"
make_glob_seed "$glob_seed"

glob_matrix=$(write_fixture glob_matrix <<'DS'
let all = 0
for file in glob("**/*.ds") { all = all + 1 }
echo "all={all}"
for file in glob("src/**/*.c") { echo "c={file}" }
for file in glob("missing/**/*.ds") { echo "bad={file}" }
echo "nomatch-ok"
for file in glob("**/.hidden-file.ds") { echo "hidden-file={file}" }
for file in glob(".config/**/*.json") { echo "hidden-dir={file}" }
for file in glob("src/-leading.ds") { echo "hostile={file}" }
for file in glob("src/[[]literal]/*.ds") { echo "hostile={file}" }
for file in glob("src/dollar$name.ds") { echo "hostile={file}" }
for file in glob("src/quote'name.ds") { echo "hostile={file}" }
for file in glob("src/space dir/*.ds") { echo "hostile={file}" }
let many = 0
for file in glob("many/**/*.ds") { many = many + 1 }
echo "many={many}"
DS
)
run_parity_in_seed glob_matrix "$glob_matrix" "$glob_seed" $'all=32
c=src/helper.c
c=src/nested/helper.c
nomatch-ok
hidden-file=src/.hidden-file.ds
hidden-dir=.config/sub/app.json
hostile=src/-leading.ds
hostile=src/[literal]/bracket.ds
hostile=src/dollar$name.ds
hostile=src/quote\'name.ds
hostile=src/space dir/has space.ds
many=22
' 0

glob_basic=$(write_fixture glob_basic <<'DS'
for file in glob("src/**/*.c") { echo $file }
DS
)
run_parity_in_seed glob_basic "$glob_basic" "$glob_seed" $'src/helper.c
src/nested/helper.c
' 0

glob_required_fail=$(write_fixture glob_required_fail <<'DS'
for file in glob!("missing/**/*.ds") { file.write("marker.txt", "bad") }
echo after
DS
)
run_runtime_failure_in_seed glob_required_fail "$glob_required_fail" "$glob_seed" 'required glob' ''
[ ! -e "$TMP/glob_required_fail_vm_work/marker.txt" ] || fail 'VM glob! failure created marker'
pass 'VM glob! failure prevents marker side effect'
[ ! -e "$TMP/glob_required_fail_bash_work/marker.txt" ] || fail 'Bash glob! failure created marker'
pass 'Bash glob! failure prevents marker side effect'

if (cd "$glob_seed" && ln -s src/nested/deep.ds linked-file.ds && ln -s src linked-dir && ln -s missing-target.ds broken.ds) 2>/dev/null; then
  glob_symlink=$(write_fixture glob_symlink <<'DS'
for file in glob("broken.ds") { echo $file }
for file in glob("linked-file.ds") { echo $file }
for file in glob("src/nested/deep.ds") { echo $file }
for file in glob("linked-dir/**/*.ds") { echo "dir-symlink={file}" }
DS
)
  run_parity_in_seed glob_symlink "$glob_symlink" "$glob_seed" $'broken.ds
linked-file.ds
src/nested/deep.ds
' 0
else
  pass 'symlink setup unavailable; skipping symlink recursive glob assertion'
fi

glob_dynamic_good=$(write_fixture glob_dynamic_good <<'DS'
script { arg pattern: string }
let count = 0
for file in glob(pattern) { count = count + 1 }
echo $count
DS
)
run_parity_in_seed glob_dynamic_good "$glob_dynamic_good" "$glob_seed" $'22
' 0 'many/**/*.ds'

glob_dynamic_bad=$(write_fixture glob_dynamic_bad <<'DS'
script { arg pattern: string }
echo before
for file in glob(pattern) { echo $file }
file.write("marker.txt", "bad")
DS
)
run_runtime_failure_in_seed glob_dynamic_bad "$glob_dynamic_bad" "$glob_seed" 'multiple recursive `**` glob segments are unsupported' $'before
' 'src/**/tests/**/*.ds'
[ ! -e "$TMP/glob_dynamic_bad_vm_work/marker.txt" ] || fail 'VM invalid dynamic glob created marker'
pass 'VM invalid dynamic glob prevents marker side effect'
[ ! -e "$TMP/glob_dynamic_bad_bash_work/marker.txt" ] || fail 'Bash invalid dynamic glob prevents marker side effect'
pass 'Bash invalid dynamic glob prevents marker side effect'
for row in \
  'dynamic_partial|src/**file.ds|recursive `**` glob patterns must use `**` as a complete path segment' \
  'dynamic_dotdot|src/**/../*.ds|recursive `**` glob patterns with `..` path segments are unsupported' \
  'dynamic_adjacent|src/**/**/*.ds|multiple recursive `**` glob segments are unsupported'; do
  IFS='|' read -r case_name pattern needle <<<"$row"
  run_runtime_failure_in_seed "glob_${case_name}" "$glob_dynamic_bad" "$glob_seed" "$needle" $'before
' "$pattern"
done
IFS=$' \t\n'
run_parity_in_seed glob_dynamic_empty "$glob_dynamic_good" "$glob_seed" $'0
' 0 ''
for row in \
  'partial|src/**file.ds|recursive `**` glob patterns must use `**` as a complete path segment' \
  'dotdot|src/**/../*.ds|recursive `**` glob patterns with `..` path segments are unsupported' \
  'multiple|src/**/nested/**/*.ds|multiple recursive `**` glob segments are unsupported'; do
  IFS='|' read -r case_name pattern needle <<<"$row"
  bad_glob=$(write_fixture "glob_bad_${case_name}" <<DS
for file in glob("$pattern") { echo \$file }
DS
)
  assert_rejected "glob_bad_${case_name}" "$bad_glob" "$needle"
done
IFS=$' \t\n'

glob_hygiene_script="$TMP/glob_hygiene.sh"
emit_checked glob_hygiene "$glob_basic" "$glob_hygiene_script"
set +e
(cd "$glob_seed" && timeout "$CASE_TIMEOUT" bash -O globstar -O dotglob -O nullglob "$glob_hygiene_script") >"$TMP/glob_hygiene_hostile.out" 2>"$TMP/glob_hygiene_hostile.err"
glob_hostile_rc=$?
set -e
printf '%s' "$glob_hostile_rc" >"$TMP/glob_hygiene_hostile.rc"
assert_status glob_hygiene_hostile 0
assert_text glob_hygiene_hostile_stdout $'src/helper.c
src/nested/helper.c
' "$TMP/glob_hygiene_hostile.out"
set +e
(cd "$glob_seed" && IFS='|' timeout "$CASE_TIMEOUT" bash "$glob_hygiene_script") >"$TMP/glob_hygiene_ifs.out" 2>"$TMP/glob_hygiene_ifs.err"
glob_ifs_rc=$?
set -e
printf '%s' "$glob_ifs_rc" >"$TMP/glob_hygiene_ifs.rc"
assert_status glob_hygiene_ifs 0
assert_text glob_hygiene_ifs_stdout $'src/helper.c
src/nested/helper.c
' "$TMP/glob_hygiene_ifs.out"
if locale -a 2>/dev/null | grep -Eiv '^(C|C\.utf8|POSIX)$' >"$TMP/non_c_locales.txt"; then
  non_c_locale=$(head -n 1 "$TMP/non_c_locales.txt")
  set +e
  (cd "$glob_seed" && LC_ALL="$non_c_locale" timeout "$CASE_TIMEOUT" bash "$glob_hygiene_script") >"$TMP/glob_hygiene_locale.out" 2>"$TMP/glob_hygiene_locale.err"
  glob_locale_rc=$?
  set -e
  printf '%s' "$glob_locale_rc" >"$TMP/glob_hygiene_locale.rc"
  assert_status glob_hygiene_locale 0
  assert_text glob_hygiene_locale_stdout $'src/helper.c
src/nested/helper.c
' "$TMP/glob_hygiene_locale.out"
else
  pass 'non-C locale unavailable; recursive glob ordering is asserted under LC_ALL=C-compatible expectations'
fi

perm_seed="$SEED/perm"
mkdir -p "$perm_seed/private" "$perm_seed/open"
: >"$perm_seed/open/ok.ds"
: >"$perm_seed/private/nope.ds"
chmod 000 "$perm_seed/private" 2>/dev/null || true
perm_fixture=$(write_fixture glob_perm <<'DS'
for file in glob("**/*.ds") { echo $file }
echo after
DS
)
if [ "$(id -u)" = 0 ]; then
  if command -v su >/dev/null 2>&1 && id nobody >/dev/null 2>&1; then
    emit_checked glob_perm "$perm_fixture" "$TMP/glob_perm.sh"
    chmod -R a+rx "$TMP" "$ROOT/ds" 2>/dev/null || true
    chmod 000 "$perm_seed/private" 2>/dev/null || true
    set +e
    su nobody -s /bin/sh -c "cd '$perm_seed' && '$ROOT/ds' run '$perm_fixture'" >"$TMP/glob_perm_vm.out" 2>"$TMP/glob_perm_vm.err"
    glob_perm_vm_rc=$?
    su nobody -s /bin/sh -c "cd '$perm_seed' && bash '$TMP/glob_perm.sh'" >"$TMP/glob_perm_bash.out" 2>"$TMP/glob_perm_bash.err"
    glob_perm_bash_rc=$?
    set -e
    printf '%s' "$glob_perm_vm_rc" >"$TMP/glob_perm_vm.rc"
    printf '%s' "$glob_perm_bash_rc" >"$TMP/glob_perm_bash.rc"
    assert_nonzero_status glob_perm_vm
    assert_nonzero_status glob_perm_bash
    assert_contains "$TMP/glob_perm_vm.err" 'failed to traverse recursive glob directory' 'unprivileged VM permission diagnostic'
    assert_contains "$TMP/glob_perm_bash.err" 'failed to traverse recursive glob directory' 'unprivileged Bash permission diagnostic'
  else
    pass 'running as root and no nobody/su fallback available; skipping permission traversal assertion'
  fi
else
  run_runtime_failure_in_seed glob_perm "$perm_fixture" "$perm_seed" 'failed to traverse recursive glob directory' ''
fi
chmod 700 "$perm_seed/private" 2>/dev/null || true

# 4. Regex stabilization.
regex_core=$(write_fixture regex_core <<'DS'
let literal = "release/12" matches /^release\/[0-9]+$/
let alt = "api" matches /^(api|web)$/
let ci = "API" matches /^api$/i
let pattern = "^worker-[0-9]+$"
let runtime = "worker-7" matches pattern
let patterns = ["^api$", "^web$"]
let config = { svc: "^db-[0-9]+$" }
let from_array = "api" matches patterns[0]
let from_map = "db-3" matches config.svc
let command = run printf "^cmd$"
let from_command = "cmd" matches command.stdout
let summary = "{literal}:{alt}:{ci}:{runtime}:{from_array}:{from_map}:{from_command}"
echo $summary

let m = regex.match("ac", /^(a)?(b)?(c)$/)
let n = regex.match("zzz", /^(a)?(b)?(c)$/)
let m_matched = m.matched
let m_full = m.full
let m0 = m["0"]
let m1 = m["1"]
let m2 = m["2"]
let m3 = m["3"]
let n_matched = n.matched
let n_full = n.full
let n0 = n["0"]
let n1 = n["1"]
let n2 = n["2"]
let n3 = n["3"]
echo "m={m_matched}:{m_full}:{m0}:{m1}:{m2}:{m3}"
echo "n={n_matched}:{n_full}:{n0}:{n1}:{n2}:{n3}"
for key, value in m {
  if key == "1" || key == "2" || key == "3" || key == "matched" { echo "match-{key}={value}" }
}
m["1"] = "changed"
let changed = m["1"]
echo "mutated={changed}"

let r1 = regex.replace("api-123 web-456", /([a-z]+)-([0-9]+)/, "$1:$2")
let r2 = regex.replace("a1 b2", /[a-z][0-9]/, "[$0]")
let r3 = regex.replace("api-123", /([a-z]+)-([0-9]+)/, "$$$1=$2")
let r4 = regex.replace("abc", /x/, "Y")
let r5 = regex.replace("abc", /b/, "")
let r6 = regex.replace("x1x2x3", /x[0-9]/, "<$0>")
let r7 = regex.replace("aba", /^a|a$/, "X")
let r8 = regex.replace("aba", /a$|^a/, "X")
let r9 = regex.replace("API api", "api", "svc", "i")
echo $r1
echo $r2
echo $r3
echo $r4
echo $r5
echo $r6
echo $r7
echo $r8
echo $r9
DS
)
run_parity regex_core "$regex_core" $'true:true:true:true:true:true:true
m=true:ac:ac:a::c
n=false:::::
match-1=a
match-2=
match-3=c
match-matched=true
mutated=changed
api:123 web:456
[a1] [b2]
$api=123
abc
ac
<x1><x2><x3>
XbX
XbX
svc svc
' 0

regex_runtime_sources=$(write_fixture regex_runtime_sources <<'DS'
script { arg pattern: string }
fn pattern_from_fn() { return "^fn-[0-9]+$" }
let from_fn = "fn-12" matches pattern_from_fn()
let from_arg = "arg-34" matches pattern
let from_env = "env-56" matches env.DS_V033_REGEX
echo "sources={from_fn}:{from_arg}:{from_env}"
DS
)
run_parity_env regex_runtime_sources "$regex_runtime_sources" $'sources=true:true:true
' 0 DS_V033_REGEX='^env-[0-9]+$' 'arg-[0-9]+$'

regex_integration=$(write_fixture regex_integration <<'DS'
fn parse(s) { return regex.match(s, /^([a-z]+)-([0-9]+)$/) }
fn normalize(s) { return regex.replace(s, /([a-z]+)-([0-9]+)/, "$1:$2") }
let m = parse("api-123")
let matched = m.matched
let svc = m["1"]
let id = m["2"]
if matched { echo "if={svc}" }
let state = "false"
if matched { state = "true" }
case state { "true" { echo "case={id}" } "false" { echo bad } }
for part in normalize("api-1 web-2").split(" ") { echo $part }
printf "%s\n" "cmd={svc}"
DS
)
run_parity regex_integration "$regex_integration" $'if=api
case=123
api:1
web:2
cmd=api
' 0

regex_flags=$(write_fixture regex_flags <<'DS'
let out = regex.replace("API", "api", "svc", "i")
let sensitive = "API" matches "api"
let insensitive = "API" matches /api/i
echo $out
echo $sensitive
echo $insensitive
DS
)
run_parity regex_flags "$regex_flags" $'svc
false
true
' 0
regex_flags_script="$TMP/regex_flags.sh"
emit_checked regex_flags_hostile "$regex_flags" "$regex_flags_script"
capture_cmd regex_flags_hostile bash -O nocasematch "$regex_flags_script"
assert_status regex_flags_hostile 0
assert_text regex_flags_hostile_stdout $'svc
false
true
' "$TMP/regex_flags_hostile.out"

for row in \
  'lookaround|let ok = "abc" matches /a(?=b)/|lookaround, inline flags, named captures' \
  'backref|let ok = "aa" matches /(a)\1/|unsupported regex escape' \
  'lazy|let ok = "abc" matches /a+?/|lazy regex quantifiers' \
  'inline|let ok = "abc" matches /(?i)abc/|lookaround, inline flags, named captures' \
  'named|let ok = "abc" matches /(?P<x>a)/|lookaround, inline flags, named captures' \
  'unicode|let ok = "abc" matches /\p{L}/|unsupported regex escape' \
  'malformed|let ok = "abc" matches /(abc/|invalid regex pattern' \
  'flag|let ok = "abc" matches /abc/z|invalid regex literal' \
  'too_many|let m = regex.match("abcdefghij", /(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)/)|more than nine capture groups' \
  'bad_repl_ref|let out = regex.replace("api-123", /([a-z]+)-([0-9]+)/, "$3")|references a capture group that cannot exist'; do
  IFS='|' read -r case_name code needle <<<"$row"
  fixture=$(write_fixture "regex_static_${case_name}" <<DS
$code
DS
)
  assert_rejected "regex_static_${case_name}" "$fixture" "$needle"
done
IFS=$' \t\n'

regex_dynamic_bad=$(write_fixture regex_dynamic_bad <<'DS'
script {
  arg pattern: string
  option repl: string = "x"
  option flags: string = ""
}
echo before
let out = regex.replace("abc", pattern, repl, flags)
echo $out
file.write("marker.txt", "bad")
DS
)
run_runtime_failure regex_dynamic_pattern "$regex_dynamic_bad" 'invalid regex pattern' $'before
' '('
run_runtime_failure regex_dynamic_flags "$regex_dynamic_bad" 'regex flags must be either an empty string or `i`' $'before
' 'a' --repl x --flags g
run_runtime_failure regex_dynamic_repl "$regex_dynamic_bad" 'regex replacement supports only `$0` through `$9` and `$$`' $'before
' 'a' --repl '$x'
run_runtime_failure regex_dynamic_zero "$regex_dynamic_bad" 'regex.replace patterns that match empty strings are unsupported' $'before
' '^' --repl x

regex_large=$(write_fixture regex_large <<'DS'
let text = "x1 x2 x3 x4 x5 x6 x7 x8 x9 x10"
let out = regex.replace(text, /x([0-9]+)/, "n$1")
echo $out
DS
)
run_parity regex_large "$regex_large" $'n1 n2 n3 n4 n5 n6 n7 n8 n9 n10
' 0

# 5. Generated Bash helper dependency and standalone hygiene.
literal_only=$(write_fixture literal_only <<'DS'
let ok = "api" matches /api/
echo $ok
DS
)
literal_script="$TMP/literal_only.sh"
emit_checked literal_only "$literal_only" "$literal_script"
assert_helper_absent "$literal_script" '__ds_regex_test()' 'literal-only regex omits runtime regex helper'
assert_helper_absent "$literal_script" '__ds_regex_match_into()' 'literal-only regex omits match helper'
assert_helper_absent "$literal_script" '__ds_regex_replace()' 'literal-only regex omits replace helper'

runtime_match_only=$(write_fixture runtime_match_only <<'DS'
let pattern = "^api$"
let ok = "api" matches pattern
echo $ok
DS
)
runtime_script="$TMP/runtime_match_only.sh"
emit_checked runtime_match_only "$runtime_match_only" "$runtime_script"
assert_helper_present "$runtime_script" '__ds_regex_test()' 'runtime string matches emits base regex helper'
assert_helper_absent "$runtime_script" '__ds_regex_match_into()' 'runtime string matches omits map helper'
assert_helper_absent "$runtime_script" '__ds_regex_replace()' 'runtime string matches omits replace helper'

match_only=$(write_fixture match_only <<'DS'
let m = regex.match("v1", /^v([0-9]+)$/)
let matched = m.matched
echo $matched
DS
)
match_script="$TMP/match_only.sh"
emit_checked match_only "$match_only" "$match_script"
assert_helper_present "$match_script" '__ds_regex_match_into()' 'regex.match emits map helper'
assert_helper_absent "$match_script" '__ds_regex_replace()' 'regex.match omits replace helper'

replace_only=$(write_fixture replace_only <<'DS'
let out = regex.replace("api-123", /([a-z]+)-([0-9]+)/, "$1:$2")
echo $out
DS
)
replace_script="$TMP/replace_only.sh"
emit_checked replace_only "$replace_only" "$replace_script"
assert_helper_present "$replace_script" '__ds_regex_replace()' 'regex.replace emits replace helper'
assert_helper_absent "$replace_script" '__ds_regex_match_into()' 'regex.replace omits match helper'

nonrecursive_glob=$(write_fixture nonrecursive_glob <<'DS'
for file in glob("*.ds") { echo $file }
DS
)
nonrecursive_script="$TMP/nonrecursive_glob.sh"
emit_checked nonrecursive_glob "$nonrecursive_glob" "$nonrecursive_script"
assert_helper_present "$nonrecursive_script" '__ds_stdlib_glob()' 'non-recursive glob emits stdlib glob helper'
assert_helper_absent "$nonrecursive_script" '__ds_glob_recursive()' 'non-recursive glob omits recursive helper'
recursive_script="$TMP/recursive_glob.sh"
emit_checked recursive_glob "$glob_basic" "$recursive_script"
assert_helper_present "$recursive_script" '__ds_glob_recursive()' 'recursive glob emits recursive helper'
assert_helper_count "$recursive_script" '__ds_glob_recursive()' 1 'recursive glob helper emitted once'

collection_script="$TMP/collection_return.sh"
emit_checked collection_return_helpers "$collection_return" "$collection_script"
assert_helper_present "$collection_script" '__ds_call_value_capture()' 'collection returns emit structured return capture helper'
assert_helper_present "$collection_script" '__ds_map_sorted_keys()' 'map iteration emits sorted key helper'
assert_helper_absent "$collection_script" '__ds_regex_replace()' 'collection-only script omits regex replace helper'
assert_helper_absent "$collection_script" '__ds_glob_recursive()' 'collection-only script omits recursive glob helper'

helper_positions=$(write_fixture helper_positions <<'DS'
fn show(x = "") { echo $x }
fn normalize(x = "") { return regex.replace(x, /([a-z]+)-([0-9]+)/, "$1:$2") }
let xs = ["api-1", "web-2"]
let shown = regex.replace("api-123", /([a-z]+)-([0-9]+)/, "$1:$2")
show(shown)
let interp = regex.replace("web-456", /([a-z]+)-([0-9]+)/, "$1:$2")
echo "interp={interp}"
let cmd_value = regex.replace("worker-7", /([a-z]+)-([0-9]+)/, "$1:$2")
printf "%s\n" "cmd={cmd_value}"
for x in xs {
  let normalized = normalize(x)
  echo $normalized
}
let if_value = regex.replace("api-1", /1/, "2")
if if_value == "api-2" { echo if-ok }
let case_value = regex.replace("a", /a/, "b")
case case_value { "b" { echo case-ok } }
let returnless = normalize("job-9")
test "helper in test" { assert normalize("api-3") == "api:3" }
DS
)
run_parity helper_positions "$helper_positions" $'api:123
interp=web:456
cmd=worker:7
api:1
web:2
if-ok
case-ok
' 0
run_ok helper_positions_test "$DS" test "$helper_positions"

handler_helpers=$(write_fixture handler_helpers <<'DS'
defer {
  let clean = regex.replace("cleanup-1", /([a-z]+)-([0-9]+)/, "$1:$2")
  echo $clean
}
trap "EXIT" {
  let trap_value = regex.replace("trap-2", /([a-z]+)-([0-9]+)/, "$1:$2")
  echo $trap_value
}
echo main
DS
)
handler_script="$TMP/handler_helpers.sh"
emit_checked handler_helpers "$handler_helpers" "$handler_script"
assert_helper_present "$handler_script" '__ds_regex_replace()' 'defer/trap bodies emit regex replace helper dependencies'
run_parity handler_helpers "$handler_helpers" $'main
trap:2
cleanup:1
' 0

# 6. Documentation/examples reconciliation.
for doc in docs/status.md docs/language.ds docs/runtime.md docs/parity-contracts.md docs/diagnostics.md docs/source-map.md docs/concept-map.md docs/technical-debt.md docs/release-checklist.md; do
  assert_doc_not_contains "$doc" 'recursive glob remains deferred' "current doc does not defer recursive glob: $doc"
  assert_doc_not_contains "$doc" 'runtime regex strings remain deferred' "current doc does not defer runtime regex strings: $doc"
  assert_doc_not_contains "$doc" 'regex captures remain deferred' "current doc does not defer regex captures: $doc"
  assert_doc_not_contains "$doc" 'regex replacement remains deferred' "current doc does not defer regex replacement: $doc"
  assert_doc_not_contains "$doc" 'map iteration remains deferred' "current doc does not defer map iteration: $doc"
  assert_doc_not_contains "$doc" 'index assignment remains deferred' "current doc does not defer index assignment: $doc"
  assert_doc_not_contains "$doc" 'structured returns remain deferred' "current doc does not defer structured returns: $doc"
done
assert_doc_contains docs/runtime.md 'flat scalar collection boundary' 'runtime docs describe flat collection boundary'
assert_doc_contains docs/runtime.md 'structured function returns' 'runtime docs describe structured returns'
assert_doc_contains docs/runtime.md 'command-result' 'runtime docs describe command-result fields'
assert_doc_contains docs/runtime.md 'recursive `**`' 'runtime docs describe recursive glob contract'
assert_doc_contains docs/runtime.md 'runtime string patterns' 'runtime docs describe runtime regex strings'
assert_doc_contains docs/release-checklist.md 'v0.36.0' 'release checklist records current DX pass'

run_example_parity() {
  local name="$1" file="$2"
  local script="$TMP/${name}.sh"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"
  capture_cmd "${name}_vm" "$DS" run "$file"
  capture_cmd "${name}_bash" bash "$script"
  assert_status "${name}_vm" 0
  assert_status "${name}_bash" 0
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name example stdout parity"
  assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name example stderr parity"
}

for ex in collections command-result filtering function-values functions import-main regex stdlib strings; do
  run_example_parity "example_${ex}" "examples/${ex}.ds"
done
capture_cmd bad_example "$DS" check examples/bad.ds
assert_nonzero_status bad_example
assert_contains "$TMP/bad_example.err" ': error:' 'examples/bad.ds remains intentionally invalid'

# 7. Diagnostics ownership and fail-fast behavior.
parser_bad_collection=$(write_fixture parser_bad_collection <<'DS'
let xs = ["a",
DS
)
assert_check_fails parser_bad_collection "$parser_bad_collection" 'expected `]` to close array literal'
parser_bad_loop=$(write_fixture parser_bad_loop <<'DS'
for item ["a"] { echo $item }
DS
)
assert_check_fails parser_bad_loop "$parser_bad_loop" 'expected `in`'
unsupported_interp=$(write_fixture unsupported_interp <<'DS'
let xs = ["a"]
echo "xs={xs}"
DS
)
assert_rejected unsupported_interp "$unsupported_interp" 'cannot interpolate array value'

structured_guard=$(write_fixture structured_guard <<'DS'
fn bad() {
  echo leaked
  return ["a"]
}
let xs = bad()
echo after
DS
)
assert_rejected structured_guard "$structured_guard" 'contains plain command statements'

# 8. Manual review evidence markers.
assert_doc_contains docs/technical-debt.md 'helper scanning remains discovery-only' 'H1 reviewer note recorded'
assert_doc_contains docs/technical-debt.md 'runtime concern mix remains concentrated in `src/vm_stdlib.c`' 'H2 reviewer note recorded'
assert_doc_contains docs/technical-debt.md 'generated-helper catalog remains broad' 'H3 reviewer note recorded'
assert_doc_contains docs/technical-debt.md 'collection portability gates remain lowerer-owned' 'H4 reviewer note recorded'
assert_doc_contains docs/technical-debt.md 'standalone Bash intentionally duplicates regex validation' 'H5 reviewer note recorded'
assert_doc_contains docs/technical-debt.md 'examples and release checklist were reconciled' 'H6 reviewer note recorded'
assert_doc_contains docs/technical-debt.md 'large dispatchers were not broadened' 'H7 reviewer note recorded'
assert_doc_contains docs/technical-debt.md 'aggregate regression runtime remains a known operational risk' 'H8 reviewer note recorded'

echo "v0.33.0 tests passed ($pass_count checks)"
