#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_24_tests.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != 1 ]]; then
  make -C "$ROOT" clean all >/dev/null
fi

cd "$ROOT"

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

assert_vm_bash_same() {
  local name="$1" fixture="$2" expected_status="$3"; shift 3
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
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  else
    pass "$name non-zero stderr checked by focused assertions when required"
  fi
}

run_and_signal() {
  local name="$1" mode="$2" signal="$3" fixture="$4" expected_status="$5" expected_stdout="$6"
  local out="$TMP/${name}.out"
  local err="$TMP/${name}.err"
  local rc_file="$TMP/${name}.rc"
  local script="$TMP/${name}.sh"
  local run_dir
  local -a cmd

  : >"$out"
  : >"$err"
  run_dir="$(dirname "$fixture")"

  case "$mode" in
    vm)
      cmd=("$DS" run "$fixture")
      ;;
    bash)
      run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
      run_ok "${name}_bash_n" bash -n "$script"
      assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
      cmd=(bash "$script")
      ;;
    *)
      fail "$name unknown signal harness mode: $mode"
      ;;
  esac

  set +e
  python3 - "$run_dir" "$out" "$err" "$rc_file" "$signal" "${cmd[@]}" <<'PYSIGNAL'
import os
import signal
import subprocess
import sys
import time

run_dir, out_path, err_path, rc_path, sig_name = sys.argv[1:6]
cmd = sys.argv[6:]
signum = getattr(signal, "SIG" + sig_name)

def preexec():
    os.setsid()
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    signal.signal(signal.SIGTERM, signal.SIG_DFL)

with open(out_path, "ab", buffering=0) as out, open(err_path, "ab", buffering=0) as err:
    proc = subprocess.Popen(cmd, cwd=run_dir, stdout=out, stderr=err, preexec_fn=preexec)

ready = False
for _ in range(200):
    try:
        with open(out_path, "r", encoding="utf-8", errors="replace") as f:
            if any(line.rstrip("\n") == "ready" for line in f):
                ready = True
                break
    except FileNotFoundError:
        pass
    if proc.poll() is not None:
        break
    time.sleep(0.05)

if not ready:
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    proc.wait(timeout=2)
    sys.exit(90)

try:
    os.killpg(proc.pid, signum)
except ProcessLookupError:
    sys.exit(91)

try:
    rc = proc.wait(timeout=10)
except subprocess.TimeoutExpired:
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    proc.wait(timeout=2)
    sys.exit(92)

if rc < 0:
    rc = 128 + (-rc)
with open(rc_path, "w", encoding="utf-8") as f:
    f.write(str(rc))
PYSIGNAL
  local harness_rc=$?
  set -e

  if [ "$harness_rc" -ne 0 ]; then
    echo "--- $out" >&2
    cat "$out" >&2 || true
    echo "--- $err" >&2
    cat "$err" >&2 || true
    case "$harness_rc" in
      90) fail "$name did not print ready before signal" ;;
      91) fail "$name could not signal process group" ;;
      92) fail "$name did not exit after $signal" ;;
      *) fail "$name signal harness failed with exit $harness_rc" ;;
    esac
  fi

  assert_status "$name" "$expected_status"
  assert_same_text "$expected_stdout" "$out" "$name stdout"
  assert_same_text '' "$err" "$name stderr"
}

assert_no_duplicate_helper_defs() {
  local script="$1" name="$2"
  local defs dupes
  defs="$TMP/${name}_defs.txt"
  dupes="$TMP/${name}_dupes.txt"
  grep -E '^(__ds_[A-Za-z0-9_]+)[[:space:]]*\(\)[[:space:]]*\{' "$script" | sed -E 's/^(__ds_[A-Za-z0-9_]+).*/\1/' | sort >"$defs" || true
  uniq -d "$defs" >"$dupes"
  [ ! -s "$dupes" ] || {
    cat "$dupes" >&2
    fail "$name: duplicate __ds helper definitions"
  }
  pass "$name duplicate helper audit"
}

