#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
DS="$ROOT/ds"
CASE_TIMEOUT=${DS_TEST_CASE_TIMEOUT:-30}
TMP=${TMPDIR:-/tmp}/ds_v0_36_tests.$$
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

assert_helper_count() {
  local script="$1" helper="$2" expected="$3" name="$4" count
  count=$(grep -c -F -- "$helper()" "$script" || true)
  [ "$count" = "$expected" ] || fail "$name: expected $helper definition count $expected, got $count"
  pass "$name"
}

emit_basic() {
  local name="$1" file="$2" script="$3"
  run_ok "${name}_emit" "$DS" emit bash "$file" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
}

emit_checked() {
  local name="$1" file="$2" script="$3"
  emit_basic "$name" "$file" "$script"
  assert_no_ds_call "$script" "$name emitted Bash standalone"
  assert_no_duplicate_helpers "$script" "$name emitted Bash"
}

run_parity() {
  local name="$1" file="$2" expected_stdout="$3"
  shift 3
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  rm -rf "$vm_work" "$bash_work"
  mkdir -p "$vm_work" "$bash_work"
  run_ok "${name}_check" "$DS" check "$file"
  emit_basic "$name" "$file" "$script"
  set +e
  (cd "$vm_work" && timeout "$CASE_TIMEOUT" "$DS" run "$file" "$@") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && timeout "$CASE_TIMEOUT" bash "$script" "$@") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"
  assert_status "${name}_vm" 0
  assert_status "${name}_bash" 0
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name VM/Bash stdout parity"
  assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name VM/Bash stderr parity"
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
}

run_bash_hostile() {
  local name="$1" file="$2" expected_stdout="$3"
  local script="$TMP/$name.sh" work="$TMP/${name}_hostile_work"
  rm -rf "$work"
  mkdir -p "$work"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"
  set +e
  (cd "$work" && IFS=':' LC_ALL=C LANG=C timeout "$CASE_TIMEOUT" \
    bash -c 'set -euo pipefail; clean() { printf user-clean; }; bash "$1"' bash "$script") \
    >"$TMP/${name}.out" 2>"$TMP/${name}.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/${name}.rc"
  assert_status "$name" 0
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}.out"
}

assert_failed_artifact_absent() {
  local path="$1" name="$2"
  [ ! -s "$path" ] || { echo "--- $path" >&2; cat "$path" >&2 || true; fail "$name: expected failed emit to leave no usable artifact"; }
  pass "$name"
}

assert_static_rejected() {
  local name="$1" file="$2" needle="$3"
  local script="$TMP/$name.sh"
  for cmd in check hir bytecode run; do
    capture_cmd "${name}_${cmd}" "$DS" "$cmd" "$file"
    assert_nonzero_status "${name}_${cmd}"
    assert_contains "$TMP/${name}_${cmd}.err" ': error:' "$name $cmd diagnostic shape"
    assert_contains "$TMP/${name}_${cmd}.err" "$needle" "$name $cmd diagnostic message"
  done
  capture_cmd "${name}_emit" "$DS" emit bash "$file" -o "$script"
  assert_nonzero_status "${name}_emit"
  assert_contains "$TMP/${name}_emit.err" ': error:' "$name emit diagnostic shape"
  assert_contains "$TMP/${name}_emit.err" "$needle" "$name emit diagnostic message"
  assert_failed_artifact_absent "$script" "$name failed emit artifact"
}

assert_run_rejected_without_marker() {
  local name="$1" file="$2" needle="$3" marker="$4"
  local work="$TMP/${name}_run_work"
  rm -rf "$work"
  mkdir -p "$work"
  set +e
  (cd "$work" && timeout "$CASE_TIMEOUT" "$DS" run "$file") >"$TMP/${name}_run.out" 2>"$TMP/${name}_run.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/${name}_run.rc"
  assert_nonzero_status "${name}_run"
  assert_contains "$TMP/${name}_run.err" "$needle" "$name run diagnostic message"
  [ ! -e "$work/$marker" ] || fail "$name created marker $marker"
  pass "$name did not create marker $marker"
}

typed_param=$(write_fixture scope_typed_param <<'DS'
fn greet(name: string) {
  echo "{name}"
}
DS
)
assert_static_rejected scope_typed_param "$typed_param" 'typed function parameters are deferred'

