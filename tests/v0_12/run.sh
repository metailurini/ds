#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_12_tests.$$"
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

assert_diag() {
  local name="$1"
  local fixture="$2"
  local message="$3"

  run_fail "${name}_check" "$DS" check "$fixture"
  assert_contains "$TMP/${name}_check.err" "$fixture:" "$name diagnostic path"
  assert_contains "$TMP/${name}_check.err" ': error:' "$name diagnostic shape"
  assert_contains "$TMP/${name}_check.err" "$message" "$name diagnostic message"

  run_fail "${name}_bytecode" "$DS" bytecode "$fixture"
  assert_contains "$TMP/${name}_bytecode.err" "$message" "$name bytecode rejects consistently"

  run_fail "${name}_run" "$DS" run "$fixture"
  assert_contains "$TMP/${name}_run.err" "$message" "$name run rejects consistently"

  run_fail "${name}_direct" "$DS" "$fixture"
  assert_contains "$TMP/${name}_direct.err" "$message" "$name direct execution rejects consistently"

  printf 'stale' >"$TMP/${name}.sh"
  run_fail "${name}_emit" "$DS" emit bash "$fixture" -o "$TMP/${name}.sh"
  assert_contains "$TMP/${name}_emit.err" "$message" "$name emit rejects consistently"
  assert_file_missing_or_empty "$TMP/${name}.sh" "$name failed emit removes stale artifact"
}

assert_runtime_failure_parity() {
  local name="$1"
  local fixture="$2"
  local seed="$3"
  local message="$4"
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"
  if [ -d "$seed" ]; then
    (cd "$seed" && tar cf - .) | (cd "$vm_work" && tar xf -)
    (cd "$seed" && tar cf - .) | (cd "$bash_work" && tar xf -)
  fi

  capture_status "${name}_vm" bash -c "cd '$vm_work' && '$DS' run '$fixture'"
  assert_nonzero_status "${name}_vm"
  assert_contains "$TMP/${name}_vm.err" "$message" "$name VM runtime diagnostic"
  assert_not_contains "$TMP/${name}_vm.out" 'after' "$name VM fail-fast"

  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_syntax" bash -n "$script"
  capture_status "${name}_bash" bash -c "cd '$bash_work' && bash '$script'"
  assert_nonzero_status "${name}_bash"
  assert_contains "$TMP/${name}_bash.err" "$message" "$name Bash runtime diagnostic"
  assert_not_contains "$TMP/${name}_bash.out" 'after' "$name Bash fail-fast"
}

assert_no_grep() {
  local name="$1"
  local pattern="$2"
  shift 2
  local out="$TMP/${name}.grep"
  if grep -R -nE -- "$pattern" "$@" >"$out" 2>/dev/null; then
    cat "$out" >&2
    fail "$name: unexpected grep matches for $pattern"
  fi
  pass "$name"
}

FIX="$TMP/fixtures"
SEED="$TMP/seeds"
mkdir -p "$FIX" "$SEED"

# Static cleanup checks: shared stdlib metadata, hashmap absorption, and build wiring.
for helper in \
  'file.exists' 'file.is_file' 'file.read' 'file.write' 'file.append' \
  'dir.exists' 'path.cwd' 'path.join' 'path.basename' 'path.dirname' 'path.ext' \
  'cmd.exists' 'cmd.require' 'env.get' 'env.set' 'env.unset' 'glob' 'glob!' 'lines'; do
  assert_contains src/ds_stdlib.c "$helper" "stdlib metadata contains $helper"
done
assert_contains Makefile '0-12' "Makefile wires v0.12 suite"
assert_contains Makefile 'src/runtime/hashmap.c' "Makefile builds absorbed hashmap"
assert_contains Makefile 'src/vm_stdlib.c' "Makefile builds split VM stdlib runtime"
assert_contains Makefile 'src/bash_helpers.c' "Makefile builds split Bash helper bodies"
assert_contains compile_flags.txt '-Iinclude' "compile flags keep public include path"
assert_no_grep "no_old_hashmap_build_paths" 'libs/hashmap|-Ilibs/hashmap' Makefile compile_flags.txt docs/editor.md docs/architecture.md docs/runtime.md docs/product-principles.md docs/roadmap.md src include
assert_no_grep "no_raw_hashmap_leak_outside_runtime_bridge" '#include[[:space:]]+[<"].*hashmap|\bhm_[a-z_]+' \
  src/ast.c src/bash_emit.c src/bash_helpers.c src/ds_command.c src/ds_command_word.c src/ds_command_result.c src/ds_command_pipeline.c src/diag.c src/lexer.c \
  src/lower.c src/main.c src/parser.c src/source.c src/ds_stdlib.c src/vm.c \
  src/vm_stdlib.c include
