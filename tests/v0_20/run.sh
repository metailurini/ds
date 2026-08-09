#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_20_tests.$$"
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

capture_in_dir() {
  local name="$1" dir="$2"; shift 2
  mkdir -p "$dir"
  set +e
  (cd "$dir" && "$@") >"$TMP/$name.out" 2>"$TMP/$name.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/$name.rc"
}

run_in_dir_ok() {
  local name="$1" dir="$2"; shift 2
  mkdir -p "$dir"
  (cd "$dir" && "$@") >"$TMP/$name.out" 2>"$TMP/$name.err" || {
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected success"
  }
  pass "$name"
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

assert_file_equals() {
  local file="$1" expected="$2" name="$3"
  local expected_file="$TMP/${name//[^A-Za-z0-9_]/_}.expected"
  printf '%s' "$expected" >"$expected_file"
  assert_same "$expected_file" "$file" "$name"
}

assert_vm_bash_parity_args() {
  local name="$1" fixture="$2" expected_status="$3" output_files="$4"; shift 4
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/$name.sh"
  mkdir -p "$vm_work" "$bash_work"

  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
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

FIX="$TMP/fixtures with spaces"
mkdir -p "$FIX"

# Static and build wiring tests.
assert_contains Makefile '0-20' 'TEST_VERSIONS contains v0.20'
assert_matches Makefile '^TEST_VERSIONS := .*0-19 0-20($| )' 'v0.20 follows v0.19 in TEST_VERSIONS'
assert_contains Makefile 'DS_SKIP_BUILD=1 ./tests/v$(subst -,_,$(patsubst test-v%,%,$@))/run.sh' 'pattern target invokes version suite'
assert_contains Makefile '$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address" test' 'asan runs aggregate tests'
assert_contains Makefile '$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined" test' 'ubsan runs aggregate tests'
assert_not_matches src/lexer.c 'TOKEN_(RETURN|UNTIL|TRAP)' 'no deferred keyword token added'
assert_not_matches src/parser.c 'TOKEN_(RETURN|UNTIL|TRAP)' 'parser has no deferred keyword token handling'
assert_not_contains include/ds.h 'hashmap' 'public header does not expose hashmap internals'
assert_matches src/runtime/hashmap.c '__ds|hashmap|Ds' 'runtime hashmap remains private implementation file'

# Examples remain coherent.
for example in basic args import-main command-result redirection functions collections control-flow pipeline strings stdlib vm; do
  path="examples/$example.ds"
  run_ok "example_${example}_check" "$DS" check "$path"
  run_ok "example_${example}_emit" "$DS" emit bash "$path" -o "$TMP/example_${example}.sh"
  run_ok "example_${example}_bash_n" bash -n "$TMP/example_${example}.sh"
  assert_not_matches "$TMP/example_${example}.sh" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$example emitted Bash is standalone"
done
run_fail example_bad_check "$DS" check examples/bad.ds
assert_diag "$TMP/example_bad_check.err" 'expected expression' 'bad example stays invalid'
run_ok control_flow_example_fmt "$DS" fmt --check examples/control-flow.ds
run_ok pipeline_example_fmt "$DS" fmt --check examples/pipeline.ds
run_ok strings_example_fmt "$DS" fmt --check examples/strings.ds

# Cross-feature VM/Bash parity: control flow + split/indexing + formatting.
write_fixture "$FIX/control_strings.ds" <<'DS'
let raw = " api,web,,worker "
let parts = raw.trim().split(",")
let i = 0
while i < 4 {
  let part = parts[i].trim()
  if part == "" {
    i += 1
    continue
  }
  if part.starts_with("w") {
    echo "web-ish={part:upper}"
  } else {
    echo "svc={part:<6}done"
  }
  i += 1
}
DS
assert_vm_bash_parity_args control_strings "$FIX/control_strings.ds" 0 ''
cat >"$TMP/control_strings.expected" <<'OUT'
svc=api   done
web-ish=WEB
web-ish=WORKER
OUT
assert_same "$TMP/control_strings.expected" "$TMP/control_strings_vm.out" 'control/string composition exact stdout'

write_fixture "$FIX/nested_loops.ds" <<'DS'
let outer = ["a", "b"]
let inner = ["1", "skip", "2", "stop", "3"]
for o in outer {
  for i in inner {
    if i == "skip" {
      continue
    }
    if i == "stop" {
      break
    }
    echo "{o}:{i}"
  }
}
DS
assert_vm_bash_parity_args nested_loops "$FIX/nested_loops.ds" 0 ''
cat >"$TMP/nested_loops.expected" <<'OUT'
a:1
a:2
b:1
b:2
OUT
assert_same "$TMP/nested_loops.expected" "$TMP/nested_loops_vm.out" 'nested loop lexical break/continue stdout'

write_fixture "$FIX/captured_pipeline_loop.ds" <<'DS'
let i = 0
while i < 3 {
  let r = run printf "b\na\n" | sort
  if r.failed {
    fail "pipeline failed"
  }
  let sorted = r.stdout.trim().replace("\n", ",")
  echo "round={i}:{sorted}"
  i += 1
}
DS
assert_vm_bash_parity_args captured_pipeline_loop "$FIX/captured_pipeline_loop.ds" 0 ''
cat >"$TMP/captured_pipeline_loop.expected" <<'OUT'
round=0:a,b
round=1:a,b
round=2:a,b
OUT
assert_same "$TMP/captured_pipeline_loop.expected" "$TMP/captured_pipeline_loop_vm.out" 'captured pipeline loop exact stdout'

write_fixture "$FIX/case_strings.ds" <<'DS'
let label = " Production "
let normalized = label.trim().lower()
case normalized {
  "production" { echo "prod" }
  "staging" { echo "stage" }
  _ { echo "other" }
}

case normalized.starts_with("prod") {
  true { echo "prefix" }
  _ { echo "no-prefix" }
}
DS
assert_vm_bash_parity_args case_strings "$FIX/case_strings.ds" 0 ''
printf 'prod\nprefix\n' >"$TMP/case_strings.expected"
assert_same "$TMP/case_strings.expected" "$TMP/case_strings_vm.out" 'string-derived case selectors exact stdout'

write_fixture "$FIX/script_args.ds" <<'DS'
script {
  arg app: string
  option target: string = "dev"
  option retries: int = 2
  flag force: bool = false
}

let normalized = target.trim().lower()
let i = 0
while i < retries {
  echo "{i:02d}:{app:<6}:{normalized:upper}:{force}"
  i += 1
}
DS
assert_vm_bash_parity_args script_args "$FIX/script_args.ds" 0 '' api --target ' Prod ' --retries 3 --force
cat >"$TMP/script_args.expected" <<'OUT'
00:api   :PROD:true
01:api   :PROD:true
02:api   :PROD:true
OUT
assert_same "$TMP/script_args.expected" "$TMP/script_args_vm.out" 'script args formatting/control exact stdout'

mkdir -p "$FIX/imports"
write_fixture "$FIX/imports/lib.ds" <<'DS'
fn print_services(csv = "api,web") {
  let services = csv.split(",")
  for service in services {
    let cleaned = service.trim().lower()
    if cleaned == "" {
      continue
    }
    echo "service={cleaned:<8}done"
  }
}
DS
write_fixture "$FIX/imports/main.ds" <<'DS'
import "./lib.ds"
print_services(" API, ,Worker ")
DS
assert_vm_bash_parity_args imports_functions "$FIX/imports/main.ds" 0 ''
cat >"$TMP/imports_functions.expected" <<'OUT'
service=api     done
service=worker  done
OUT
assert_same "$TMP/imports_functions.expected" "$TMP/imports_functions_vm.out" 'imports/functions Wave 2 stdout'

write_fixture "$FIX/test_blocks.ds" <<'DS'
test "strings and loops" {
  let parts = "a,b,c".split(",")
  let i = 0
  while i < 3 {
    assert parts[i] != ""
    i += 1
  }
}

echo "normal"
DS
run_ok wave2_test_blocks "$DS" test "$FIX/test_blocks.ds"
assert_contains "$TMP/wave2_test_blocks.out" 'strings and loops' 'test block with Wave 2 features runs'
assert_vm_bash_parity_args test_blocks_normal "$FIX/test_blocks.ds" 0 ''
printf 'normal\n' >"$TMP/test_blocks_normal.expected"
assert_same "$TMP/test_blocks_normal.expected" "$TMP/test_blocks_normal_vm.out" 'normal run ignores test blocks'

# Pipeline behavior tests.
write_fixture "$FIX/plain_pipeline.ds" <<'DS'
printf "b\na\n" | sort
DS
assert_vm_bash_parity_args plain_pipeline "$FIX/plain_pipeline.ds" 0 ''
printf 'a\nb\n' >"$TMP/plain_pipeline.expected"
assert_same "$TMP/plain_pipeline.expected" "$TMP/plain_pipeline_vm.out" 'plain pipeline exact stdout'

write_fixture "$FIX/plain_pipeline_fail.ds" <<'DS'
false | cat
echo "unreachable"
DS
assert_vm_bash_parity_args plain_pipeline_fail "$FIX/plain_pipeline_fail.ds" 1 ''
assert_not_contains "$TMP/plain_pipeline_fail_vm.out" 'unreachable' 'VM plain pipeline fail-fast skips later command'
assert_not_contains "$TMP/plain_pipeline_fail_bash.out" 'unreachable' 'Bash plain pipeline fail-fast skips later command'

write_fixture "$FIX/captured_pipeline_fail.ds" <<'DS'
let r = run false | cat
echo "failed={r.failed} code={r.code}"
DS
assert_vm_bash_parity_args captured_pipeline_fail "$FIX/captured_pipeline_fail.ds" 0 ''
assert_contains "$TMP/captured_pipeline_fail_vm.out" 'failed=true code=' 'captured pipeline failure is inspectable'

write_fixture "$FIX/redirect_pipeline.ds" <<'DS'
printf "b\na\n" | sort |> "out.txt"
let data = lines("out.txt")
for line in data {
  echo "line={line}"
}
DS
assert_vm_bash_parity_args redirect_pipeline "$FIX/redirect_pipeline.ds" 0 'out.txt'
printf 'line=a\nline=b\n' >"$TMP/redirect_pipeline.expected"
assert_same "$TMP/redirect_pipeline.expected" "$TMP/redirect_pipeline_vm.out" 'pipeline redirection exact stdout'
printf 'a\nb\n' >"$TMP/redirect_pipeline_file.expected"
assert_same "$TMP/redirect_pipeline_file.expected" "$TMP/redirect_pipeline_vm_work/out.txt" 'pipeline redirection writes sorted data'

for bad in leading trailing double between captured_redirect; do
  file="$FIX/pipeline_bad_$bad.ds"
  case "$bad" in
    leading) printf '| cat\n' >"$file" ;;
    trailing) printf 'echo hi |\n' >"$file" ;;
    double) printf 'echo hi || cat\n' >"$file" ;;
    between) printf 'printf "x\\n" |> "mid.txt" | cat\n' >"$file" ;;
    captured_redirect) printf 'let r = run printf "x\\n" |> "out.txt" | cat\n' >"$file" ;;
  esac
  run_fail "pipeline_bad_${bad}_check" "$DS" check "$file"
  assert_contains "$TMP/pipeline_bad_${bad}_check.err" ': error:' "bad pipeline $bad emits error"
