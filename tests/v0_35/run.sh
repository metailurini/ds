#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
DS="$ROOT/ds"
CASE_TIMEOUT=${DS_TEST_CASE_TIMEOUT:-30}
TMP=${TMPDIR:-/tmp}/ds_v0_35_tests.$$
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

emit_checked() {
  local name="$1" file="$2" script="$3"
  run_ok "${name}_emit" "$DS" emit bash "$file" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_no_ds_call "$script" "$name emitted Bash standalone"
  assert_no_duplicate_helpers "$script" "$name emitted Bash"
}

emit_basic() {
  local name="$1" file="$2" script="$3"
  run_ok "${name}_emit" "$DS" emit bash "$file" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
}

run_parity() {
  local name="$1" file="$2" expected_stdout="$3" expected_status="${4:-0}"
  if [ "$#" -ge 4 ]; then shift 4; else shift "$#"; fi
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
  local name="$1" file="$2" expected_stdout="$3" env_lc="$4"
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  rm -rf "$vm_work" "$bash_work"
  mkdir -p "$vm_work" "$bash_work"
  run_ok "${name}_check" "$DS" check "$file"
  emit_basic "$name" "$file" "$script"
  set +e
  (cd "$vm_work" && env LC_ALL="$env_lc" LANG="$env_lc" timeout "$CASE_TIMEOUT" "$DS" run "$file") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && env LC_ALL="$env_lc" LANG="$env_lc" timeout "$CASE_TIMEOUT" bash "$script") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"
  assert_status "${name}_vm" 0
  assert_status "${name}_bash" 0
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name locale VM/Bash stdout parity"
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
}

run_bash_hostile() {
  local name="$1" file="$2" expected_stdout="$3"
  local script="$TMP/$name.sh" work="$TMP/${name}_work"
  rm -rf "$work"
  mkdir -p "$work"
  run_ok "${name}_check" "$DS" check "$file"
  emit_basic "$name" "$file" "$script"
  set +e
  (cd "$work" && \
    IFS=':' LC_ALL=C.utf8 LANG=C.utf8 timeout "$CASE_TIMEOUT" \
    bash -c 'set -euo pipefail; shopt -s nullglob dotglob globstar 2>/dev/null || true; echo() { builtin echo "$@"; }; bash "$1"' bash "$script") \
    >"$TMP/${name}.out" 2>"$TMP/${name}.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/${name}.rc"
  assert_status "$name" 0
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}.out"
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

assert_runtime_failure_marker() {
  local name="$1" file="$2" needle="$3" expected_stdout="$4" marker="${5:-marker.txt}"
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  rm -rf "$vm_work" "$bash_work"
  mkdir -p "$vm_work" "$bash_work"
  run_ok "${name}_check" "$DS" check "$file"
  emit_basic "$name" "$file" "$script"
  set +e
  (cd "$vm_work" && timeout "$CASE_TIMEOUT" "$DS" run "$file") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && timeout "$CASE_TIMEOUT" bash "$script") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"
  assert_nonzero_status "${name}_vm"
  assert_nonzero_status "${name}_bash"
  assert_same "$TMP/${name}_vm.rc" "$TMP/${name}_bash.rc" "$name runtime status parity"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name runtime stdout parity"
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
  assert_contains "$TMP/${name}_vm.err" "$needle" "$name VM diagnostic message"
  assert_contains "$TMP/${name}_bash.err" "$needle" "$name Bash diagnostic message"
  [ ! -e "$vm_work/$marker" ] || fail "$name VM created fail-fast marker"
  pass "$name VM did not create fail-fast marker"
  [ ! -e "$bash_work/$marker" ] || fail "$name Bash created fail-fast marker"
  pass "$name Bash did not create fail-fast marker"
}