return_annotation=$(write_fixture scope_return_annotation <<'DS'
fn greet(name) -> string {
  echo "{name}"
}
DS
)
assert_static_rejected scope_return_annotation "$return_annotation" 'expected `{` after function declaration'

named_arg=$(write_fixture scope_named_arg <<'DS'
fn clean(line) {
  return line.trim()
}

let value = clean(line: " api ")
echo "{value}"
DS
)
assert_static_rejected scope_named_arg "$named_arg" 'expected `)` after function call arguments'

# 2. Parser and rejected typed syntax regression.
untyped=$(write_fixture untyped_function <<'DS'
fn greet(name) {
  echo "hello {name}"
}

greet("api")
DS
)
run_parity untyped_function "$untyped" $'hello api\n'
run_ok untyped_hir "$DS" hir "$untyped"
assert_not_contains "$TMP/untyped_hir.out" 'inferred string' 'plain interpolation stays neutral in HIR'

default_before_required=$(write_fixture default_before_required <<'DS'
fn bad(name = "api", suffix) {
  echo "{name}"
}
DS
)
assert_static_rejected default_before_required "$default_before_required" 'cannot follow a default parameter'

# 3. String parameter inference.
clean=$(write_fixture clean_string <<'DS'
fn clean(line) {
  return line.trim()
}

let cleaned = clean("  api  ")
echo "{cleaned}"
DS
)
run_ok clean_string_check "$DS" check "$clean"
run_ok clean_string_hir "$DS" hir "$clean"
run_ok clean_string_bytecode "$DS" bytecode "$clean"
assert_contains "$TMP/clean_string_hir.out" 'Function clean(line: inferred string)' 'HIR shows string inferred receiver'
run_parity clean_string "$clean" $'api\n'

normalize=$(write_fixture normalize_string <<'DS'
fn normalize(line) {
  return line.trim().lower().replace("-", "_")
}

let normalized = normalize("  API-WEB  ")
echo "{normalized}"
DS
)
run_parity normalize_string "$normalize" $'api_web\n'

contains=$(write_fixture contains_args <<'DS'
fn has_marker(line, marker) {
  return line.contains(marker)
}

if has_marker("abc:def", ":") {
  echo "yes"
}
DS
)
run_parity contains_args "$contains" $'yes\n'
run_ok contains_hir "$DS" hir "$contains"
assert_contains "$TMP/contains_hir.out" 'Function has_marker(line: inferred string, marker: inferred string)' 'HIR infers string receiver and string argument'

before=$(write_fixture before_helper <<'DS'
fn before(marker, line) {
  let idx = line.index_of(marker)
  if idx < 0 {
    return line
  }
  return line.slice(0, idx)
}

let name = before("=", "name=value")
echo "{name}"
DS
)
run_parity before_helper "$before" $'name\n'

split_index=$(write_fixture split_index_inference <<'DS'
fn first_clean(line, sep) {
  return line.split(sep)[0].trim()
}

let first = first_clean("  api  : worker", ":")
echo "{first}"
DS
)
run_parity split_index_inference "$split_index" $'api\n'

interp_upper=$(write_fixture interpolation_upper_infers_string <<'DS'
fn shout(label) {
  echo "{label:upper}"
}

shout("api")
DS
)
run_parity interpolation_upper_infers_string "$interp_upper" $'API\n'
run_ok interpolation_upper_hir "$DS" hir "$interp_upper"
assert_contains "$TMP/interpolation_upper_hir.out" 'Function shout(label: inferred string)' 'HIR infers string from interpolation format'

# 4. Integer parameter inference.
triple=$(write_fixture triple_int <<'DS'
fn triple(n) {
  return n * 3
}

let value = triple(7)
echo "{value}"
DS
)
run_parity triple_int "$triple" $'21\n'

middle=$(write_fixture middle_slice <<'DS'
fn middle(text, start, end) {
  return text.slice(start, end)
}

let part = middle("abcdef", 1, 4)
echo "{part}"
DS
)
run_parity middle_slice "$middle" $'bcd\n'
run_ok middle_hir "$DS" hir "$middle"
assert_contains "$TMP/middle_hir.out" 'Function middle(text: inferred string, start: inferred int, end: inferred int)' 'HIR infers slice string and int args'