done

# String and formatting edge tests.
write_fixture "$FIX/string_chain.ds" <<'DS'
let s = " aa aa "
let out = s.trim().replace("aa", "b").upper().replace("B", "cc")
echo "{out}"
DS
assert_vm_bash_parity_args string_chain "$FIX/string_chain.ds" 0 ''
printf 'cc cc\n' >"$TMP/string_chain.expected"
assert_same "$TMP/string_chain.expected" "$TMP/string_chain_vm.out" 'string chain exact stdout'

write_fixture "$FIX/replace_edges.ds" <<'DS'
let repeated = "aaaa".replace("aa", "b")
let no_match = "abc".replace("x", "y")
let longer = "xx".replace("x", "long")
let removed = "xx".replace("x", "")
echo "{repeated}"
echo "{no_match}"
echo "{longer}"
echo "{removed}"
DS
assert_vm_bash_parity_args replace_edges "$FIX/replace_edges.ds" 0 ''
cat >"$TMP/replace_edges.expected" <<'OUT'
bb
abc
longlong

OUT
assert_same "$TMP/replace_edges.expected" "$TMP/replace_edges_vm.out" 'replace edge exact stdout'
write_fixture "$FIX/replace_empty_from.ds" <<'DS'
echo "abc".replace("", "x")
DS
run_fail replace_empty_from_check "$DS" check "$FIX/replace_empty_from.ds"
assert_diag "$TMP/replace_empty_from_check.err" 'empty' 'empty replace source rejected'

