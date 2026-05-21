#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_17_tests.$$"
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

assert_diag_shape() {
  local file="$1" path_fragment="$2" severity="$3" text="$4" name="$5"
  assert_contains "$file" "$path_fragment:" "$name path"
  assert_contains "$file" ": $severity:" "$name severity"
  assert_contains "$file" "$text" "$name text"
  assert_contains "$file" '^' "$name caret"
}

assert_vm_bash_with_args() {
  local name="$1" fixture="$2" expected_status="$3" expected_stdout="$4"; shift 4
  local script="$TMP/$name.sh"
  capture_in_dir "${name}_vm" "$TMP/${name}_vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  capture_in_dir "${name}_bash" "$TMP/${name}_bash_work" bash "$script" "$@"
  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  printf '%s' "$expected_stdout" >"$TMP/${name}_expected.out"
  assert_same "$TMP/${name}_expected.out" "$TMP/${name}_vm.out" "$name VM stdout"
  assert_same "$TMP/${name}_expected.out" "$TMP/${name}_bash.out" "$name Bash stdout"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  fi
  assert_contains "$script" '#!/usr/bin/env bash' "$name emitted Bash has shebang"
  assert_not_contains "$script" "$ROOT/ds" "$name emitted Bash does not reference project binary"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
}

assert_test_summary() {
  local file="$1" total="$2" passed="$3" failed="$4" name="$5"
  if grep -Fq "total:  $total" "$file"; then
    assert_contains "$file" "total:  $total" "$name total"
    assert_contains "$file" "passed: $passed" "$name passed"
    assert_contains "$file" "failed: $failed" "$name failed"
  else
    assert_contains "$file" "$total tests, $passed passed, $failed failed" "$name summary"
  fi
}

FIX="$TMP/fixtures"
mkdir -p "$FIX"

# Static and build wiring tests.
count_017="$(grep -E '^TEST_VERSIONS :=' Makefile | grep -o '0-17' | wc -l | tr -d ' ')"
[ "$count_017" = 1 ] || fail "TEST_VERSIONS should contain 0-17 exactly once, got $count_017"
pass 'TEST_VERSIONS contains 0-17 exactly once'
assert_matches Makefile '^TEST_VERSIONS := .*0-16 0-17($| )' 'v0.17 follows v0.16 in TEST_VERSIONS'
assert_contains Makefile 'DS_SKIP_BUILD=1 ./tests/v$(subst -,_,$(patsubst test-v%,%,$@))/run.sh' 'pattern target invokes version suite'
assert_contains Makefile '$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address" test' 'asan runs aggregate test suite'
assert_contains Makefile '$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined" test' 'ubsan runs aggregate test suite'
assert_contains Makefile 'src/parse_stmt.c' 'statement parser source is built'
assert_contains Makefile 'src/lower_stmt.c' 'statement lowerer source is built'
assert_contains Makefile 'src/bash_stmt.c' 'Bash statement emitter source is built'
assert_contains Makefile 'src/vm_compile.c' 'VM compiler source is built'
assert_not_contains Makefile 'libs/hashmap' 'build does not reference stale libs/hashmap path'
assert_not_contains include/ds.h 'hashmap' 'public umbrella does not expose hashmap internals'

for file in src/parser.c src/parse_stmt.c src/ast.c src/lower_stmt.c src/hir.c src/format.c src/checker.c src/vm_compile.c src/vm_dump.c src/bash_stmt.c src/bash_deps.c; do
  [ -f "$file" ] || fail "$file exists"
  pass "$file exists"
done
for file in src/parse_stmt.c src/ast.c src/lower_stmt.c src/hir.c src/format.c src/checker.c src/vm_compile.c src/bash_stmt.c src/bash_deps.c; do
  assert_matches "$file" 'While|WHILE|while' "$file handles while/control-flow path"