dynamic_int=$(write_fixture dynamic_int <<'DS'
fn take(text, count) {
  return text.slice(0, count + 1)
}

let n = 2
let part = take("abcdef", n)
echo "{part}"
DS
)
run_parity dynamic_int "$dynamic_int" $'abc\n'

array_index=$(write_fixture array_index_int <<'DS'
fn pick(index) {
  let values = ["zero", "one", "two"]
  return values[index]
}

let item = pick(2)
echo "{item}"
DS
)
run_parity array_index_int "$array_index" $'two\n'

int_format=$(write_fixture interpolation_int_format <<'DS'
fn pad(n) {
  echo "{n:03d}"
}

pad(7)
DS
)
run_parity interpolation_int_format "$int_format" $'007\n'
run_ok interpolation_int_hir "$DS" hir "$int_format"
assert_contains "$TMP/interpolation_int_hir.out" 'Function pad(n: inferred int)' 'HIR infers int from interpolation format'

int_compare=$(write_fixture int_comparison <<'DS'
fn size(n) {
  if n > 10 {
    return "big"
  }
  return "small"
}

let big = size(11)
let small = size(3)
echo "{big}"
echo "{small}"
DS
)
run_parity int_comparison "$int_compare" $'big\nsmall\n'

# 5. Boolean parameter inference.
bool_direct=$(write_fixture bool_direct <<'DS'
fn show(flag) {
  if flag {
    echo "yes"
  } else {
    echo "no"
  }
}

show(true)
show(false)
DS
)
run_parity bool_direct "$bool_direct" $'yes\nno\n'

bool_ops=$(write_fixture bool_ops <<'DS'
fn both(left, right) {
  return left && right
}

if both(true, true) {
  echo "both"
}
if !both(true, false) {
  echo "not-both"
}
DS
)
run_parity bool_ops "$bool_ops" $'both\nnot-both\n'

bool_case=$(write_fixture bool_case <<'DS'
fn label(flag) {
  case flag {
    true { return "enabled" }
    false { return "disabled" }
    _ { return "unknown" }
  }
}

let enabled = label(true)
let disabled = label(false)
echo "{enabled}"
echo "{disabled}"
DS
)
run_parity bool_case "$bool_case" $'enabled\ndisabled\n'
run_ok bool_case_hir "$DS" hir "$bool_case"
assert_contains "$TMP/bool_case_hir.out" 'Function label(flag: inferred bool)' 'HIR infers bool from case patterns'

# 6. Neutral uses and unknown parameters.
neutral=$(write_fixture neutral_interpolation <<'DS'
fn show(value) {
  echo "value={value}"
}

show("api")
show(7)
show(true)
DS
)
run_parity neutral_interpolation "$neutral" $'value=api\nvalue=7\nvalue=true\n'
run_ok neutral_hir "$DS" hir "$neutral"
assert_not_contains "$TMP/neutral_hir.out" 'Function show(value: inferred' 'plain interpolation does not infer scalar kind'
neutral_script="$TMP/neutral.sh"
emit_checked neutral_interpolation "$neutral" "$neutral_script"
assert_not_contains "$neutral_script" 'function parameter kind mismatch' 'neutral function emits no inferred parameter validator'

identity=$(write_fixture identity_unknown <<'DS'
fn id(value) {
  return value
}

let value = id("api")
echo "{value}"
DS
)
assert_static_rejected identity_unknown "$identity" 'function return value kind must be known'

relay=$(write_fixture relay_neutral <<'DS'
fn relay(value) {
  let copy = value
  echo "{copy}"
}

relay("api")
DS
)
run_parity relay_neutral "$relay" $'api\n'

# 7. Call-site validation.
wrong_string=$(write_fixture wrong_string_arg <<'DS'
fn clean(line) {
  echo "entered" |> "body-marker.txt"
  return line.trim()
}

clean(42)
echo "after" |> "after-marker.txt"
DS
)
assert_static_rejected wrong_string_arg "$wrong_string" 'expects argument 1 `line` to be string, got int'
assert_run_rejected_without_marker wrong_string_arg "$wrong_string" 'expects argument 1 `line` to be string, got int' 'body-marker.txt'

wrong_int=$(write_fixture wrong_int_arg <<'DS'
fn add_one(n) {
  return n + 1
}

let value = add_one("1")
echo "{value}"
DS
)
assert_static_rejected wrong_int_arg "$wrong_int" 'expects argument 1 `n` to be int, got string'