write_fixture "$FIX/split_edges.ds" <<'DS'
let parts = ",a,,".split(",")
let i = 0
while i < 4 {
  let part = parts[i]
  echo "{i}:{part}"
  i += 1
}
DS
assert_vm_bash_parity_args split_edges "$FIX/split_edges.ds" 0 ''
cat >"$TMP/split_edges.expected" <<'OUT'
0:
1:a
2:
3:
OUT
assert_same "$TMP/split_edges.expected" "$TMP/split_edges_vm.out" 'split edge exact stdout'

write_fixture "$FIX/format_bounds.ds" <<'DS'
let name = "abcdef"
let one = 1
let seven = 7
let n = 0 - 7
echo "<{name:<1}>"
echo "<{name:^1}>"
echo "{seven:01d}"
echo "{seven:1024d}"
echo "{one:.1024f}"
echo "{n:04d}"
DS
assert_vm_bash_parity_args format_bounds "$FIX/format_bounds.ds" 0 ''
assert_contains "$TMP/format_bounds_vm.out" '<abcdef>' 'left width does not truncate'
assert_contains "$TMP/format_bounds_vm.out" '-007' 'negative zero padding is sign-aware'
for spec in d f; do
  file="$FIX/format_bad_${spec}.ds"
  if [ "$spec" = d ]; then
    printf 'let n = 7\necho "{%s}"\n' 'n:1025d' >"$file"
    msg='unsupported interpolation format specifier'
  else
    printf 'let n = 1\necho "{%s}"\n' 'n:.1025f' >"$file"
    msg='unsupported interpolation format specifier'
  fi
  run_fail "format_bad_${spec}_check" "$DS" check "$file"
  assert_diag "$TMP/format_bad_${spec}_check.err" "$msg" "oversized format $spec rejected"