done
assert_contains src/vm_dump.c 'OP_JUMP_POP' 'VM dump handles patched loop-control jumps'
assert_contains src/lexer.c 'DS_TOK_CASE' 'lexer recognizes case token'
assert_contains src/lexer.c 'DS_TOK_WHILE' 'lexer recognizes while token'
assert_contains src/lexer.c 'DS_TOK_BREAK' 'lexer recognizes break token'
assert_contains src/lexer.c 'DS_TOK_CONTINUE' 'lexer recognizes continue token'
assert_contains src/lexer.c 'DS_TOK_PIPE' 'lexer recognizes case alternative separator token'
assert_contains src/vm_compile.c '===' 'VM bytecode uses kind-aware case comparison'
assert_contains src/bash_stmt.c '__ds_type_' 'Bash case emission uses sidecar kind tags'

# Documentation and status checks.
assert_contains README.md 'v0.17.0' 'README mentions v0.17.0'
assert_contains README.md 'implementation and tests are complete for scoped control flow' 'README marks v0.17 tests complete'
assert_contains docs/status.md 'implementation and test pass' 'status identifies post-test-pass state'
for phrase in 'while' 'break' 'continue' 'case selector' 'scalar reassignment' 'kind-aware' 'case target' 'case $target' 'until' 'loop `else`' 'function `return`' 'map iteration' 'ranges' 'pipelines' 'string methods' 'Comment-preserving'; do
  assert_contains docs/status.md "$phrase" "status documents $phrase"
done
assert_contains docs/language.ds '`while`, `break`, `continue`, scalar reassignment, and expression-style' 'language marks v0.17 control flow implemented'
assert_contains docs/language.ds 'case target' 'language uses expression-style case'
assert_contains docs/runtime.md 'kind-aware exact' 'runtime documents exact case matching'
assert_contains docs/architecture.md 'sidecar type tags' 'architecture documents Bash kind tags for case'
assert_contains docs/milestones/v0.17.0-spec.md 'Tests complete' 'spec completion review records tests complete'
assert_contains docs/milestones/v0.17.0-test-plan.md 'Implemented' 'test plan status records implementation'

# Public command help.
run_ok help_top "$DS" --help
assert_contains "$TMP/help_top.out" 'ds v0.21.0' 'help reports v0.17.0'
assert_contains "$TMP/help_top.out" 'ds emit bash <file.ds> -o <file.sh>' 'help lists emit bash'

# Token, AST, HIR, and bytecode coverage.
write_fixture "$FIX/debug.ds" <<'DS'
let i = 0
while i < 2 {
  i += 1
  if i == 1 { continue }
  break
}
case i {
  1 | 2 { echo "small" }
  _ { echo "other" }
}
DS
run_ok debug_tokens "$DS" tokens "$FIX/debug.ds"
for token in 'WHILE' 'BREAK' 'CONTINUE' 'CASE' 'PIPE' 'PLUS' 'EQUAL'; do
  assert_contains "$TMP/debug_tokens.out" "$token" "tokens include $token"
done
run_ok debug_ast "$DS" ast "$FIX/debug.ds"
for node in 'AssignStmt i +=' 'WhileStmt' 'BreakStmt' 'ContinueStmt' 'CaseStmt' 'Arm 1 2' 'Arm _'; do
  assert_contains "$TMP/debug_ast.out" "$node" "AST includes $node"
done
assert_not_matches "$TMP/debug_ast.out" '0x[0-9A-Fa-f]+' 'AST output contains no pointer addresses'
run_ok debug_hir "$DS" hir "$FIX/debug.ds"
for node in 'Assign i +=' 'While @' 'Break @' 'Continue @' 'Case @' 'Arm 1 2'; do
  assert_contains "$TMP/debug_hir.out" "$node" "HIR includes $node"
done
run_ok debug_bytecode "$DS" bytecode "$FIX/debug.ds"
for instr in 'JUMP_IF_FALSE' 'JUMP_POP' 'JUMP           ' 'BINARY' 'STORE_VAR' 'COMPARE        ' '==='; do
  assert_contains "$TMP/debug_bytecode.out" "$instr" "bytecode includes $instr"
