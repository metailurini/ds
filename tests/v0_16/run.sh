#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_16_tests.$$"
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
  assert_contains "$file" "$text" "$name message"
  assert_contains "$file" '^' "$name caret"
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

assert_same_run_triplet() {
  local left="$1" right="$2" name="$3"
  assert_same "$TMP/$left.rc" "$TMP/$right.rc" "$name status"
  assert_same "$TMP/$left.out" "$TMP/$right.out" "$name stdout"
  assert_same "$TMP/$left.err" "$TMP/$right.err" "$name stderr"
}

assert_bash_standalone() {
  local script="$1" name="$2"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_contains "$script" '#!/usr/bin/env bash' "$name shebang"
  assert_not_contains "$script" "$ROOT/ds" "$name does not reference project binary"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name does not invoke ds command"
}

assert_no_side_effect() {
  local path="$1" name="$2"
  [ ! -e "$path" ] || fail "$name: unexpected side-effect file $path"
  pass "$name"
}

FIX="$TMP/fixtures"
mkdir -p "$FIX"

# Static build wiring and source-boundary checks.
count_016="$(grep -E '^TEST_VERSIONS :=' Makefile | grep -o '0-16' | wc -l | tr -d ' ')"
[ "$count_016" = 1 ] || fail "TEST_VERSIONS should contain 0-16 exactly once, got $count_016"
pass 'TEST_VERSIONS contains 0-16 exactly once'
assert_matches Makefile '^TEST_VERSIONS := .*0-15 0-16($| )' 'v0.16 follows v0.15 in TEST_VERSIONS'
assert_contains Makefile 'src/cli_program.c' 'CLI program source is built normally'
assert_contains Makefile 'asan:' 'asan target exists'
assert_contains Makefile 'ubsan:' 'ubsan target exists'
assert_contains Makefile 'ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 DS_SKIP_BUILD=1 $(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address" test' 'asan target runs aggregate test suite'
assert_contains Makefile 'DS_SKIP_BUILD=1 $(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined" test' 'ubsan target runs aggregate test suite'
assert_not_contains Makefile 'libs/hashmap' 'build does not reference stale libs/hashmap path'
assert_not_contains include/ds.h 'hashmap' 'public umbrella does not expose raw hashmap'
assert_contains src/runtime.c '#include "runtime/hashmap.h"' 'runtime bridge owns raw hashmap include'
raw_includes="$(grep -R --include='*.c' --include='*.h' -n '#include "runtime/hashmap.h"\|#include "hashmap.h"' src include | grep -v 'src/runtime.c:' | grep -v 'src/runtime/hashmap.c:' | grep -v 'src/runtime/hashmap.h:' || true)"
[ -z "$raw_includes" ] || { echo "$raw_includes" >&2; fail 'raw hashmap include leaked outside runtime bridge'; }
pass 'raw hashmap include stays inside runtime bridge/implementation'
hm_calls="$(grep -R --include='*.c' --include='*.h' -n '\bhm_[A-Za-z0-9_]*[[:space:]]*(' src include | grep -v 'src/runtime.c:' | grep -v 'src/runtime/hashmap.c:' | grep -v 'src/runtime/hashmap.h:' || true)"
[ -z "$hm_calls" ] || { echo "$hm_calls" >&2; fail 'raw hashmap calls leaked outside runtime bridge'; }
pass 'raw hashmap calls stay inside runtime bridge/implementation'
assert_not_contains src/cli_program.c '#include "../include/ds.h"' 'new CLI source does not reach through public umbrella path'

# Status/documentation checks.
[ -f docs/status.md ] || fail 'docs/status.md exists'
pass 'docs/status.md exists'
for phrase in \
  'tokens' 'ast' 'check' 'fmt' 'hir' 'bytecode' 'run' 'test' 'direct script execution' 'emit bash' \
  'standalone Bash' 'Test-only syntax' 'Comment-preserving formatting remains deferred' \
  'while' 'break' 'continue' 'case' 'function return values' 'string methods' \
  'regex' 'membership' 'environment append/prepend shorthand' 'recursive `**`' 'map iteration' 'nested collections' 'v0.17.0'; do
  assert_contains docs/status.md "$phrase" "status documents $phrase"