assert_all_helper_defs_namespaced() {
  local script="$1" name="$2"
  if grep -E '^[A-Za-z_][A-Za-z0-9_]+[[:space:]]*\(\)[[:space:]]*\{' "$script" | grep -Ev '^__ds_' >/dev/null; then
    grep -E '^[A-Za-z_][A-Za-z0-9_]+[[:space:]]*\(\)[[:space:]]*\{' "$script" >&2 || true
    fail "$name: found non-__ds helper definition"
  fi
  pass "$name helper namespace audit"
}

FIX="$TMP/fixtures with spaces"
mkdir -p "$FIX"

assert_contains Makefile '0-24' 'TEST_VERSIONS contains v0.24'

# 2. Examples audit.
for example in \
  examples/command-result.ds \
  examples/functions.ds \
  examples/control-flow.ds \
  examples/pipeline.ds \
  examples/strings.ds \
  examples/function-values.ds; do
  name="example_$(basename "$example" .ds | tr '-' '_')"
  run_ok "${name}_check" "$DS" check "$example"
  run_ok "${name}_fmt_check" "$DS" fmt --check "$example"
  assert_vm_bash_same "$name" "$ROOT/$example" 0
done
for example in \
  examples/basic.ds \
  examples/collections.ds \
  examples/filtering.ds \
  examples/vm.ds; do
  name="example_$(basename "$example" .ds | tr '-' '_')"
  run_ok "${name}_check" "$DS" check "$example"
  assert_vm_bash_same "$name" "$ROOT/$example" 0
done
run_ok example_import_main_check "$DS" check examples/import-main.ds
run_ok example_import_main_fmt_check "$DS" fmt --check examples/import-main.ds
assert_vm_bash_same example_import_main "$ROOT/examples/import-main.ds" 0
run_ok example_import_lib_check "$DS" check examples/import-lib.ds
run_ok example_import_lib_fmt_check "$DS" fmt --check examples/import-lib.ds
assert_vm_bash_same example_import_lib "$ROOT/examples/import-lib.ds" 0
run_ok example_args_check "$DS" check examples/args.ds
run_ok example_args_fmt_check "$DS" fmt --check examples/args.ds
assert_parity example_args "$ROOT/examples/args.ds" 0 $'Deploying api to prod\nretries=2\nforce enabled\n' api --target prod --retries 2 --force
run_ok example_redirection_check "$DS" check examples/redirection.ds
run_ok example_redirection_fmt_check "$DS" fmt --check examples/redirection.ds
redirection="$ROOT/examples/redirection.ds"
assert_parity example_redirection "$redirection" 0 $'build output written to build.log\n'
run_ok example_stdlib_check "$DS" check examples/stdlib.ds
assert_vm_bash_same example_stdlib "$ROOT/examples/stdlib.ds" 0
run_fail example_bad_check "$DS" check examples/bad.ds
assert_diag "$TMP/example_bad_check.err" 'expected expression' 'bad example diagnostic'
run_fail example_bad_emit "$DS" emit bash examples/bad.ds -o "$TMP/bad.sh"
assert_file_missing_or_empty "$TMP/bad.sh" 'bad example no partial Bash'

# 3. Broad VM/Bash parity smoke script.
broad="$FIX/broad.ds"
write_fixture "$broad" <<'DS'
script {
  arg app: string
  option target: string = "staging"
  flag force: bool = false
}

let allowed = ["api", "web", "worker"]
if !(app in allowed) {
  fail "unknown app"
}

let labels = "api,web,worker".split(",")
let seen = false
for idx in 0..2 {
  if labels[idx] == app {
    seen = true
  }
}

if !seen {
  fail "missing label"
}

fn suffix(name = "api") {
  if name matches /^a/ {
    return "alpha"
  }
  return "other"
}

let result = run printf "deploy:%s:%s:%s\n" $app $target $force
if result.failed {
  fail "printf failed"
}

let kind = suffix(app)
echo "{result.stdout:trim}:{kind}"
DS
assert_parity broad "$broad" 0 $'deploy:api:production:true:alpha\n' api --target production --force