done

write_fixture "$FIX/format_guards.ds" <<'DS'
let label = "api"
echo "{label:upper}"
echo "{label:lower}"
echo "{label:trim}"
DS
assert_vm_bash_parity_args format_guards "$FIX/format_guards.ds" 0 ''
script="$TMP/format_guards.sh"
assert_not_contains "$script" 'BASH_VERSINFO' 'format helpers do not require Bash 4 guard'
assert_not_contains "$script" '__ds_run_pipeline' 'string-only program does not emit pipeline helper'

write_fixture "$FIX/triple.ds" <<'DS'
let name = "ds"
let text = """
hello {name}
line two "quote" \\ slash
"""
echo "<{text}>"
DS
assert_vm_bash_parity_args triple "$FIX/triple.ds" 0 ''
run_ok triple_fmt "$DS" fmt "$FIX/triple.ds"
assert_contains "$TMP/triple_fmt.out" 'line two "quote" \\ slash' 'formatter preserves triple-quoted body text'

# Case/control-flow diagnostics.
write_fixture "$FIX/break_bad.ds" <<'DS'
break
DS
run_fail break_bad_check "$DS" check "$FIX/break_bad.ds"
assert_diag "$TMP/break_bad_check.err" 'break' 'break outside loop rejected'
write_fixture "$FIX/continue_bad.ds" <<'DS'
continue
DS
run_fail continue_bad_check "$DS" check "$FIX/continue_bad.ds"
assert_diag "$TMP/continue_bad_check.err" 'continue' 'continue outside loop rejected'
write_fixture "$FIX/reassign_bad.ds" <<'DS'
let xs = [1]
xs = [2]
DS
run_fail reassign_bad_check "$DS" check "$FIX/reassign_bad.ds"
assert_diag "$TMP/reassign_bad_check.err" 'collection reassignment' 'collection reassignment remains rejected'
write_fixture "$FIX/case_kind_exact.ds" <<'DS'
case true {
  "true" { echo "bad" }
  _ { echo "default" }
}
DS
assert_vm_bash_parity_args case_kind_exact "$FIX/case_kind_exact.ds" 0 ''
assert_file_equals "$TMP/case_kind_exact_vm.out" $'default\n' 'case kind mismatch stays exact and falls through'
write_fixture "$FIX/case_default_bad.ds" <<'DS'
case "x" {
  _ { echo "first" }
  _ { echo "second" }
}
DS
run_fail case_default_bad_check "$DS" check "$FIX/case_default_bad.ds"
assert_diag "$TMP/case_default_bad_check.err" 'default' 'duplicate case default rejected'
write_fixture "$FIX/string_bool_receiver_bad.ds" <<'DS'
let s = "x"
let y = s.contains("x").trim()
DS
run_fail string_bool_receiver_bad_check "$DS" check "$FIX/string_bool_receiver_bad.ds"
assert_diag "$TMP/string_bool_receiver_bad_check.err" 'string.trim' 'string method on bool receiver rejected'
write_fixture "$FIX/fn_param_receiver.ds" <<'DS'
fn show(name) {
  echo "{name.trim()}"
}

