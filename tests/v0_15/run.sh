#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_15_tests.$$"
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

assert_no_trailing_ws() {
  local file="$1" name="$2"
  if grep -nE '[[:blank:]]+$' "$file" >/dev/null; then
    echo "--- $file" >&2
    cat -n "$file" >&2 || true
    fail "$name: expected no trailing whitespace"
  fi
  pass "$name"
}

assert_one_trailing_newline() {
  local file="$1" name="$2"
  python3 - "$file" <<'PY'
import sys
p = sys.argv[1]
data = open(p, 'rb').read()
if not data:
    sys.exit(2)
if not data.endswith(b'\n') or data.endswith(b'\n\n'):
    sys.exit(1)
PY
  local rc=$?
  [ "$rc" = 0 ] || fail "$name: expected exactly one trailing newline"
  pass "$name"
}

assert_diag_shape() {
  local file="$1" fixture="$2" severity="$3" text="$4" name="$5"
  assert_contains "$file" "$fixture:" "$name path"
  assert_contains "$file" ": $severity:" "$name severity"
  assert_contains "$file" "$text" "$name message"
  assert_contains "$file" '^' "$name caret"
}

format_once_twice() {
  local input="$1" prefix="$2"
  run_ok "${prefix}_fmt_once" "$DS" fmt "$input"
  cp "$TMP/${prefix}_fmt_once.out" "$TMP/${prefix}_once.ds"
  run_ok "${prefix}_fmt_twice" "$DS" fmt "$TMP/${prefix}_once.ds"
  cp "$TMP/${prefix}_fmt_twice.out" "$TMP/${prefix}_twice.ds"
  assert_same "$TMP/${prefix}_once.ds" "$TMP/${prefix}_twice.ds" "$prefix formatter is idempotent"
  run_ok "${prefix}_fmt_check_fmt" "$DS" fmt --check "$TMP/${prefix}_twice.ds"
}

run_in_dir() {
  local name="$1" dir="$2"; shift 2
  mkdir -p "$dir"
  (cd "$dir" && "$@") >"$TMP/$name.out" 2>"$TMP/$name.err"
}

assert_behavior_preserved_vm() {
  local fixture="$1" prefix="$2"; shift 2
  local original_work="$TMP/${prefix}_orig_work"
  local formatted_work="$TMP/${prefix}_fmt_work"
  local formatted_fixture="$(dirname "$fixture")/${prefix}_formatted.ds"
  run_ok "${prefix}_make_fmt" "$DS" fmt "$fixture"
  cp "$TMP/${prefix}_make_fmt.out" "$formatted_fixture"
  capture_in_dir "${prefix}_orig_run" "$original_work" "$DS" run "$fixture" "$@"
  capture_in_dir "${prefix}_fmt_run" "$formatted_work" "$DS" run "$formatted_fixture" "$@"
  assert_same "$TMP/${prefix}_orig_run.rc" "$TMP/${prefix}_fmt_run.rc" "$prefix VM status preserved"
  assert_same "$TMP/${prefix}_orig_run.out" "$TMP/${prefix}_fmt_run.out" "$prefix VM stdout preserved"
  assert_same "$TMP/${prefix}_orig_run.err" "$TMP/${prefix}_fmt_run.err" "$prefix VM stderr preserved"
}