# 4. Imports, functions, ranges, membership, and regex.
cat >"$FIX/lib.ds" <<'DS'
fn valid_app(name = "api") {
  return (name == "api") || (name == "web")
}

fn release_branch(branch = "release/1") {
  return branch matches /^release\/[0-9]+$/
}
DS
imports="$FIX/main.ds"
cat >"$imports" <<'DS'
import "./lib.ds"

let count = 0
for n in 1..3 {
  count += n
}

if valid_app("web") && release_branch("release/24") && count == 6 {
  echo ok
} else {
  echo bad
}
DS
run_ok import_check "$DS" check "$imports"
run_ok import_hir "$DS" hir "$imports"
run_ok import_bytecode "$DS" bytecode "$imports"
assert_parity imports_cross_feature "$imports" 0 $'ok\n'

# 5. Cleanup handlers with cross-feature bodies.
cleanup="$FIX/cleanup.ds"
write_fixture "$cleanup" <<'DS'
defer {
  let nums = [1, 2, 3]
  if 2 in nums {
    echo "exit-clean"
  }
}

defer on: "TERM" {
  if "release/9" matches /^release\/[0-9]+$/ {
    echo "term-clean"
  }
}

for n in 1..2 {
  echo "n={n}"
}
DS
assert_parity cleanup_normal "$cleanup" 0 $'n=1\nn=2\nexit-clean\n'
cleanup_script="$TMP/cleanup.sh"
run_ok cleanup_emit "$DS" emit bash "$cleanup" -o "$cleanup_script"
run_ok cleanup_bash_n bash -n "$cleanup_script"
assert_contains "$cleanup_script" 'trap' 'cleanup Bash contains traps'

# Reuse the deterministic v0.22 signal-harness shape to keep v0.24 honest
# about foreground direct commands and simple foreground pipelines.
cat >"$FIX/ready_exec_sleep" <<'SH'
#!/usr/bin/env bash
echo ready
exec sleep 5
SH
chmod +x "$FIX/ready_exec_sleep"

signal_direct="$FIX/signal_direct.ds"
write_fixture "$signal_direct" <<'DS'
trap "TERM" { echo term-trap }
defer on: "TERM" { echo term-defer }
trap "INT" { echo int-trap }
defer on: "INT" { echo int-defer }
trap "EXIT" { echo exit-trap }
defer { echo exit-defer }
./ready_exec_sleep
echo after
DS
run_and_signal signal_vm_term_direct_v0_24 vm TERM "$signal_direct" 143 $'ready\nterm-trap\nterm-defer\nexit-trap\nexit-defer\n'
run_and_signal signal_bash_term_direct_v0_24 bash TERM "$signal_direct" 143 $'ready\nterm-trap\nterm-defer\nexit-trap\nexit-defer\n'
run_and_signal signal_vm_int_direct_v0_24 vm INT "$signal_direct" 130 $'ready\nint-trap\nint-defer\nexit-trap\nexit-defer\n'
run_and_signal signal_bash_int_direct_v0_24 bash INT "$signal_direct" 130 $'ready\nint-trap\nint-defer\nexit-trap\nexit-defer\n'

signal_pipeline="$FIX/signal_pipeline.ds"
write_fixture "$signal_pipeline" <<'DS'
trap "TERM" { echo term-trap }
defer on: "TERM" { echo term-defer }
trap "INT" { echo int-trap }
defer on: "INT" { echo int-defer }
trap "EXIT" { echo exit-trap }
defer { echo exit-defer }
./ready_exec_sleep | cat
echo after
DS
run_and_signal signal_vm_term_pipeline_v0_24 vm TERM "$signal_pipeline" 143 $'ready\nterm-trap\nterm-defer\nexit-trap\nexit-defer\n'
run_and_signal signal_bash_term_pipeline_v0_24 bash TERM "$signal_pipeline" 143 $'ready\nterm-trap\nterm-defer\nexit-trap\nexit-defer\n'
run_and_signal signal_vm_int_pipeline_v0_24 vm INT "$signal_pipeline" 130 $'ready\nint-trap\nint-defer\nexit-trap\nexit-defer\n'
run_and_signal signal_bash_int_pipeline_v0_24 bash INT "$signal_pipeline" 130 $'ready\nint-trap\nint-defer\nexit-trap\nexit-defer\n'

