#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
if [[ "${DS_SKIP_BUILD:-0}" != "1" ]]; then make >/dev/null; fi
TMP=${TMPDIR:-/tmp}/ds_v0_27_tests.$$
mkdir -p "$TMP/fixtures"
trap 'rm -rf "$TMP"' EXIT
N=0
ok(){ N=$((N+1)); printf 'ok %d - %s\n' "$N" "$1"; }
fail(){ printf 'not ok %d - %s\n' "$((N+1))" "$1" >&2; exit 1; }
write_fixture(){ local path=$1; shift; cat >"$path"; }
assert_eq(){ local name=$1 exp=$2 act=$3; [[ "$exp" == "$act" ]] || { printf 'expected:\n%s\nactual:\n%s\n' "$exp" "$act" >&2; fail "$name"; }; ok "$name"; }
assert_contains(){ local name=$1 file=$2 needle=$3; grep -Fq "$needle" "$file" || { cat "$file" >&2; fail "$name"; }; ok "$name"; }
assert_no_contains(){ local name=$1 file=$2 needle=$3; ! grep -Fq "$needle" "$file" || { cat "$file" >&2; fail "$name"; }; ok "$name"; }

run_parity(){
  local name=$1 file=$2 expected=$3
  ./ds check "$file" >"$TMP/$name.check.out" 2>"$TMP/$name.check.err"; ok "$name check"
  ./ds emit bash "$file" -o "$TMP/$name.sh" >"$TMP/$name.emit.out" 2>"$TMP/$name.emit.err"; ok "$name emit"
  bash -n "$TMP/$name.sh"; ok "$name bash -n"
  assert_no_contains "$name emitted Bash standalone" "$TMP/$name.sh" './ds '
  ./ds run "$file" >"$TMP/$name.vm.out" 2>"$TMP/$name.vm.err"; local vm_rc=$?
  bash "$TMP/$name.sh" >"$TMP/$name.bash.out" 2>"$TMP/$name.bash.err"; local bash_rc=$?
  assert_eq "$name exit parity" "$vm_rc" "$bash_rc"
  assert_eq "$name stdout parity" "$(cat "$TMP/$name.vm.out")" "$(cat "$TMP/$name.bash.out")"
  assert_eq "$name stderr parity" "$(cat "$TMP/$name.vm.err")" "$(cat "$TMP/$name.bash.err")"
  assert_eq "$name expected stdout" "$expected" "$(cat "$TMP/$name.vm.out")"
}

write_fixture "$TMP/fixtures/scalar_command_interp.ds" <<'DS'
fn app() {
  return "api"
}
fn build() {
  return 42
}
fn enabled() {
  return true
}
echo "deploying {app()}-{build()}-{enabled()}"
printf "%s\n" "arg={app()}"
DS
run_parity scalar_command_interp "$TMP/fixtures/scalar_command_interp.ds" $'deploying api-42-true\narg=api'

write_fixture "$TMP/fixtures/quoted_literal_interp.ds" <<'DS'
fn literal() {
  return "semi; $(echo bad) *?[x]"
}
fn spaced() {
  return "a b c"
}
printf "%s\n" "{literal()}"
sh "-c" "printf '<%s>\n' \"$1\"" "_" "{spaced()}"
DS
run_parity quoted_literal_interp "$TMP/fixtures/quoted_literal_interp.ds" $'semi; $(echo bad) *?[x]\n<a b c>'

write_fixture "$TMP/fixtures/left_to_right_env.ds" <<'DS'
fn left() {
  env.DS_ORDER = "left"
  return env.DS_ORDER
}
fn right() {
  env.DS_ORDER = "right"
  return env.DS_ORDER
}
echo "{left()}:{right()}:{env.DS_ORDER}"
DS
run_parity left_to_right_env "$TMP/fixtures/left_to_right_env.ds" 'left:right:right'

