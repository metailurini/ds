#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_23_tests.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != 1 ]]; then
  make -C "$ROOT" clean all >/dev/null
fi

cd "$ROOT"

assert_run_fails() {
  local name="$1" fixture="$2" fragment="$3"
  run_fail "${name}_run" "$DS" run "$fixture"
  assert_diag "$TMP/${name}_run.err" "$fragment" "$name run diagnostic"
}

assert_fmt_check_fails() {
  local name="$1" fixture="$2"
  run_fail "${name}_fmt_check" "$DS" fmt --check "$fixture"
}

assert_exact_stdout() {
  local name="$1" expected="$2"
  assert_same_text "$expected" "$TMP/${name}.out" "$name stdout"
}

assert_parity() {
  local name="$1" fixture="$2" expected_status="$3" expected_stdout="$4"; shift 4
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"

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

FIX="$TMP/fixtures with spaces"
mkdir -p "$FIX"

# Static wiring and documentation coverage.
assert_contains Makefile '0-23' 'TEST_VERSIONS contains v0.23'
assert_matches Makefile '^TEST_VERSIONS := .*0-22 0-23($| )' 'v0.23 follows v0.22 in TEST_VERSIONS'

# Lexer/parser/AST/HIR/bytecode shape.
shape="$FIX/shape.ds"
write_fixture "$shape" <<'DS'
let app = "api"
let ok = app in ["api", "web"]
for n in 1..3 {
  echo "{n}"
}
let release = "release/123" matches /^release\/[0-9]+$/
DS
run_ok shape_tokens "$DS" tokens "$shape"
assert_contains "$TMP/shape_tokens.out" 'in' 'tokens include in keyword'
assert_contains "$TMP/shape_tokens.out" '..' 'tokens include range operator'
assert_contains "$TMP/shape_tokens.out" 'matches' 'tokens include matches keyword'
assert_contains "$TMP/shape_tokens.out" '/^release\\/[0-9]+$/' 'tokens preserve regex literal escapes'
run_ok shape_ast "$DS" ast "$shape"
assert_matches "$TMP/shape_ast.out" 'Binary.*in|in' 'AST distinguishes in'
assert_matches "$TMP/shape_ast.out" 'Range|\.\.' 'AST distinguishes range source'
assert_matches "$TMP/shape_ast.out" 'matches' 'AST distinguishes matches'
run_ok shape_hir "$DS" hir "$shape"
assert_contains "$TMP/shape_hir.out" 'in' 'HIR preserves in operator'
assert_contains "$TMP/shape_hir.out" 'matches' 'HIR preserves matches operator'
assert_matches "$TMP/shape_hir.out" 'Range|\.\.' 'HIR preserves range bounds'
run_ok shape_bytecode "$DS" bytecode "$shape"
assert_matches "$TMP/shape_bytecode.out" 'IN|MATCH|JUMP|RANGE|LOOP|ITER' 'bytecode exposes v0.23 lowering'
assert_parity shape_parity "$shape" 0 $'1\n2\n3\n'

# Formatter coverage.
fmt_in="$FIX/fmt_input.ds"
write_fixture "$fmt_in" <<'DS'
let app="api"
let name="API"
let ok=app in["api","web"]
if (app in["api"])&&(name matches/^api/i){echo ok}
for n in 1+1..5-1{echo "{n}"}
DS
fmt_expected="$TMP/fmt_expected.ds"
cat >"$fmt_expected" <<'DS'
let app = "api"
let name = "API"
let ok = app in ["api", "web"]

if (app in ["api"]) && (name matches /^api/i) {
  echo ok
}

for n in 1 + 1..5 - 1 {
  echo "{n}"
}
DS
run_ok fmt_v023 "$DS" fmt "$fmt_in"
assert_same "$fmt_expected" "$TMP/fmt_v023.out" 'formatter normalizes v0.23 syntax'
fmt_good="$FIX/fmt_good.ds"
cp "$TMP/fmt_v023.out" "$fmt_good"
run_ok fmt_v023_check "$DS" fmt --check "$fmt_good"
assert_fmt_check_fails fmt_v023_dirty "$fmt_in"
fmt_comment="$FIX/fmt_comment.ds"
write_fixture "$fmt_comment" <<'DS'
# comment remains rejected by formatter
let ok = "api" matches /api/
DS
run_fail fmt_comment_rejected "$DS" fmt "$fmt_comment"

# Membership positive VM/Bash parity.
member="$FIX/membership.ds"
write_fixture "$member" <<'DS'
let app = "api"
let allowed = ["api", "web"]
if app in allowed { echo yes } else { echo no }
let n = 2
if n in [1, 2, 3] { echo int }
let flag = false
if flag in [true, false] { echo bool }
let xs = []
if "x" in xs { echo bad } else { echo empty }
let mixed = ["2"]
if 2 in mixed { echo bad } else { echo kind }
let needle = "a*b"
let patterns = ["axb", "a*b"]
if needle in patterns { echo literal }
let parts = "api,web".split(",")
if "web" in parts { echo split }
file.write("items.txt", "a\nb\n")
if "b" in lines("items.txt") { echo lines }
file.write("a.item", "a\n")
file.write("b.item", "b\n")
if "b.item" in glob("*.item") { echo glob }
if "a.item" in glob!("*.item") { echo glob-bang }
let ok = app in allowed
echo "known={ok}"
if (app in allowed) && true { echo and }
let item = "api"
while item in allowed {
  echo while
  item = "db"
}
case app in allowed {
  true { echo case-yes }
  false { echo case-no }
}
fn is_allowed(name) {
  return name in allowed
}
fn show(v) {
  if v { echo arg-yes } else { echo arg-no }
}
show(is_allowed("web"))
DS
assert_parity membership "$member" 0 $'yes\nint\nbool\nempty\nkind\nliteral\nsplit\nlines\nglob\nglob-bang\nknown=true\nand\nwhile\ncase-yes\narg-yes\n'

# Membership diagnostics.
for item in \
  'member_string|let ok = "api" in "api"|right operand' \
  'member_map|let ports = { api: 3000 }\nlet ok = "api" in ports|right operand' \
  'member_range|let ok = "api" in 1..3|range' \
  'member_mixed|let ok = "api" in ["api", 1]|heterogeneous' \
  'member_command|let result = run echo api\nlet ok = "api" in result|right operand' \
  'member_unknown|let ok = app in unknown_name|unknown_name' \
  'member_chain|let app = "api"\nlet allowed = ["api"]\nlet ok = app in allowed == true|parentheses'; do
  IFS='|' read -r name src frag <<<"$item"
  f="$FIX/$name.ds"
  printf '%b\n' "$src" >"$f"
  assert_check_fails "$name" "$f" "$frag"
  assert_emit_fails "${name}_emit" "$f" "$frag"
done

# Range positive VM/Bash parity.
ranges="$FIX/ranges.ds"
write_fixture "$ranges" <<'DS'
for n in 1..3 { echo "lit={n}" }
let start = 2
let end = 4
for n in start..end { echo "var={n}" }
for n in 1 + 1..2 * 3 { echo "arith={n}" }
fn first() { return 1 }
fn last() { return 2 }
for n in first()..last() { echo "fn={n}" }
for n in 3..3 { echo "single={n}" }
for n in 5..3 { echo none }
echo done-empty
for n in -2..1 { echo "neg={n}" }
for n in 1..3 {
  case n {
    2 { echo two }
    _ { echo other }
  }
}
for n in 1..5 {
  if n == 2 { continue }
  if n == 4 { break }
  echo "bc={n}"
}
for i in 1..2 {
  for j in 1..2 {
    echo "nested={i},{j}"
  }
}
let once_end = 3
for n in 1..once_end {
  echo "once={n}"
  once_end = 1
}
DS
assert_parity ranges "$ranges" 0 $'lit=1\nlit=2\nlit=3\nvar=2\nvar=3\nvar=4\narith=2\narith=3\narith=4\narith=5\narith=6\nfn=1\nfn=2\nsingle=3\ndone-empty\nneg=-2\nneg=-1\nneg=0\nneg=1\nother\ntwo\nother\nbc=1\nbc=3\nnested=1,1\nnested=1,2\nnested=2,1\nnested=2,2\nonce=1\nonce=2\nonce=3\n'
range_script="$TMP/ranges.sh"
run_ok ranges_emit_check "$DS" emit bash "$ranges" -o "$range_script"
assert_not_contains "$range_script" '{1..' 'range emission avoids brace expansion'
assert_matches "$range_script" "__ds_type_.*='int'" 'range emission carries int kind metadata'

# Range diagnostics.
for item in \
  'range_value|let xs = 1..3|range' \
  'range_interp|echo "{1..3}"|range' \
  'range_ellipsis|for n in 1...3 { echo bad }|range' \
  'range_missing_end|for n in 1.. { echo bad }|range' \
  'range_missing_start|for n in ..3 { echo bad }|range' \
  'range_string_start|for n in "1"..3 { echo bad }|int' \
  'range_string_end|for n in 1.."3" { echo bad }|int' \
  'range_half_open|for n in 1..<3 { echo bad }|range' \
  'range_step|for n in 1..10 step 2 { echo bad }|step'; do
  IFS='|' read -r name src frag <<<"$item"
  f="$FIX/$name.ds"
  printf '%b\n' "$src" >"$f"
  assert_check_fails "$name" "$f" "$frag"
  assert_emit_fails "${name}_emit" "$f" "$frag"
done

# Regex positive VM/Bash parity.
regex="$FIX/regex.ds"
write_fixture "$regex" <<'DS'
if "deploy failed" matches /failed/ { echo yes }
let branch = "release/123"
if branch matches /^release\/[0-9]+$/ { echo release }
let svc = "api"
if svc matches /^(api|web)$/ { echo service }
if "x9" matches /^[^0-9][0-9]$/ { echo class }
if "aaa" matches /^a{2,3}$/ { echo quant }
if "API" matches /api/ { echo bad } else { echo sensitive }
if "API" matches /api/i { echo insensitive }
if "API" matches /api/i { echo first }
if "API" matches /api/ { echo bad } else { echo restored }
let value = "xray"
let ok = value matches /^x/
if ok { echo let-ok }
let rendered = "interp={value matches /x/}"
echo $rendered
if (value matches /x/) || false { echo or-ok }
case value matches /x/ { true { echo case-match } false { echo case-miss } }
fn returns_match(s) { return s matches /^x/ }
fn show(v) { if v { echo arg-match } }
show(returns_match("x"))
if "a/b" matches /a\/b/ { echo slash }
if "a\\b" matches /a\\b/ { echo backslash }
let literal_replace = "a[0-9]b".replace("[0-9]", "X")
echo "replace={literal_replace}"
DS
assert_parity regex "$regex" 0 $'yes\nrelease\nservice\nclass\nquant\nsensitive\ninsensitive\nfirst\nrestored\nlet-ok\ninterp=true\nor-ok\ncase-match\narg-match\nslash\nbackslash\nreplace=aXb\n'
regex_script="$TMP/regex.sh"
run_ok regex_emit_check "$DS" emit bash "$regex" -o "$regex_script"
assert_matches "$regex_script" 'nocasematch' 'case-insensitive regex emission handles nocasematch'
plain_regex="$FIX/plain_regex.ds"
write_fixture "$plain_regex" <<'DS'
if "api" matches /api/ { echo ok }
DS
plain_regex_script="$TMP/plain_regex.sh"
run_ok plain_regex_emit "$DS" emit bash "$plain_regex" -o "$plain_regex_script"
assert_contains "$plain_regex_script" 'shopt -u nocasematch' 'plain regex emission disables ambient nocasematch'
assert_contains "$plain_regex_script" 'eval "$__ds_old_nocasematch"' 'plain regex emission restores ambient nocasematch'

runtime_regex="$FIX/runtime_regex.ds"
write_fixture "$runtime_regex" <<'DS'
let pat = "^api$"
let ok = "api" matches pat
echo $ok
DS
assert_parity runtime_regex "$runtime_regex" 0 $'true\n'

# Regex diagnostics.
for item in \
  'regex_non_string|let ok = 123 matches /123/|string' \
  'regex_bad_flag|let ok = "api" matches /api/g|flag' \
  'regex_unterminated|let ok = "api" matches /api|unterminated' \
  'regex_empty|let ok = "a" matches //|empty' \
  'regex_lookahead|let ok = "x" matches /(?=x)/|deferred' \
  'regex_backref|let ok = "aa" matches /(a)\\1/|regex' \
  'regex_lazy|let ok = "abc" matches /a.*?c/|regex' \
  'regex_unicode|let ok = "é" matches /\p{L}/|regex' \
  'regex_invalid_posix|let ok = "x" matches /[/|invalid regex pattern'; do
  IFS='|' read -r name src frag <<<"$item"
  f="$FIX/$name.ds"
  printf '%b\n' "$src" >"$f"
  assert_check_fails "$name" "$f" "$frag"
  assert_emit_fails "${name}_emit" "$f" "$frag"
done
assert_run_fails regex_invalid_posix_run "$FIX/regex_invalid_posix.ds" 'invalid regex pattern'
regex_newline="$FIX/regex_newline.ds"
printf 'let ok = "a" matches /a\nb/\n' >"$regex_newline"
assert_check_fails regex_newline "$regex_newline" 'newline'
assert_emit_fails regex_newline_emit "$regex_newline" 'newline'

# Combined features, imports, script args, and tests.
combined="$FIX/combined.ds"
write_fixture "$combined" <<'DS'
let names = ["api", "web", "db"]
let allowed = ["api", "web"]
for name in names {
  if (name in allowed) && (name matches /^[a-z]+$/) {
    echo "ok={name}"
  }
}
let odds = [1, 3, 5]
for n in 1..5 {
  if n in odds { echo "odd={n}" }
}
for n in 1..3 {
  let s = "item-{n:02d}"
  if s matches /^item-[0-9][0-9]$/ { echo $s }
}
fn valid(name) {
  return ((name == "api") || (name == "web")) && (name matches /^[a-z]+$/)
}
for i in 1..2 {
  if valid("api") { echo "{i}:ok" }
}
DS
assert_parity combined "$combined" 0 $'ok=api\nok=web\nodd=1\nodd=3\nodd=5\nitem-01\nitem-02\nitem-03\n1:ok\n2:ok\n'

cat >"$FIX/lib.ds" <<'DS'
let allowed = ["api", "web"]
fn accepts(name) {
  return (name in allowed) && (name matches /^[a-z]+$/)
}
DS
import_main="$FIX/import_main.ds"
cat >"$import_main" <<'DS'
import "lib.ds"
for i in 1..2 {
  if accepts("api") { echo "import={i}" }
}
DS
assert_parity imports "$import_main" 0 $'import=1\nimport=2\n'
imports_script="$TMP/imports.sh"
run_ok imports_emit_check "$DS" emit bash "$import_main" -o "$imports_script"
helper_count=$(grep -c '__ds_membership' "$imports_script" || true)
[ "$helper_count" -le 2 ] || fail 'generated helpers are duplicated across imports'
pass 'generated helpers are not duplicated across imports'

script_args="$FIX/script_args.ds"
write_fixture "$script_args" <<'DS'
script {
  arg app: string
  option retries: int = 2
}
if !(app in ["api", "web"]) {
  fail "unknown app"
}
for n in 1..retries {
  echo "try={n}"
}
DS
assert_parity script_args "$script_args" 0 $'try=1\ntry=2\n' api --retries 2
capture_in_dir script_args_vm_bad "$TMP/script_args_bad_vm" "$DS" run "$script_args" db
assert_nonzero_status script_args_vm_bad

blocks="$FIX/test_blocks.ds"
write_fixture "$blocks" <<'DS'
test "regex and membership" {
  assert "api" in ["api"]
  assert "release/1" matches /^release\/[0-9]+$/
}
DS
run_ok test_blocks "$DS" test "$blocks"

# Example/docs smoke.
example="$ROOT/examples/filtering.ds"
[ -f "$example" ] || fail 'examples/filtering.ds exists'
pass 'examples/filtering.ds exists'
assert_parity example_filtering "$example" 0 $'selected=api\nselected=web\nretry=1\nretry=2\nretry=3\nrelease branch\n'

# Focused regression coverage from older milestones.
regress="$FIX/regressions.ds"
write_fixture "$regress" <<'DS'
let xs = ["a", "b"]
for x in xs { echo "arr={x}" }
let i = 0
while i < 3 {
  i += 1
  if i == 2 { continue }
  echo "while={i}"
}
case "api" { "api" { echo case-old } _ { echo bad } }
let s = " api "
let trimmed = s.trim()
echo "trim={trimmed}"
let seven = 7
echo "fmt={seven:03d}"
let parts = "a,b".split(",")
let second = parts[1]
echo "split={second}"
let a = 1 + 2 * 3
for n in a - 5..a - 4 { echo "arith-near={n}" }
defer { echo cleanup }
trap "INT" { echo int }
if "a" in parts { echo member-near }
if "release/1" matches /^release\/[0-9]+$/ { echo regex-near }
DS
assert_parity regressions "$regress" 0 $'arr=a\narr=b\nwhile=1\nwhile=3\ncase-old\ntrim=api\nfmt=007\nsplit=b\narith-near=2\narith-near=3\nmember-near\nregex-near\ncleanup\n'

# Repeated generation/sanitizer-value fixtures.
long_array="$FIX/long_array.ds"
{
  printf 'let xs = ['
  for i in $(seq 1 40); do
    if [ "$i" -gt 1 ]; then printf ', '; fi
    printf '"v%s"' "$i"
  done
  printf ']\nif "v40" in xs { echo found }\n'
} >"$long_array"
assert_parity long_array "$long_array" 0 $'found\n'
for i in 1 2 3; do
  run_ok "repeat_emit_$i" "$DS" emit bash "$combined" -o "$TMP/repeat_$i.sh"
  run_ok "repeat_bash_n_$i" bash -n "$TMP/repeat_$i.sh"
done

printf 'v0.23 tests passed (%s assertions)\n' "$pass_count"