# 6. Test-runner integration.
test_blocks="$FIX/test_blocks.ds"
write_fixture "$test_blocks" <<'DS'
test "cross feature assertions" {
  let app = "api"
  assert app in ["api", "web"]
  assert "release/1" matches /^release\/[0-9]+$/

  let total = 0
  for n in 1..3 {
    total += n
  }
  assert total == 6
}
DS
run_ok test_runner_cross_features "$DS" test "$test_blocks"
assert_parity test_blocks_production "$test_blocks" 0 ''
failing_test="$FIX/failing_test.ds"
write_fixture "$failing_test" <<'DS'
test "clear failure" {
  assert "api" in ["web"]
}
DS
run_fail test_runner_failure "$DS" test "$failing_test"
assert_contains "$TMP/test_runner_failure.err" 'assertion failed' 'failing assertion is clear'

# 7. Generated Bash helper audit.
helper="$FIX/helper_audit.ds"
write_fixture "$helper" <<'DS'
import "./lib.ds"
let apps = ["api", "web"]
let ports = { api: 3000, web: 8080 }
let trimmed = " api ".trim()
let parts = "api,web".split(",")
let result = run printf "%s\n" $trimmed | grep api
let port = ports.api
fn ok(name = "api") {
  return ((name == "api") || (name == "web")) && (name matches /^a/)
}
if ok(parts[0]) && result.ok {
  echo "{port}:{result.stdout:trim}"
}
for n in 1..2 {
  echo "n={n}"
}
defer { echo done }
test "not emitted as runner" { assert ok("api") }
DS
helper_script="$TMP/helper_audit.sh"
run_ok helper_emit "$DS" emit bash "$helper" -o "$helper_script"
run_ok helper_bash_n bash -n "$helper_script"
assert_not_matches "$helper_script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' 'helper audit Bash does not call ds'
assert_all_helper_defs_namespaced "$helper_script" 'helper audit'
assert_no_duplicate_helper_defs "$helper_script" 'helper audit'
assert_matches "$helper_script" 'BASH_VERSINFO|Bash 4' 'helper audit has Bash version guard'
run_ok helper_emit_repeat "$DS" emit bash "$helper" -o "$TMP/helper_audit_repeat.sh"
assert_same "$helper_script" "$TMP/helper_audit_repeat.sh" 'helper emission deterministic'
minimal="$FIX/minimal.ds"
write_fixture "$minimal" <<'DS'
echo ok
DS
minimal_script="$TMP/minimal.sh"
run_ok minimal_emit "$DS" emit bash "$minimal" -o "$minimal_script"
assert_not_contains "$minimal_script" '__ds_membership' 'minimal Bash omits membership helper'
assert_not_contains "$minimal_script" '__ds_regex' 'minimal Bash omits regex helper'

# 8. Formatter/checker regression audit.
fmt_dirty="$FIX/fmt_dirty.ds"
write_fixture "$fmt_dirty" <<'DS'
let ok=app in["api","web"]
for n in 1+1..3+1{echo "{n}"}
if "release/1" matches/^release\/[0-9]+$/{echo ok}
DS
fmt_expected="$TMP/fmt_expected.ds"
cat >"$fmt_expected" <<'DS'
let ok = app in ["api", "web"]

for n in 1 + 1..3 + 1 {
  echo "{n}"
}

