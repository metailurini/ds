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

run_parity_env(){
  local name=$1 file=$2 expected=$3
  shift 3
  local env_args=("PATH=$PATH" "$@")
  ./ds check "$file" >"$TMP/$name.check.out" 2>"$TMP/$name.check.err"; ok "$name check"
  ./ds emit bash "$file" -o "$TMP/$name.sh" >"$TMP/$name.emit.out" 2>"$TMP/$name.emit.err"; ok "$name emit"
  bash -n "$TMP/$name.sh"; ok "$name bash -n"
  assert_no_contains "$name emitted Bash standalone" "$TMP/$name.sh" './ds '
  env -i "${env_args[@]}" ./ds run "$file" >"$TMP/$name.vm.out" 2>"$TMP/$name.vm.err"; local vm_rc=$?
  env -i "${env_args[@]}" bash "$TMP/$name.sh" >"$TMP/$name.bash.out" 2>"$TMP/$name.bash.err"; local bash_rc=$?
  assert_eq "$name exit parity" "$vm_rc" "$bash_rc"
  assert_eq "$name stdout parity" "$(cat "$TMP/$name.vm.out")" "$(cat "$TMP/$name.bash.out")"
  assert_eq "$name stderr parity" "$(cat "$TMP/$name.vm.err")" "$(cat "$TMP/$name.bash.err")"
  assert_eq "$name expected stdout" "$expected" "$(cat "$TMP/$name.vm.out")"
}

assert_reject(){
  local name=$1 file=$2 needle=$3
  ! ./ds check "$file" >"$TMP/$name.check.out" 2>"$TMP/$name.check.err" || fail "$name check rejects"
  ok "$name check rejects"
  assert_contains "$name check diagnostic" "$TMP/$name.check.err" "$needle"
  ! ./ds emit bash "$file" -o "$TMP/$name.sh" >"$TMP/$name.emit.out" 2>"$TMP/$name.emit.err" || fail "$name emit rejects"
  ok "$name emit rejects"
  assert_contains "$name emit diagnostic" "$TMP/$name.emit.err" "$needle"
}