done
assert_contains docs/architecture.md '`tokens` and `ast` are root-file frontend/debug views' 'architecture documents root-file command boundary'
assert_contains docs/runtime.md 'Generated Bash must remain standalone' 'runtime docs document standalone Bash'
assert_contains docs/language.ds '[deferred]' 'language catalog marks deferred syntax'
assert_contains README.md 'keeps comment-preserving formatting deferred' 'README documents formatter comment deferral'

# Help and usage remain current and do not execute scripts on usage errors.
run_ok help_top "$DS" --help
assert_contains "$TMP/help_top.out" 'ds v0.33.0' 'help reports current version'
assert_contains "$TMP/help_top.out" 'ds emit bash <file.ds> -o <file.sh>' 'help lists emit bash'
write_fixture "$FIX/usage_side_effect.ds" <<'DS'
touch SHOULD_NOT_EXIST
DS
capture_status unknown_cmd "$DS" unknown "$FIX/usage_side_effect.ds"
assert_nonzero_status unknown_cmd
assert_contains "$TMP/unknown_cmd.err" 'unknown command' 'unknown command diagnostic'
assert_no_side_effect "$ROOT/SHOULD_NOT_EXIST" 'unknown command does not execute source'
capture_status emit_missing_out "$DS" emit bash "$FIX/usage_side_effect.ds" -o
assert_nonzero_status emit_missing_out
assert_no_side_effect "$ROOT/SHOULD_NOT_EXIST" 'emit usage error does not execute source'
capture_status fmt_bad_combo "$DS" fmt --check --write "$FIX/usage_side_effect.ds"
assert_nonzero_status fmt_bad_combo
assert_contains "$TMP/fmt_bad_combo.err" '`ds fmt --check --write` is invalid' 'fmt rejects check/write combination'
assert_no_side_effect "$ROOT/SHOULD_NOT_EXIST" 'fmt flag error does not execute source'

# Formatter/checker non-execution and conservative warnings.
write_fixture "$FIX/no_exec.ds" <<'DS'
touch SHOULD_NOT_EXIST
let unused = "x"
DS
NO_EXEC_FILE="$ROOT/SHOULD_NOT_EXIST"
rm -f "$NO_EXEC_FILE"
for cmd in fmt 'fmt --check' 'fmt --write' check 'check --warnings-as-errors' 'check --no-warnings'; do
  name="no_exec_${cmd// /_}"
  set +e
  # shellcheck disable=SC2086
  "$DS" $cmd "$FIX/no_exec.ds" >"$TMP/$name.out" 2>"$TMP/$name.err"
  set -e
  assert_no_side_effect "$NO_EXEC_FILE" "$cmd does not execute command statement"
done
capture_status warn_normal "$DS" check "$FIX/no_exec.ds"
assert_status warn_normal 0
assert_contains "$TMP/warn_normal.err" 'warning: unused variable `unused`' 'normal check reports warning'
assert_diag_shape "$TMP/warn_normal.err" "$FIX/no_exec.ds" warning 'unused variable `unused`' 'warning diagnostic shape'
capture_status warn_error "$DS" check --warnings-as-errors "$FIX/no_exec.ds"
assert_nonzero_status warn_error
capture_status warn_suppressed "$DS" check --no-warnings "$FIX/no_exec.ds"
assert_status warn_suppressed 0
assert_same_text '' "$TMP/warn_suppressed.err" 'no-warnings suppresses warning diagnostics'
capture_status warn_flags_invalid "$DS" check --warnings-as-errors --no-warnings "$FIX/no_exec.ds"
assert_nonzero_status warn_flags_invalid
assert_contains "$TMP/warn_flags_invalid.err" '`ds check --warnings-as-errors --no-warnings` is invalid' 'checker rejects contradictory warning flags'