wrong_bool=$(write_fixture wrong_bool_arg <<'DS'
fn enabled(flag) {
  if flag {
    echo "yes"
  }
}

enabled("true")
DS
)
assert_static_rejected wrong_bool_arg "$wrong_bool" 'expects argument 1 `flag` to be bool, got string'

clean_twice=$(write_fixture clean_twice <<'DS'
fn clean(line) {
  return line.trim()
}

fn clean_twice(line) {
  return clean(clean(line))
}

let cleaned = clean_twice("  api  ")
echo "{cleaned}"
DS
)
run_parity clean_twice "$clean_twice" $'api\n'
run_ok clean_twice_hir "$DS" hir "$clean_twice"
assert_contains "$TMP/clean_twice_hir.out" 'Function clean_twice(line: inferred string)' 'callee requirement infers caller parameter'

callee_conflict=$(write_fixture callee_conflict <<'DS'
fn clean(line) {
  return line.trim()
}

fn double(n) {
  return n * 2
}

fn bad(value) {
  let cleaned = clean(value)
  let doubled = double(value)
  echo "{cleaned}"
  echo "{doubled}"
}
DS
)
assert_static_rejected callee_conflict "$callee_conflict" 'inferred as string but later used as int'

# 8. Conflict diagnostics inside function bodies.
string_int_conflict=$(write_fixture string_int_conflict <<'DS'
fn bad(value) {
  let cleaned = value.trim()
  let doubled = value * 2
  echo "{cleaned}:{doubled}"
}
DS
)
assert_static_rejected string_int_conflict "$string_int_conflict" 'inferred as string but later used as int'

bool_string_conflict=$(write_fixture bool_string_conflict <<'DS'
fn bad(flag) {
  if flag {
    echo "enabled"
  }
  let upper = flag.upper()
  echo "{upper}"
}
DS
)
assert_static_rejected bool_string_conflict "$bool_string_conflict" 'inferred as bool but later used as string'

int_bool_conflict=$(write_fixture int_bool_conflict <<'DS'
fn bad(value) {
  let next = value + 1
  if value {
    echo "{next}"
  }
}
DS
)
assert_static_rejected int_bool_conflict "$int_bool_conflict" 'inferred as int but later used as bool'

nested_conflict=$(write_fixture nested_conflict <<'DS'
fn clean(line) {
  return line.trim()
}

fn bad(value) {
  let text = clean(value)
  let next = value + 1
  echo "{text}:{next}"
}
DS
)
assert_static_rejected nested_conflict "$nested_conflict" 'inferred as string but later used as int'

# 9. Defaulted parameter preservation.
default_string=$(write_fixture default_string <<'DS'
fn label(name = " api ") {
  return name.trim().upper()
}

let default_label = label()
let custom_label = label(" web ")
echo "{default_label}"
echo "{custom_label}"
DS
)
run_parity default_string "$default_string" $'API\nWEB\n'

default_wrong=$(write_fixture default_wrong_kind <<'DS'
fn label(name = "api") {
  return name.upper()
}

let value = label(42)
echo "{value}"
DS
)
assert_static_rejected default_wrong_kind "$default_wrong" 'expects argument 1 `name` to be string, got int'

required_before_default=$(write_fixture required_before_default <<'DS'
fn join_clean(left, right = "suffix") {
  let l = left.trim()
  let r = right.trim()
  return "{l}:{r}"
}

let default_joined = join_clean(" api ")
let custom_joined = join_clean(" api ", " worker ")
echo "{default_joined}"
echo "{custom_joined}"
DS
)
run_parity required_before_default "$required_before_default" $'api:suffix\napi:worker\n'

default_conflict=$(write_fixture default_conflict <<'DS'
fn bad(value = "api") {
  let next = value + 1
  echo "{next}"
}
DS
)
assert_static_rejected default_conflict "$default_conflict" 'inferred as string but later used as int'

# 10. Return-kind interaction.
return_inferred=$(write_fixture return_inferred_param <<'DS'
fn echo_clean(line) {
  let t = line.trim()
  return line
}

let cleaned = echo_clean(" api ").trim()
echo "{cleaned}"
DS
)
run_parity return_inferred_param "$return_inferred" $'api\n'

mixed_return=$(write_fixture mixed_return <<'DS'
fn bad(flag, text) {
  if flag {
    return text.trim()
  }
  return 1
}
DS
)
assert_static_rejected mixed_return "$mixed_return" 'all return statements in a function must have the same value kind'