done

# Reassignment behavior.
write_fixture "$FIX/reassign.ds" <<'DS'
let name = "api"
echo "before={name}"
name = "web"
echo "after={name}"
let i = 3
i += 2
echo "plus={i}"
i -= 4
echo "minus={i}"
i -= 2
echo "negative={i}"
i += 1
echo "zero={i}"
{
  name = "block"
  let local = "hidden"
  echo "inner={name}:{local}"
}
echo "outer={name}"
DS
assert_vm_bash_with_args reassign "$FIX/reassign.ds" 0 $'before=api\nafter=web\nplus=5\nminus=1\nnegative=-1\nzero=0\ninner=block:hidden\nouter=block\n'
run_ok reassign_direct "$DS" "$FIX/reassign.ds"
assert_same_text $'before=api\nafter=web\nplus=5\nminus=1\nnegative=-1\nzero=0\ninner=block:hidden\nouter=block\n' "$TMP/reassign_direct.out" 'direct execution matches reassignment output'

write_fixture "$FIX/reassign_scopes.ds" <<'DS'
script {
  arg app: string
  option target: string = "staging"
  flag force: bool = false
}
app = "web"
target = "prod"
force = false
fn set(value = "fn") {
  value = "changed"
  echo "param={value}"
  let counter = 1
  counter += 2
  echo "counter={counter}"
}
let items = ["a"]
for item in items {
  item = "loop"
  echo "item={item}"
}
set("input")
echo "script={app}:{target}:{force}"
DS
assert_vm_bash_with_args reassign_scopes "$FIX/reassign_scopes.ds" 0 $'item=loop\nparam=changed\ncounter=3\nscript=web:prod:false\n' cli --target dev --force

for case_name in unknown index field env string_plus bool_plus missing_rhs; do
  file="$FIX/bad_assign_${case_name}.ds"
  case "$case_name" in
    unknown) printf 'unknown = 1\n' >"$file" ;;
    index) printf 'let xs = ["a"]\nxs[0] = "x"\n' >"$file" ;;
    field) printf 'let ports = { api: 3000 }\nports.api = 3001\n' >"$file" ;;
    env) printf 'env.PATH = "bin"\n' >"$file" ;;
    string_plus) printf 'let name = "a"\nname += "b"\n' >"$file" ;;
    bool_plus) printf 'let ok = true\nok += 1\n' >"$file" ;;
    missing_rhs) printf 'let count = 1\ncount +=\n' >"$file" ;;
  esac
  capture_status "bad_assign_${case_name}_check" "$DS" check "$file"
  assert_nonzero_status "bad_assign_${case_name}_check"
  assert_contains "$TMP/bad_assign_${case_name}_check.err" ': error:' "bad assignment $case_name has error"
  assert_contains "$TMP/bad_assign_${case_name}_check.err" '^' "bad assignment $case_name has caret"
  capture_status "bad_assign_${case_name}_run" "$DS" run "$file"
  assert_nonzero_status "bad_assign_${case_name}_run"
  capture_status "bad_assign_${case_name}_emit" "$DS" emit bash "$file" -o "$TMP/bad_assign_${case_name}.sh"
  assert_nonzero_status "bad_assign_${case_name}_emit"
done

# While loops and body semantics.
write_fixture "$FIX/while_basic.ds" <<'DS'
let i = 0
while i < 3 {
  echo "i={i}"
  i += 1
}
echo "done={i}"
DS
assert_vm_bash_with_args while_basic "$FIX/while_basic.ds" 0 $'i=0\ni=1\ni=2\ndone=3\n'

write_fixture "$FIX/while_semantics.ds" <<'DS'
let done = false
let i = 0
while !done {
  i += 1
  if i == 2 {
    done = true
  }
}
echo "toggle={i}:{done}"
let text = "yes"
while text {
  echo "truthy-string"
  text = ""
}
let n = 1
while n {
  echo "truthy-int={n}"
  n -= 1
}
DS
assert_vm_bash_with_args while_semantics "$FIX/while_semantics.ds" 0 $'toggle=2:true\ntruthy-string\ntruthy-int=1\n'

