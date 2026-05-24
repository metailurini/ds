#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_22_signal_runtime.$$"
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

FIX="$TMP/fixtures with spaces"
mkdir -p "$FIX"

cat >"$FIX/ready_exec_sleep" <<'SH_CHILD'
#!/usr/bin/env bash
echo ready
exec sleep 5
SH_CHILD
chmod +x "$FIX/ready_exec_sleep"

write_fixture "$FIX/term_direct_runtime.ds" <<'DS'
trap "TERM" {
  echo term-trap
}

defer on: "TERM" {
  echo term-defer-first
}

defer on: "TERM" {
  echo term-defer-second
}

trap "EXIT" {
  echo exit-trap
}

defer {
  echo exit-defer-first
}

defer {
  echo exit-defer-second
}

./ready_exec_sleep
echo after
DS
run_and_signal signal_vm_term_direct_runtime vm TERM "$FIX/term_direct_runtime.ds" 143 $'ready\nterm-trap\nterm-defer-second\nterm-defer-first\nexit-trap\nexit-defer-second\nexit-defer-first\n'
run_and_signal signal_bash_term_direct_runtime bash TERM "$FIX/term_direct_runtime.ds" 143 $'ready\nterm-trap\nterm-defer-second\nterm-defer-first\nexit-trap\nexit-defer-second\nexit-defer-first\n'

write_fixture "$FIX/int_direct_runtime.ds" <<'DS'
trap "INT" {
  echo int-trap
}

defer on: "INT" {
  echo int-defer-first
}

defer on: "INT" {
  echo int-defer-second
}

trap "EXIT" {
  echo exit-trap
}

defer {
  echo exit-defer-first
}

defer {
  echo exit-defer-second
}

./ready_exec_sleep
echo after
DS
run_and_signal signal_vm_int_direct_runtime vm INT "$FIX/int_direct_runtime.ds" 130 $'ready\nint-trap\nint-defer-second\nint-defer-first\nexit-trap\nexit-defer-second\nexit-defer-first\n'
run_and_signal signal_bash_int_direct_runtime bash INT "$FIX/int_direct_runtime.ds" 130 $'ready\nint-trap\nint-defer-second\nint-defer-first\nexit-trap\nexit-defer-second\nexit-defer-first\n'

write_fixture "$FIX/term_pipeline_runtime.ds" <<'DS'
trap "TERM" {
  echo term-trap
}

defer on: "TERM" {
  echo term-defer-first
}

defer on: "TERM" {
  echo term-defer-second
}

trap "EXIT" {
  echo exit-trap
}

defer {
  echo exit-defer-first
}

defer {
  echo exit-defer-second
}

./ready_exec_sleep | cat
echo after
DS
run_and_signal signal_vm_term_pipeline_runtime vm TERM "$FIX/term_pipeline_runtime.ds" 143 $'ready\nterm-trap\nterm-defer-second\nterm-defer-first\nexit-trap\nexit-defer-second\nexit-defer-first\n'
run_and_signal signal_bash_term_pipeline_runtime bash TERM "$FIX/term_pipeline_runtime.ds" 143 $'ready\nterm-trap\nterm-defer-second\nterm-defer-first\nexit-trap\nexit-defer-second\nexit-defer-first\n'

write_fixture "$FIX/int_pipeline_runtime.ds" <<'DS'
trap "INT" {
  echo int-trap
}

defer on: "INT" {
  echo int-defer-first
}

defer on: "INT" {
  echo int-defer-second
}

trap "EXIT" {
  echo exit-trap
}

defer {
  echo exit-defer-first
}

defer {
  echo exit-defer-second
}

./ready_exec_sleep | cat
echo after
DS
run_and_signal signal_vm_int_pipeline_runtime vm INT "$FIX/int_pipeline_runtime.ds" 130 $'ready\nint-trap\nint-defer-second\nint-defer-first\nexit-trap\nexit-defer-second\nexit-defer-first\n'
run_and_signal signal_bash_int_pipeline_runtime bash INT "$FIX/int_pipeline_runtime.ds" 130 $'ready\nint-trap\nint-defer-second\nint-defer-first\nexit-trap\nexit-defer-second\nexit-defer-first\n'

printf 'v0.22 signal runtime focused tests passed (%d checks)\n' "$pass_count"