assert_behavior_preserved_bash() {
  local fixture="$1" prefix="$2"; shift 2
  local original_work="$TMP/${prefix}_orig_bash_work"
  local formatted_work="$TMP/${prefix}_fmt_bash_work"
  local original_sh="$TMP/${prefix}_orig.sh"
  local formatted_sh="$TMP/${prefix}_fmt.sh"
  local formatted_fixture="$(dirname "$fixture")/${prefix}_formatted_bash.ds"
  run_ok "${prefix}_make_fmt_bash" "$DS" fmt "$fixture"
  cp "$TMP/${prefix}_make_fmt_bash.out" "$formatted_fixture"
  run_ok "${prefix}_emit_orig" "$DS" emit bash "$fixture" -o "$original_sh"
  run_ok "${prefix}_emit_fmt" "$DS" emit bash "$formatted_fixture" -o "$formatted_sh"
  assert_not_contains "$formatted_sh" ' ds fmt ' "$prefix emitted Bash has no formatter dependency"
  run_ok "${prefix}_bash_n_orig" bash -n "$original_sh"
  run_ok "${prefix}_bash_n_fmt" bash -n "$formatted_sh"
  capture_in_dir "${prefix}_orig_bash" "$original_work" bash "$original_sh" "$@"
  capture_in_dir "${prefix}_fmt_bash" "$formatted_work" bash "$formatted_sh" "$@"
  assert_same "$TMP/${prefix}_orig_bash.rc" "$TMP/${prefix}_fmt_bash.rc" "$prefix Bash status preserved"
  assert_same "$TMP/${prefix}_orig_bash.out" "$TMP/${prefix}_fmt_bash.out" "$prefix Bash stdout preserved"
  assert_same "$TMP/${prefix}_orig_bash.err" "$TMP/${prefix}_fmt_bash.err" "$prefix Bash stderr preserved"
}

assert_test_behavior_preserved() {
  local fixture="$1" prefix="$2"
  local formatted_fixture="$(dirname "$fixture")/${prefix}_formatted_test.ds"
  run_ok "${prefix}_make_fmt_test" "$DS" fmt "$fixture"
  cp "$TMP/${prefix}_make_fmt_test.out" "$formatted_fixture"
  capture_status "${prefix}_orig_test" "$DS" test "$fixture"
  capture_status "${prefix}_fmt_test" "$DS" test "$formatted_fixture"
  assert_same "$TMP/${prefix}_orig_test.rc" "$TMP/${prefix}_fmt_test.rc" "$prefix test status preserved"
  assert_same "$TMP/${prefix}_orig_test.out" "$TMP/${prefix}_fmt_test.out" "$prefix test stdout preserved"
  if cmp -s "$TMP/${prefix}_orig_test.rc" "$TMP/${prefix}_fmt_test.rc" && [ "$(cat "$TMP/${prefix}_orig_test.rc")" = 0 ]; then
    assert_same "$TMP/${prefix}_orig_test.err" "$TMP/${prefix}_fmt_test.err" "$prefix test stderr preserved"
  else
    assert_contains "$TMP/${prefix}_orig_test.err" 'test `' "$prefix original failing diagnostic shape"
    assert_contains "$TMP/${prefix}_fmt_test.err" 'test `' "$prefix formatted failing diagnostic shape"
    assert_contains "$TMP/${prefix}_fmt_test.err" '^' "$prefix formatted failing diagnostic caret"
  fi
}

FIX="$TMP/fixtures"
mkdir -p "$FIX"

# Static wiring and CLI usage.
count_015="$(grep -E '^TEST_VERSIONS :=' Makefile | grep -o '0-15' | wc -l | tr -d ' ')"
[ "$count_015" = 1 ] || fail "TEST_VERSIONS should contain 0-15 exactly once, got $count_015"
pass 'TEST_VERSIONS contains 0-15 exactly once'
assert_matches Makefile '^TEST_VERSIONS := .*0-14 0-15($| )' 'v0.15 follows v0.14 in TEST_VERSIONS'
assert_contains Makefile 'asan:' 'asan target exists'
assert_contains Makefile 'ubsan:' 'ubsan target exists'

run_ok help_top "$DS" --help
assert_contains "$TMP/help_top.out" 'ds fmt <file.ds> [--check] [--write|-w]' 'top-level help lists ds fmt'
assert_contains "$TMP/help_top.out" 'ds check <file.ds> [--warnings-as-errors] [--no-warnings]' 'top-level help lists checker controls'
assert_contains "$TMP/help_top.out" 'ds test <file.ds>' 'existing test command remains listed'
capture_status fmt_no_args "$DS" fmt
assert_nonzero_status fmt_no_args
assert_contains "$TMP/fmt_no_args.err" 'expected `ds fmt [--check] [--write|-w] <file.ds>`' 'fmt without path usage error'
capture_status fmt_unknown "$DS" fmt --unknown "$FIX/missing.ds"
assert_nonzero_status fmt_unknown
assert_contains "$TMP/fmt_unknown.err" 'unknown fmt flag `--unknown`' 'fmt unknown flag diagnostic'
capture_status check_unknown "$DS" check --unknown "$FIX/missing.ds"
assert_nonzero_status check_unknown
assert_contains "$TMP/check_unknown.err" 'unknown check flag `--unknown`' 'check unknown flag diagnostic'