if "release/1" matches /^release\/[0-9]+$/ {
  echo ok
}
DS
run_ok fmt_dirty "$DS" fmt "$fmt_dirty"
assert_same "$fmt_expected" "$TMP/fmt_dirty.out" 'formatter v0.24 syntax style'
run_fail fmt_dirty_check "$DS" fmt --check "$fmt_dirty"
fmt_clean="$FIX/fmt_clean.ds"
cp "$TMP/fmt_dirty.out" "$fmt_clean"
run_ok fmt_clean_check "$DS" fmt --check "$fmt_clean"
warn="$FIX/warn.ds"
write_fixture "$warn" <<'DS'
let unused = 1
echo ok
DS
run_ok check_warn_default "$DS" check "$warn"
assert_contains "$TMP/check_warn_default.err" 'warning:' 'checker emits deterministic warning'
run_fail check_warn_as_error "$DS" check --warnings-as-errors "$warn"
assert_contains "$TMP/check_warn_as_error.err" 'warning:' 'warnings-as-errors reports warning'
run_ok check_no_warnings "$DS" check --no-warnings "$warn"
assert_file_missing_or_empty "$TMP/check_no_warnings.err" 'no-warnings suppresses warning'
run_fail check_conflicting_warning_flags "$DS" check --warnings-as-errors --no-warnings "$warn"
no_exec="$FIX/check_no_exec.ds"
write_fixture "$no_exec" <<'DS'
sh -c "printf bad > should_not_exist"
DS
run_ok check_does_not_execute "$DS" check "$no_exec"
[ ! -e should_not_exist ] || fail 'check executed user command'
pass 'check does not execute user commands'

regex_runtime_ok="$FIX/regex_runtime_ok.ds"
write_fixture "$regex_runtime_ok" <<'DS'
let pattern = "^abc$"
let ok = "abc" matches pattern
echo $ok
DS
assert_parity regex_runtime_ok "$regex_runtime_ok" 0 $'true\n'

# 9. Diagnostics for unsupported/deferred constructs.
for item in \
  'range_value|let x = 1..3|range' \
  'range_step|for n in 1..10..2 { echo "{n}" }|range' \
  'member_map|let ok = "api" in { api: true }|right operand' \
  'regex_named_capture|let ok = "abc" matches /(?<name>a)/|deferred' \
  'shell_and|cmd1 && cmd2|not supported' \
  'process_sub|cat <(printf hi)|unsupported' \
  'heredoc|cat <<EOF\nhello\nEOF|unsupported' \
  'defer_hup|defer on: "HUP" {\n  echo hup\n}|unsupported defer on: signal' \
  'map_destructure|for key, value in { api: 1 } {\n  echo "{key}={value}"\n}|temporary map literals are not supported as map loop iterables'; do
  IFS='|' read -r name src frag <<<"$item"
  f="$FIX/$name.ds"
  printf '%b\n' "$src" >"$f"
  assert_check_fails "$name" "$f" "$frag"
  assert_emit_fails "${name}_emit" "$f" "$frag"
done

# 10. Arithmetic and runtime error parity.
div_zero="$FIX/div_zero.ds"
write_fixture "$div_zero" <<'DS'
defer { echo cleanup }
let x = 1 / 0
echo "x={x}"
DS
assert_parity div_zero "$div_zero" 1 $'cleanup\n'
assert_contains "$TMP/div_zero_vm.err" 'division or modulo by zero' 'VM div-zero diagnostic text'
assert_contains "$TMP/div_zero_vm.err" ': error:' 'VM div-zero diagnostic shape'
assert_contains "$TMP/div_zero_vm.err" '^' 'VM div-zero diagnostic caret'
assert_contains "$TMP/div_zero_bash.err" 'division or modulo by zero' 'Bash div-zero diagnostic text'
assert_contains "$TMP/div_zero_bash.err" ': error:' 'Bash div-zero diagnostic shape'
mod_zero="$FIX/mod_zero.ds"
write_fixture "$mod_zero" <<'DS'
defer { echo cleanup }
let x = 1 % 0
echo "x={x}"
DS
assert_parity mod_zero "$mod_zero" 1 $'cleanup\n'
assert_contains "$TMP/mod_zero_vm.err" 'division or modulo by zero' 'VM mod-zero diagnostic text'
assert_contains "$TMP/mod_zero_vm.err" ': error:' 'VM mod-zero diagnostic shape'
assert_contains "$TMP/mod_zero_vm.err" '^' 'VM mod-zero diagnostic caret'
assert_contains "$TMP/mod_zero_bash.err" 'division or modulo by zero' 'Bash mod-zero diagnostic text'
assert_contains "$TMP/mod_zero_bash.err" ': error:' 'Bash mod-zero diagnostic shape'
missing_return="$FIX/missing_return.ds"
write_fixture "$missing_return" <<'DS'
fn bad(flag = true) {
  if flag {
    return 1
  }
}
let value = bad(false)
echo "{value}"
DS
assert_check_fails missing_return "$missing_return" 'return'
streaming_call="$FIX/streaming_call.ds"
write_fixture "$streaming_call" <<'DS'
fn noisy() {
  echo noise
  return 1
}
let value = noisy()
echo "value={value}"
DS
assert_check_fails streaming_call "$streaming_call" 'plain command statements'