write_fixture "$FIX/no_false_warnings.ds" <<'DS'
script {
  arg app: string
  option target: string = "staging"
  flag force: bool = false
}

let services = ["api", "web"]
let first = services[0]
let ports = { api: 3000, web: 5173 }
let api_port = ports.api
fn helper(name = "world") {
  echo "hello {name}"
}
helper(app)
for service in services {
  echo "{service}:{first}:{api_port}:{target}:{force}"
}
DS
capture_status no_false_warn "$DS" check "$FIX/no_false_warnings.ds"
assert_status no_false_warn 0
assert_same_text '' "$TMP/no_false_warn.err" 'used variables and script args avoid warning false positives'

# Formatter comment behavior: comments are accepted by parser/runtime but rejected by fmt without rewriting.
write_fixture "$FIX/commented.ds" <<'DS'
# formatter must not drop this comment
let name = "Danh"
echo "hello {name}"
DS
for mode in 'fmt' 'fmt --check'; do
  name="comment_${mode// /_}"
  set +e
  # shellcheck disable=SC2086
  "$DS" $mode "$FIX/commented.ds" >"$TMP/$name.out" 2>"$TMP/$name.err"
  rc=$?
  set -e
  [ "$rc" != 0 ] || fail "$mode should reject comments"
  assert_contains "$TMP/$name.err" 'formatter cannot preserve comments yet' "$mode gives unsupported-comment diagnostic"
  assert_same_text '' "$TMP/$name.out" "$mode does not emit partial formatted output"
done
cp "$FIX/commented.ds" "$TMP/commented_before.ds"
capture_status comment_write "$DS" fmt --write "$FIX/commented.ds"
assert_nonzero_status comment_write
assert_same "$TMP/commented_before.ds" "$FIX/commented.ds" 'fmt --write leaves commented source unchanged'
run_ok comment_check "$DS" check "$FIX/commented.ds"
run_ok comment_run "$DS" run "$FIX/commented.ds"
run_ok comment_emit "$DS" emit bash "$FIX/commented.ds" -o "$TMP/commented.sh"
assert_bash_standalone "$TMP/commented.sh" 'commented emitted Bash'

# Import boundary: root debug views versus composed commands.
mkdir -p "$FIX/import space"
write_fixture "$FIX/import space/lib.ds" <<'DS'
echo "lib"
DS
write_fixture "$FIX/import_main.ds" <<'DS'
import "./import space/lib.ds"
echo "main"
DS
run_ok import_tokens "$DS" tokens "$FIX/import_main.ds"
assert_contains "$TMP/import_tokens.out" 'IMPORT' 'tokens show root import'
assert_not_contains "$TMP/import_tokens.out" 'lib"' 'tokens do not inline imported executable statements'
run_ok import_ast "$DS" ast "$FIX/import_main.ds"
assert_contains "$TMP/import_ast.out" 'Import' 'ast shows root import'
assert_not_contains "$TMP/import_ast.out" 'Echo' 'ast does not inline imported echo statement'
run_ok import_check "$DS" check "$FIX/import_main.ds"
run_ok import_hir "$DS" hir "$FIX/import_main.ds"
assert_contains "$TMP/import_hir.out" 'lib' 'hir includes composed imported executable statement'
run_ok import_bytecode "$DS" bytecode "$FIX/import_main.ds"
assert_contains "$TMP/import_bytecode.out" 'import space/lib.ds' 'bytecode preserves imported source spans'
capture_status import_run "$DS" run "$FIX/import_main.ds"
assert_status import_run 0
assert_same_text $'lib\nmain\n' "$TMP/import_run.out" 'run executes import before root'
capture_status import_direct "$DS" "$FIX/import_main.ds"
assert_same_run_triplet import_run import_direct 'direct execution matches run for imports'
run_ok import_emit "$DS" emit bash "$FIX/import_main.ds" -o "$TMP/import_main.sh"
assert_bash_standalone "$TMP/import_main.sh" 'import emitted Bash'
capture_status import_bash bash "$TMP/import_main.sh"
assert_same_run_triplet import_run import_bash 'Bash matches VM for imports'
run_ok fmt_import_root "$DS" fmt "$FIX/import_main.ds"
assert_contains "$TMP/fmt_import_root.out" 'import "./import space/lib.ds"' 'fmt preserves import path string meaning'
assert_same_text $'echo "lib"\n' "$FIX/import space/lib.ds" 'fmt root view does not rewrite imported file'