# Formatter golden: basic declarations and spacing.
write_fixture "$FIX/basic_messy.ds" <<'DS'
let   name="Danh"
let count=7
if count>3{echo "big"}else{echo "small"}
DS
write_fixture "$FIX/basic_expected.ds" <<'DS'
let name = "Danh"
let count = 7

if count > 3 {
  echo "big"
} else {
  echo "small"
}
DS
run_ok basic_fmt "$DS" fmt "$FIX/basic_messy.ds"
assert_same "$FIX/basic_expected.ds" "$TMP/basic_fmt.out" 'basic formatter golden matches'
assert_same_text '' "$TMP/basic_fmt.err" 'basic formatter stderr is empty'
assert_no_trailing_ws "$TMP/basic_fmt.out" 'basic formatter removes trailing whitespace'
assert_one_trailing_newline "$TMP/basic_fmt.out" 'basic formatter emits one trailing newline'
format_once_twice "$FIX/basic_messy.ds" basic

# Script block formatting.
write_fixture "$FIX/script_messy.ds" <<'DS'
script{arg app:string
option target:string="staging"
flag force:bool=false}
DS
write_fixture "$FIX/script_expected.ds" <<'DS'
script {
  arg app: string
  option target: string = "staging"
  flag force: bool = false
}
DS
run_ok script_fmt "$DS" fmt "$FIX/script_messy.ds"
assert_same "$FIX/script_expected.ds" "$TMP/script_fmt.out" 'script formatter golden matches'
format_once_twice "$FIX/script_messy.ds" script

# Functions, calls, imports, tests, arrays, maps, loops, commands, stdlib.
write_fixture "$FIX/lib.ds" <<'DS'
fn helper(name="world"){echo "helper {name}"}
DS
write_fixture "$FIX/mixed_messy.ds" <<'DS'
script{arg app:string
option target:string="staging"
flag force:bool=false}
import   "./lib.ds"
fn greet( name="world",title="" ){echo "hello {name}"}
let services=["api","web"]
let ports={api:3000,web:5173}
let first=services[0]
let port=ports.api
for service in services{if service=="api"{echo "api"}else{echo "other"}}
echo   "hello"
printf "ok" |> "out.txt"
printf "err" &> "both.log"
let result=run sh -c "printf captured"
let cwd=path.cwd()
let exists=file.exists("package.json")
cmd.require("sh")
greet("Danh","Mr")
helper(app)
test   "works"{assert true}
DS
run_ok mixed_fmt "$DS" fmt "$FIX/mixed_messy.ds"
assert_contains "$TMP/mixed_fmt.out" 'import "./lib.ds"' 'formatted import preserved'
assert_contains "$TMP/mixed_fmt.out" 'fn greet(name = "world", title = "") {' 'formatted function header'
assert_contains "$TMP/mixed_fmt.out" 'greet("Danh", "Mr")' 'formatted call comma spacing'
assert_contains "$TMP/mixed_fmt.out" 'let services = ["api", "web"]' 'formatted array spacing'
assert_contains "$TMP/mixed_fmt.out" 'let ports = {api: 3000, web: 5173}' 'formatted map spacing'
assert_contains "$TMP/mixed_fmt.out" 'let first = services[0]' 'formatted index compact'
assert_contains "$TMP/mixed_fmt.out" 'let port = ports.api' 'formatted field compact'
assert_contains "$TMP/mixed_fmt.out" 'for service in services {' 'formatted for header'
assert_contains "$TMP/mixed_fmt.out" '} else {' 'formatted else shape'
assert_contains "$TMP/mixed_fmt.out" 'printf "ok" |> "out.txt"' 'formatted stdout redirection'
assert_contains "$TMP/mixed_fmt.out" 'printf "err" &> "both.log"' 'formatted all-output redirection'
assert_contains "$TMP/mixed_fmt.out" 'let result = run sh -c "printf captured"' 'formatted captured run'
assert_contains "$TMP/mixed_fmt.out" 'let cwd = path.cwd()' 'formatted stdlib call path.cwd'
assert_contains "$TMP/mixed_fmt.out" 'cmd.require("sh")' 'formatted stdlib statement call'
assert_contains "$TMP/mixed_fmt.out" 'test "works" {' 'formatted test block'
assert_contains "$TMP/mixed_fmt.out" '  assert true' 'formatted assertion indentation'
assert_no_trailing_ws "$TMP/mixed_fmt.out" 'mixed formatter removes trailing whitespace'
assert_one_trailing_newline "$TMP/mixed_fmt.out" 'mixed formatter emits one trailing newline'
format_once_twice "$FIX/mixed_messy.ds" mixed