show("  api  ")
DS
run_ok fn_param_receiver_check "$DS" check "$FIX/fn_param_receiver.ds"
assert_vm_bash_parity_args fn_param_receiver "$FIX/fn_param_receiver.ds" 0 ""
assert_file_equals "$TMP/fn_param_receiver_vm.out" $'api\n' 'function parameter receiver now infers string'

write_fixture "$FIX/fn_default_string_case.ds" <<'DS'
fn show(name = "api") {
  case name {
    "api" { echo yes }
    _ { echo no }
  }
}
show()
DS
assert_vm_bash_parity_args fn_default_string_case "$FIX/fn_default_string_case.ds" 0 ''
assert_file_equals "$TMP/fn_default_string_case_vm.out" $'yes\n' 'defaulted string parameter case selector matches by kind'

write_fixture "$FIX/fn_default_scalar_case.ds" <<'DS'
fn show(n = 2, b = true) {
  case n {
    2 { echo two }
    _ { echo other }
  }
  case b {
    true { echo yes }
    _ { echo no }
  }
}
show()
DS
assert_vm_bash_parity_args fn_default_scalar_case "$FIX/fn_default_scalar_case.ds" 0 ''
assert_file_equals "$TMP/fn_default_scalar_case_vm.out" $'two\nyes\n' 'defaulted int/bool parameters case selectors match by kind'
assert_contains "$TMP/fn_default_scalar_case.sh" '__ds_type_n' 'defaulted int parameter emits Bash type tag'
assert_contains "$TMP/fn_default_scalar_case.sh" '__ds_type_b' 'defaulted bool parameter emits Bash type tag'

# Formatter behavior.
write_fixture "$FIX/format_mix.ds" <<'DS'
let xs = ["api", "skip", "web"]
let i = 0
while i < 3 {
  let item = xs[i].trim()
  case item {
    "skip" {
      i += 1
      continue
    }
    "web" {
      echo "web={item:upper}"
      break
    }
    _ {
      echo "svc={item:<6}done"
    }
  }
  i += 1
}
let r = run printf "b\na\n" | sort
let text = """
{r.stdout}
"""
echo "<{text}>"
DS
run_ok fmt_mix_once "$DS" fmt "$FIX/format_mix.ds"
run_ok fmt_mix_twice "$DS" fmt "$TMP/fmt_mix_once.out"
assert_same "$TMP/fmt_mix_once.out" "$TMP/fmt_mix_twice.out" 'formatter is idempotent for mixed Wave 2 syntax'
run_ok fmt_mix_check "$DS" check "$TMP/fmt_mix_once.out"
assert_vm_bash_parity_args fmt_mix_before "$FIX/format_mix.ds" 0 ''
assert_vm_bash_parity_args fmt_mix_after "$TMP/fmt_mix_once.out" 0 ''
assert_same "$TMP/fmt_mix_before_vm.out" "$TMP/fmt_mix_after_vm.out" 'formatted mixed script preserves VM stdout'