assert_success_marker() {
  local name="$1" file="$2" expected_stdout="$3" marker="$4" expected_marker="$5"
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  rm -rf "$vm_work" "$bash_work"
  mkdir -p "$vm_work" "$bash_work"
  run_ok "${name}_check" "$DS" check "$file"
  emit_basic "$name" "$file" "$script"
  (cd "$vm_work" && timeout "$CASE_TIMEOUT" "$DS" run "$file") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  (cd "$bash_work" && timeout "$CASE_TIMEOUT" bash "$script") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name VM/Bash stdout parity"
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
  assert_same_text "$expected_marker" "$vm_work/$marker" "$name VM marker content"
  assert_same_text "$expected_marker" "$bash_work/$marker" "$name Bash marker content"
}

assert_helper_def_count() {
  local script="$1" helper="$2" expected="$3" name="$4" count
  count=$(grep -c -F -- "$helper()" "$script" || true)
  [ "$count" = "$expected" ] || fail "$name: expected $helper definition count $expected, got $count"
  pass "$name"
}

assert_helper_present_once() { assert_helper_def_count "$1" "$2" 1 "$3"; }
assert_helper_absent() { assert_helper_def_count "$1" "$2" 0 "$3"; }

# 1. Planning, docs, and scope guard.
[ -f docs/milestones/v0.35.0-spec.md ] || fail 'missing v0.35 spec'
pass 'v0.35 spec exists'
[ -f docs/milestones/v0.35.0-test-plan.md ] || fail 'missing v0.35 test plan'
pass 'v0.35 test plan exists'
for helper in len index_of last_index_of count char_at slice; do
  assert_contains docs/milestones/v0.35.0-spec.md "$helper" "v0.35 spec names $helper"
done
assert_contains docs/roadmap.md 'v0.34.0 — Text Literal and Broken-Pipe DX' 'roadmap keeps v0.34 before v0.35'
assert_contains docs/roadmap.md 'v0.35.0 — Core String Parsing Helpers' 'roadmap lists v0.35 string helpers'
assert_contains docs/roadmap.md 'v0.36.0 — Function Parameter Kind Inference' 'roadmap keeps v0.36 after v0.35'
assert_contains docs/roadmap.md 'v0.37.0 — Lightweight Rows and In-Memory Data Processing' 'roadmap keeps v0.37 after v0.36'
assert_contains docs/roadmap.md 'v0.38.0 — Recursive Walk Helpers and DX Integration Cleanup' 'roadmap keeps v0.38 after v0.37'
assert_contains docs/language.ds '.index_of()' 'language docs list index_of'
assert_contains docs/language.ds 'byte-oriented' 'language docs mention byte-oriented helpers'
assert_contains docs/status.md '.slice(start, end)' 'status docs list slice helper'
assert_contains docs/runtime.md 'byte-oriented' 'runtime docs mention byte-oriented helper behavior'
assert_contains docs/diagnostics.md 'char_at' 'diagnostics docs mention char_at'
assert_contains docs/dx-issues.md '`v0.35.0` adds the core byte-oriented helpers' 'DX issues mark v0.35 helper issue addressed'
assert_contains docs/parity-contracts.md 'Command words and interpolation' 'parity docs retain command interpolation contract'

regex_split=$(write_fixture scope_regex_split <<'DS'
let xs = regex.split("a,b", ",")
DS
)
assert_rejected scope_regex_split "$regex_split" 'unknown standard-library helper `regex.split`'
row_helper=$(write_fixture scope_row_helper <<'DS'
let xs = rows.sort_by([], "name")
DS
)
assert_rejected scope_row_helper "$row_helper" 'unknown string method `sort_by`'

# 2. Helper registration, parsing, and debug visibility.
all_helpers=$(write_fixture all_helpers <<'DS'
let s = "ababa"
echo "len={s.len()}"
let first = s.index_of("ba")
let last = s.last_index_of("ba")
let count = s.count("ba")
echo "first={first}"
echo "last={last}"
echo "count={count}"
echo "char={s.char_at(1)}"
echo "slice={s.slice(1, 4)}"
DS
)
run_parity all_helpers "$all_helpers" $'len=5
first=1
last=3
count=2
char=b
slice=bab
'