statement_ignore=$(write_fixture statement_ignore_return <<'DS'
fn clean(line) {
  return line.trim()
}

clean(" api ")
echo "done"
DS
)
run_parity statement_ignore_return "$statement_ignore" $'done\n'

# 11. Same-file, forward, and imported functions.
forward=$(write_fixture forward_call <<'DS'
let cleaned = clean("  api  ")
echo "{cleaned}"

fn clean(line) {
  return line.trim()
}
DS
)
run_parity forward_call "$forward" $'api\n'

cat >"$FIX/lib_clean.ds" <<'DS'
fn clean(line) {
  return line.trim()
}
DS
imported=$(write_fixture imported_clean <<'DS'
import "./lib_clean.ds"

let cleaned = clean("  api  ")
echo "{cleaned}"
DS
)
run_parity imported_clean "$imported" $'api\n'

import_wrong=$(write_fixture imported_wrong_kind <<'DS'
import "./lib_clean.ds"

clean(42)
echo "after" |> "after-marker.txt"
DS
)
assert_static_rejected imported_wrong_kind "$import_wrong" 'expects argument 1 `line` to be string, got int'

cat >"$FIX/lib_bad.ds" <<'DS'
fn bad(value) {
  let cleaned = value.trim()
  let next = value + 1
  echo "{cleaned}"
  echo "{next}"
}
DS
import_conflict=$(write_fixture imported_conflict <<'DS'
import "./lib_bad.ds"

bad("api")
DS
)
assert_static_rejected imported_conflict "$import_conflict" 'inferred as string but later used as int'

# 12. Script arguments, options, and command-result fields.
script_arg=$(write_fixture script_arg_clean <<'DS'
script {
  arg name: string
}

fn clean(line) {
  return line.trim()
}

let cleaned = clean(name)
echo "{cleaned}"
DS
)
run_parity script_arg_clean "$script_arg" $'api\n' '  api  '

script_option=$(write_fixture script_option_int <<'DS'
script {
  option count: int = 2
}

fn repeat_count(n) {
  return n + 1
}

let repeated = repeat_count(count)
echo "{repeated}"
DS
)
run_parity script_option_int "$script_option" $'3\n'

cmd_stdout=$(write_fixture command_stdout <<'DS'
fn clean(line) {
  return line.trim()
}

let result = run printf "  api  "
let cleaned = clean(result.stdout)
echo "{cleaned}"
DS
)
run_parity command_stdout "$cmd_stdout" $'api\n'

cmd_code=$(write_fixture command_code <<'DS'
fn bump(code) {
  return code + 1
}

let result = run false
let bumped = bump(result.code)
echo "{bumped}"
DS
)
run_parity command_code "$cmd_code" $'2\n'

cmd_ok=$(write_fixture command_ok <<'DS'
fn label(ok) {
  if ok {
    return "ok"
  }
  return "bad"
}

let result = run false
let status = label(result.ok)
echo "{status}"
DS
)
run_parity command_ok "$cmd_ok" $'bad\n'

# 13. Collections and known element kinds.
array_element=$(write_fixture array_element <<'DS'
fn clean(line) {
  return line.trim()
}

let names = [" api ", " web "]
let cleaned = clean(names[1])
echo "{cleaned}"
DS
)
run_parity array_element "$array_element" $'web\n'

split_element=$(write_fixture split_element <<'DS'
fn clean(line) {
  return line.trim()
}

let first = " api : web".split(":")[0]
let cleaned = clean(first)
echo "{cleaned}"
DS
)
run_parity split_element "$split_element" $'api\n'

array_loop=$(write_fixture array_loop <<'DS'
fn clean(line) {
  return line.trim()
}

let names = [" api ", " web "]
for name in names {
  let cleaned = clean(name)
  echo "{cleaned}"
}
DS
)
run_parity array_loop "$array_loop" $'api\nweb\n'

map_value=$(write_fixture map_value <<'DS'
fn clean(line) {
  return line.trim()
}

let labels = {"app": " api "}
let cleaned = clean(labels["app"])
echo "{cleaned}"
DS
)
run_parity map_value "$map_value" $'api\n'