write_fixture "$FIX/comment_fmt.ds" <<'DS'
# header comment
let x = "api" # trailing comment
echo "{x}"
DS
run_ok comment_check "$DS" check "$FIX/comment_fmt.ds"
run_fail comment_fmt "$DS" fmt "$FIX/comment_fmt.ds"
assert_contains "$TMP/comment_fmt.err" 'cannot preserve comments' 'formatter rejects comments clearly'
work_noexec="$TMP/fmt_noexec_work"
mkdir -p "$work_noexec"
write_fixture "$work_noexec/noexec.ds" <<'DS'
printf "should-not-run" |> "created-by-command.txt"
DS
run_in_dir_ok fmt_noexec "$work_noexec" "$DS" fmt "$work_noexec/noexec.ds"
[ ! -e "$work_noexec/created-by-command.txt" ] || fail 'fmt should not execute commands'
pass 'fmt does not execute commands'
run_in_dir_ok check_noexec "$work_noexec" "$DS" check "$work_noexec/noexec.ds"
[ ! -e "$work_noexec/created-by-command.txt" ] || fail 'check should not execute commands'
pass 'check does not execute commands'

# Helper emission.
write_fixture "$FIX/minimal.ds" <<'DS'
echo "hello"
DS
run_ok minimal_emit "$DS" emit bash "$FIX/minimal.ds" -o "$TMP/minimal.sh"
assert_contains "$TMP/minimal.sh" '#!/usr/bin/env bash' 'minimal emitted Bash has shebang'
assert_contains "$TMP/minimal.sh" 'set -euo pipefail' 'minimal emitted Bash has strict mode'
assert_not_contains "$TMP/minimal.sh" '__ds_string_' 'minimal program has no string helpers'
assert_not_contains "$TMP/minimal.sh" '__ds_run_pipeline' 'minimal program has no pipeline helpers'
assert_not_contains "$TMP/minimal.sh" '__ds_command_' 'minimal program has no command-result helpers'

write_fixture "$FIX/helper_combo.ds" <<'DS'
let a = " API ".trim().lower()
let b = "web,worker".split(",")
printf "x\n" | cat
for item in b {
  echo "{a}:{item:upper}"
}
DS
assert_vm_bash_parity_args helper_combo "$FIX/helper_combo.ds" 0 ''
run_ok helper_combo_emit2 "$DS" emit bash "$FIX/helper_combo.ds" -o "$TMP/helper_combo_2.sh"
assert_same "$TMP/helper_combo.sh" "$TMP/helper_combo_2.sh" 'repeated Bash emission is deterministic'
for helper in __ds_string_trim __ds_string_lower __ds_string_split __ds_string_upper; do
  count="$(grep -c "^${helper}()" "$TMP/helper_combo.sh" | tr -d ' ')"
  [ "$count" = 1 ] || fail "$helper emitted $count times"
  pass "$helper emitted once"
done
assert_not_contains "$TMP/helper_combo.sh" 'BASH_VERSINFO' 'string/pipeline helper combo does not require Bash 4 guard'

mkdir -p "$FIX/import_helpers"
write_fixture "$FIX/import_helpers/lib.ds" <<'DS'
fn lib(name = " API ") {
  echo "lib={name:trim}"
}
DS
write_fixture "$FIX/import_helpers/main.ds" <<'DS'
import "./lib.ds"
let local = " WEB ".trim()
lib()
echo "local={local}"
DS
assert_vm_bash_parity_args import_helpers "$FIX/import_helpers/main.ds" 0 ''
[ "$(grep -c '^__ds_string_trim()' "$TMP/import_helpers.sh" | tr -d ' ')" = 1 ] || fail 'import helper deduplicates string trim'
pass 'import helper deduplicates string trim'

# Checker and warning tests.
write_fixture "$FIX/warnings.ds" <<'DS'
let used_case = "api"
let used_format = "web"
let used_command = "worker"
let used_run = run printf "ok\n"
let i = 0
while i < 1 {
  let unused_inside = "x"
  i += 1
}
case used_case {
  "api" { echo "case" }
  _ { echo "other" }
}
echo "fmt={used_format}"
echo $used_command
echo "run={used_run.stdout}"
DS
capture_status warnings_check "$DS" check "$FIX/warnings.ds"
assert_status warnings_check 0
assert_contains "$TMP/warnings_check.err" 'unused variable `unused_inside`' 'unused local inside while warns'
assert_not_contains "$TMP/warnings_check.err" 'used_case' 'case use counts as used'
assert_not_contains "$TMP/warnings_check.err" 'used_format' 'format interpolation use counts as used'
assert_not_contains "$TMP/warnings_check.err" 'used_command' 'command interpolation use counts as used'
assert_not_contains "$TMP/warnings_check.err" 'used_run' 'captured pipeline use counts as used'
capture_status warnings_as_errors "$DS" check --warnings-as-errors "$FIX/warnings.ds"
assert_nonzero_status warnings_as_errors
assert_contains "$TMP/warnings_as_errors.err" 'warning:' 'warnings-as-errors reports warning text'
run_ok warnings_suppressed "$DS" check --no-warnings "$FIX/warnings.ds"
assert_same_text '' "$TMP/warnings_suppressed.err" 'no-warnings suppresses warning output'
capture_status warnings_flags_bad "$DS" check --warnings-as-errors --no-warnings "$FIX/warnings.ds"
assert_nonzero_status warnings_flags_bad
assert_contains "$TMP/warnings_flags_bad.err" 'invalid' 'conflicting warning flags rejected'