write_fixture "$TMP/fixtures/unset_env.ds" <<'DS'
env.DS_V027_UNSET = "set"
echo "before={env.DS_V027_UNSET}"
unset env.DS_V027_UNSET
echo "after={env.DS_V027_UNSET}"
let result = run "printenv" "DS_V027_UNSET"
echo "status={result.status}"
echo "stdout={result.stdout}"
DS
run_parity unset_env "$TMP/fixtures/unset_env.ds" $'before=set\nafter=\nstatus=1\nstdout='

write_fixture "$TMP/fixtures/captured_run_interp.ds" <<'DS'
fn word() {
  return "hello"
}
let result = run "printf" "%s\n" "{word()}"
echo result.stdout
DS
run_parity captured_run_interp "$TMP/fixtures/captured_run_interp.ds" 'hello'

write_fixture "$TMP/fixtures/return_run_interp.ds" <<'DS'
fn word() {
  return "hello"
}
fn probe() {
  return run "printf" "%s" "x={word()}"
}
let result = probe()
echo result.stdout
DS
run_parity return_run_interp "$TMP/fixtures/return_run_interp.ds" 'x=hello'

write_fixture "$TMP/fixtures/trailing_newline_interp.ds" <<'DS'
fn block() {
  return """api
"""
}
printf "[%s]" "{block()}"
DS
run_parity trailing_newline_interp "$TMP/fixtures/trailing_newline_interp.ds" $'[api
]'

write_fixture "$TMP/fixtures/failing_interp.ds" <<'DS'
fn bad() {
  return 1 / 0
}
sh "-c" "echo launched > should_not_exist" "{bad()}"
DS
( cd "$TMP" && ! "$ROOT/ds" run "$TMP/fixtures/failing_interp.ds" >failing.vm.out 2>failing.vm.err ) || fail "failing interp VM fails"
ok "failing interp VM fails"
[[ ! -e "$TMP/should_not_exist" ]] || fail "failing interp VM prevents launch"
ok "failing interp VM prevents launch"
./ds emit bash "$TMP/fixtures/failing_interp.ds" -o "$TMP/failing.sh" >/dev/null
bash -n "$TMP/failing.sh"; ok "failing interp bash -n"
( cd "$TMP" && ! bash "$TMP/failing.sh" >failing.bash.out 2>failing.bash.err ) || fail "failing interp Bash fails"
ok "failing interp Bash fails"
[[ ! -e "$TMP/should_not_exist" ]] || fail "failing interp Bash prevents launch"
ok "failing interp Bash prevents launch"
assert_contains "failing interp VM real diagnostic" "$TMP/failing.vm.err" 'division or modulo by zero'
assert_contains "failing interp Bash real diagnostic" "$TMP/failing.bash.err" 'division or modulo by zero'
assert_no_contains "failing interp Bash avoids internal payload diagnostic" "$TMP/failing.bash.err" 'invalid internal int function return payload'

write_fixture "$TMP/fixtures/env_shadow_rejected.ds" <<'DS'
let env = "not-env"
echo env.DS_WHATEVER
DS
! ./ds check "$TMP/fixtures/env_shadow_rejected.ds" >"$TMP/env_shadow.out" 2>"$TMP/env_shadow.err" || fail "env namespace shadow rejected"
ok "env namespace shadow rejected"
assert_contains "env namespace shadow diagnostic" "$TMP/env_shadow.err" 'reserved environment namespace'

write_fixture "$TMP/fixtures/structured_interp_rejected.ds" <<'DS'
fn names() {
  return ["api", "web"]
}
echo "{names()}"
DS
! ./ds check "$TMP/fixtures/structured_interp_rejected.ds" >"$TMP/reject.out" 2>"$TMP/reject.err" || fail "structured interpolation rejected check"
ok "structured interpolation rejected check"
assert_contains "structured interpolation rejected diagnostic" "$TMP/reject.err" 'interpolation expression must be scalar'

assert_contains "status documents command interpolation" docs/status.md 'direct scalar value-returning function calls in quoted command'
assert_contains "runtime documents pre-materialization" docs/runtime.md 'pre-materializing each interpolated call'
assert_contains "language documents command interpolation" docs/language.ds 'quoted command words'
assert_contains "Makefile includes v0.27" Makefile '0-27'

printf 'v0.27.0 focused tests passed (%d assertions)\n' "$N"