write_fixture "$FIX/while_body.ds" <<'DS'
let i = 0
while i < 2 {
  let r = run sh -c "if [ {i} -eq 0 ]; then printf ok; else printf bad >&2; exit 4; fi"
  echo "r={r.stdout}:{r.stderr}:{r.code}:{r.failed}"
  echo "line={i}" |>> "loop.log"
  i += 1
}
for line in lines("loop.log") {
  echo $line
}
DS
assert_vm_bash_with_args while_body "$FIX/while_body.ds" 0 $'r=ok::0:false\nr=:bad:4:true\nline=0\nline=1\n'

write_fixture "$FIX/while_nested.ds" <<'DS'
let outer = 0
while outer < 2 {
  let inner = 0
  while inner < 2 {
    echo "w={outer}:{inner}"
    inner += 1
  }
  let xs = ["a", "b"]
  for x in xs {
    echo "f={outer}:{x}"
  }
  outer += 1
}
let ys = ["u", "v"]
for y in ys {
  let i = 0
  while i < 2 {
    echo "fw={y}:{i}"
    i += 1
  }
}
DS
assert_vm_bash_with_args while_nested "$FIX/while_nested.ds" 0 $'w=0:0\nw=0:1\nf=0:a\nf=0:b\nw=1:0\nw=1:1\nf=1:a\nf=1:b\nfw=u:0\nfw=u:1\nfw=v:0\nfw=v:1\n'

for case_name in missing_condition missing_block missing_close no_block; do
  file="$FIX/bad_while_${case_name}.ds"
  case "$case_name" in
    missing_condition) printf 'while { echo "bad" }\n' >"$file" ;;
    missing_block) printf 'let i = 0\nwhile i < 3\n' >"$file" ;;
    missing_close) printf 'let i = 0\nwhile i < 3 {\n  i += 1\n' >"$file" ;;
    no_block) printf 'let i = 0\nwhile i < 3 echo "bad"\n' >"$file" ;;
  esac
  capture_status "bad_while_${case_name}" "$DS" check "$file"
  assert_nonzero_status "bad_while_${case_name}"
  assert_diag_shape "$TMP/bad_while_${case_name}.err" "$file" error '' "bad while $case_name diagnostic"
done

# Break and continue.
write_fixture "$FIX/loop_control.ds" <<'DS'
let items = ["a", "skip", "b", "stop", "c"]
for item in items {
  if item == "skip" { continue }
  if item == "stop" { break }
  echo $item
}
let i = 0
while i < 5 {
  i += 1
  if i == 2 { continue }
  if i == 4 { break }
  echo "i={i}"
}
echo "done={i}"
DS
assert_vm_bash_with_args loop_control "$FIX/loop_control.ds" 0 $'a\nb\ni=1\ni=3\ndone=4\n'

write_fixture "$FIX/nested_control.ds" <<'DS'
let outer = 0
while outer < 3 {
  let inner = 0
  while inner < 3 {
    inner += 1
    if inner == 2 { break }
    echo "break-inner={outer}:{inner}"
  }
  outer += 1
}
let xs = ["a", "b"]
for x in xs {
  let n = 0
  while n < 3 {
    n += 1
    if n == 2 { continue }
    echo "cont-inner={x}:{n}"
  }
}
let ys = ["skip", "go"]
for y in ys {
  if y == "skip" { continue }
  echo "outer-continue={y}"
}
DS
assert_vm_bash_with_args nested_control "$FIX/nested_control.ds" 0 $'break-inner=0:1\nbreak-inner=1:1\nbreak-inner=2:1\ncont-inner=a:1\ncont-inner=a:3\ncont-inner=b:1\ncont-inner=b:3\nouter-continue=go\n'