write_fixture "$FIX/dup_lib.ds" <<'DS'
echo "dup-lib"
DS
write_fixture "$FIX/dup_import.ds" <<'DS'
import "./dup_lib.ds"
import "./dup_lib.ds"
echo "root"
DS
capture_status dup_import_run "$DS" run "$FIX/dup_import.ds"
assert_status dup_import_run 0
assert_same_text $'dup-lib\nroot\n' "$TMP/dup_import_run.out" 'duplicate imports execute once'
write_fixture "$FIX/cycle_a.ds" <<'DS'
import "./cycle_b.ds"
DS
write_fixture "$FIX/cycle_b.ds" <<'DS'
import "./cycle_a.ds"
DS
capture_status cycle_diag "$DS" check "$FIX/cycle_a.ds"
assert_nonzero_status cycle_diag
assert_contains "$TMP/cycle_diag.err" 'import cycle detected' 'import cycle diagnostic is readable'
write_fixture "$FIX/missing_import.ds" <<'DS'
import "./missing.ds"
DS
capture_status missing_import "$DS" check "$FIX/missing_import.ds"
assert_nonzero_status missing_import
assert_contains "$TMP/missing_import.err" 'failed to open imported file' 'missing import diagnostic mentions imported file'
write_fixture "$FIX/bad_imported.ds" <<'DS'
let =
DS
write_fixture "$FIX/bad_import_root.ds" <<'DS'
import "./bad_imported.ds"
DS
capture_status bad_import "$DS" check "$FIX/bad_import_root.ds"
assert_nonzero_status bad_import
assert_diag_shape "$TMP/bad_import.err" "$FIX/bad_imported.ds" error 'expected identifier' 'malformed imported file diagnostic shape'
write_fixture "$FIX/script_imported.ds" <<'DS'
script {
  arg app: string
}
DS
write_fixture "$FIX/script_import_root.ds" <<'DS'
import "./script_imported.ds"
DS
capture_status imported_script "$DS" check "$FIX/script_import_root.ds"
assert_nonzero_status imported_script
assert_contains "$TMP/imported_script.err" 'imported files cannot declare `script` blocks' 'imported script block remains rejected'