assert_reject_all(){
  local name=$1 file=$2 needle=$3
  assert_reject "$name" "$file" "$needle"
  ! ./ds run "$file" >"$TMP/$name.run.out" 2>"$TMP/$name.run.err" || fail "$name run rejects"
  ok "$name run rejects"
  assert_contains "$name run diagnostic" "$TMP/$name.run.err" "$needle"
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

write_fixture "$TMP/fixtures/materialized_run_binding_scope.ds" <<'DS'
fn word(value = "hello") {
  return value.upper()
}
let first = run "printf" "%s" "{word()}"
let second = run "printf" "%s" "{word("world")}"
echo "{first.stdout}:{second.stdout}"
DS
run_parity materialized_run_binding_scope "$TMP/fixtures/materialized_run_binding_scope.ds" 'HELLO:WORLD'

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

write_fixture "$TMP/fixtures/env_present.ds" <<'DS'
echo env.DS_TEST_ENV
DS
run_parity_env env_present "$TMP/fixtures/env_present.ds" 'present' DS_TEST_ENV=present

write_fixture "$TMP/fixtures/env_runtime_read.ds" <<'DS'
echo env.DS_TEST_ENV
DS
./ds check "$TMP/fixtures/env_runtime_read.ds" >"$TMP/env_runtime.check.out" 2>"$TMP/env_runtime.check.err"; ok "env runtime read check"
env -i PATH="$PATH" DS_TEST_ENV=emit-time ./ds emit bash "$TMP/fixtures/env_runtime_read.ds" -o "$TMP/env_runtime.sh" >"$TMP/env_runtime.emit.out" 2>"$TMP/env_runtime.emit.err"; ok "env runtime read emit"
bash -n "$TMP/env_runtime.sh"; ok "env runtime read bash -n"
env -i PATH="$PATH" DS_TEST_ENV=first bash "$TMP/env_runtime.sh" >"$TMP/env_runtime.first.out" 2>"$TMP/env_runtime.first.err"
assert_eq "env runtime first output" 'first' "$(cat "$TMP/env_runtime.first.out")"
env -i PATH="$PATH" DS_TEST_ENV=second bash "$TMP/env_runtime.sh" >"$TMP/env_runtime.second.out" 2>"$TMP/env_runtime.second.err"
assert_eq "env runtime second output" 'second' "$(cat "$TMP/env_runtime.second.out")"
assert_no_contains "env runtime emitted Bash standalone" "$TMP/env_runtime.sh" './ds '

write_fixture "$TMP/fixtures/env_empty.ds" <<'DS'
echo "[{env.DS_TEST_ENV}]"
DS
run_parity_env env_empty "$TMP/fixtures/env_empty.ds" '[]' DS_TEST_ENV=

write_fixture "$TMP/fixtures/env_missing.ds" <<'DS'
echo "[{env.DS_TEST_MISSING}]"
DS
run_parity_env env_missing "$TMP/fixtures/env_missing.ds" '[]'

write_fixture "$TMP/fixtures/env_conditional.ds" <<'DS'
let mode = env.DS_MODE
if mode == "prod" {
  echo "production"
} else {
  echo "other"
}
DS
run_parity_env env_conditional "$TMP/fixtures/env_conditional.ds" 'production' DS_MODE=prod

write_fixture "$TMP/fixtures/env_function_read.ds" <<'DS'
fn region() {
  return env.DS_REGION
}
let r = region()
echo "{r}"
DS
run_parity_env env_function_read "$TMP/fixtures/env_function_read.ds" 'apac' DS_REGION=apac

write_fixture "$TMP/fixtures/env_assignment_scalars.ds" <<'DS'
env.DS_APP = "api"
echo env.DS_APP
env.DS_RETRIES = 3
echo env.DS_RETRIES
sh "-c" "printf '%s\n' \"$DS_RETRIES\""
env.DS_DEBUG = true
echo env.DS_DEBUG
sh "-c" "printf '%s\n' \"$DS_DEBUG\""
DS
run_parity env_assignment_scalars "$TMP/fixtures/env_assignment_scalars.ds" $'api\n3\n3\ntrue\ntrue'

write_fixture "$TMP/fixtures/env_override_ordering.ds" <<'DS'
echo "before={env.DS_APP}"
env.DS_APP = "new"
echo "after={env.DS_APP}"
env.DS_ORDER = "one"
echo env.DS_ORDER
env.DS_ORDER = "two"
echo env.DS_ORDER
DS
run_parity_env env_override_ordering "$TMP/fixtures/env_override_ordering.ds" $'before=old\nafter=new\none\ntwo' DS_APP=old

write_fixture "$TMP/fixtures/env_literal_empty_function.ds" <<'DS'
env.DS_LITERAL = "semi; $(echo bad) *?[x]"
echo env.DS_LITERAL
sh "-c" "printf '%s\n' \"$DS_LITERAL\""
env.DS_EMPTY = ""
echo "[{env.DS_EMPTY}]"
sh "-c" "printf '[%s]\n' \"$DS_EMPTY\""
fn configure() {
  env.DS_FROM_FN = "configured"
}
configure()
echo env.DS_FROM_FN
sh "-c" "printf '%s\n' \"$DS_FROM_FN\""
DS
run_parity env_literal_empty_function "$TMP/fixtures/env_literal_empty_function.ds" $'semi; $(echo bad) *?[x]\nsemi; $(echo bad) *?[x]\n[]\n[]\nconfigured\nconfigured'

write_fixture "$TMP/fixtures/unset_child_only.ds" <<'DS'
env.DS_TEMP = "set"
unset env.DS_TEMP
sh "-c" "printenv DS_TEMP >/dev/null; if [ $? -eq 1 ]; then echo absent; else echo present; fi"
DS
run_parity unset_child_only "$TMP/fixtures/unset_child_only.ds" 'absent'

write_fixture "$TMP/fixtures/bad_unset_target.ds" <<'DS'
unset DS_TEMP
DS
assert_reject bad_unset_target "$TMP/fixtures/bad_unset_target.ds" 'unset requires an environment target'

write_fixture "$TMP/fixtures/interp_empty_multi.ds" <<'DS'
env.DS_COUNT = 0
fn empty() {
  return ""
}
fn tick() {
  if env.DS_COUNT == "0" {
    env.DS_COUNT = 1
  } else {
    env.DS_COUNT = 2
  }
  return env.DS_COUNT
}
echo "before{empty()}after"
printf "[%s]\n" "{empty()}"
echo "{tick()}-{tick()}-{env.DS_COUNT}"
DS
run_parity interp_empty_multi "$TMP/fixtures/interp_empty_multi.ds" $'beforeafter\n[]\n1-2-2'

cat >"$TMP/fixtures/v027_mod.ds" <<'DS'
fn app() {
  return "api"
}
DS
write_fixture "$TMP/fixtures/imported_interp.ds" <<'DS'
import "./v027_mod.ds"
echo "app={app()}"
DS
run_parity imported_interp "$TMP/fixtures/imported_interp.ds" 'app=api'

write_fixture "$TMP/fixtures/failing_short_circuit.ds" <<'DS'
fn bad() {
  return 1 / 0
}
fn side_effect() {
  env.DS_SHOULD_NOT_SET = "set"
  return "x"
}
echo "{bad()} {side_effect()}"
echo env.DS_SHOULD_NOT_SET
DS
( cd "$TMP" && ! "$ROOT/ds" run "$TMP/fixtures/failing_short_circuit.ds" >short.vm.out 2>short.vm.err ) || fail "failing short-circuit VM fails"
ok "failing short-circuit VM fails"
./ds emit bash "$TMP/fixtures/failing_short_circuit.ds" -o "$TMP/short.sh" >/dev/null
bash -n "$TMP/short.sh"; ok "failing short-circuit bash -n"
( cd "$TMP" && ! bash "$TMP/short.sh" >short.bash.out 2>short.bash.err ) || fail "failing short-circuit Bash fails"
ok "failing short-circuit Bash fails"
assert_eq "failing short-circuit stdout parity" "$(cat "$TMP/short.vm.out")" "$(cat "$TMP/short.bash.out")"
assert_contains "failing short-circuit VM real diagnostic" "$TMP/short.vm.err" 'division or modulo by zero'
assert_contains "failing short-circuit Bash real diagnostic" "$TMP/short.bash.err" 'division or modulo by zero'
assert_no_contains "failing short-circuit skipped later interpolation" "$TMP/short.vm.out" 'set'

write_fixture "$TMP/fixtures/bad_env_name.ds" <<'DS'
echo env.1BAD
DS
assert_reject bad_env_name "$TMP/fixtures/bad_env_name.ds" 'invalid environment variable name'

write_fixture "$TMP/fixtures/env_assign_array_rejected.ds" <<'DS'
env.DS_BAD = ["a", "b"]
DS
assert_reject env_assign_array_rejected "$TMP/fixtures/env_assign_array_rejected.ds" 'environment variable assignment requires a scalar value'

write_fixture "$TMP/fixtures/env_assign_map_rejected.ds" <<'DS'
env.DS_BAD = { name: "api" }
DS
assert_reject env_assign_map_rejected "$TMP/fixtures/env_assign_map_rejected.ds" 'environment variable assignment requires a scalar value'

write_fixture "$TMP/fixtures/env_assign_command_rejected.ds" <<'DS'
env.DS_BAD = run "printf" "x"
DS
assert_reject env_assign_command_rejected "$TMP/fixtures/env_assign_command_rejected.ds" 'environment variable assignment requires a scalar value'

write_fixture "$TMP/fixtures/map_interp_rejected.ds" <<'DS'
fn service() {
  return { name: "api" }
}
echo "{service()}"
DS
assert_reject map_interp_rejected "$TMP/fixtures/map_interp_rejected.ds" 'interpolation expression must be scalar'

write_fixture "$TMP/fixtures/command_result_interp_rejected.ds" <<'DS'
fn result() {
  return run "printf" "ok"
}
echo "{result()}"
DS
assert_reject command_result_interp_rejected "$TMP/fixtures/command_result_interp_rejected.ds" 'interpolation expression must be scalar'

write_fixture "$TMP/fixtures/noisy_interp_rejected.ds" <<'DS'
fn noisy() {
  echo "debug"
  return "value"
}
echo "{noisy()}"
DS
assert_reject noisy_interp_rejected "$TMP/fixtures/noisy_interp_rejected.ds" 'cannot be used as a value because it contains plain command statements'

write_fixture "$TMP/fixtures/mixed_interp_rejected.ds" <<'DS'
fn maybe(flag = true) {
  if flag {
    return "name"
  }
  return ["name"]
}
echo "{maybe(true)}"
DS
assert_reject mixed_interp_rejected "$TMP/fixtures/mixed_interp_rejected.ds" 'same value kind'

write_fixture "$TMP/fixtures/malformed_interp_rejected.ds" <<'DS'
echo "{name(}"
DS
assert_reject malformed_interp_rejected "$TMP/fixtures/malformed_interp_rejected.ds" 'unknown function `name`'

write_fixture "$TMP/fixtures/invalid_arithmetic_command_interp_rejected.ds" <<'DS'
echo "{1 +}"
DS
assert_reject_all invalid_arithmetic_command_interp_rejected "$TMP/fixtures/invalid_arithmetic_command_interp_rejected.ds" 'invalid arithmetic interpolation in v0.21.0'

write_fixture "$TMP/fixtures/interp_prefix_env.ds" <<'DS'
fn name() {
  return "api"
}
echo "{env.DS_PREFIX}-{name()}"
env.DS_PREFIX = "svc"
fn prefix() {
  return env.DS_PREFIX
}
printf "%s\n" "service-{name()}-v1"
echo "{prefix()}-api"
DS
run_parity_env interp_prefix_env "$TMP/fixtures/interp_prefix_env.ds" $'svc-api\nservice-api-v1\nsvc-api' DS_PREFIX=svc

write_fixture "$TMP/fixtures/env_only_helpers.ds" <<'DS'
env.DS_APP = "api"
echo env.DS_APP
DS
./ds emit bash "$TMP/fixtures/env_only_helpers.ds" -o "$TMP/env_only_helpers.sh" >/dev/null; ok "env-only helper emit"
bash -n "$TMP/env_only_helpers.sh"; ok "env-only helper bash -n"
assert_no_contains "env-only does not emit value-call helper" "$TMP/env_only_helpers.sh" '__ds_call_value_into'

write_fixture "$TMP/fixtures/interp_helpers_once.ds" <<'DS'
fn a() {
  return "a"
}
fn b() {
  return "b"
}
echo "{a()} {b()} {a()}"
DS
run_parity interp_helpers_once "$TMP/fixtures/interp_helpers_once.ds" 'a b a'
helper_count=$(grep -c '^__ds_call_value_into()' "$TMP/interp_helpers_once.sh" || true)
assert_eq "interpolation helper emitted once" '1' "$helper_count"

write_fixture "$TMP/fixtures/interp_temp_collision.ds" <<'DS'
let __ds_cmd_interp_0 = "user"
fn name() {
  return "api"
}
echo "{name()}"
echo $__ds_cmd_interp_0
DS
run_parity interp_temp_collision "$TMP/fixtures/interp_temp_collision.ds" $'api
user'

write_fixture "$TMP/fixtures/interp_temp_hidden.ds" <<'DS'
fn name() {
  return "api"
}
echo "{name()}"
let leaked = __ds_cmd_interp_0
echo "leaked={leaked}"
DS
assert_reject interp_temp_hidden "$TMP/fixtures/interp_temp_hidden.ds" 'unknown variable `__ds_cmd_interp_0`'

write_fixture "$TMP/fixtures/bad_env_assign_hyphen.ds" <<'DS'
env.BAD-NAME = "x"
DS
assert_reject bad_env_assign_hyphen "$TMP/fixtures/bad_env_assign_hyphen.ds" 'invalid environment variable name `BAD-NAME`'

write_fixture "$TMP/fixtures/bad_env_unset_hyphen.ds" <<'DS'
unset env.BAD-NAME
DS
assert_reject bad_env_unset_hyphen "$TMP/fixtures/bad_env_unset_hyphen.ds" 'invalid environment variable name `BAD-NAME`'

assert_contains "status documents command interpolation" docs/status.md 'direct scalar value-returning function calls in quoted command'
assert_contains "runtime documents pre-materialization" docs/runtime.md 'pre-materializing each interpolated call'
assert_contains "language documents command interpolation" docs/language.ds 'quoted command words'
assert_contains "Makefile includes v0.27" Makefile '0-27'

printf 'v0.27.0 focused tests passed (%d assertions)\n' "$N"