[ -f src/runtime/hashmap.LICENSE ] || fail "absorbed hashmap license is missing"
pass "absorbed hashmap license is present"

# Stdlib happy path with side effects and VM/Bash parity.
seed_stdlib="$SEED/stdlib"
mkdir -p "$seed_stdlib"
write_fixture "$FIX/stdlib_matrix.ds" <<'DS'
file.write("input.txt", "one\ntwo\n")
file.append("input.txt", "three\n")

if file.exists("input.txt") { echo "exists" }
if file.is_file("input.txt") { echo "file" }
if dir.exists(".") { echo "dir" }

let text = file.read("input.txt")
echo "text={text}"
let joined = path.join(".", "input.txt")
let base = path.basename("input.txt")
let parent = path.dirname("input.txt")
let ext = path.ext("archive.tar.gz")
echo "join={joined}"
echo "base={base}"
echo "parent={parent}"
echo "ext={ext}"

let target = env.get("DS_V012_TARGET", "staging")
env.set("DS_V012_TARGET", "prod")
let updated = env.get("DS_V012_TARGET")
echo "target={updated}"
env.unset("DS_V012_TARGET")
let fallback = env.get("DS_V012_TARGET", target)
echo "fallback={fallback}"

for f in glob("*.txt") { echo "glob={f}" }
for f in glob!("*.txt") { echo "required={f}" }
for line in lines("input.txt") { echo "line={line}" }
DS
assert_vm_bash_parity "stdlib_matrix" "$FIX/stdlib_matrix.ds" 0 "input.txt"
assert_contains "$TMP/stdlib_matrix_vm.out" 'exists' "stdlib matrix file.exists output"
assert_contains "$TMP/stdlib_matrix_vm.out" 'target=prod' "stdlib matrix env mutation output"
assert_contains "$TMP/stdlib_matrix_vm.out" 'fallback=staging' "stdlib matrix env fallback output"
assert_contains "$TMP/stdlib_matrix_vm.out" 'line=three' "stdlib matrix lines output"

# Cross-feature matrix: imports, functions, namespace-like variables, and command args via scalar bindings.
write_fixture "$FIX/import-lib.ds" <<'DS'
fn lib_check(target) {
  if file.exists(target) {
    echo "lib-exists"
  }
  let base = path.basename(target)
  echo "lib-base={base}"
}
DS
write_fixture "$FIX/cross_feature.ds" <<'DS'
import "./import-lib.ds"

fn print_file(target) {
  let base = path.basename(target)
  echo "base={base}"
  for line in lines(target) {
    echo "line={line}"
  }
}

let file = "a.txt"
let path = "namespace variable"
file.write(file, "alpha\nbeta\n")
print_file(file)
lib_check(file)
let echoed = run echo $file
echo "cmd={echoed.stdout}"
echo "path-var={path}"
DS
assert_vm_bash_parity "cross_feature" "$FIX/cross_feature.ds" 0 "a.txt"
assert_contains "$TMP/cross_feature_vm.out" 'lib-exists' "imported stdlib call output"
assert_contains "$TMP/cross_feature_vm.out" 'cmd=a.txt' "command arg through scalar binding output"
assert_contains "$TMP/cross_feature_vm.out" 'path-var=namespace variable' "namespace-like variable output"

# Map/hashmap behavior remains intact after absorption.
write_fixture "$FIX/map_hashmap.ds" <<'DS'
let ports = { api: 3000, web: 5173 }
let labels = { "api-service": "api", "web service": "web value" }
let key = "web"
let api = ports.api
let web = ports[key]
let api_label = labels["api-service"]
let web_label = labels["web service"]
echo "ports={api}:{web}"
echo "labels={api_label}:{web_label}"
file.write("map.txt", "ok\n")
if file.exists("map.txt") { echo "map-with-stdlib" }
DS
assert_vm_bash_parity "map_hashmap" "$FIX/map_hashmap.ds" 0 "map.txt"
assert_same_text $'ports=3000:5173\nlabels=api:web value\nmap-with-stdlib\n' "$TMP/map_hashmap_vm.out" "map behavior output after hashmap absorption"

write_fixture "$FIX/array_only.ds" <<'DS'
let xs = ["a", "b"]
for x in xs { echo $x }
DS
run_ok "array_only_emit" "$DS" emit bash "$FIX/array_only.ds" -o "$TMP/array_only.sh"
run_ok "array_only_bash_syntax" bash -n "$TMP/array_only.sh"
assert_not_contains "$TMP/array_only.sh" 'maps require Bash 4 or newer' "array-only script avoids map guard"