write_fixture "$FIX/case_loop_control.ds" <<'DS'
let items = ["go", "skip", "stop", "after"]
for item in items {
  case item {
    "skip" { continue }
    "stop" { break }
    _ { echo "item={item}" }
  }
}
DS
assert_vm_bash_with_args case_loop_control "$FIX/case_loop_control.ds" 0 $'item=go\n'

for case_name in break_top continue_top break_fn continue_fn break_test continue_test break_arg continue_arg break_case continue_case; do
  file="$FIX/bad_loop_${case_name}.ds"
  case "$case_name" in
    break_top) printf 'break\n' >"$file" ;;
    continue_top) printf 'continue\n' >"$file" ;;
    break_fn) printf 'fn bad() { break }\nlet xs = ["a"]\nfor x in xs { bad() }\n' >"$file" ;;
    continue_fn) printf 'fn bad() { continue }\nlet xs = ["a"]\nfor x in xs { bad() }\n' >"$file" ;;
    break_test) printf 'test "bad" { break }\n' >"$file" ;;
    continue_test) printf 'test "bad" { continue }\n' >"$file" ;;
    break_arg) printf 'let xs = ["a"]\nfor x in xs { break 1 }\n' >"$file" ;;
    continue_arg) printf 'let xs = ["a"]\nfor x in xs { continue label }\n' >"$file" ;;
    break_case) printf 'let x = "a"\ncase x { _ { break } }\n' >"$file" ;;
    continue_case) printf 'let x = "a"\ncase x { _ { continue } }\n' >"$file" ;;
  esac
  capture_status "bad_loop_${case_name}_check" "$DS" check "$file"
  assert_nonzero_status "bad_loop_${case_name}_check"
  assert_contains "$TMP/bad_loop_${case_name}_check.err" ': error:' "bad loop control $case_name diagnostic"
  capture_status "bad_loop_${case_name}_emit" "$DS" emit bash "$file" -o "$TMP/bad_loop_${case_name}.sh"
  assert_nonzero_status "bad_loop_${case_name}_emit"
done

# Case behavior.
write_fixture "$FIX/case_string.ds" <<'DS'
let lang = "sh"
case lang {
  "bash" | "sh" { echo "shell" }
  "python" { echo "python" }
  _ { echo "other" }
}
DS
assert_vm_bash_with_args case_string "$FIX/case_string.ds" 0 $'shell\n'

write_fixture "$FIX/case_exact.ds" <<'DS'
let one = 1
case one {
  "1" { echo "string-one" }
  1 { echo "int-one" }
  _ { echo "other" }
}
let truth = true
case truth {
  "true" { echo "string-true" }
  true { echo "bool-true" }
  _ { echo "other-bool" }
}
let file = "readme.txt"
case file {
  "*.txt" { echo "glob" }
  "readme.txt" { echo "literal" }
}
let quoted = "a b\"c\\d"
case quoted {
  "a b\"c\\d" { echo "quoted" }
  _ { echo "missing" }
}
let none = "z"
case none {
  "a" { echo "a" }
}
echo "after-none"
DS
assert_vm_bash_with_args case_exact "$FIX/case_exact.ds" 0 $'int-one\nbool-true\nliteral\nquoted\nafter-none\n'

write_fixture "$FIX/case_scope.ds" <<'DS'
fn say(value = "x") {
  echo "say={value}"
}

let out = "none"
let item = "b"
case item {
  "a" { out = "a" }
  "b" {
    out = "b"
    say(out)
  }
  _ { out = "other" }
}
echo "out={out}"
case out {
  "b" {
    let i = 0
    while i < 2 {
      echo "inside={i}"
      i += 1
    }
  }
}
DS
assert_vm_bash_with_args case_scope "$FIX/case_scope.ds" 0 $'say=b\nout=b\ninside=0\ninside=1\n'