# Comments are explicitly unsupported by the current formatter, never silently dropped.
write_fixture "$FIX/comment_leading.ds" <<'DS'
# file header
let x = 1
DS
capture_status comment_fmt "$DS" fmt "$FIX/comment_leading.ds"
assert_nonzero_status comment_fmt
assert_same_text '' "$TMP/comment_fmt.out" 'comment formatter does not output partial source'
assert_diag_shape "$TMP/comment_fmt.err" "$FIX/comment_leading.ds" error 'formatter cannot preserve comments yet' 'leading comment diagnostic'
write_fixture "$FIX/comment_inline.ds" <<'DS'
let name = "Danh" # user name
DS
capture_status comment_inline_fmt "$DS" fmt "$FIX/comment_inline.ds"
assert_nonzero_status comment_inline_fmt
assert_same_text '' "$TMP/comment_inline_fmt.out" 'inline comment formatter does not output partial source'
assert_diag_shape "$TMP/comment_inline_fmt.err" "$FIX/comment_inline.ds" error 'formatter cannot preserve comments yet' 'inline comment diagnostic'

# fmt --check and write mode.
cp "$TMP/basic_fmt.out" "$FIX/basic_formatted.ds"
run_ok fmt_check_clean "$DS" fmt --check "$FIX/basic_formatted.ds"
assert_same_text '' "$TMP/fmt_check_clean.out" 'fmt --check clean stdout empty'
assert_same_text '' "$TMP/fmt_check_clean.err" 'fmt --check clean stderr empty'
capture_status fmt_check_dirty "$DS" fmt --check "$FIX/basic_messy.ds"
assert_nonzero_status fmt_check_dirty
assert_same_text '' "$TMP/fmt_check_dirty.out" 'fmt --check dirty stdout empty'
assert_contains "$TMP/fmt_check_dirty.err" "$FIX/basic_messy.ds: needs formatting" 'fmt --check dirty message'
assert_contains "$FIX/basic_messy.ds" 'let   name="Danh"' 'fmt --check leaves input unchanged'
write_fixture "$FIX/parse_bad.ds" <<'DS'
let =
DS
capture_status fmt_check_parse_bad "$DS" fmt --check "$FIX/parse_bad.ds"
assert_nonzero_status fmt_check_parse_bad
assert_diag_shape "$TMP/fmt_check_parse_bad.err" "$FIX/parse_bad.ds" error 'expected identifier after `let`' 'fmt --check parse diagnostic'
cp "$FIX/basic_messy.ds" "$FIX/write_target.ds"
chmod 754 "$FIX/write_target.ds"
run_ok fmt_write "$DS" fmt --write "$FIX/write_target.ds"
assert_same "$FIX/basic_expected.ds" "$FIX/write_target.ds" 'fmt --write rewrites file'
if find "$FIX" -maxdepth 1 -name 'write_target.ds.tmp.*' -print -quit | grep -q .; then
  fail 'fmt --write should not leave temporary files behind'