unknown_method=$(write_fixture unknown_method <<'DS'
let x = "abc".substring(0, 1)
DS
)
assert_rejected unknown_method "$unknown_method" 'unknown string method `substring`'
assert_contains "$TMP/unknown_method_check.err" 'trim, upper, lower, replace, contains, split, starts_with, ends_with, len, index_of, last_index_of, count, char_at, slice' 'unknown method diagnostic lists old and new methods'

debug_fixture=$(write_fixture debug_methods <<'DS'
let x = " api:web ".trim().slice(0, 3).upper()
echo "{x}"
DS
)
for cmd in ast hir bytecode; do
  run_ok "debug_${cmd}" "$DS" "$cmd" "$debug_fixture"
  assert_contains "$TMP/debug_${cmd}.out" 'string.trim' "debug $cmd shows trim helper"
  assert_contains "$TMP/debug_${cmd}.out" 'string.slice' "debug $cmd shows slice helper"
  assert_contains "$TMP/debug_${cmd}.out" 'string.upper' "debug $cmd shows upper helper"
done

fmt_fixture=$(write_fixture fmt_chain <<'DS'
let x="a,b".split(",")[0].trim().slice(0,1)
echo "{x}"
DS
)
run_ok fmt_chain "$DS" fmt "$fmt_fixture"
assert_contains "$TMP/fmt_chain.out" 'split(",")[0].trim().slice(0, 1)' 'formatter preserves split index and method order'
fmt_out="$TMP/fmt_chain_out.ds"
cp "$TMP/fmt_chain.out" "$fmt_out"
run_parity fmt_chain_formatted "$fmt_out" $'a
'
run_parity fmt_chain_original "$fmt_fixture" $'a
'

# 3. Arity and argument-kind diagnostics.
declare -a arity_cases=(
  'len_extra|let a = "abc".len(1)|expects 1 arguments'
  'index_missing|let a = "abc".index_of()|expects 2 arguments'
  'index_extra|let a = "abc".index_of("a", 1)|expects 2 arguments'
  'last_missing|let a = "abc".last_index_of()|expects 2 arguments'
  'count_missing|let a = "abc".count()|expects 2 arguments'
  'char_missing|let a = "abc".char_at()|expects 2 arguments'
  'char_extra|let a = "abc".char_at(0, 1)|expects 2 arguments'
  'slice_missing|let a = "abc".slice(0)|expects 3 arguments'
  'slice_extra|let a = "abc".slice(0, 1, 2)|expects 3 arguments'
)
for row in "${arity_cases[@]}"; do
  IFS='|' read -r name source needle <<<"$row"
  file=$(write_fixture "arity_$name" <<<"$source")
  assert_rejected "arity_$name" "$file" "$needle"
done

declare -a receiver_cases=(
  'int_len|let a = 123.len()|requires a string receiver'
  'bool_index|let a = true.index_of("t")|requires a string receiver'
  'array_slice|let a = ["a"].slice(0, 1)|requires a string receiver'
  'map_char|let a = { name: "api" }.char_at(0)|requires a string receiver'
  'command_result|let r = run printf "abc"\nlet a = r.index_of("a")|requires a string receiver'
)
for row in "${receiver_cases[@]}"; do
  IFS='|' read -r name source needle <<<"$row"
  file=$(write_fixture "receiver_$name" <<<"$(printf '%b' "$source")")
  assert_rejected "receiver_$name" "$file" "$needle"
done

command_result_field=$(write_fixture command_result_field <<'DS'
let r = run printf "abc"
let idx = r.stdout.index_of("b")
echo "{idx}"
DS
)
run_parity command_result_field "$command_result_field" $'1
'

declare -a needle_cases=(
  'index_int|let a = "abc".index_of(1)|expects string arguments'
  'last_bool|let a = "abc".last_index_of(false)|expects string arguments'
  'count_array|let a = "abc".count(["a"])|expects string arguments'
)
for row in "${needle_cases[@]}"; do
  IFS='|' read -r name source needle <<<"$row"
  file=$(write_fixture "needle_$name" <<<"$source")
  assert_rejected "needle_$name" "$file" "$needle"
done