write_fixture "$FIX/case_unknown_kind.ds" <<'DS'
fn choose(x) {
  case x {
    "a" { echo "a" }
    _ { echo "other" }
  }
}
choose("a")
DS
capture_status case_unknown_kind "$DS" check "$FIX/case_unknown_kind.ds"
assert_nonzero_status case_unknown_kind
assert_contains "$TMP/case_unknown_kind.err" 'known scalar string, int, or bool kind' 'unknown-kind case selector is rejected clearly'

for case_name in dollar missing_selector missing_body default_late duplicate_default empty_case bad_alt regex glob bare_no_brace duplicate_literal array_selector null_selector; do
  file="$FIX/bad_case_${case_name}.ds"
  case "$case_name" in
    dollar) printf 'let lang = "sh"\ncase $lang { _ { echo "bad" } }\n' >"$file" ;;
    missing_selector) printf 'case { _ { echo "bad" } }\n' >"$file" ;;
    missing_body) printf 'let lang = "sh"\ncase lang { "bash" }\n' >"$file" ;;
    default_late) printf 'let lang = "sh"\ncase lang { _ { echo "default" } "bash" { echo "late" } }\n' >"$file" ;;
    duplicate_default) printf 'let lang = "sh"\ncase lang { _ { echo "one" } _ { echo "two" } }\n' >"$file" ;;
    empty_case) printf 'let lang = "sh"\ncase lang { }\n' >"$file" ;;
    bad_alt) printf 'let lang = "sh"\ncase lang { "a" | { echo "bad" } }\n' >"$file" ;;
    regex) printf 'let lang = "sh"\ncase lang { /a/ { echo "regex" } }\n' >"$file" ;;
    glob) printf 'let lang = "sh"\ncase lang { *.txt { echo "glob" } }\n' >"$file" ;;
    bare_no_brace) printf 'let lang = "sh"\ncase lang { "sh" echo "bad" }\n' >"$file" ;;
    duplicate_literal) printf 'let lang = "sh"\ncase lang { "sh" { echo one } "sh" { echo two } }\n' >"$file" ;;
    array_selector) printf 'let xs = ["a"]\ncase xs { "a" { echo bad } }\n' >"$file" ;;
    null_selector) printf 'let n = null\ncase n { _ { echo bad } }\n' >"$file" ;;
  esac
  capture_status "bad_case_${case_name}_check" "$DS" check "$file"
  assert_nonzero_status "bad_case_${case_name}_check"
  assert_contains "$TMP/bad_case_${case_name}_check.err" ': error:' "bad case $case_name diagnostic"
  capture_status "bad_case_${case_name}_emit" "$DS" emit bash "$file" -o "$TMP/bad_case_${case_name}.sh"
  assert_nonzero_status "bad_case_${case_name}_emit"
done

# Imports, tests, script args, formatter, and examples.
mkdir -p "$FIX/imports"
write_fixture "$FIX/imports/lib.ds" <<'DS'
fn classify_a() {
  let i = 0
  while i < 2 {
    i += 1
  }
  let value = "a"
  case value {
    "a" { echo "lib=a:{i}" }
    _ { echo "lib=other:{i}" }
  }
}

test "imported control" {
  let i = 0
  while i < 2 { i += 1 }
  assert i == 2
}
DS
write_fixture "$FIX/imports/main.ds" <<'DS'
script {
  arg value: string
}

import "lib.ds"

case value {
  "a" { classify_a() }
  _ { echo "main=other" }
}

test "root control" {
  let x = "a"
  case x { "a" { assert true } _ { fail "bad" } }
}
DS
assert_vm_bash_with_args imports "$FIX/imports/main.ds" 0 $'lib=a:2\n' a
run_ok imports_test "$DS" test "$FIX/imports/main.ds"
assert_test_summary "$TMP/imports_test.out" 2 2 0 'imported/root control tests'
run_ok imports_tokens "$DS" tokens "$FIX/imports/main.ds"
assert_not_contains "$TMP/imports_tokens.out" 'lib=a' 'tokens remain root-file debug view'
run_ok imports_hir "$DS" hir "$FIX/imports/main.ds"
assert_contains "$TMP/imports_hir.out" 'Function classify_a' 'HIR uses composed imports'