run_ok "map_hashmap_emit" "$DS" emit bash "$FIX/map_hashmap.ds" -o "$TMP/map_hashmap.sh"
run_ok "map_hashmap_bash_syntax" bash -n "$TMP/map_hashmap.sh"
assert_contains "$TMP/map_hashmap.sh" 'maps require Bash 4 or newer' "map script emits Bash 4 guard"
assert_contains "$TMP/map_hashmap.sh" 'declare -A' "map script uses Bash associative arrays"
assert_not_contains "$TMP/map_hashmap.sh" "$DS" "emitted Bash is standalone"
assert_contains "$TMP/map_hashmap.sh" '__ds_' "stdlib/map Bash helpers use project prefix"

write_fixture "$FIX/bad_map_missing_runtime.ds" <<'DS'
let ports = { api: 3000 }
let missing = ports["web"]
echo "after {missing}"
DS
run_ok "bad_map_missing_emit" "$DS" emit bash "$FIX/bad_map_missing_runtime.ds" -o "$TMP/bad_map_missing.sh"
capture_status "bad_map_missing_vm" "$DS" run "$FIX/bad_map_missing_runtime.ds"
assert_nonzero_status "bad_map_missing_vm"
assert_contains "$TMP/bad_map_missing_vm.err" 'missing map key' "VM missing map key diagnostic still clear"
capture_status "bad_map_missing_bash" bash "$TMP/bad_map_missing.sh"
assert_nonzero_status "bad_map_missing_bash"
assert_contains "$TMP/bad_map_missing_bash.err" 'missing map key' "Bash missing map key diagnostic still clear"

# Diagnostics consistency for shared metadata/type rules and unsupported/deferred forms.
write_fixture "$FIX/bad_unknown_namespace.ds" <<'DS'
let x = file.missing("x")
DS
assert_diag "bad_unknown_namespace" "$FIX/bad_unknown_namespace.ds" 'unknown standard-library helper `file.missing`'

write_fixture "$FIX/bad_unknown_bare.ds" <<'DS'
globall("*.ds")
DS
assert_diag "bad_unknown_bare" "$FIX/bad_unknown_bare.ds" 'unknown function `globall`'

write_fixture "$FIX/bad_path_join_arity.ds" <<'DS'
let p = path.join()
DS
assert_diag "bad_path_join_arity" "$FIX/bad_path_join_arity.ds" 'expects 1 to'

write_fixture "$FIX/bad_env_get_arity.ds" <<'DS'
let v = env.get("A", "B", "C")
DS
assert_diag "bad_env_get_arity" "$FIX/bad_env_get_arity.ds" 'expects 1 to 2 arguments'

write_fixture "$FIX/bad_file_arg_kind.ds" <<'DS'
let ok = file.exists(123)
DS
assert_diag "bad_file_arg_kind" "$FIX/bad_file_arg_kind.ds" 'expects string arguments'

write_fixture "$FIX/bad_statement_as_value.ds" <<'DS'
let x = file.write("out.txt", "x")
DS
assert_diag "bad_statement_as_value" "$FIX/bad_statement_as_value.ds" 'statement-only'

write_fixture "$FIX/bad_value_as_statement.ds" <<'DS'
file.exists("README.md")
DS
assert_diag "bad_value_as_statement" "$FIX/bad_value_as_statement.ds" 'returns a value'

write_fixture "$FIX/bad_builtin_glob.ds" <<'DS'
fn glob(pattern) {
  echo pattern
}
DS
assert_diag "bad_builtin_glob" "$FIX/bad_builtin_glob.ds" 'conflicts with a'

write_fixture "$FIX/bad_builtin_lines.ds" <<'DS'
fn lines(path) {
  echo path
}
DS
assert_diag "bad_builtin_lines" "$FIX/bad_builtin_lines.ds" 'conflicts with a'

write_fixture "$FIX/bad_env_direct.ds" <<'DS'
env.PATH += "./bin"
DS
assert_diag "bad_env_direct" "$FIX/bad_env_direct.ds" 'environment assignment supports only `=`'

write_fixture "$FIX/bad_recursive_glob.ds" <<'DS'
for f in glob("**/*.txt") {
  echo $f
}
DS
assert_diag "bad_recursive_glob" "$FIX/bad_recursive_glob.ds" 'recursive `**` glob patterns are deferred'

write_fixture "$FIX/bad_duplicate_key.ds" <<'DS'
let ports = { api: 3000, api: 4000 }
DS
assert_diag "bad_duplicate_key" "$FIX/bad_duplicate_key.ds" 'duplicate map key `api`'

