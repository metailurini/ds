#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_19_tests.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != 1 ]]; then
  make -C "$ROOT" clean all >/dev/null
fi

cd "$ROOT"

capture_env_in_dir() {
  local name="$1" dir="$2"; shift 2
  mkdir -p "$dir"
  set +e
  (cd "$dir" && env LC_ALL=C PATH="$FAKEBIN:$PATH" "$@") >"$TMP/$name.out" 2>"$TMP/$name.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/$name.rc"
}

run_env_ok() {
  local name="$1"; shift
  env LC_ALL=C PATH="$FAKEBIN:$PATH" "$@" >"$TMP/$name.out" 2>"$TMP/$name.err" || {
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected success"
  }
  pass "$name"
}

assert_vm_bash_env_parity() {
  local name="$1" fixture="$2" expected_status="$3" output_files="$4"; shift 4
  local script="$TMP/$name.sh"
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  mkdir -p "$vm_work" "$bash_work"

  capture_env_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_env_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  capture_env_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  else
    assert_contains "$TMP/${name}_vm.err" ': error:' "$name VM error diagnostic"
    assert_contains "$TMP/${name}_bash.err" ': error:' "$name Bash error diagnostic"
  fi
  assert_contains "$script" '#!/usr/bin/env bash' "$name emitted Bash shebang"
  assert_contains "$script" 'set -euo pipefail' "$name emitted Bash strict mode"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  local rel
  for rel in $output_files; do
    [ -f "$vm_work/$rel" ] || fail "$name VM missing side-effect file $rel"
    [ -f "$bash_work/$rel" ] || fail "$name Bash missing side-effect file $rel"
    assert_same "$vm_work/$rel" "$bash_work/$rel" "$name side-effect parity $rel"
  done
}

FIX="$TMP/fixtures"
FAKEBIN="$TMP/fakebin"
mkdir -p "$FIX" "$FAKEBIN"

cat >"$FAKEBIN/emit_branch" <<'SH'
#!/usr/bin/env bash
printf ' branch\n'
SH
cat >"$FAKEBIN/must_not_run" <<'SH'
#!/usr/bin/env bash
printf 'checker executed command\n' > must_not_exist.txt
exit 23
SH
cat >"$FAKEBIN/emit_services" <<'SH'
#!/usr/bin/env bash
printf 'api,web,worker\n'
SH
cat >"$FAKEBIN/filter_api" <<'SH'
#!/usr/bin/env bash
while IFS= read -r line; do
  case "$line" in *api*) printf '%s\n' "$line" ;; esac