declare -a index_kind_cases=(
  'char_string|let a = "abc".char_at("0")|expects int index arguments'
  'slice_start_string|let a = "abc".slice("0", 1)|expects int index arguments'
  'slice_end_string|let a = "abc".slice(0, "1")|expects int index arguments'
  'slice_bools|let a = "abc".slice(true, false)|expects int index arguments'
)
for row in "${index_kind_cases[@]}"; do
  IFS='|' read -r name source needle <<<"$row"
  file=$(write_fixture "index_kind_$name" <<<"$source")
  assert_rejected "index_kind_$name" "$file" "$needle"
done

required_param=$(write_fixture required_param_boundary <<'DS'
fn f(line) {
  return line.index_of("x")
}
let out = f("abc")
echo "{out}"
DS
)
run_parity required_param_boundary "$required_param" $'-1
'

default_param=$(write_fixture default_param_workaround <<'DS'
fn f(line = "") {
  return line.index_of("x")
}
let out = f("abc")
echo "{out}"
DS
)
run_parity default_param_workaround "$default_param" $'-1
'

# 4-9. New helper semantics.
len_basic=$(write_fixture len_basic <<'DS'
let empty = ""
let abc = "abc"
let space = "a b"
let newline = "a\nb"
let tab = "a\tb"
echo "empty={empty.len()}"
echo "abc={abc.len()}"
echo "space={space.len()}"
echo "newline={newline.len()}"
echo "tab={tab.len()}"
DS
)
run_parity len_basic "$len_basic" $'empty=0
abc=3
space=3
newline=3
tab=3
'

len_metachar=$(write_fixture len_metachar <<'DS'
let dollar = "$HOME"
let sub = "$(echo bad)"
let ticks = "`echo bad`"
let quotes = "\"api\""
let braces = "{{api}}"
let glob = "*?["
let dash = "-name"
echo "dollar={dollar.len()}"
echo "sub={sub.len()}"
echo "ticks={ticks.len()}"
echo "quotes={quotes.len()}"
echo "braces={braces.len()}"
echo "glob={glob.len()}"
echo "dash={dash.len()}"
DS
)
run_parity len_metachar "$len_metachar" $'dollar=5
sub=11
ticks=10
quotes=5
braces=5
glob=3
dash=5
'

non_ascii=$(write_fixture byte_non_ascii <<'DS'
let e = "é"
echo "len={e.len()}"
echo "slice={e.slice(0, 1).len()}"
DS
)
run_parity byte_non_ascii "$non_ascii" $'len=2
slice=1
'
if locale -a | grep -qi '^C\.utf8$'; then
  run_parity_env byte_non_ascii_locale "$non_ascii" $'len=2
slice=1
' C.utf8
else
  pass 'byte_non_ascii_locale skipped: C.utf8 locale unavailable'
fi

index_basic=$(write_fixture index_basic <<'DS'
let s = "abcabc"
echo "{s.index_of("a")}"
echo "{s.index_of("b")}"
echo "{s.index_of("c")}"
echo "{s.index_of("abc")}"
echo "{s.index_of("bc")}"
echo "{s.index_of("z")}"
echo "{s.index_of("abcd")}"
DS
)
run_parity index_basic "$index_basic" $'0
1
2
0
1
-1
-1
'

index_empty=$(write_fixture index_empty <<'DS'
let s = "abc"
let empty = ""
echo "{s.index_of(empty)}"
echo "{empty.index_of(empty)}"
DS
)
run_parity index_empty "$index_empty" $'0
0
'

index_newlines=$(write_fixture index_newlines <<'DS'
let s = "first\nsecond\nthird"
let nl = "\n"
let a = s.index_of(nl)
let b = s.index_of("second")
let c = s.index_of("d\nth")
echo "{a}"
echo "{b}"
echo "{c}"
DS
)
run_parity index_newlines "$index_newlines" $'5
6
11
'

index_braces=$(write_fixture index_braces <<'DS'
let s = "{{ name: \"api\" }}"
let open = "{{"
let close = "}}"
echo "{s.index_of(open)}"
echo "{s.index_of(close)}"
DS
)
run_parity index_braces "$index_braces" $'0
14
'