fi
pass 'fmt --write removes temporary file'
if perm="$(stat -c '%a' "$FIX/write_target.ds" 2>/dev/null)"; then
  :
else
  perm="$(stat -f '%Lp' "$FIX/write_target.ds")"
fi
[ "$perm" = 754 ] || fail "fmt --write should preserve permissions, got $perm"
pass 'fmt --write preserves permissions'
run_ok fmt_write_idempotent "$DS" fmt -w "$FIX/write_target.ds"
assert_same "$FIX/basic_expected.ds" "$FIX/write_target.ds" 'fmt -w is idempotent and equivalent'
cp "$FIX/parse_bad.ds" "$FIX/write_bad.ds"
capture_status fmt_write_bad "$DS" fmt --write "$FIX/write_bad.ds"
assert_nonzero_status fmt_write_bad
assert_contains "$FIX/write_bad.ds" 'let =' 'fmt --write parse error leaves file unchanged'
capture_status fmt_check_write "$DS" fmt --check --write "$FIX/write_target.ds"
assert_nonzero_status fmt_check_write
assert_contains "$TMP/fmt_check_write.err" '`ds fmt --check --write` is invalid' 'fmt --check --write rejected'

# Behavior preservation across VM, Bash, and test runner.
write_fixture "$FIX/behavior.ds" <<'DS'
script{arg name:string
option target:string="dev"
flag loud:bool=false}
fn greet(value="world"){echo "hello {value}"}
let services=["api","web"]
let ports={api:3000,web:5173}
greet(name)
for service in services{echo "svc={service}"}
echo "target={target}"
printf "side" |> "side.txt"
let captured=run sh -c "printf cap"
echo "captured={captured.stdout}"
DS
assert_behavior_preserved_vm "$FIX/behavior.ds" behavior_vm Danh --target prod --loud
assert_behavior_preserved_bash "$FIX/behavior.ds" behavior_bash Danh --target prod --loud
assert_same "$TMP/behavior_vm_orig_work/side.txt" "$TMP/behavior_vm_fmt_work/side.txt" 'VM side-effect file preserved after formatting'
assert_same "$TMP/behavior_bash_orig_bash_work/side.txt" "$TMP/behavior_bash_fmt_bash_work/side.txt" 'Bash side-effect file preserved after formatting'
write_fixture "$FIX/tests_behavior.ds" <<'DS'
fn truth(){echo "truth"}
test "pass"{assert true}
test "fail"{assert false}
DS
assert_test_behavior_preserved "$FIX/tests_behavior.ds" tests_behavior
assert_contains "$TMP/tests_behavior_fmt_test.out" '2 tests, 1 passed, 1 failed' 'formatted failing test summary preserved'