write_fixture "$FIX/test_blocks_ignored.ds" <<'DS'
echo "run"
test "not normal" {
  echo "test output"
  assert false
}
DS
assert_vm_bash_with_args test_blocks_ignored "$FIX/test_blocks_ignored.ds" 0 $'run\n'
capture_status test_blocks_run "$DS" test "$FIX/test_blocks_ignored.ds"
assert_nonzero_status test_blocks_run
assert_contains "$TMP/test_blocks_run.out" 'test output' 'ds test runs test block body'

write_fixture "$FIX/format_me.ds" <<'DS'
let i=0
while i<2 {
  i+=1
  if i==1 { continue }
  break
}
case i { 1|2 { echo "small" } _ { echo "other" } }
DS
run_ok format_once "$DS" fmt "$FIX/format_me.ds"
cp "$TMP/format_once.out" "$TMP/format_once.ds"
run_ok format_twice "$DS" fmt "$TMP/format_once.ds"
assert_same "$TMP/format_once.out" "$TMP/format_twice.out" 'formatter is idempotent for v0.17 syntax'
run_ok format_check "$DS" fmt --check "$TMP/format_once.ds"
assert_contains "$TMP/format_once.out" 'while i < 2 {' 'formatter prints while block'
assert_contains "$TMP/format_once.out" 'case i {' 'formatter prints case block'
assert_contains "$TMP/format_once.out" '1 | 2 {' 'formatter spaces case alternatives'
write_fixture "$FIX/commented.ds" <<'DS'
# keep me
while true { break }
DS
capture_status fmt_comment "$DS" fmt "$FIX/commented.ds"
assert_nonzero_status fmt_comment
assert_contains "$TMP/fmt_comment.err" 'formatter cannot preserve comments yet' 'formatter still rejects comment-bearing control-flow file'

# Production examples and direct VM/Bash parity where deterministic.
for example in examples/*.ds; do
  base="$(basename "$example" .ds)"
  example_abs="$ROOT/$example"
  if [ "$base" = bad ]; then
    capture_status "example_${base}_check" "$DS" check "$example_abs"
    assert_nonzero_status "example_${base}_check"
    assert_contains "$TMP/example_${base}_check.err" ': error:' 'bad example remains intentionally invalid'
    continue
  fi
  run_ok "example_${base}_check" "$DS" check "$example_abs"
  if grep -q '^script {' "$example_abs"; then
    case "$base" in
      args) set -- demo --target prod --retries 2 --force ;;
      import-main) set -- ;;
      *) set -- demo ;;
    esac
  else
    set --
  fi
  capture_in_dir "example_${base}_vm" "$TMP/example_${base}_vm_work" "$DS" run "$example_abs" "$@"
  assert_status "example_${base}_vm" 0
  run_ok "example_${base}_emit" "$DS" emit bash "$example_abs" -o "$TMP/example_${base}.sh"
  run_ok "example_${base}_bash_n" bash -n "$TMP/example_${base}.sh"
  capture_in_dir "example_${base}_bash" "$TMP/example_${base}_bash_work" bash "$TMP/example_${base}.sh" "$@"
  assert_status "example_${base}_bash" 0
  assert_same "$TMP/example_${base}_vm.out" "$TMP/example_${base}_bash.out" "example $base VM/Bash stdout parity"
  assert_same "$TMP/example_${base}_vm.err" "$TMP/example_${base}_bash.err" "example $base VM/Bash stderr parity"
done
assert_contains examples/control-flow.ds 'while i < 5' 'control-flow example includes while'
assert_contains examples/control-flow.ds 'continue' 'control-flow example includes continue'
assert_contains examples/control-flow.ds 'break' 'control-flow example includes break'
assert_contains examples/control-flow.ds 'case target' 'control-flow example includes expression-style case'
run_ok control_flow_fmt_check "$DS" fmt --check examples/control-flow.ds

printf 'v0.17.0 tests: %s assertions\n' "$pass_count"