last_basic=$(write_fixture last_basic <<'DS'
let s = "abcabc"
echo "{s.last_index_of("a")}"
echo "{s.last_index_of("b")}"
echo "{s.last_index_of("c")}"
echo "{s.last_index_of("abc")}"
echo "{s.last_index_of("bc")}"
echo "{s.last_index_of("z")}"
echo "{s.last_index_of("abcd")}"
DS
)
run_parity last_basic "$last_basic" $'3
4
5
3
4
-1
-1
'

last_empty_overlap=$(write_fixture last_empty_overlap <<'DS'
let abc = "abc"
let empty = ""
let aaaa = "aaaa"
let abababa = "abababa"
echo "{abc.last_index_of(empty)}"
echo "{empty.last_index_of(empty)}"
echo "{aaaa.last_index_of("aa")}"
echo "{abababa.last_index_of("aba")}"
DS
)
run_parity last_empty_overlap "$last_empty_overlap" $'3
0
2
4
'

count_cases=$(write_fixture count_cases <<'DS'
let aaaa = "aaaa"
let abababa = "abababa"
let abc = "abc"
let empty = ""
echo "{aaaa.count("aa")}"
echo "{abababa.count("aba")}"
echo "{abc.count("z")}"
echo "{empty.count("x")}"
echo "{abc.count(empty)}"
echo "{empty.count(empty)}"
let path_text = "src/runtime/hashmap.c"
let fields = "api:web:worker"
let slashes = path_text.count("/")
let field_count = fields.count(":") + 1
echo "slashes={slashes}"
echo "fields={field_count}"
DS
)
run_parity count_cases "$count_cases" $'2
2
0
0
4
1
slashes=2
fields=3
'

char_valid=$(write_fixture char_valid <<'DS'
let s = "abc"
echo "{s.char_at(0)}"
echo "{s.char_at(1)}"
echo "{s.char_at(2)}"
let last = s.len() - 1
echo "{s.char_at(last)}"
echo "{s.char_at(1 + 1)}"
DS
)
run_parity char_valid "$char_valid" $'a
b
c
c
c
'

char_meta=$(write_fixture char_meta <<'DS'
let s = "$* \"{{\n"
echo "{s.char_at(0)}"
echo "{s.char_at(1)}"
echo "{s.char_at(2)}"
echo "{s.char_at(3)}"
echo "{s.char_at(4)}"
DS
)
run_parity char_meta "$char_meta" $'$
*
 