# Example inventory: valid examples check/run/emit, invalid example fails clearly.
for example in examples/*.ds; do
  base="$(basename "$example" .ds)"
  if [ "$base" = 'bad' ]; then
    capture_status "example_${base}_check" "$DS" check "$example"
    assert_nonzero_status "example_${base}_check"
    assert_contains "$TMP/example_${base}_check.err" 'expected expression' 'bad example has stable diagnostic fragment'
    continue
  fi
  run_ok "example_${base}_check" "$DS" check "$example"
  case "$base" in
    args)
      capture_status "example_${base}_run" "$DS" run "$example" api --target prod --retries 3 --force ;;
    redirection|stdlib)
      if [ "$base" = 'stdlib' ]; then rm -f /tmp/ds-stdlib-example.txt; fi
      capture_in_dir "example_${base}_run" "$TMP/example_${base}_vm_work" "$DS" run "$ROOT/$example" ;;
    *)
      capture_status "example_${base}_run" "$DS" run "$example" ;;
  esac
  assert_status "example_${base}_run" 0
  run_ok "example_${base}_emit" "$DS" emit bash "$example" -o "$TMP/example_${base}.sh"
  assert_bash_standalone "$TMP/example_${base}.sh" "example $base emitted Bash"
  case "$base" in
    args)
      capture_status "example_${base}_bash" bash "$TMP/example_${base}.sh" api --target prod --retries 3 --force ;;
    redirection)
      capture_in_dir "example_${base}_bash" "$TMP/example_${base}_bash_work" bash "$TMP/example_${base}.sh" ;;
    stdlib)
      rm -f /tmp/ds-stdlib-example.txt
      capture_in_dir "example_${base}_bash" "$TMP/example_${base}_bash_work" bash "$TMP/example_${base}.sh" ;;
    *)
      capture_status "example_${base}_bash" bash "$TMP/example_${base}.sh" ;;
  esac
  assert_same_run_triplet "example_${base}_run" "example_${base}_bash" "example $base VM/Bash parity"
done
capture_status args_help "$DS" run examples/args.ds --help
assert_status args_help 0
assert_contains "$TMP/args_help.out" 'Usage:' 'script --help prints usage'
capture_status args_missing "$DS" run examples/args.ds
assert_nonzero_status args_missing
assert_contains "$TMP/args_missing.err" 'missing required argument `app`' 'missing script arg diagnostic'
capture_status args_bad_int "$DS" run examples/args.ds api --retries nope
assert_nonzero_status args_bad_int
assert_contains "$TMP/args_bad_int.err" 'invalid int value `nope` for `retries`' 'bad int option diagnostic'
capture_status args_end_options "$DS" run examples/args.ds -- --literal
assert_status args_end_options 0
assert_contains "$TMP/args_end_options.out" 'Deploying --literal' '-- stops option parsing for script args'

# Mixed VM/Bash parity matrix.
write_fixture "$FIX/lib.ds" <<'DS'
echo "from-lib"
DS
write_fixture "$FIX/mixed.ds" <<'DS'
script {
  arg app: string
  option target: string = "staging"
  option retries: int = 2
  flag force: bool = false
}

import "./lib.ds"

let services = ["api", "web"]
services.push(app)
let ports = { api: 3000, web: 5173 }

fn greet(name = "world") {
  echo "hello {name}"
}

greet(app)

for service in services {
  echo "service={service}"
}

let api_port = ports.api
echo "api={api_port} target={target} retries={retries} force={force}"

file.write("input.txt", "one\ntwo\n")
file.append("input.txt", "three\n")
let text = file.read("input.txt")
echo "text={text}"
let base = path.basename("input.txt")
let ext = path.ext("archive.tar.gz")
echo "base={base}"
echo "ext={ext}"

let capture = run sh -c "printf cap; printf err >&2; exit 7"
echo "capture={capture.stdout}:{capture.stderr}:{capture.code}:{capture.failed}"
DS
run_ok mixed_check "$DS" check "$FIX/mixed.ds"
capture_in_dir mixed_vm "$TMP/mixed_vm_work" "$DS" run "$FIX/mixed.ds" api --target prod --retries 3 --force
assert_status mixed_vm 0
run_ok mixed_emit "$DS" emit bash "$FIX/mixed.ds" -o "$TMP/mixed.sh"
assert_bash_standalone "$TMP/mixed.sh" 'mixed emitted Bash'
capture_in_dir mixed_bash "$TMP/mixed_bash_work" bash "$TMP/mixed.sh" api --target prod --retries 3 --force
assert_same_run_triplet mixed_vm mixed_bash 'mixed VM/Bash parity'
assert_contains "$TMP/mixed_vm.out" 'from-lib' 'mixed output includes imported statement'
assert_contains "$TMP/mixed_vm.out" 'capture=cap:err:7:true' 'mixed output includes captured command fields'
assert_not_contains "$TMP/mixed.sh" 'test ' 'mixed normal Bash excludes test-only machinery'

# Individual edge cases: functions, collections, commands, stdlib.
write_fixture "$FIX/function_edges.ds" <<'DS'
fn greet(name = "world") {
  echo "hello {name}"
}
greet()
greet("ds")
DS
capture_status function_edges "$DS" run "$FIX/function_edges.ds"
assert_status function_edges 0
assert_same_text $'hello world\nhello ds\n' "$TMP/function_edges.out" 'function defaults and args work'
write_fixture "$FIX/too_many.ds" <<'DS'
fn f(x) { echo $x }
f("a", "b")
DS
capture_status too_many "$DS" run "$FIX/too_many.ds"
assert_nonzero_status too_many
assert_contains "$TMP/too_many.err" 'expects 1 arguments but got 2' 'function too-many-args diagnostic'
write_fixture "$FIX/collections_edges.ds" <<'DS'
let xs = ["a"]
xs.push("b")
let second = xs[1]
echo "{second}"
let ports = { api: 3000 }
let api_port = ports.api
echo "{api_port}"
DS
capture_status collections_edges "$DS" run "$FIX/collections_edges.ds"
assert_status collections_edges 0
assert_same_text $'b\n3000\n' "$TMP/collections_edges.out" 'collections indexing/map/push work'
write_fixture "$FIX/missing_key.ds" <<'DS'
let ports = { api: 3000 }
let web_port = ports.web
echo "{web_port}"
DS
capture_status missing_key "$DS" run "$FIX/missing_key.ds"
assert_nonzero_status missing_key
assert_contains "$TMP/missing_key.err" 'missing map key `web`' 'missing map key diagnostic'
write_fixture "$FIX/redir_space.ds" <<'DS'
sh -c "printf out; printf err >&2" &> "path with spaces.log"
DS
capture_in_dir redir_space "$TMP/redir_work" "$DS" run "$FIX/redir_space.ds"
assert_status redir_space 0
assert_same_text 'outerr' "$TMP/redir_work/path with spaces.log" 'combined redirection path with spaces works'
write_fixture "$FIX/stdlib_edges.ds" <<'DS'
file.write("a.txt", "one\ntwo")
let exists = file.exists("a.txt")
let is_file = file.is_file("a.txt")
let dir_ok = dir.exists(".")
let parent = path.dirname(path.join(path.cwd(), "a.txt"))
let base = path.basename("a.txt")
let ext = path.ext("archive.tar.gz")
env.set("DS_V016", "yes")
let env_yes = env.get("DS_V016", "no")
env.unset("DS_V016")
let env_no = env.get("DS_V016", "no")
echo "{exists}"
echo "{is_file}"
echo "{dir_ok}"
echo "{parent}"
echo "{base}"
echo "{ext}"
echo "{env_yes}"
echo "{env_no}"
for line in lines("a.txt") { echo "line={line}" }
for match in glob("*.txt") {
  let match_name = path.basename(match)
  echo "match={match_name}"
}
DS
capture_in_dir stdlib_edges "$TMP/stdlib_work" "$DS" run "$FIX/stdlib_edges.ds"
assert_status stdlib_edges 0
assert_contains "$TMP/stdlib_edges.out" 'true' 'stdlib boolean helpers work'
assert_contains "$TMP/stdlib_edges.out" 'a.txt' 'path.basename works'
assert_contains "$TMP/stdlib_edges.out" '.gz' 'path.ext works'
assert_contains "$TMP/stdlib_edges.out" 'yes' 'env.get sees set value'
assert_contains "$TMP/stdlib_edges.out" 'no' 'env.get default works after unset'
assert_contains "$TMP/stdlib_edges.out" 'line=one' 'lines iterates file'
assert_contains "$TMP/stdlib_edges.out" 'match=a.txt' 'glob returns file match'

# Test runner isolation and test-only behavior.
write_fixture "$FIX/test_iso.ds" <<'DS'
echo "prod"

test "pass" {
  assert true
}

test "fail" {
  assert false
}
DS
capture_status test_iso "$DS" test "$FIX/test_iso.ds"
assert_nonzero_status test_iso
assert_contains "$TMP/test_iso.out" 'ok   pass' 'passing test is reported'
assert_contains "$TMP/test_iso.out" 'fail fail' 'failing test is reported'
assert_diag_shape "$TMP/test_iso.err" "$FIX/test_iso.ds" error 'assertion failed' 'failing assertion diagnostic shape'
capture_status test_iso_run "$DS" run "$FIX/test_iso.ds"
assert_status test_iso_run 0
assert_same_text $'prod\n' "$TMP/test_iso_run.out" 'normal run ignores test blocks'
capture_status test_iso_direct "$DS" "$FIX/test_iso.ds"
assert_same_run_triplet test_iso_run test_iso_direct 'direct execution ignores test blocks like run'
run_ok test_iso_emit "$DS" emit bash "$FIX/test_iso.ds" -o "$TMP/test_iso.sh"
assert_bash_standalone "$TMP/test_iso.sh" 'test-isolation emitted Bash'
capture_status test_iso_bash bash "$TMP/test_iso.sh"
assert_same_run_triplet test_iso_run test_iso_bash 'emitted Bash ignores test blocks'
write_fixture "$FIX/test_control.ds" <<'DS'
test "fail message" {
  fail "boom"
}

test "exit pass" {
  exit 0
  fail "unreachable"
}

test "exit fail" {
  exit 2
}
DS
capture_status test_control "$DS" test "$FIX/test_control.ds"
assert_nonzero_status test_control
assert_contains "$TMP/test_control.out" 'fail fail message' 'fail message affects only active test'
assert_contains "$TMP/test_control.out" 'ok   exit pass' 'exit 0 passes/stops active test'
assert_contains "$TMP/test_control.out" 'fail exit fail' 'exit nonzero fails active test'
write_fixture "$FIX/imported_tests_lib.ds" <<'DS'
test "imported pass" {
  assert true
}
DS
write_fixture "$FIX/imported_tests_main.ds" <<'DS'
import "./imported_tests_lib.ds"

test "root pass" {
  assert true
}
DS
capture_status imported_tests "$DS" test "$FIX/imported_tests_main.ds"
assert_status imported_tests 0
assert_contains "$TMP/imported_tests.out" 'ok   imported pass' 'imported test block is discovered'
assert_contains "$TMP/imported_tests.out" 'ok   root pass' 'root test block still runs with imported tests'
run_ok imported_tests_emit "$DS" emit bash "$FIX/imported_tests_main.ds" -o "$TMP/imported_tests.sh"
assert_bash_standalone "$TMP/imported_tests.sh" 'imported-tests emitted Bash'
capture_status imported_tests_bash bash "$TMP/imported_tests.sh"
assert_status imported_tests_bash 0
assert_same_text '' "$TMP/imported_tests_bash.out" 'emitted Bash ignores imported test blocks'

write_fixture "$FIX/runtime_regex_matches.ds" <<'DS'
let pat = "^a$"
if "a" matches pat { echo "yes" }
DS
capture_status runtime_regex_matches_vm "$DS" run "$FIX/runtime_regex_matches.ds"
assert_status runtime_regex_matches_vm 0
assert_same_text $'yes\n' "$TMP/runtime_regex_matches_vm.out" 'runtime regex-string matches now succeeds'
run_ok runtime_regex_matches_emit "$DS" emit bash "$FIX/runtime_regex_matches.ds" -o "$TMP/runtime_regex_matches.sh"
assert_bash_standalone "$TMP/runtime_regex_matches.sh" 'runtime regex matches Bash'
capture_status runtime_regex_matches_bash bash "$TMP/runtime_regex_matches.sh"
assert_same_run_triplet runtime_regex_matches_vm runtime_regex_matches_bash 'Bash matches VM for runtime regex-string matches'

# Malformed and unsupported syntax diagnostics fail before execution/emission.
for pair in \
  'malformed_let|let =|expected identifier' \
  'malformed_if|if true {|expected' \
  'malformed_fn|fn broken( {\n}|expected' \
  'malformed_test|test "bad" {\n  assert\n}|expected expression'; do
  IFS='|' read -r name body frag <<<"$pair"
  printf '%b\n' "$body" >"$FIX/$name.ds"
  for cmd in check fmt run test hir bytecode; do
    capture_status "${name}_${cmd}" "$DS" "$cmd" "$FIX/$name.ds"
    assert_nonzero_status "${name}_${cmd}"
    assert_contains "$TMP/${name}_${cmd}.err" "$FIX/$name.ds:" "$name $cmd diagnostic has filename"
    assert_contains "$TMP/${name}_${cmd}.err" '^' "$name $cmd diagnostic has caret"
  done
  capture_status "${name}_emit" "$DS" emit bash "$FIX/$name.ds" -o "$TMP/$name.sh"
  assert_nonzero_status "${name}_emit"
  assert_contains "$TMP/${name}_emit.err" "$frag" "$name emit diagnostic contains expected fragment"
done

unsupported_cases=(
  'unknown_string_method|let x = "a".regex_replace("a", "b")|unknown string method'
  'env_append|env.PATH += ":/tmp/bin"|environment assignment supports only `=`'
  'map_iteration|for k in { a: 1 } { echo $k }|expected iterable expression after `in`'
  'nested_collection|let xs = [[1]]|nested collections are deferred'
  'bash_if|if [ -f foo ]; then echo yes; fi|expected `]` to close array literal'
  'raw_fd_redir|echo hi 2>&1|unsupported command operator `>`'
)
for pair in "${unsupported_cases[@]}"; do
  IFS='|' read -r name body frag <<<"$pair"
  printf '%s\n' "$body" >"$FIX/$name.ds"
  capture_status "unsupported_${name}_check" "$DS" check "$FIX/$name.ds"
  assert_nonzero_status "unsupported_${name}_check"
  assert_contains "$TMP/unsupported_${name}_check.err" "$frag" "unsupported $name check diagnostic"
  capture_status "unsupported_${name}_emit" "$DS" emit bash "$FIX/$name.ds" -o "$TMP/$name.sh"
  assert_nonzero_status "unsupported_${name}_emit"
  [ ! -s "$TMP/$name.sh" ] || fail "unsupported $name should not emit successful Bash"
done
write_fixture "$FIX/recursive_glob.ds" <<'DS'
for path in glob("**file.ds") {
  echo $path
}
DS
capture_status recursive_glob "$DS" run "$FIX/recursive_glob.ds"
assert_nonzero_status recursive_glob
assert_contains "$TMP/recursive_glob.err" 'recursive `**` glob patterns must use `**` as a complete path segment' 'recursive glob rejected clearly'

# Runtime ownership smoke: allocation-heavy supported paths remain stable and leave no command-capture temp files.
write_fixture "$FIX/ownership.ds" <<'DS'
let names = ["api", "web", "worker", "cron"]
fn greet(name, prefix = "svc") {
  let base = path.basename("data.txt")
  echo "{prefix}:{name}:{base}"
}
file.write("data.txt", "a\nb\nc")
for name in names {
  greet(name)
  greet(name, "again")
}
let cap = run sh -c "printf stdout; printf stderr >&2"
echo "cap={cap.stdout}:{cap.stderr}:{cap.code}"
for line in lines("data.txt") { echo "line={line}" }
for match in glob("*.txt") {
  let match_name = path.basename(match)
  echo "glob={match_name}"
}
DS
capture_in_dir ownership "$TMP/ownership_work" "$DS" run "$FIX/ownership.ds"
assert_status ownership 0
assert_contains "$TMP/ownership.out" 'again:worker:data.txt' 'ownership fixture exercises interpolation/functions/arrays'
assert_contains "$TMP/ownership.out" 'cap=stdout:stderr:0' 'ownership fixture exercises command capture'
unexpected_tmp="$(find "$TMP/ownership_work" -maxdepth 1 -type f ! -name 'data.txt' -print)"
[ -z "$unexpected_tmp" ] || { echo "$unexpected_tmp" >&2; fail 'ownership fixture left unexpected temp files'; }
pass 'ownership fixture leaves no unexpected temp files'
run_ok ownership_emit "$DS" emit bash "$FIX/ownership.ds" -o "$TMP/ownership.sh"
assert_bash_standalone "$TMP/ownership.sh" 'ownership emitted Bash'

printf 'v0.16.0 cleanup tests passed (%d assertions)\n' "$pass_count"