# 11. Command, pipeline, redirection, and capture parity.
cmds="$FIX/commands.ds"
write_fixture "$cmds" <<'DS'
let ok = run printf "plain\n"
echo "ok={ok.stdout:trim}:{ok.code}"
let failed = run sh -c "printf err >&2; exit 7"
echo "failed={failed.failed}:{failed.code}:{failed.stderr}"
let pipe = run printf "b\na\n" | sort | grep a
echo "pipe={pipe.stdout:trim}:{pipe.code}"
printf "side" |> "side.txt"
let captured = run cat "side.txt"
echo "side={captured.stdout:trim}"
DS
assert_parity commands "$cmds" 0 $'ok=plain:0\nfailed=true:7:err\npipe=a:0\nside=side\n'
plain_fail="$FIX/plain_fail.ds"
write_fixture "$plain_fail" <<'DS'
defer { echo cleanup }
sh -c "printf fail >&2; exit 4"
echo after
DS
assert_parity plain_fail "$plain_fail" 4 $'cleanup\n'
assert_contains "$TMP/plain_fail_vm.err" 'fail' 'VM plain failure preserves command stderr'
assert_contains "$TMP/plain_fail_vm.err" 'error: command' 'VM plain failure diagnostic shape'
assert_contains "$TMP/plain_fail_vm.err" '^' 'VM plain failure diagnostic caret'
assert_contains "$TMP/plain_fail_bash.err" 'fail' 'Bash plain failure preserves command stderr'
assert_contains "$TMP/plain_fail_bash.err" 'error: command' 'Bash plain failure diagnostic shape'

# 12. Cleanup and signal static regressions.
cleanup_order="$FIX/cleanup_order.ds"
write_fixture "$cleanup_order" <<'DS'
defer { echo first }
defer { echo second }
trap "EXIT" { echo replacement }
DS
assert_parity cleanup_order "$cleanup_order" 0 $'replacement\nsecond\nfirst\n'
signal_bad="$FIX/signal_bad.ds"
write_fixture "$signal_bad" <<'DS'
defer on: "" { echo bad }
DS
assert_check_fails signal_bad "$signal_bad" 'signal'
function_local_cleanup="$FIX/function_local_cleanup.ds"
write_fixture "$function_local_cleanup" <<'DS'
fn bad() {
  let local = "x"
  defer { echo "{local}" }
}
bad()
DS
assert_check_fails function_local_cleanup "$function_local_cleanup" 'function'

# 13. Sanitizer-value fixtures are included in this suite for aggregate sanitizer targets.
san="$FIX/sanitizer_surface.ds"
write_fixture "$san" <<'DS'
import "./lib.ds"
let xs = ["api", "web"]
let ports = { api: 3000 }
let api_port = ports.api
for app in xs {
  if app in xs && app matches /^[a-z]+$/ {
    echo "{app}:{api_port}"
  }
}
let r = run printf "api\nweb\n"
for line in r.stdout.trim().split("\n") {
  echo $line
}
DS
assert_parity sanitizer_surface "$san" 0 $'api:3000\nweb:3000\napi\nweb\n'

printf 'v0.24 tests passed (%s assertions)\n' "$pass_count"