"
{
'

char_oob=$(write_fixture char_oob <<'DS'
echo "before"
let bad = "abc".char_at(3)
echo "{bad}"
file.write("marker.txt", "bad")
DS
)
assert_runtime_failure_marker char_oob "$char_oob" 'char_at' $'before
'

char_empty=$(write_fixture char_empty <<'DS'
let bad = "".char_at(0)
echo "{bad}"
file.write("marker.txt", "bad")
DS
)
assert_runtime_failure_marker char_empty "$char_empty" 'char_at' ''

char_negative=$(write_fixture char_negative <<'DS'
let bad = "abc".char_at(-1)
echo "{bad}"
file.write("marker.txt", "bad")
DS
)
assert_runtime_failure_marker char_negative "$char_negative" 'char_at' ''

slice_basic=$(write_fixture slice_basic <<'DS'
let s = "abc"
echo "[{s.slice(0, 0)}]"
echo "[{s.slice(0, 1)}]"
echo "[{s.slice(0, 2)}]"
echo "[{s.slice(1, 3)}]"
echo "[{s.slice(0, 3)}]"
echo "[{s.slice(3, 3)}]"
DS
)
run_parity slice_basic "$slice_basic" $'[]
[a]
[ab]
[bc]
[abc]
[]
'

slice_dynamic=$(write_fixture slice_dynamic <<'DS'
let s = "prefix:name(suffix)"
let colon = s.index_of(":")
let open = s.index_of("(")
let name = s.slice(colon + 1, open)
echo "{name}"
DS
)
run_parity slice_dynamic "$slice_dynamic" $'name
'

for row in \
  'slice_neg_start|let bad = "abc".slice(-1, 1)|slice' \
  'slice_neg_end|let bad = "abc".slice(0, -1)|slice' \
  'slice_reversed|let bad = "abc".slice(2, 1)|slice' \
  'slice_end_oob|let bad = "abc".slice(0, 4)|slice' \
  'slice_empty_oob|let bad = "abc".slice(4, 4)|slice'; do
  IFS='|' read -r name source needle <<<"$row"
  file=$(write_fixture "$name" <<DS
$source
file.write("marker.txt", "bad")
DS
)
  assert_runtime_failure_marker "$name" "$file" "$needle" ''
done

slice_bytes=$(write_fixture slice_bytes <<'DS'
let s = " a\t\n\"$(x)`y`*?[{{ }} "
echo "[{s.slice(0, 3)}]"
echo "[{s.slice(3, 7)}]"
echo "[{s.slice(7, s.len())}]"
DS
)
run_parity slice_bytes "$slice_bytes" $'[ a	]
[
"$(]
[x)`y`*?[{ } ]
'

# 10. Composition with existing features.
string_chain=$(write_fixture string_chain <<'DS'
let s = "  Api:Worker  "
let out = s.trim().lower().slice(0, 3).upper()
echo "{out}"
DS
)
run_parity string_chain "$string_chain" $'API
'

int_arithmetic=$(write_fixture int_arithmetic <<'DS'
let s = "api:web:worker"
let idx = s.index_of(":")
if idx >= 0 {
  echo "{s.slice(0, idx)}"
}
echo "{s.count(":") + 1}"
echo "{s.len() - s.last_index_of(":")}"
DS
)
run_parity int_arithmetic "$int_arithmetic" $'api
3
7
'

command_fields=$(write_fixture command_fields <<'DS'
let r = run printf "  abc:def  "
let s = r.stdout.trim()
echo "{s.index_of(":")}"
echo "{s.slice(0, s.index_of(":"))}"
DS
)
run_parity command_fields "$command_fields" $'3
abc
'

function_return=$(write_fixture function_return <<'DS'
fn value() {
  return "abc:def"
}
let s = value()
echo "{s.slice(0, s.index_of(":"))}"
DS
)
run_parity function_return "$function_return" $'abc
'

cat >"$FIX/import_value.ds" <<'DS'
fn label() {
  return "api:web"
}
DS
import_value=$(write_fixture import_value_main <<DS
import "$FIX/import_value.ds"
let s = label()
echo "{s.slice(0, s.index_of(":"))}"
DS
)
run_parity import_value "$import_value" $'api
'

# 11. Direct indexing after method calls.
split_basic=$(write_fixture split_basic <<'DS'
let first = "api:web:worker".split(":")[0]
let second = "api:web:worker".split(":")[1]
let third = "api:web:worker".split(":")[2]
echo "{first}|{second}|{third}"
DS
)
run_parity split_basic "$split_basic" $'api|web|worker
'

split_chain=$(write_fixture split_chain <<'DS'
let sig = " static int main(void) "
let before = sig.split("(")[0].trim()
let name = before.split(" ")[2].trim()
echo "{name}"
DS
)
run_parity split_chain "$split_chain" $'main
'

split_parenthesized=$(write_fixture split_parenthesized <<'DS'
let value = (" a,b,c ".trim().split(","))[1].upper()
echo "{value}"
DS
)
run_parity split_parenthesized "$split_parenthesized" $'B
'

split_dynamic=$(write_fixture split_dynamic <<'DS'
let idx = 1 + 1
let value = "a:b:c".split(":")[idx]
echo "{value}"
DS
)
run_parity split_dynamic "$split_dynamic" $'c
'

split_oob=$(write_fixture split_oob <<'DS'
let bad = "a,b".split(",")[2]
echo "{bad}"
file.write("marker.txt", "bad")
DS
)
assert_runtime_failure_marker split_oob "$split_oob" 'index' ''

temporary_mutation=$(write_fixture temporary_mutation <<'DS'
"a,b".split(",")[0] = "x"
DS
)
assert_rejected temporary_mutation "$temporary_mutation" 'unknown command variable'

string_index=$(write_fixture string_index <<'DS'
let x = "abc"[0]
DS
)
assert_rejected string_index "$string_index" 'indexing requires an array or map'

# 12. Bash helper hygiene and standalone behavior.
for row in \
  'len|let s = "abc"\nlet x = s.len()\necho "{x}"|__ds_string_len' \
  'index_of|let s = "abc"\nlet x = s.index_of("b")\necho "{x}"|__ds_string_index_of' \
  'last_index_of|let s = "abcabc"\nlet x = s.last_index_of("b")\necho "{x}"|__ds_string_last_index_of' \
  'count|let s = "abcabc"\nlet x = s.count("b")\necho "{x}"|__ds_string_count' \
  'char_at|let s = "abc"\nlet x = s.char_at(1)\necho "{x}"|__ds_string_char_at' \
  'slice|let s = "abc"\nlet x = s.slice(0, 2)\necho "{x}"|__ds_string_slice'; do
  IFS='|' read -r name source helper <<<"$row"
  file=$(write_fixture "helper_$name" <<<"$(printf '%b' "$source")")
  script="$TMP/helper_$name.sh"
  emit_checked "helper_$name" "$file" "$script"
  assert_helper_present_once "$script" "$helper" "helper $name emits $helper exactly once"
  for other in \
    __ds_string_len \
    __ds_string_index_of \
    __ds_string_last_index_of \
    __ds_string_count \
    __ds_string_char_at \
    __ds_string_slice; do
    if [ "$other" != "$helper" ]; then
      assert_helper_absent "$script" "$other" "helper $name excludes unrelated $other"
    fi
  done
done

trim_split_only=$(write_fixture trim_split_only <<'DS'
let first = " a,b ".trim().split(",")[0]
echo "{first}"
DS
)
trim_split_script="$TMP/trim_split_only.sh"
emit_checked trim_split_only "$trim_split_only" "$trim_split_script"
for helper in __ds_string_len __ds_string_index_of __ds_string_last_index_of __ds_string_count __ds_string_char_at __ds_string_slice; do
  assert_helper_absent "$trim_split_script" "$helper" "trim/split-only Bash excludes $helper"
done

nested_deps=$(write_fixture nested_deps <<'DS'
let x = " a,b ".trim().split(",")[0].slice(0, 1).upper()
echo "{x}"
DS
)
nested_script="$TMP/nested_deps.sh"
emit_checked nested_deps "$nested_deps" "$nested_script"
for helper in __ds_array_get __ds_string_trim __ds_string_split __ds_string_slice __ds_string_upper; do
  assert_helper_present_once "$nested_script" "$helper" "nested deps include $helper"
done
run_bash_hostile nested_hostile "$nested_deps" $'A
'

# 13. Fail-fast and diagnostics parity.
slice_fail_fast=$(write_fixture slice_fail_fast <<'DS'
let bad = "abc".slice(2, 1)
echo "{bad}"
file.write("marker.txt", "bad")
DS
)
assert_runtime_failure_marker slice_fail_fast "$slice_fail_fast" 'slice' ''

missing_not_failure=$(write_fixture missing_not_failure <<'DS'
let s = "abc"
echo "{s.index_of("z")}"
echo "{s.last_index_of("z")}"
echo "{s.count("z")}"
file.write("marker.txt", "ok")
DS
)
assert_success_marker missing_not_failure "$missing_not_failure" $'-1
-1
0
' marker.txt 'ok'

static_or_runtime_char=$(write_fixture static_or_runtime_char <<'DS'
let bad = "abc".char_at(3)
echo "{bad}"
DS
)
assert_runtime_failure_marker static_or_runtime_char "$static_or_runtime_char" 'char_at' ''
static_or_runtime_slice=$(write_fixture static_or_runtime_slice <<'DS'
let bad = "abc".slice(0, 4)
echo "{bad}"
DS
)
assert_runtime_failure_marker static_or_runtime_slice "$static_or_runtime_slice" 'slice' ''

# 14. Existing string method regressions and interpolation boundaries.
existing_methods=$(write_fixture existing_methods <<'DS'
let s = "  Api:Worker  "
let abc = "abc"
echo "{s.trim()}"
echo "{s.upper()}"
echo "{s.lower()}"
echo "{s.replace("Api", "Web").trim()}"
echo "{s.contains("Worker")}"
let parts = "a,b,c".split(",")
echo "{parts[1]}"
echo "{abc.starts_with("ab")}"
echo "{abc.ends_with("bc")}"
DS
)
run_parity existing_methods "$existing_methods" $'Api:Worker
  API:WORKER  
  api:worker  
Web:Worker
true
b
true
true
'

split_empty=$(write_fixture split_empty <<'DS'
let a = "abc".split("")
DS
)
assert_rejected split_empty "$split_empty" 'split with an empty separator'
replace_empty=$(write_fixture replace_empty <<'DS'
let b = "abc".replace("", "x")
DS
)
assert_rejected replace_empty "$replace_empty" 'replace with an empty source'

interp_results=$(write_fixture interp_results <<'DS'
let s = "api:web"
let idx = s.index_of(":")
let len = s.len()
let prefix = s.slice(0, 3)
echo "idx={idx}, len={len}, prefix={prefix}"
printf "%s\n" "{s.slice(0, 3)}"
DS
)
run_parity interp_results "$interp_results" $'idx=3, len=7, prefix=api
api
'

command_word_boundary=$(write_fixture command_word_boundary <<'DS'
let s = "abc:def"
printf "%s\n" "{s.slice(0, 3)}"
DS
)
run_parity command_word_boundary "$command_word_boundary" $'abc
'

# 15. Realistic analyzer-style smoke.
analyzer=$(write_fixture analyzer <<'DS'
let line = "static int parse_name(const char *s) {{"
let open = line.index_of("(")
let before = line.slice(0, open).trim()
let last_space = before.last_index_of(" ")
let name = before.slice(last_space + 1, before.len())
let argc_delims = line.count(",") + 1
echo "name={name}"
echo "argc-delims={argc_delims}"
echo "first={line.char_at(0)}"
DS
)
run_parity analyzer "$analyzer" $'name=parse_name
argc-delims=1
first=s
'

# 16. Imports and multi-file behavior.
cat >"$FIX/sig.ds" <<'DS'
fn sig() {
  return "int main(void)"
}
DS
import_sig=$(write_fixture import_sig <<DS
import "$FIX/sig.ds"
let s = sig()
let open = s.index_of("(")
let prefix = s.slice(0, open)
let name = prefix.split(" ")[1]
echo "{name}"
DS
)
run_parity import_sig "$import_sig" $'main
'

cat >"$FIX/prefix.ds" <<'DS'
fn prefix(line = "") {
  let open = line.index_of("(")
  if open < 0 {
    return line
  }
  return line.slice(0, open).trim()
}
DS
import_prefix=$(write_fixture import_prefix <<DS
import "$FIX/prefix.ds"
let p = prefix(" int main(void) ")
echo "{p}"
DS
)
run_parity import_prefix "$import_prefix" $'int main
'

# 17-18. Matrix/manual smoke consolidation.
manual_smoke=$(write_fixture manual_smoke <<'DS'
let s = "  api:web:worker  ".trim()
let first_colon = s.index_of(":")
let last_colon = s.last_index_of(":")
let first = s.slice(0, first_colon)
let last = s.slice(last_colon + 1, s.len())
let fields = s.count(":") + 1
let initial = s.char_at(0).upper()
let middle = s.split(":")[1].trim()
echo "first={first}"
echo "last={last}"
echo "fields={fields}"
echo "initial={initial}"
echo "middle={middle}"
DS
)
run_parity manual_smoke "$manual_smoke" $'first=api
last=worker
fields=3
initial=A
middle=web
'

run_ok cli_help "$DS" --help
assert_contains "$TMP/cli_help.out" 'ds v0.38.0' 'CLI help reports v0.38.0'

printf 'v0.35 tests passed: %s assertions\n' "$pass_count"
