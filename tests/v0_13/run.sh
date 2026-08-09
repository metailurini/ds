#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_13_tests.$$"
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

make_fakebin() {
  local bin="$TMP/fakebin"
  mkdir -p "$bin"
  cat >"$bin/fake-ok" <<'SH'
#!/usr/bin/env bash
printf 'ok:%s\n' "$*"
SH
  cat >"$bin/fake-fail" <<'SH'
#!/usr/bin/env bash
printf 'out:%s\n' "$*"
printf 'err:%s\n' "$*" >&2
exit 7
SH
  cat >"$bin/fake-fail-now" <<'SH'
#!/usr/bin/env bash
printf 'before-fail\n'
printf 'plain-fail\n' >&2
exit 9
SH
  cat >"$bin/fake-err" <<'SH'
#!/usr/bin/env bash
printf 'err:%s\n' "$*" >&2
SH
  cat >"$bin/fake-mixed" <<'SH'
#!/usr/bin/env bash
printf 'out:%s\n' "$*"
printf 'err:%s\n' "$*" >&2
SH
  chmod +x "$bin"/*
  printf '%s' "$bin"
}

normalize_paths() {
  local src="$1" dst="$2"
  sed -E "s#${TMP//\/\\}#[TMP]#g; s#${ROOT//\/\\}#[ROOT]#g" "$src" >"$dst"
}

assert_matches() {
  local file="$1"
  local regex="$2"
  local name="$3"
  grep -E -- "$regex" "$file" >/dev/null || {
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected to match /$regex/"
  }
  pass "$name"
}

assert_not_matches() {
  local file="$1"
  local regex="$2"
  local name="$3"
  if grep -E -- "$regex" "$file" >/dev/null; then
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected not to match /$regex/"
  fi
  pass "$name"
}

assert_line_count() {
  local expected="$1" file="$2" pattern="$3" name="$4"
  local count
  count="$(grep -E -c -- "$pattern" "$file" || true)"
  [ "$count" = "$expected" ] || {
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected $expected matching lines, got $count"
  }
  pass "$name"
}

assert_stdout_stderr_status_same() {
  local left="$1" right="$2" name="$3"
  assert_same "$TMP/${left}.out" "$TMP/${right}.out" "$name stdout"
  assert_same "$TMP/${left}.err" "$TMP/${right}.err" "$name stderr"
  assert_same "$TMP/${left}.rc" "$TMP/${right}.rc" "$name status"
}

assert_diag_span() {
  local file="$1" fixture="$2" message="$3" name="$4"
  assert_contains "$file" "$fixture:" "$name path"
  assert_contains "$file" ': error:' "$name error shape"
  assert_contains "$file" "$message" "$name message"
  assert_contains "$file" '^' "$name caret"
}

FAKEBIN="$(make_fakebin)"
FIX="$TMP/fixtures"
mkdir -p "$FIX"

# Static and wiring tests.
assert_contains Makefile '0-13' 'Makefile lists v0.13 exactly somewhere'
count_013="$(grep -E '^TEST_VERSIONS :=' Makefile | grep -o '0-13' | wc -l | tr -d ' ')"
[ "$count_013" = 1 ] || fail "TEST_VERSIONS should contain 0-13 exactly once, got $count_013"
pass 'TEST_VERSIONS contains 0-13 exactly once'
assert_contains Makefile '$(TEST_TARGETS): $(BIN)' 'pattern target still drives version suites'
assert_contains Makefile 'asan:' 'asan target exists'
assert_contains Makefile 'ubsan:' 'ubsan target exists'

run_ok help_top "$DS" --help
for text in \
  'ds <file.ds> [args...]' \
  'ds run <file.ds> [args...]' \
  'ds run [--trace-cmd] [--trace-vm] <file.ds> [args...]' \
  'ds tokens <file.ds>' \
  'ds ast <file.ds>' \
  'ds check <file.ds>' \
  'ds hir <file.ds>' \
  'ds bytecode <file.ds>' \
  'ds emit bash <file.ds> -o <file.sh>'; do
  assert_contains "$TMP/help_top.out" "$text" "help lists $text"
done

write_fixture "$FIX/side_effect.ds" <<'DS'
file.write("should_not_exist.txt", "bad")
DS
capture_status unknown_trace "$DS" run --trace-nope "$FIX/side_effect.ds"
assert_nonzero_status unknown_trace
assert_contains "$TMP/unknown_trace.err" 'unknown run flag `--trace-nope`' 'unknown trace flag diagnostic'
[ ! -e should_not_exist.txt ] || fail 'unknown trace flag must not execute script'
pass 'unknown trace flag does not execute source'
capture_status unknown_after_trace "$DS" run --trace-cmd --unknown "$FIX/side_effect.ds"
assert_nonzero_status unknown_after_trace
assert_contains "$TMP/unknown_after_trace.err" 'unknown run flag `--unknown`' 'unknown flag after trace diagnostic'
capture_status missing_trace_file "$DS" run --trace-vm
assert_nonzero_status missing_trace_file
assert_contains "$TMP/missing_trace_file.err" 'Usage:' 'missing file under trace prints usage'
write_fixture "$FIX/trace_arg.ds" <<'DS'
script {
  arg value: string
}
echo "value={value}"
DS
capture_status trace_like_script_arg "$DS" run "$FIX/trace_arg.ds" --trace-cmd
assert_nonzero_status trace_like_script_arg
assert_contains "$TMP/trace_like_script_arg.err" "$FIX/trace_arg.ds:" 'trace-looking arg after file reaches script parser'
assert_contains "$TMP/trace_like_script_arg.err" 'unknown option `--trace-cmd`' 'trace-looking arg after file is not parsed as ds flag'

# HIR debug output tests.
write_fixture "$FIX/minimal.ds" <<'DS'
let name = "world"
echo "hello {name}"
DS
run_ok hir_minimal_a "$DS" hir "$FIX/minimal.ds"
run_ok hir_minimal_b "$DS" hir "$FIX/minimal.ds"
assert_same "$TMP/hir_minimal_a.out" "$TMP/hir_minimal_b.out" 'minimal HIR deterministic'
assert_same_text '' "$TMP/hir_minimal_a.err" 'minimal HIR stderr empty'
assert_contains "$TMP/hir_minimal_a.out" 'Program' 'HIR has Program marker'
assert_contains "$TMP/hir_minimal_a.out" 'Let name' 'HIR includes let statement'
assert_contains "$TMP/hir_minimal_a.out" 'Command ["echo", "\"hello {name}\""]' 'HIR includes command words'
assert_contains "$TMP/hir_minimal_a.out" "$FIX/minimal.ds:1:1" 'HIR includes source span'
assert_not_matches "$TMP/hir_minimal_a.out" '0x[0-9a-fA-F]+' 'HIR is pointer-free'

write_fixture "$FIX/lib.ds" <<'DS'
fn helper(label = "lib") {
  echo "from {label}"
}
DS
write_fixture "$FIX/full.ds" <<'DS'
script {
  arg app: string
  option target: string = "staging"
  option retries: int = 3
  flag force: bool = false
}

import "./lib.ds"

fn deploy(name = "api") {
  let files = ["a.txt", "b.txt"]
  for file in files {
    echo "file={file}"
  }
  let ports = { api: 3000, web: 5173 }
  let port = ports.api
  echo "port={port}"
  let result = run echo "deploy {name}"
  if result.ok {
    echo result.stdout
  }
  cmd.require("echo")
}

helper()
deploy(app)
DS
run_ok hir_full "$DS" hir "$FIX/full.ds"
for text in \
  'Script' \
  'Arg app: string' \
  'Option target: string = "staging"' \
  'Option retries: int = 3' \
  'Flag force: bool = false' \
  'Function helper(label: default string = "lib")' \
  'Function deploy(name: default string = "api")' \
  'Array' \
  'Map' \
  'For file' \
  'Run ["echo"' \
  'CallStmt cmd.require' \
  'Field .ok'; do
  assert_contains "$TMP/hir_full.out" "$text" "full HIR includes $text"
done
assert_contains "$TMP/hir_full.out" "$FIX/lib.ds" 'full HIR includes imported file path'
assert_not_matches "$TMP/hir_full.out" '0x[0-9a-fA-F]+' 'full HIR pointer-free'

write_fixture "$FIX/redir_hir.ds" <<'DS'
echo "hi" |> "out.txt"
echo "err" !> "err.txt"
echo "all" &> "all.txt"
DS
run_ok hir_redir "$DS" hir "$FIX/redir_hir.ds"
assert_contains "$TMP/hir_redir.out" 'Redirect >' 'HIR includes stdout redirect'
assert_contains "$TMP/hir_redir.out" 'Redirect 2>' 'HIR includes stderr redirect'
assert_contains "$TMP/hir_redir.out" 'Redirect &>' 'HIR includes combined redirect'
assert_contains "$TMP/hir_redir.out" 'out.txt' 'HIR includes redirection target'

write_fixture "$FIX/bad_parse.ds" <<'DS'
let x =
DS
run_fail hir_bad_parse "$DS" hir "$FIX/bad_parse.ds"
assert_same_text '' "$TMP/hir_bad_parse.out" 'invalid HIR prints no stdout'
assert_diag_span "$TMP/hir_bad_parse.err" "$FIX/bad_parse.ds" 'expected expression' 'invalid HIR parse diagnostic'
write_fixture "$FIX/bad_arity.ds" <<'DS'
fn glob() {
  echo "bad"
}
DS
run_fail hir_bad_lower "$DS" hir "$FIX/bad_arity.ds"
assert_same_text '' "$TMP/hir_bad_lower.out" 'invalid lowered HIR prints no stdout'
assert_diag_span "$TMP/hir_bad_lower.err" "$FIX/bad_arity.ds" 'conflicts with a v0.11.0 standard-library helper name' 'invalid HIR lower diagnostic'
write_fixture "$FIX/bad_import_root.ds" <<'DS'
import "./bad_imported.ds"
DS
write_fixture "$FIX/bad_imported.ds" <<'DS'
fn glob() {
  echo "bad"
}
DS
run_fail hir_bad_import "$DS" hir "$FIX/bad_import_root.ds"
assert_contains "$TMP/hir_bad_import.err" "$FIX/bad_imported.ds:" 'HIR import diagnostic uses imported source'
assert_contains "$TMP/hir_bad_import.err" 'conflicts with a v0.11.0 standard-library helper name' 'HIR import diagnostic message'

# Bytecode debug output tests.
write_fixture "$FIX/bytecode_meta.ds" <<'DS'
script {
  arg app: string
  option retries: int = 2
  flag force: bool = false
}

fn greet(name = "world") {
  echo "hello {name}"
}

greet(app)
DS
run_ok bytecode_meta_a "$DS" bytecode "$FIX/bytecode_meta.ds"
run_ok bytecode_meta_b "$DS" bytecode "$FIX/bytecode_meta.ds"
assert_same "$TMP/bytecode_meta_a.out" "$TMP/bytecode_meta_b.out" 'bytecode deterministic'
assert_same_text '' "$TMP/bytecode_meta_a.err" 'bytecode stderr empty'
for text in 'args:' 'arg app: string' 'option retries: int = 2' 'flag force: bool = false' 'functions:' 'greet' 'constants:' 'instructions:' '0000' 'CALL'; do
  assert_contains "$TMP/bytecode_meta_a.out" "$text" "bytecode includes $text"
done
assert_contains "$TMP/bytecode_meta_a.out" "$FIX/bytecode_meta.ds:" 'bytecode includes source span'
assert_not_matches "$TMP/bytecode_meta_a.out" '0x[0-9a-fA-F]+' 'bytecode pointer-free'
write_fixture "$FIX/bytecode_stdlib.ds" <<'DS'
file.write("tmp.txt", "one\n")
let text = file.read("tmp.txt")
let xs = ["a", "b"]
let ports = { api: 3000 }
for x in xs {
  echo "x={x}"
}
let port = ports.api
echo "port={port}"
DS
run_ok bytecode_stdlib "$DS" bytecode "$FIX/bytecode_stdlib.ds"
for text in 'file.write' 'file.read' 'ARRAY_LITERAL' 'MAP_LITERAL' 'FOR_ARRAY' 'one\n'; do
  assert_contains "$TMP/bytecode_stdlib.out" "$text" "bytecode collections/stdlib includes $text"
done
run_fail bytecode_bad "$DS" bytecode "$FIX/bad_parse.ds"
assert_same_text '' "$TMP/bytecode_bad.out" 'invalid bytecode prints no stdout'
assert_contains "$TMP/bytecode_bad.err" 'expected expression' 'invalid bytecode diagnostic'

# VM command tracing tests.
write_fixture "$FIX/plain_trace.ds" <<'DS'
let target = "prod"
fake-ok $target
DS
capture_status plain_normal env PATH="$FAKEBIN:$PATH" "$DS" run "$FIX/plain_trace.ds"
assert_status plain_normal 0
capture_status plain_trace env PATH="$FAKEBIN:$PATH" "$DS" run --trace-cmd "$FIX/plain_trace.ds"
assert_status plain_trace 0
assert_same "$TMP/plain_normal.out" "$TMP/plain_trace.out" 'trace-cmd preserves stdout'
assert_same_text '' "$TMP/plain_normal.err" 'normal command stderr empty'
assert_contains "$TMP/plain_trace.err" 'trace: cmd' 'trace-cmd emits command trace'
assert_contains "$TMP/plain_trace.err" "$FIX/plain_trace.ds:2:1" 'trace-cmd source span'
assert_contains "$TMP/plain_trace.err" '"fake-ok" "prod"' 'trace-cmd shows expanded argv'

write_fixture "$FIX/captured_trace.ds" <<'DS'
let result = run fake-fail "abc"
echo "ok={result.ok}"
echo "code={result.code}"
echo "out={result.stdout}"
echo "err={result.stderr}"
DS
capture_status captured_normal env PATH="$FAKEBIN:$PATH" "$DS" run "$FIX/captured_trace.ds"
assert_status captured_normal 0
capture_status captured_trace env PATH="$FAKEBIN:$PATH" "$DS" run --trace-cmd "$FIX/captured_trace.ds"
assert_status captured_trace 0
assert_same "$TMP/captured_normal.out" "$TMP/captured_trace.out" 'captured trace preserves stdout'
assert_contains "$TMP/captured_trace.err" 'trace: cmd' 'captured run emits trace'
assert_contains "$TMP/captured_trace.err" "$FIX/captured_trace.ds:1:14" 'captured run trace uses expression span'
assert_not_contains "$TMP/captured_trace.err" 'plain-fail' 'captured run stderr does not leak directly'
assert_contains "$TMP/captured_trace.out" 'ok=false' 'captured failure remains non-fatal'
assert_contains "$TMP/captured_trace.out" 'code=7' 'captured failure code visible'
assert_contains "$TMP/captured_trace.out" 'err=err:abc' 'captured stderr stored'

write_fixture "$FIX/plain_fail_trace.ds" <<'DS'
fake-fail-now
echo "after"
DS
capture_status plain_fail_normal env PATH="$FAKEBIN:$PATH" "$DS" run "$FIX/plain_fail_trace.ds"
assert_status plain_fail_normal 9
capture_status plain_fail_trace env PATH="$FAKEBIN:$PATH" "$DS" run --trace-cmd "$FIX/plain_fail_trace.ds"
assert_status plain_fail_trace 9
assert_same "$TMP/plain_fail_normal.out" "$TMP/plain_fail_trace.out" 'failed trace preserves stdout'
assert_contains "$TMP/plain_fail_trace.err" 'trace: cmd' 'failed trace emits command trace'
assert_contains "$TMP/plain_fail_trace.err" "$FIX/plain_fail_trace.ds:1:1: error:" 'failed trace keeps source diagnostic'
assert_contains "$TMP/plain_fail_trace.err" 'command `fake-fail-now` failed with exit 9' 'failed trace diagnostic message'
assert_not_contains "$TMP/plain_fail_trace.out" 'after' 'plain failure remains fail-fast under trace'

write_fixture "$FIX/control_trace.ds" <<'DS'
fn run_one(name) {
  fake-ok $name
}

let xs = ["a", "b"]
for x in xs {
  run_one(x)
}

if true {
  fake-ok "then"
} else {
  fake-ok "else"
}
DS
capture_status control_trace env PATH="$FAKEBIN:$PATH" "$DS" run --trace-cmd "$FIX/control_trace.ds"
assert_status control_trace 0
assert_line_count 3 "$TMP/control_trace.err" '^trace: cmd ' 'control trace command count'
assert_contains "$TMP/control_trace.err" '"fake-ok" "a"' 'control trace loop first iteration'
assert_contains "$TMP/control_trace.err" '"fake-ok" "b"' 'control trace loop second iteration'
assert_contains "$TMP/control_trace.err" '"fake-ok" "then"' 'control trace then branch'
assert_not_contains "$TMP/control_trace.err" 'else' 'control trace omits non-executed branch'

write_fixture "$FIX/imported_cmd.ds" <<'DS'
fn imported_run() {
  fake-ok "imported"
}
DS
write_fixture "$FIX/import_trace.ds" <<'DS'
import "./imported_cmd.ds"
imported_run()
DS
capture_status import_trace env PATH="$FAKEBIN:$PATH" "$DS" run --trace-cmd "$FIX/import_trace.ds"
assert_status import_trace 0
assert_contains "$TMP/import_trace.err" "$FIX/imported_cmd.ds:2:3" 'imported command trace uses imported source span'
assert_contains "$TMP/import_trace.err" '"fake-ok" "imported"' 'imported command trace argv'

write_fixture "$FIX/redir_trace.ds" <<'DS'
fake-ok "one" |> "out.txt"
fake-err "two" !> "err.txt"
fake-mixed "three" &> "all.txt"
DS
mkdir -p "$TMP/redir_vm_normal" "$TMP/redir_vm_trace"
capture_status redir_normal bash -c "cd '$TMP/redir_vm_normal' && PATH='$FAKEBIN':\$PATH '$DS' run '$FIX/redir_trace.ds'"
assert_status redir_normal 0
capture_status redir_trace bash -c "cd '$TMP/redir_vm_trace' && PATH='$FAKEBIN':\$PATH '$DS' run --trace-cmd '$FIX/redir_trace.ds'"
assert_status redir_trace 0
assert_same "$TMP/redir_normal.out" "$TMP/redir_trace.out" 'redir trace preserves stdout'
assert_contains "$TMP/redir_trace.err" 'trace: cmd' 'redir trace emits traces'
assert_contains "$TMP/redir_trace.err" '> "out.txt"' 'redir trace shows stdout target'
assert_contains "$TMP/redir_trace.err" '2> "err.txt"' 'redir trace shows stderr target'
assert_contains "$TMP/redir_trace.err" '&> "all.txt"' 'redir trace shows combined target'
assert_same "$TMP/redir_vm_normal/out.txt" "$TMP/redir_vm_trace/out.txt" 'redir stdout file parity'
assert_same "$TMP/redir_vm_normal/err.txt" "$TMP/redir_vm_trace/err.txt" 'redir stderr file parity'
assert_same "$TMP/redir_vm_normal/all.txt" "$TMP/redir_vm_trace/all.txt" 'redir combined file parity'

# Emitted Bash command tracing tests.
run_ok emit_plain_trace "$DS" emit bash "$FIX/plain_trace.ds" -o "$TMP/plain_trace.sh"
run_ok bash_plain_syntax bash -n "$TMP/plain_trace.sh"
assert_not_contains "$TMP/plain_trace.sh" "$DS" 'emitted Bash remains standalone'
capture_status bash_plain_default env PATH="$FAKEBIN:$PATH" bash "$TMP/plain_trace.sh"
assert_status bash_plain_default 0
assert_same "$TMP/plain_normal.out" "$TMP/bash_plain_default.out" 'Bash default stdout matches VM'
assert_not_contains "$TMP/bash_plain_default.err" 'trace: cmd' 'Bash default has no trace noise'
capture_status bash_plain_trace env PATH="$FAKEBIN:$PATH" DS_TRACE_CMD=1 bash "$TMP/plain_trace.sh"
assert_status bash_plain_trace 0
assert_same "$TMP/bash_plain_default.out" "$TMP/bash_plain_trace.out" 'Bash trace preserves stdout'
assert_contains "$TMP/bash_plain_trace.err" 'trace: cmd' 'Bash trace emits command trace'
assert_contains "$TMP/bash_plain_trace.err" "$FIX/plain_trace.ds:2:1" 'Bash trace source span'
assert_contains "$TMP/bash_plain_trace.err" 'fake-ok' 'Bash trace command name'
assert_not_contains "$TMP/plain_trace.sh" ' ds ' 'emitted Bash does not invoke ds command'
capture_status bash_plain_trace_empty env PATH="$FAKEBIN:$PATH" DS_TRACE_CMD= bash "$TMP/plain_trace.sh"
assert_status bash_plain_trace_empty 0
assert_same "$TMP/bash_plain_default.out" "$TMP/bash_plain_trace_empty.out" 'Bash empty trace preserves stdout'
assert_not_contains "$TMP/bash_plain_trace_empty.err" 'trace: cmd' 'Bash empty DS_TRACE_CMD does not trace'
capture_status bash_plain_trace_zero env PATH="$FAKEBIN:$PATH" DS_TRACE_CMD=0 bash "$TMP/plain_trace.sh"
assert_status bash_plain_trace_zero 0
assert_same "$TMP/bash_plain_default.out" "$TMP/bash_plain_trace_zero.out" 'Bash zero trace preserves stdout'
assert_not_contains "$TMP/bash_plain_trace_zero.err" 'trace: cmd' 'Bash DS_TRACE_CMD=0 does not trace'

run_ok emit_captured_trace "$DS" emit bash "$FIX/captured_trace.ds" -o "$TMP/captured_trace.sh"
run_ok bash_captured_syntax bash -n "$TMP/captured_trace.sh"
capture_status bash_captured_default env PATH="$FAKEBIN:$PATH" bash "$TMP/captured_trace.sh"
assert_status bash_captured_default 0
capture_status bash_captured_trace env PATH="$FAKEBIN:$PATH" DS_TRACE_CMD=1 bash "$TMP/captured_trace.sh"
assert_status bash_captured_trace 0
assert_same "$TMP/bash_captured_default.out" "$TMP/bash_captured_trace.out" 'Bash captured trace preserves stdout'
assert_contains "$TMP/bash_captured_trace.err" 'trace: cmd' 'Bash captured run trace emitted'
assert_contains "$TMP/bash_captured_trace.err" "$FIX/captured_trace.ds:1:14" 'Bash captured run trace uses expression source'
assert_not_contains "$TMP/bash_captured_trace.err" 'plain command exited' 'Bash captured failure is non-fatal'

write_fixture "$FIX/script_arg_trace.ds" <<'DS'
script {
  arg app: string
  option target: string = "staging"
  flag force: bool = false
}

fake-ok $app $target
if force {
  fake-ok "force"
}
DS
run_ok emit_script_arg_trace "$DS" emit bash "$FIX/script_arg_trace.ds" -o "$TMP/script_arg_trace.sh"
run_ok bash_script_arg_syntax bash -n "$TMP/script_arg_trace.sh"
capture_status bash_script_default env PATH="$FAKEBIN:$PATH" bash "$TMP/script_arg_trace.sh" api --target prod --force
assert_status bash_script_default 0
capture_status bash_script_trace env PATH="$FAKEBIN:$PATH" DS_TRACE_CMD=1 bash "$TMP/script_arg_trace.sh" api --target prod --force
assert_status bash_script_trace 0
assert_same "$TMP/bash_script_default.out" "$TMP/bash_script_trace.out" 'Bash script-arg trace preserves stdout'
assert_contains "$TMP/bash_script_trace.err" 'api' 'Bash script-arg trace includes arg value'
assert_contains "$TMP/bash_script_trace.err" 'prod' 'Bash script-arg trace includes option value'
assert_contains "$TMP/bash_script_trace.err" 'force' 'Bash script-arg trace includes flag-controlled command'

run_ok emit_fail_trace "$DS" emit bash "$FIX/plain_fail_trace.ds" -o "$TMP/plain_fail_trace.sh"
run_ok bash_fail_syntax bash -n "$TMP/plain_fail_trace.sh"
capture_status bash_fail_default env PATH="$FAKEBIN:$PATH" bash "$TMP/plain_fail_trace.sh"
assert_status bash_fail_default 9
assert_contains "$TMP/bash_fail_default.err" "$FIX/plain_fail_trace.ds:1:1: error:" 'Bash command failure source marker'
assert_contains "$TMP/bash_fail_default.err" 'command failed with exit 9' 'Bash command failure status'
assert_not_contains "$TMP/bash_fail_default.out" 'after' 'Bash command failure fail-fast'

# VM instruction tracing tests.
write_fixture "$FIX/vm_trace.ds" <<'DS'
let x = 3
echo "x={x}"
DS
capture_status vm_trace_normal "$DS" run "$FIX/vm_trace.ds"
assert_status vm_trace_normal 0
capture_status vm_trace "$DS" run --trace-vm "$FIX/vm_trace.ds"
assert_status vm_trace 0
assert_same "$TMP/vm_trace_normal.out" "$TMP/vm_trace.out" 'trace-vm preserves stdout'
assert_contains "$TMP/vm_trace.err" 'trace: vm ip=' 'VM trace prefix includes ip'
assert_contains "$TMP/vm_trace.err" 'op=' 'VM trace includes opcode names'
assert_contains "$TMP/vm_trace.err" "$FIX/vm_trace.ds:" 'VM trace includes source marker'
assert_same_text '' "$TMP/vm_trace_normal.err" 'normal VM trace fixture stderr empty'

capture_status combined_trace env PATH="$FAKEBIN:$PATH" "$DS" run --trace-cmd --trace-vm "$FIX/plain_trace.ds"
assert_status combined_trace 0
assert_contains "$TMP/combined_trace.err" 'trace: vm ip=' 'combined trace includes VM lines'
assert_contains "$TMP/combined_trace.err" 'trace: cmd' 'combined trace includes command lines'
assert_same "$TMP/plain_trace.out" "$TMP/combined_trace.out" 'combined trace preserves stdout'
capture_status combined_trace_reversed env PATH="$FAKEBIN:$PATH" "$DS" run --trace-vm --trace-cmd "$FIX/plain_trace.ds"
assert_status combined_trace_reversed 0
assert_contains "$TMP/combined_trace_reversed.err" 'trace: vm ip=' 'combined trace reversed includes VM lines'
assert_contains "$TMP/combined_trace_reversed.err" 'trace: cmd' 'combined trace reversed includes command lines'
assert_same "$TMP/plain_trace.out" "$TMP/combined_trace_reversed.out" 'combined trace reversed preserves stdout'
capture_status duplicate_trace_flag env PATH="$FAKEBIN:$PATH" "$DS" run --trace-cmd --trace-cmd "$FIX/plain_trace.ds"
assert_status duplicate_trace_flag 0
assert_line_count 1 "$TMP/duplicate_trace_flag.err" '^trace: cmd ' 'duplicate trace flag is idempotent'

write_fixture "$FIX/special_trace.ds" <<'DS'
fake-ok "space arg" "dollar $HOME" "star *" "" "slash \\"
DS
capture_status special_trace env PATH="$FAKEBIN:$PATH" "$DS" run --trace-cmd "$FIX/special_trace.ds"
assert_status special_trace 0
for text in '"space arg"' '"dollar $HOME"' '"star *"' '""' '"slash \\"'; do
  assert_contains "$TMP/special_trace.err" "$text" "trace renders special arg $text"
done

write_fixture "$FIX/vm_trace_flow.ds" <<'DS'
fn show(x) {
  echo "x={x}"
}

let xs = ["a", "b"]
for x in xs {
  show(x)
}
DS
capture_status vm_trace_flow env PATH="$FAKEBIN:$PATH" "$DS" run --trace-vm "$FIX/vm_trace_flow.ds"
assert_status vm_trace_flow 0
assert_contains "$TMP/vm_trace_flow.err" 'CALL' 'VM trace includes function call opcode'
assert_contains "$TMP/vm_trace_flow.err" 'FOR_ARRAY' 'VM trace includes loop opcode'
assert_contains "$TMP/vm_trace_flow.err" 'RUN_CMD' 'VM trace includes command opcode'

# Runtime diagnostics with source spans.
write_fixture "$FIX/args_diag.ds" <<'DS'
script {
  arg app: string
  option retries: int = 1
  flag force: bool = false
}

echo "app={app} retries={retries} force={force}"
DS
capture_status missing_arg_diag "$DS" run "$FIX/args_diag.ds"
assert_nonzero_status missing_arg_diag
assert_diag_span "$TMP/missing_arg_diag.err" "$FIX/args_diag.ds" 'missing required argument `app`' 'missing required arg source diagnostic'
capture_status invalid_int_diag "$DS" run "$FIX/args_diag.ds" api --retries nope
assert_nonzero_status invalid_int_diag
assert_diag_span "$TMP/invalid_int_diag.err" "$FIX/args_diag.ds" 'invalid int value `nope` for `retries`' 'invalid int source diagnostic'
capture_status missing_value_diag "$DS" run "$FIX/args_diag.ds" api --retries
assert_nonzero_status missing_value_diag
assert_diag_span "$TMP/missing_value_diag.err" "$FIX/args_diag.ds" 'option `--retries` requires a value' 'missing option value source diagnostic'
capture_status duplicate_diag "$DS" run "$FIX/args_diag.ds" api --force --force
assert_nonzero_status duplicate_diag
assert_diag_span "$TMP/duplicate_diag.err" "$FIX/args_diag.ds" 'duplicate option `--force`' 'duplicate flag source diagnostic'
capture_status extra_arg_diag "$DS" run "$FIX/args_diag.ds" api extra
assert_nonzero_status extra_arg_diag
assert_diag_span "$TMP/extra_arg_diag.err" "$FIX/args_diag.ds" 'unexpected extra positional argument `extra`' 'extra arg source diagnostic'

write_fixture "$FIX/file_read_missing.ds" <<'DS'
let text = file.read("missing.txt")
echo text
DS
capture_status file_read_missing bash -c "cd '$TMP' && '$DS' run '$FIX/file_read_missing.ds'"
assert_nonzero_status file_read_missing
assert_diag_span "$TMP/file_read_missing.err" "$FIX/file_read_missing.ds" 'failed to read file `missing.txt`' 'file.read source diagnostic'
write_fixture "$FIX/lines_missing.ds" <<'DS'
for line in lines("missing.txt") {
  echo line
}
DS
capture_status lines_missing bash -c "cd '$TMP' && '$DS' run '$FIX/lines_missing.ds'"
assert_nonzero_status lines_missing
assert_diag_span "$TMP/lines_missing.err" "$FIX/lines_missing.ds" 'failed to read lines from `missing.txt`' 'lines source diagnostic'
write_fixture "$FIX/glob_missing.ds" <<'DS'
for path in glob!("definitely-no-match-*.txt") {
  echo path
}
DS
capture_status glob_missing bash -c "cd '$TMP' && '$DS' run '$FIX/glob_missing.ds'"
assert_nonzero_status glob_missing
assert_diag_span "$TMP/glob_missing.err" "$FIX/glob_missing.ds" 'required glob `definitely-no-match-*.txt` had no matches' 'glob! source diagnostic'
write_fixture "$FIX/cmd_require_missing.ds" <<'DS'
cmd.require("definitely_missing_cmd_zzz")
DS
capture_status cmd_require_missing "$DS" run "$FIX/cmd_require_missing.ds"
assert_nonzero_status cmd_require_missing
assert_diag_span "$TMP/cmd_require_missing.err" "$FIX/cmd_require_missing.ds" 'required command `definitely_missing_cmd_zzz` was not found on PATH' 'cmd.require source diagnostic'
write_fixture "$FIX/file_write_missing.ds" <<'DS'
file.write("missing-dir/out.txt", "x")
DS
capture_status file_write_missing bash -c "cd '$TMP' && '$DS' run '$FIX/file_write_missing.ds'"
assert_nonzero_status file_write_missing
assert_diag_span "$TMP/file_write_missing.err" "$FIX/file_write_missing.ds" 'failed to write file `missing-dir/out.txt`' 'file.write source diagnostic'
write_fixture "$FIX/file_append_missing.ds" <<'DS'
file.append("missing-dir/out.txt", "x")
DS
capture_status file_append_missing bash -c "cd '$TMP' && '$DS' run '$FIX/file_append_missing.ds'"
assert_nonzero_status file_append_missing
assert_diag_span "$TMP/file_append_missing.err" "$FIX/file_append_missing.ds" 'failed to append file `missing-dir/out.txt`' 'file.append source diagnostic'

write_fixture "$FIX/missing_import_root.ds" <<'DS'
import "./missing_imported.ds"
DS
capture_status missing_import_diag "$DS" run "$FIX/missing_import_root.ds"
assert_nonzero_status missing_import_diag
assert_diag_span "$TMP/missing_import_diag.err" "$FIX/missing_import_root.ds" 'failed to open imported file' 'missing import source diagnostic'
write_fixture "$FIX/import_cycle_a.ds" <<'DS'
import "./import_cycle_b.ds"
DS
write_fixture "$FIX/import_cycle_b.ds" <<'DS'
import "./import_cycle_a.ds"
DS
capture_status import_cycle_diag "$DS" run "$FIX/import_cycle_a.ds"
assert_nonzero_status import_cycle_diag
assert_diag_span "$TMP/import_cycle_diag.err" "$FIX/import_cycle_b.ds" 'import cycle detected' 'import cycle source diagnostic'
write_fixture "$FIX/imported_fail.ds" <<'DS'
fn fail_imported() {
  fake-fail-now
}
DS
write_fixture "$FIX/imported_fail_root.ds" <<'DS'
import "./imported_fail.ds"
fail_imported()
DS
capture_status imported_runtime_fail env PATH="$FAKEBIN:$PATH" "$DS" run "$FIX/imported_fail_root.ds"
assert_status imported_runtime_fail 9
assert_diag_span "$TMP/imported_runtime_fail.err" "$FIX/imported_fail.ds" 'command `fake-fail-now` failed with exit 9' 'imported runtime failure source diagnostic'

write_fixture "$FIX/helper_collision.ds" <<'DS'
fn __ds_trace_cmd() {
  echo "user helper"
}

__ds_trace_cmd()
DS
run_ok helper_collision_emit "$DS" emit bash "$FIX/helper_collision.ds" -o "$TMP/helper_collision.sh"
run_ok helper_collision_syntax bash -n "$TMP/helper_collision.sh"
capture_status helper_collision_bash env DS_TRACE_CMD=1 bash "$TMP/helper_collision.sh"
assert_status helper_collision_bash 0
assert_contains "$TMP/helper_collision_bash.out" 'user helper' 'user __ds_ function does not collide with Bash helpers'

run_ok emit_cwd_trace "$DS" emit bash "$FIX/redir_trace.ds" -o "$TMP/redir_trace.sh"
run_ok bash_cwd_trace_syntax bash -n "$TMP/redir_trace.sh"
mkdir -p "$TMP/bash_cwd_trace"
capture_status bash_cwd_trace bash -c "cd '$TMP/bash_cwd_trace' && PATH='$FAKEBIN':\$PATH DS_TRACE_CMD=1 bash '$TMP/redir_trace.sh'"
assert_status bash_cwd_trace 0
assert_contains "$TMP/bash_cwd_trace.err" '> "out.txt"' 'Bash trace from different cwd shows relative stdout redirection'
assert_contains "$TMP/bash_cwd_trace.err" '&> "all.txt"' 'Bash trace from different cwd shows relative combined redirection'
[ -f "$TMP/bash_cwd_trace/out.txt" ] || fail 'Bash different cwd redirection created out.txt'
[ -f "$TMP/bash_cwd_trace/all.txt" ] || fail 'Bash different cwd redirection created all.txt'
pass 'Bash different cwd redirection artifacts created'

# Regression coverage proving old normal execution and Bash emission remain quiet/compatible.
run_ok old_check "$DS" check examples/basic.ds
run_ok old_run "$DS" run examples/basic.ds
run_ok old_direct "$DS" examples/basic.ds
run_ok old_emit "$DS" emit bash examples/basic.ds -o "$TMP/old_basic.sh"
run_ok old_bash_syntax bash -n "$TMP/old_basic.sh"
run_ok old_bash bash "$TMP/old_basic.sh"
assert_same "$TMP/old_run.out" "$TMP/old_direct.out" 'direct/run stdout parity remains'
assert_same "$TMP/old_run.out" "$TMP/old_bash.out" 'VM/Bash stdout parity remains'
assert_not_contains "$TMP/old_run.err" 'trace:' 'normal VM output has no trace'
assert_not_contains "$TMP/old_bash.err" 'trace:' 'normal Bash output has no trace'

printf 'v0.13 tests passed: %d assertions\n' "$pass_count"