# Checker warning positives and negatives.
write_fixture "$FIX/warn_unused.ds" <<'DS'
let unused = "x"
echo "done"
fn f(name, unused_param = "x") {
  let inner = 1
  echo "hello {name}"
}
test "unreachable" {
  fail "stop"
  echo "after fail"
}
DS
capture_status check_warnings "$DS" check "$FIX/warn_unused.ds"
assert_status check_warnings 0
assert_diag_shape "$TMP/check_warnings.err" "$FIX/warn_unused.ds" warning 'unused variable `unused`' 'unused top-level variable warning'
assert_diag_shape "$TMP/check_warnings.err" "$FIX/warn_unused.ds" warning 'unused parameter `unused_param`' 'unused function parameter warning'
assert_diag_shape "$TMP/check_warnings.err" "$FIX/warn_unused.ds" warning 'unused variable `inner`' 'unused inner variable warning'
assert_diag_shape "$TMP/check_warnings.err" "$FIX/warn_unused.ds" warning 'unreachable statement after test-only `fail` or `exit`' 'unreachable warning'
write_fixture "$FIX/warn_negative.ds" <<'DS'
let name = "Danh"
echo "hello {name}"
let shell = "world"
echo $shell
let a = 1
let b = a
echo "{b}"
let ok = true
if ok { echo "yes" }
let xs = ["api"]
let item = xs[0]
echo "{item}"
let ports = {api: 3000}
let value = ports.api
echo "{value}"
fn use_param(param) {
  echo "{param}"
}
use_param(name)
test "uses assert" {
  assert ok
}
DS
run_ok check_no_warnings "$DS" check "$FIX/warn_negative.ds"
assert_same_text '' "$TMP/check_no_warnings.err" 'used variables and params do not warn'
write_fixture "$FIX/unreachable_conditional.ds" <<'DS'
let cond = true
test "conditional" {
  if cond {
    fail "stop"
  }
  echo "reachable unless cond"
}
DS
run_ok check_conditional_unreachable "$DS" check "$FIX/unreachable_conditional.ds"
assert_not_contains "$TMP/check_conditional_unreachable.err" 'unreachable statement' 'conditional fail does not warn as unreachable'
capture_status warnings_as_errors "$DS" check --warnings-as-errors "$FIX/warn_unused.ds"
assert_nonzero_status warnings_as_errors
assert_contains "$TMP/warnings_as_errors.err" 'warning:' 'warnings-as-errors keeps warning wording'
run_ok no_warnings "$DS" check --no-warnings "$FIX/warn_unused.ds"
assert_same_text '' "$TMP/no_warnings.err" 'no-warnings suppresses warning output'
capture_status warning_flags_conflict "$DS" check --warnings-as-errors --no-warnings "$FIX/warn_unused.ds"
assert_nonzero_status warning_flags_conflict
assert_contains "$TMP/warning_flags_conflict.err" '`ds check --warnings-as-errors --no-warnings` is invalid' 'conflicting warning flags rejected'
write_fixture "$FIX/duplicate_hard_error.ds" <<'DS'
let x = 1
let x = 2
DS
capture_status duplicate_check "$DS" check "$FIX/duplicate_hard_error.ds"
assert_nonzero_status duplicate_check
assert_contains "$TMP/duplicate_check.err" 'duplicate variable `x`' 'duplicate declaration remains a hard error'
assert_not_contains "$TMP/duplicate_check.err" 'warning:' 'duplicate declaration is not downgraded to warning'
capture_status no_warnings_hard_error "$DS" check --no-warnings "$FIX/duplicate_hard_error.ds"
assert_nonzero_status no_warnings_hard_error
assert_contains "$TMP/no_warnings_hard_error.err" 'duplicate variable `x`' 'no-warnings still reports hard errors'