# 14. Case, comparisons, and conditions.
string_case=$(write_fixture string_case <<'DS'
fn kind(name) {
  case name {
    "api" { return "service" }
    "web" { return "frontend" }
    _ { return "other" }
  }
}

let label = kind("api")
echo "{label}"
DS
)
run_parity string_case "$string_case" $'service\n'

same_unknown=$(write_fixture same_unknown <<'DS'
fn same(left, right) {
  if left == right {
    echo "same"
  }
}

same("a", "a")
DS
)
run_parity same_unknown "$same_unknown" $'same\n'
run_ok same_unknown_hir "$DS" hir "$same_unknown"
assert_not_contains "$TMP/same_unknown_hir.out" 'Function same(left: inferred' 'unknown-vs-unknown equality stays neutral'

# 15. Test blocks and checker integration.
test_ok=$(write_fixture test_block_ok <<'DS'
fn clean(line) {
  return line.trim()
}

test "clean trims" {
  assert clean(" api ") == "api"
}
DS
)
run_ok test_block_ok "$DS" test "$test_ok"
assert_contains "$TMP/test_block_ok.out" '1 tests, 1 passed, 0 failed' 'test block infers helper successfully'
run_parity test_block_run_ignored "$test_ok" $''

test_bad=$(write_fixture test_block_bad <<'DS'
fn clean(line) {
  return line.trim()
}

test "bad" {
  clean(42)
}
DS
)
assert_static_rejected test_block_bad "$test_bad" 'expects argument 1 `line` to be string, got int'

warning_fixture=$(write_fixture warning_mode <<'DS'
fn clean(line) {
  let unused = line.trim()
  return "ok"
}

let result = clean(" api ")
echo "{result}"
DS
)
run_ok warning_normal_check "$DS" check "$warning_fixture"
capture_cmd warning_as_errors "$DS" check "$warning_fixture" --warnings-as-errors
assert_nonzero_status warning_as_errors
assert_contains "$TMP/warning_as_errors.err" 'warning:' 'warnings-as-errors still reports warnings separately'

# 16. HIR, bytecode, and formatter visibility.
hir_sample=$(write_fixture hir_sample <<'DS'
fn sample(text, n, flag) {
  let clean = text.trim()
  let next = n + 1
  if flag {
    echo "{clean}:{next}"
  }
}
DS
)
run_ok hir_sample "$DS" hir "$hir_sample"
assert_contains "$TMP/hir_sample.out" 'text: inferred string' 'HIR shows inferred string parameter'
assert_contains "$TMP/hir_sample.out" 'n: inferred int' 'HIR shows inferred int parameter'
assert_contains "$TMP/hir_sample.out" 'flag: inferred bool' 'HIR shows inferred bool parameter'

neutral_hir=$(write_fixture neutral_hir <<'DS'
fn neutral(value) {
  echo "{value}"
}
DS
)
run_ok neutral_hir_cmd "$DS" hir "$neutral_hir"
assert_contains "$TMP/neutral_hir_cmd.out" 'Function neutral(value: unknown)' 'HIR keeps unknown required parameter visible'
assert_not_contains "$TMP/neutral_hir_cmd.out" 'value: inferred' 'HIR does not invent unknown kind'

run_ok bytecode_sample "$DS" bytecode "$hir_sample"
assert_contains "$TMP/bytecode_sample.out" 'param 0 text: string' 'bytecode shows inferred string param'
assert_contains "$TMP/bytecode_sample.out" 'param 1 n: int' 'bytecode shows inferred int param'
assert_contains "$TMP/bytecode_sample.out" 'param 2 flag: bool' 'bytecode shows inferred bool param'

fmt_fixture=$(write_fixture fmt_inference <<'DS'
fn clean( line ) { return line.trim() }
DS
)
capture_cmd fmt_check "$DS" fmt --check "$fmt_fixture"
assert_nonzero_status fmt_check
fmt_out="$TMP/fmt_inference_out.ds"
cp "$fmt_fixture" "$fmt_out"
run_ok fmt_write "$DS" fmt --write "$fmt_out"
assert_contains "$fmt_out" 'fn clean(line) {' 'formatter normalizes parameter spacing without typed syntax'
run_ok fmt_written_check "$DS" check "$fmt_out"