# Regression tests from previous reviews.
write_fixture "$FIX/colon_fragments.ds" <<'DS'
echo ":"
echo ":t"
echo ":tr"
echo ":tri"
echo ":trim"
let frag = ":t"
echo "{frag}"
DS
assert_vm_bash_parity_args colon_fragments "$FIX/colon_fragments.ds" 0 ''
write_fixture "$FIX/center_align.ds" <<'DS'
let label = "x"
echo "<{label:^5}>"
DS
assert_vm_bash_parity_args center_align "$FIX/center_align.ds" 0 ''
printf '<  x  >\n' >"$TMP/center_align.expected"
assert_same "$TMP/center_align.expected" "$TMP/center_align_vm.out" 'center alignment stays centered'
write_fixture "$FIX/cmd_word_helpers.ds" <<'DS'
let label = " api "
echo "{label:trim}"
DS
assert_vm_bash_parity_args cmd_word_helpers "$FIX/cmd_word_helpers.ds" 0 ''
assert_contains "$TMP/cmd_word_helpers.sh" '__ds_string_trim' 'command-word interpolation emits string helper'
write_fixture "$FIX/bool_case_kind.ds" <<'DS'
let label = "api"
case label.starts_with("a") {
  true { echo "yes" }
  _ { echo "no" }
}
DS
assert_vm_bash_parity_args bool_case_kind "$FIX/bool_case_kind.ds" 0 ''
printf 'yes\n' >"$TMP/bool_case_kind.expected"
assert_same "$TMP/bool_case_kind.expected" "$TMP/bool_case_kind_vm.out" 'bool string predicate in case exact stdout'
write_fixture "$FIX/empty_format_bad.ds" <<'DS'
let name = "api"
echo "{name:}"
DS
run_fail empty_format_bad_check "$DS" check "$FIX/empty_format_bad.ds"
assert_diag "$TMP/empty_format_bad_check.err" 'unsupported interpolation format specifier' 'empty format specifier rejected'

# v0.20 bugs fixed during implementation review.
write_fixture "$FIX/indexed_case.ds" <<'DS'
let parts = "a,b".split(",")
case parts[0] {
  "a" { echo yes }
  _ { echo no }
}
DS
assert_vm_bash_parity_args indexed_case "$FIX/indexed_case.ds" 0 ''
printf 'yes\n' >"$TMP/indexed_case.expected"
assert_same "$TMP/indexed_case.expected" "$TMP/indexed_case_vm.out" 'indexed split value works as Bash case selector'
write_fixture "$FIX/for_int_bool_case.ds" <<'DS'
let xs = [1, 2, 3]
for x in xs {
  case x {
    2 { echo "two" }
    _ { echo "other" }
  }
}
let bs = [true, false]
for b in bs {
  case b {
    true { echo "yes" }
    _ { echo "no" }
  }
}
DS
assert_vm_bash_parity_args for_int_bool_case "$FIX/for_int_bool_case.ds" 0 ''
cat >"$TMP/for_int_bool_case.expected" <<'OUT'
other
two
other
yes
no
OUT
assert_same "$TMP/for_int_bool_case.expected" "$TMP/for_int_bool_case_vm.out" 'for loop element kind retained for case selectors'

# Focused sanitizer-friendly fixtures are covered when this suite is run by make asan/ubsan.
# This test plan uses the current parser's block-style case arms rather than the
# illustrative arrow arms from the prose test plan, preserving the same coverage intent.

printf 'v0.20 assertions: %s\n' "$pass_count"