# File loading, malformed source, unsupported syntax, strings, paths.
capture_status missing_fmt "$DS" fmt "$FIX/does-not-exist.ds"
assert_nonzero_status missing_fmt
assert_contains "$TMP/missing_fmt.err" 'failed to open source file' 'fmt missing file diagnostic'
mkdir -p "$FIX/as_dir.ds"
capture_status dir_fmt "$DS" fmt "$FIX/as_dir.ds"
assert_nonzero_status dir_fmt
assert_contains "$TMP/dir_fmt.err" 'failed to read source file' 'fmt directory diagnostic'
: > "$FIX/empty.ds"
run_ok empty_fmt "$DS" fmt "$FIX/empty.ds"
assert_same_text '' "$TMP/empty_fmt.out" 'empty file formats to empty output'
printf 'let x=1' > "$FIX/no_newline.ds"
run_ok no_newline_fmt "$DS" fmt "$FIX/no_newline.ds"
assert_one_trailing_newline "$TMP/no_newline_fmt.out" 'no-final-newline input gains canonical newline'
write_fixture "$FIX/bad_if.ds" <<'DS'
if true {
DS
capture_status bad_if_fmt "$DS" fmt "$FIX/bad_if.ds"
assert_nonzero_status bad_if_fmt
assert_contains "$TMP/bad_if_fmt.err" ': error:' 'fmt malformed if emits parser diagnostic'
write_fixture "$FIX/bad_fn.ds" <<'DS'
fn broken( {
}
DS
capture_status bad_fn_check "$DS" check "$FIX/bad_fn.ds"
assert_nonzero_status bad_fn_check
assert_contains "$TMP/bad_fn_check.err" ': error:' 'check malformed function emits parser diagnostic'
write_fixture "$FIX/bad_assert.ds" <<'DS'
test "bad" {
  assert
}
DS
capture_status bad_assert_fmt "$DS" fmt "$FIX/bad_assert.ds"
assert_nonzero_status bad_assert_fmt
assert_contains "$TMP/bad_assert_fmt.err" ': error:' 'fmt malformed assert emits parser diagnostic'
write_fixture "$FIX/future.ds" <<'DS'
let x = "a".
DS
capture_status future_fmt "$DS" fmt "$FIX/future.ds"
assert_nonzero_status future_fmt
assert_contains "$TMP/future_fmt.err" ': error:' 'malformed future expression rejected by formatter'
write_fixture "$FIX/bash_if.ds" <<'DS'
if [ -f x ]; then
  echo yes
fi
DS
capture_status bash_if_fmt "$DS" fmt "$FIX/bash_if.ds"
assert_nonzero_status bash_if_fmt
assert_contains "$TMP/bash_if_fmt.err" ': error:' 'Bash-style if rejected by formatter'
write_fixture "$FIX/string_preserve.ds" <<'DS'
let name="Danh"
let s="a  b"
let quoted="quote \" here"
let interp="hello {name}"
echo "tabs\tand\nnewlines"
echo "{s}"
echo "{quoted}"
echo "{interp}"
DS
assert_behavior_preserved_vm "$FIX/string_preserve.ds" string_vm
run_ok string_fmt "$DS" fmt "$FIX/string_preserve.ds"
assert_contains "$TMP/string_fmt.out" 'let s = "a  b"' 'formatter preserves double spaces inside strings'
assert_contains "$TMP/string_fmt.out" 'let quoted = "quote \" here"' 'formatter preserves quote escape meaning'
assert_contains "$TMP/string_fmt.out" 'let interp = "hello {name}"' 'formatter preserves interpolation braces'
write_fixture "$FIX/glob_bad.ds" <<'DS'
for x in glob("**file.ds") {
  echo "{x}"
}
DS
capture_status glob_bad_orig "$DS" check "$FIX/glob_bad.ds"
run_ok glob_bad_fmt "$DS" fmt "$FIX/glob_bad.ds"
cp "$TMP/glob_bad_fmt.out" "$TMP/glob_bad_formatted.ds"
capture_status glob_bad_formatted "$DS" check "$TMP/glob_bad_formatted.ds"
assert_same "$TMP/glob_bad_orig.rc" "$TMP/glob_bad_formatted.rc" 'recursive glob validation status preserved after formatting'
assert_contains "$TMP/glob_bad_formatted.err" 'recursive `**` glob patterns must use `**` as a complete path segment' 'recursive glob validation message preserved'

# Larger mixed fixture without comments: idempotent and behavior-preserving.
write_fixture "$FIX/large.ds" <<'DS'
script{arg app:string
option stage:string="dev"
flag verbose:bool=false}
import "./lib.ds"
fn banner(name="world"){echo "== {name} =="}
let services=["api","web"]
let ports={api:3000,web:5173}
banner(app)
for service in services{echo "service={service}"}
printf "log" |>> "log.txt"
let result=run sh -c "printf ok"
echo "result={result.stdout}"
test "truth"{assert true}
DS
format_once_twice "$FIX/large.ds" large
assert_behavior_preserved_vm "$FIX/large.ds" large_vm api --stage prod --verbose
assert_behavior_preserved_bash "$FIX/large.ds" large_bash api --stage prod --verbose
write_fixture "$FIX/large_test.ds" <<'DS'
fn is_ok(){echo "ok"}
test "first"{assert true}
test "second"{fail "planned"}
DS
assert_test_behavior_preserved "$FIX/large_test.ds" large_test

printf 'v0.15 tests completed with %d assertions\n' "$pass_count"