# 17. Bash emission hygiene and standalone behavior.
hygiene=$(write_fixture bash_hygiene <<'DS'
fn clean(line) {
  return line.trim()
}

fn tag(line) {
  return clean(line).upper()
}

fn yes(flag) {
  if flag {
    return "yes"
  }
  return "no"
}

let label = tag(" api ")
let answer = yes(false)
echo "{label}"
echo "{answer}"
DS
)
hygiene_script="$TMP/bash_hygiene.sh"
run_parity bash_hygiene "$hygiene" $'API\nno\n'
emit_checked bash_hygiene "$hygiene" "$hygiene_script"
assert_contains "$hygiene_script" 'function parameter kind mismatch' 'emitted Bash validates inferred parameters'
assert_contains "$hygiene_script" '__ds_type_line' 'emitted Bash carries parameter kind metadata'
assert_helper_count "$hygiene_script" '__ds_string_trim' 1 'trim helper emitted once'
assert_helper_count "$hygiene_script" '__ds_string_upper' 1 'upper helper emitted once'
run_bash_hostile bash_hygiene_hostile "$hygiene" $'API\nno\n'

neutral_script2="$TMP/neutral_no_helpers.sh"
emit_checked neutral_no_helpers "$neutral" "$neutral_script2"
assert_not_contains "$neutral_script2" 'function parameter kind mismatch' 'neutral function does not emit inferred kind validator'

# 18. Fail-fast and side-effect ordering.
outer_wrong=$(write_fixture outer_wrong <<'DS'
fn clean(line) {
  return line.trim()
}

fn outer(value) {
  let cleaned = clean(value)
  echo "after-clean" |> "outer.txt"
  echo "{cleaned}"
}

outer(1)
DS
)
assert_static_rejected outer_wrong "$outer_wrong" 'expects argument 1 `value` to be string, got int'
assert_run_rejected_without_marker outer_wrong "$outer_wrong" 'expects argument 1 `value` to be string, got int' 'outer.txt'

unused_conflict=$(write_fixture unused_conflict <<'DS'
fn bad(value) {
  let cleaned = value.trim()
  let next = value + 1
  echo "{cleaned}"
  echo "{next}"
}

echo "main"
DS
)
assert_static_rejected unused_conflict "$unused_conflict" 'inferred as string but later used as int'

# 19. Recursion and circular inference.
recurse=$(write_fixture recurse_existing_restriction <<'DS'
fn recurse(line) {
  if line.trim() == "" {
    return "done"
  }
  return recurse(line.trim())
}
DS
)
assert_static_rejected recurse_existing_restriction "$recurse" 'recursive function calls are deferred'

circular=$(write_fixture circular_unknown <<'DS'
fn a(value) {
  return b(value)
}

fn b(value) {
  return a(value)
}
DS
)
assert_static_rejected circular_unknown "$circular" 'recursive function calls are deferred'

# 20. Edge-case matrix smoke.
edge=$(write_fixture edge_matrix <<'DS'
fn clean(line) {
  return line.trim()
}

fn first_piece(line, sep) {
  return line.split(sep)[0].trim()
}

fn prev(n) {
  return n - 1
}

fn label(flag) {
  if flag {
    return "true"
  }
  return "false"
}

let empty = clean("")
let newline = clean("\n api \n")
let meta = first_piece("$HOME:*?[", ":")
let minus = prev(0)
let false_label = label(false)
echo "empty={empty}"
echo "newline={newline}"
echo "meta={meta}"
echo "minus={minus}"
echo "false={false_label}"
DS
)
run_parity edge_matrix "$edge" $'empty=\nnewline=api\nmeta=$HOME\nminus=-1\nfalse=false\n'

bool_string_false=$(write_fixture bool_string_false <<'DS'
fn label(flag) {
  if flag {
    echo "yes"
  }
}

label("false")
DS
)
assert_static_rejected bool_string_false "$bool_string_false" 'expects argument 1 `flag` to be bool, got string'

# 21. Realistic analyzer-style smoke.
analyzer=$(write_fixture analyzer_smoke <<'DS'
fn before_paren(sig) {
  return sig.split("(")[0].trim()
}

fn function_name(sig) {
  let head = before_paren(sig)
  let pieces = head.split(" ")
  return pieces[2].trim()
}

let name = function_name("static int parse_expr(Parser *p)")
echo "{name}"
DS
)
run_parity analyzer_smoke "$analyzer" $'parse_expr\n'

echo "v0.36 tests passed ($pass_count assertions)"