done
SH
chmod +x "$FAKEBIN"/*

# Static and build wiring tests.
count_019="$(grep -E '^TEST_VERSIONS :=' Makefile | grep -o '0-19' | wc -l | tr -d ' ')"
[ "$count_019" = 1 ] || fail "TEST_VERSIONS should contain 0-19 exactly once, got $count_019"
pass 'TEST_VERSIONS contains 0-19 exactly once'
assert_matches Makefile '^TEST_VERSIONS := .*0-18 0-19($| )' 'v0.19 follows v0.18 in TEST_VERSIONS'
assert_contains Makefile 'DS_SKIP_BUILD=1 ./tests/v$(subst -,_,$(patsubst test-v%,%,$@))/run.sh' 'pattern target invokes version suite'
assert_contains Makefile '$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address" test' 'asan runs aggregate test suite'
assert_contains Makefile '$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined" test' 'ubsan runs aggregate test suite'
assert_not_contains Makefile 'libs/hashmap' 'build does not reference stale libs/hashmap path'
assert_not_contains include/ds.h 'hashmap' 'public umbrella does not expose hashmap internals'

for file in src/lexer.c src/parser.c src/parse_expr.c src/ast.c src/lower_expr.c src/hir.c src/format.c src/ds_checker.c src/ds_interpolation.c src/vm_stdlib.c src/bash_helpers.c src/bash_expr.c src/bash_deps.c; do
  [ -f "$file" ] || fail "$file exists"
  pass "$file exists"
done
assert_contains src/ds_interpolation.c 'ds_interp_parse_format_spec_for_kind' 'shared interpolation format contract is implemented once'
assert_contains src/lower_command.c 'ds_interp_parse_format_spec_for_kind' 'lowerer consumes shared interpolation format contract'
assert_contains src/vm_process.c 'ds_interp_parse_format_spec_for_kind' 'VM consumes shared interpolation format contract'
assert_contains src/bash_quote.c 'ds_interp_parse_format_spec' 'Bash consumes shared interpolation format contract'

assert_contains examples/strings.ds '.trim().lower().replace' 'strings example covers method chain'
assert_contains examples/strings.ds '.split' 'strings example covers split'
assert_contains examples/strings.ds ':05d' 'strings example covers integer format'
assert_contains examples/strings.ds '"""' 'strings example covers triple quotes'

# Debug/token/parser coverage.
write_fixture "$FIX/debug.ds" <<'DS'
let s = "  Hello  ".trim().lower()
let body = """
line one
line two
"""
echo "{s:upper}"
echo "{s:<8}"
DS
run_ok debug_tokens "$DS" tokens "$FIX/debug.ds"
assert_contains "$TMP/debug_tokens.out" 'STRING' 'tokens include normal/triple string literals'
assert_contains "$TMP/debug_tokens.out" 'DOT' 'tokens include method dot'
assert_contains "$TMP/debug_tokens.out" 'line two' 'tokens preserve triple-quoted content'
run_ok debug_ast "$DS" ast "$FIX/debug.ds"
assert_contains "$TMP/debug_ast.out" 'CallExpr string.trim' 'AST includes trim method call'
assert_contains "$TMP/debug_ast.out" 'CallExpr string.lower' 'AST includes chained lower method call'
assert_contains "$TMP/debug_ast.out" 'line two' 'AST preserves triple-quoted body'
run_ok debug_hir "$DS" hir "$FIX/debug.ds"
assert_contains "$TMP/debug_hir.out" 'Call string.trim' 'HIR lowers trim helper'
assert_contains "$TMP/debug_hir.out" 'Call string.lower' 'HIR lowers lower helper'

write_fixture "$FIX/checker_interp_use.ds" <<'DS'
let name = "api"
let needle = "p"
let matched = name.contains(needle)
let rendered = "{name:upper}"
if matched { echo "matched" }
echo rendered
DS
run_ok checker_interp_use "$DS" check "$FIX/checker_interp_use.ds"
assert_not_contains "$TMP/checker_interp_use.err" 'unused variable `name`' 'checker counts interpolation references as uses'
assert_not_contains "$TMP/checker_interp_use.err" 'unused variable `needle`' 'checker counts method-call arguments as uses'

# Core VM/Bash parity: methods, split, interpolation formats, triple strings, side effects.
write_fixture "$FIX/core.ds" <<'DS'
let a = "  hello  ".trim()
let b = "\t hello \n".trim()
let c = "".trim()
let d = "   ".trim()
echo "trim=[{a}][{b}][{c}][{d}]"
let upper = "abc XYZ 123".upper()
let lower = "abc XYZ 123".lower()
echo "case={upper}|{lower}"
let r1 = "hello hello".replace("hello", "hi")
let r2 = "aaaa".replace("aa", "b")
let r3 = "abc".replace("z", "x")
let r4 = "a-b-c".replace("-", "")
echo "replace={r1}|{r2}|{r3}|{r4}"
let s = "release/api"
if s.contains("api") { echo "contains" }
if s.starts_with("release/") { echo "starts" }
if s.ends_with("api") { echo "ends" }
if !s.contains("web") { echo "missing" }
if s.contains("") { echo "contains-empty" }
if s.starts_with("") { echo "starts-empty" }
if s.ends_with("") { echo "ends-empty" }
let paren = ("  Paren  ").trim().lower()
echo "paren={paren}"
let loop_text = "alpha"
while loop_text.contains("a") {
  echo "while-predicate"
  loop_text = "looped"
}
case s.contains("api") {
  true { echo "case-predicate" }
  _ { echo "case-missing" }
}
let parts = "a,,b".split(",")
for x in parts {
  echo "part=[{x}]"
}
let multi = "one<>two<>".split("<>")
let multi0 = multi[0]
let multi1 = multi[1]
let multi2 = multi[2]
echo "multi0={multi0} multi1={multi1} multi2=[{multi2}]"
let label = "api"
let n = 7
let neg = 0 - 7
echo "fmt=[{label:<6}][{label:>6}][{label:^7}][{label:<2}]"
echo "num={n:03d}|{neg:04d}|{n:5d}|{n:.2f}|{n:6.2f}"
let padded = "  api  "
echo "xform={label:upper}|{label:lower}|{padded:trim}"
let body = """
line one
line two
"""
echo "body={body:trim}"
echo "wrote" |> "side.txt"
DS
assert_vm_bash_env_parity core "$FIX/core.ds" 0 'side.txt'
cat >"$TMP/core_expected.out" <<'OUT'
trim=[hello][hello][][]
case=ABC XYZ 123|abc xyz 123
replace=hi hi|bb|abc|abc
contains
starts
ends
missing
contains-empty
starts-empty
ends-empty
paren=paren
while-predicate
case-predicate
part=[a]
part=[]
part=[b]
multi0=one multi1=two multi2=[]
fmt=[api   ][   api][  api  ][api]
num=007|-007|    7|7.00|  7.00
xform=API|api|api
body=line one
line two
OUT
assert_same "$TMP/core_expected.out" "$TMP/core_vm.out" 'core VM stdout exact'
assert_same "$TMP/core_expected.out" "$TMP/core_bash.out" 'core Bash stdout exact'

# Method chains and command/pipeline receivers.
write_fixture "$FIX/receivers.ds" <<'DS'
let raw = "  Hello World  "
let a = raw.trim().lower().replace(" ", "-")
let r = run emit_branch
let branch = r.stdout.trim().upper()
echo "{a}"
echo "{branch}"
let p = run emit_services | filter_api
let services = p.stdout.trim().split(",")
for service in services { echo "svc={service:upper}" }
DS
assert_vm_bash_env_parity receivers "$FIX/receivers.ds" 0 ''
cat >"$TMP/receivers_expected.out" <<'OUT'
hello-world
BRANCH
svc=API
svc=WEB
svc=WORKER
OUT
assert_same "$TMP/receivers_expected.out" "$TMP/receivers_vm.out" 'receivers VM stdout exact'

# Script arguments and tests integration.
write_fixture "$FIX/script_args.ds" <<'DS'
script {
  arg app: string
  option stage: string = " staging "
}

echo "app={app:lower}"
echo "stage={stage:trim}"
DS
assert_vm_bash_env_parity script_args "$FIX/script_args.ds" 0 '' 'API' '--stage' ' prod '
cat >"$TMP/script_args_expected.out" <<'OUT'
app=api
stage=prod
OUT
assert_same "$TMP/script_args_expected.out" "$TMP/script_args_vm.out" 'script args VM stdout exact'

write_fixture "$FIX/tests.ds" <<'DS'
test "strings" {
  let s = " api ".trim()
  assert s == "api"
  assert s.starts_with("a")
  let xs = "a,b".split(",")
  assert xs[1] == "b"
}

echo "normal"
DS
run_ok string_tests "$DS" test "$FIX/tests.ds"
assert_contains "$TMP/string_tests.out" 'strings' 'ds test discovers string test'
assert_contains "$TMP/string_tests.out" 'passed' 'ds test reports pass'
assert_vm_bash_env_parity tests_ignored "$FIX/tests.ds" 0 ''
assert_same_text 'normal
' "$TMP/tests_ignored_vm.out" 'run ignores test block'

# Imports and unknown-kind function parameter safety.
write_fixture "$FIX/lib.ds" <<'DS'
let lib_name = "  LIB  ".trim().lower()
DS
write_fixture "$FIX/import_main.ds" <<'DS'
import "./lib.ds"
echo "lib={lib_name:upper}"
DS
assert_vm_bash_env_parity imports "$FIX/import_main.ds" 0 ''
assert_same_text 'lib=LIB
' "$TMP/imports_vm.out" 'imported string helper output'
helper_count="$(grep -c '^__ds_string_upper()' "$TMP/imports.sh" || true)"
[ "$helper_count" = 1 ] || fail "expected upper helper exactly once, got $helper_count"
pass 'Bash helper emitted once for import fixture'
assert_not_contains "$TMP/imports.sh" 'BASH_VERSINFO' 'upper/lower interpolation stays Bash 3 compatible without helper guard'
assert_not_contains "$TMP/imports.sh" '${__ds_lib_name^^}' 'upper interpolation uses guarded helper instead of inline Bash 4 expansion'

write_fixture "$FIX/short_colon_fragments.ds" <<'DS'
let x = "ok"
echo ":"
echo ":t"
echo ":tr"
echo ":tri"
echo ":trim"
echo "{x:upper}"
DS
run_ok short_colon_check "$DS" check "$FIX/short_colon_fragments.ds"
run_ok short_colon_emit "$DS" emit bash "$FIX/short_colon_fragments.ds" -o "$TMP/short_colon.sh"
run_ok short_colon_bash_n bash -n "$TMP/short_colon.sh"
assert_not_contains "$TMP/short_colon.sh" 'BASH_VERSINFO' 'short colon fragments with upper format stay Bash 3 compatible without helper guard'

write_fixture "$FIX/function_param_rejected.ds" <<'DS'
fn show(name) {
  let normalized = name.trim()
  echo "{normalized}"
}
show(" api ")
DS
run_ok function_param_check "$DS" check "$FIX/function_param_rejected.ds"
assert_vm_bash_env_parity function_param_rejected "$FIX/function_param_rejected.ds" 0 ""
assert_contains "$TMP/function_param_rejected_vm.out" 'api' 'function parameter string method accepted after v0.36 inference'

# Formatter behavior.
write_fixture "$FIX/unformatted.ds" <<'DS'
let s="  X  ".trim().lower().replace("x","y")
echo "{s:upper}"
DS
capture_status fmt_unformatted_check "$DS" fmt --check "$FIX/unformatted.ds"
assert_nonzero_status fmt_unformatted_check
run_ok fmt_output "$DS" fmt "$FIX/unformatted.ds"
assert_contains "$TMP/fmt_output.out" 'let s = "  X  ".trim().lower().replace("x", "y")' 'formatter normalizes method spacing'
printf '%s' "$(cat "$TMP/fmt_output.out")" >"$TMP/formatted.ds"
run_ok fmt_second "$DS" fmt "$TMP/formatted.ds"
assert_same "$TMP/fmt_output.out" "$TMP/fmt_second.out" 'formatter idempotent for method chain'
write_fixture "$FIX/triple_fmt.ds" <<'DS'
let body = """
line one
  indented
"""
echo "{body}"
DS
run_ok fmt_triple "$DS" fmt "$FIX/triple_fmt.ds"
assert_contains "$TMP/fmt_triple.out" '  indented' 'formatter preserves triple-quoted body indentation'
write_fixture "$FIX/comment_fmt.ds" <<'DS'
# comment
let s = "x"
DS
capture_status fmt_comment "$DS" fmt "$FIX/comment_fmt.ds"
assert_nonzero_status fmt_comment
assert_diag "$TMP/fmt_comment.err" 'formatter cannot preserve comments yet' 'formatter still rejects comments'

# Checker must not execute commands.
write_fixture "$FIX/check_safe.ds" <<'DS'
let r = run must_not_run
let s = r.stdout.trim()
if s.contains("x") { echo "x" }
DS
run_env_ok check_safe "$DS" check "$FIX/check_safe.ds"
[ ! -e "$ROOT/must_not_exist.txt" ] || fail 'check executed command in project root'
[ ! -e "$TMP/must_not_exist.txt" ] || fail 'check executed command in temp dir'
pass 'ds check does not execute run commands'

# Negative parser/check/emission diagnostics and atomic-ish failed emission.
invalids=(
  'dangling_dot|let x = "a".|expected field name after `.`'
  'trim_arity|let x = "a".trim(1)|expects 1 arguments including receiver'
  'unknown_method|let x = "a".unknown()|unknown string method'
  'unknown_string_expr|let x = string.nope("a")|unknown string method `nope`'
  'unknown_string_stmt|string.nope("a")|unknown string method `nope`'
  'fn_string_namespace|fn string() {\n  echo "bad"\n}|conflicts with a v0.11.0 standard-library helper name'
  'fn_file_namespace|fn file() {\n  echo "bad"\n}|conflicts with a v0.11.0 standard-library helper name'
  'split_empty|let x = "a".split("")|split with an empty separator is deferred'
  'replace_empty|let x = "a".replace("", "b")|replace with an empty source is deferred'
  'empty_spec|let name = "x"\necho "{name:}"|unsupported interpolation format specifier'
  'bad_spec|let name = "x"\necho "{name:?}"|unsupported interpolation format specifier'
  'huge_spec|let name = "x"\necho "{name:999999999999999999999d}"|unsupported interpolation format specifier'
  'wrong_type|let name = "x"\necho "{name:05d}"|unsupported interpolation format specifier'
  'bad_percent|let name = "x"\necho "{name:%s}"|unsupported interpolation format specifier'
  'regex_method|let x = "abc".regex_replace("a", "b")|unknown string method'
  'decimal|let x = 1.25|expected field name after `.`'
)
for item in "${invalids[@]}"; do
  IFS='|' read -r name source fragment <<<"$item"
  file="$FIX/invalid_$name.ds"
  printf '%b
' "$source" >"$file"
  capture_status "${name}_check" "$DS" check "$file"
  assert_nonzero_status "${name}_check"
  assert_diag "$TMP/${name}_check.err" "$fragment" "$name check diagnostic"
  out="$TMP/${name}_bad.sh"
  rm -f "$out"
  capture_status "${name}_emit" "$DS" emit bash "$file" -o "$out"
  assert_nonzero_status "${name}_emit"
  assert_file_missing_or_empty "$out" "$name failed emit does not write output"
done
write_fixture "$FIX/unterminated_triple.ds" <<'DS'
let body = """
unterminated
DS
capture_status unterminated_check "$DS" check "$FIX/unterminated_triple.ds"
assert_nonzero_status unterminated_check
assert_diag "$TMP/unterminated_check.err" 'unterminated triple-quoted string literal' 'unterminated triple diagnostic'

# Regression examples smoke.
for example in examples/*.ds; do
  base="$(basename "$example" .ds)"
  if [ "$base" = bad ]; then
    capture_status "example_${base}_check" "$DS" check "$example"
    assert_nonzero_status "example_${base}_check"
    assert_diag "$TMP/example_${base}_check.err" 'expected expression after `=`' 'bad example remains invalid'
    continue
  fi
  run_env_ok "example_${base}_check" "$DS" check "$example"
  case "$base" in
    args) assert_vm_bash_env_parity "example_${base}" "$ROOT/$example" 0 '' api --target staging ;;
    *) assert_vm_bash_env_parity "example_${base}" "$ROOT/$example" 0 '' ;;
  esac
done

# Manual-smoke equivalent for strings example.
run_env_ok strings_example_check "$DS" check examples/strings.ds
assert_vm_bash_env_parity strings_example "$ROOT/examples/strings.ds" 0 ''
run_ok strings_example_fmt_check "$DS" fmt --check examples/strings.ds

printf 'v0.19 tests passed: %s assertions\n' "$pass_count"