# Runtime edge cases for file/lines/glob/cmd/env helpers.
seed_errors="$SEED/errors"
mkdir -p "$seed_errors/dir" "$seed_errors/bin"
printf 'x' >"$seed_errors/bin/not-exec"
printf '#!/usr/bin/env sh\nexit 0\n' >"$seed_errors/bin/tool"
chmod +x "$seed_errors/bin/tool"
printf 'one\ntwo' >"$seed_errors/unterminated.txt"
printf 'crlf\r\n' >"$seed_errors/crlf.txt"
write_fixture "$FIX/lines_edges.ds" <<'DS'
file.write("unterminated.txt", "one\ntwo")
for line in lines("unterminated.txt") { echo "line={line}" }
for f in glob("missing-*.txt") { echo "unexpected={f}" }
echo "done"
DS
assert_vm_bash_parity "lines_edges" "$FIX/lines_edges.ds" 0 ""
assert_same_text $'line=one\nline=two\ndone\n' "$TMP/lines_edges_vm.out" "lines edge behavior"

write_fixture "$FIX/glob_patterns.ds" <<'DS'
file.write("a.txt", "a")
file.write("b.txt", "b")
file.write("q1.txt", "q")
for f in glob("?.txt") { echo "qmark={f}" }
for f in glob("[ab].txt") { echo "bracket={f}" }
DS
assert_vm_bash_parity "glob_patterns" "$FIX/glob_patterns.ds" 0 "a.txt b.txt q1.txt"
assert_contains "$TMP/glob_patterns_vm.out" 'qmark=a.txt' "glob ? pattern includes a.txt"
assert_contains "$TMP/glob_patterns_vm.out" 'bracket=b.txt' "glob bracket pattern includes b.txt"

write_fixture "$FIX/cmd_env_edges.ds" <<'DS'
if cmd.exists("tool") { echo "tool-exists" }
if !cmd.exists("not-exec") { echo "not-exec-false" }
env.set("DS_V012_EMPTY", "")
let empty = env.get("DS_V012_EMPTY", "fallback")
echo "empty={empty}"
env.set("DS_V012_CHILD", "child-value")
let child = run sh -c "printf child=$DS_V012_CHILD"
echo "child-out={child.stdout}"
env.unset("DS_V012_CHILD")
let missing = env.get("DS_V012_CHILD", "gone")
echo "fallback={missing}"
DS
PATH="$seed_errors/bin:$PATH" assert_vm_bash_parity "cmd_env_edges" "$FIX/cmd_env_edges.ds" 0 ""
assert_contains "$TMP/cmd_env_edges_vm.out" 'tool-exists' "cmd.exists executable on PATH"
assert_contains "$TMP/cmd_env_edges_vm.out" 'not-exec-false' "cmd.exists ignores non-executable PATH entry"
assert_contains "$TMP/cmd_env_edges_vm.out" 'child-out=child=child-value' "child command observes env mutation"

write_fixture "$FIX/fail_file_read_missing.ds" <<'DS'
let x = file.read("missing.txt")
echo "after"
DS
assert_runtime_failure_parity "fail_file_read_missing" "$FIX/fail_file_read_missing.ds" "$seed_errors" 'failed to read file'

write_fixture "$FIX/fail_lines_dir.ds" <<'DS'
for line in lines("dir") { echo $line }
echo "after"
DS
assert_runtime_failure_parity "fail_lines_dir" "$FIX/fail_lines_dir.ds" "$seed_errors" 'failed to read lines'

write_fixture "$FIX/fail_glob_required.ds" <<'DS'
for f in glob!("missing-*.txt") { echo $f }
echo "after"
DS
assert_runtime_failure_parity "fail_glob_required" "$FIX/fail_glob_required.ds" "$seed_errors" 'had no matches'

write_fixture "$FIX/fail_cmd_require.ds" <<'DS'
cmd.require("definitely-missing-ds-v012-command")
echo "after"
DS
assert_runtime_failure_parity "fail_cmd_require" "$FIX/fail_cmd_require.ds" "$seed_errors" 'was not found'

# Failed emit should remove stale artifacts for representative invalid fixtures.
printf 'stale' >"$TMP/stale.sh"
run_fail "stale_emit_cleanup" "$DS" emit bash "$FIX/bad_file_arg_kind.ds" -o "$TMP/stale.sh"
assert_file_missing_or_empty "$TMP/stale.sh" "failed emit removes stale artifact representative"

printf 'v0.12 tests passed (%d assertions)\n' "$pass_count"
