#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_11_tests.$$"
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
  run_fail "${name}_emit" "$DS" emit bash "$fixture" -o "$TMP/${name}.sh"
  assert_file_missing_or_empty "$TMP/${name}.sh" "$name failed emit leaves no artifact"
}

assert_runtime_failure() {
  local name="$1"
  local fixture="$2"
  local work="$3"
  local message="$4"
  local script="$TMP/${name}.sh"

  capture_status "${name}_vm" bash -c "cd '$work' && '$DS' run '$fixture'"
  assert_nonzero_status "${name}_vm"
  assert_contains "$TMP/${name}_vm.err" "$message" "$name VM diagnostic"

  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_syntax" bash -n "$script"
  capture_status "${name}_bash" bash -c "cd '$work' && bash '$script'"
  assert_nonzero_status "${name}_bash"
  assert_contains "$TMP/${name}_bash.err" "$message" "$name Bash diagnostic"
}

assert_vm_bash_parity_in_work() {
  local name="$1"
  local fixture="$2"
  local work_seed="$3"
  local expected_status="$4"
  local output_files="$5"
  shift 5

  local work_vm="$TMP/${name}_vm_work"
  local work_bash="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$work_vm" "$work_bash"
  if [ -d "$work_seed" ]; then
    (cd "$work_seed" && tar cf - .) | (cd "$work_vm" && tar xf -)
    (cd "$work_seed" && tar cf - .) | (cd "$work_bash" && tar xf -)
  fi

  set +e
  (cd "$work_vm" && "$DS" run "$fixture" "$@") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"

  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_syntax" bash -n "$script"

  set +e
  (cd "$work_bash" && bash "$script" "$@") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "VM/Bash stdout parity: $name"
  assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "VM/Bash stderr parity: $name"

  local rel
  for rel in $output_files; do
    [ -f "$work_vm/$rel" ] || fail "VM/Bash $rel parity: $name: VM did not create expected output file"
    [ -f "$work_bash/$rel" ] || fail "VM/Bash $rel parity: $name: Bash did not create expected output file"
    assert_same "$work_vm/$rel" "$work_bash/$rel" "VM/Bash $rel parity: $name"
  done
}

FIX="$TMP/fixtures"
SEED="$TMP/seeds"
mkdir -p "$FIX" "$SEED"

# Lexer / parser / CLI smoke fixture.
write_fixture "$FIX/smoke.ds" <<'DS'
# stdlib token/parser smoke
if file.exists("README.md") {
  echo "readme exists"
}
if file.is_file("README.md") {
  echo "readme file"
}
if dir.exists("src") {
  echo "src dir"
}
let cwd = path.cwd()
let joined = path.join("src", "main.ds")
let base = path.basename(joined)
let parent = path.dirname(joined)
let ext = path.ext(joined)
if cmd.exists("sh") {
  echo "sh found"
}
let target = env.get("DS_V0_11_TARGET", "staging")
echo "{base}:{parent}:{ext}:{target}"
DS

run_ok "smoke_tokens" "$DS" tokens "$FIX/smoke.ds"
assert_contains "$TMP/smoke_tokens.out" 'IDENT          "file"' "tokens include namespace head"
assert_contains "$TMP/smoke_tokens.out" 'DOT' "tokens include dot"
run_ok "smoke_ast" "$DS" ast "$FIX/smoke.ds"
assert_contains "$TMP/smoke_ast.out" 'file.exists' "AST includes file.exists"
assert_contains "$TMP/smoke_ast.out" 'path.join' "AST includes path.join"
run_ok "smoke_check" "$DS" check "$FIX/smoke.ds"
run_ok "smoke_bytecode" "$DS" bytecode "$FIX/smoke.ds"
assert_contains "$TMP/smoke_bytecode.out" 'STDLIB_CALL' "bytecode includes stdlib calls"
assert_not_contains "$TMP/smoke_bytecode.out" '0x' "bytecode pointer-free smoke"

# File and directory predicates, path helpers, env default, and command helper success.
seed_basic="$SEED/basic"
mkdir -p "$seed_basic/src" "$seed_basic/space dir"
printf 'hello' >"$seed_basic/README.md"
printf 'space file' >"$seed_basic/space dir/file name.txt"
write_fixture "$FIX/basic_helpers.ds" <<'DS'
if file.exists("README.md") { echo "exists" }
if file.is_file("README.md") { echo "is-file" }
if !file.exists("missing.txt") { echo "missing" }
if dir.exists("src") { echo "is-dir" }
if !dir.exists("README.md") { echo "not-dir" }
if file.exists("space dir/file name.txt") { echo "space path" }
let cwd = path.cwd()
let joined = path.join("src/", "main.ds")
let abs = path.join(cwd, "README.md")
let base = path.basename(abs)
let parent = path.dirname("file.txt")
let ext1 = path.ext("archive.tar.gz")
let ext2 = path.ext("README")
let ext3 = path.ext(".env")
let target = env.get("DS_V0_11_TARGET", "staging")
if cmd.exists("sh") { echo "cmd yes" }
echo "{joined}:{base}:{parent}:{ext1}:{ext2}:{ext3}:{target}"
DS
assert_vm_bash_parity_in_work "basic_helpers" "$FIX/basic_helpers.ds" "$seed_basic" 0 ""
assert_contains "$TMP/basic_helpers_vm.out" 'exists' "basic helper output exists"
assert_contains "$TMP/basic_helpers_vm.out" 'src/main.ds:README.md:.:.gz:::staging' "path/env output"

# File read/write/append side effects, including trailing newline preservation.
seed_files="$SEED/files"
mkdir -p "$seed_files"
printf 'one\ntwo\n' >"$seed_files/input.txt"
: >"$seed_files/empty.txt"
printf 'space file text' >"$seed_files/name with spaces.txt"
write_fixture "$FIX/file_io.ds" <<'DS'
let text = file.read("input.txt")
file.write("out.txt", text)
file.append("out.txt", "tail")
let empty = file.read("empty.txt")
file.write("empty-copy.txt", empty)
let spaced = file.read("name with spaces.txt")
file.write("space out.txt", spaced)
DS
assert_vm_bash_parity_in_work "file_io" "$FIX/file_io.ds" "$seed_files" 0 "out.txt empty-copy.txt"
python3 - "$TMP/file_io_vm_work/out.txt" "$TMP/file_io_bash_work/out.txt" "$TMP/file_io_vm_work/space out.txt" "$TMP/file_io_bash_work/space out.txt" <<'PY'
import pathlib, sys
expected = b"one\ntwo\ntail"
for arg in sys.argv[1:3]:
    data = pathlib.Path(arg).read_bytes()
    if data != expected:
        raise SystemExit(f"unexpected bytes in {arg}: {data!r}")
space_vm = pathlib.Path(sys.argv[3]).read_bytes()
space_bash = pathlib.Path(sys.argv[4]).read_bytes()
if space_vm != b"space file text" or space_bash != space_vm:
    raise SystemExit("space path output mismatch")
PY
pass "file.read/write preserves trailing newline before append and space path output"

# Glob and lines helpers.
seed_glob="$SEED/glob"
mkdir -p "$seed_glob/sub"
printf 'A' >"$seed_glob/a.txt"
printf 'B' >"$seed_glob/b.txt"
printf 'C' >"$seed_glob/c.ds"
printf 'S' >"$seed_glob/space name.txt"
printf 'one\ntwo\n' >"$seed_glob/lines.txt"
printf 'tail' >"$seed_glob/unterminated.txt"
printf ' leading\n\ntrailing \n' >"$seed_glob/spaces.txt"
: >"$seed_glob/empty.txt"
write_fixture "$FIX/glob_lines.ds" <<'DS'
for file in glob("*.txt") {
  echo "txt={file}"
}
echo "--required--"
for file in glob!("*.ds") {
  echo "ds={file}"
}
for file in glob("nope-*.txt") {
  echo "NOPE {file}"
}
for line in lines("lines.txt") {
  echo "line={line}"
}
for line in lines("unterminated.txt") {
  echo "tail={line}"
}
for line in lines("spaces.txt") {
  echo "space={line}"
}
for line in lines("empty.txt") {
  echo "EMPTY {line}"
}
DS
assert_vm_bash_parity_in_work "glob_lines" "$FIX/glob_lines.ds" "$seed_glob" 0 ""
assert_contains "$TMP/glob_lines_vm.out" 'txt=a.txt' "glob includes a.txt"
assert_contains "$TMP/glob_lines_vm.out" 'txt=space name.txt' "glob preserves spaces"
assert_contains "$TMP/glob_lines_vm.out" 'tail=tail' "lines emits unterminated final line"
assert_contains "$TMP/glob_lines_vm.out" 'space= leading' "lines preserves leading spaces"
assert_contains "$TMP/glob_lines_vm.out" 'space=' "lines emits blank lines"
assert_not_contains "$TMP/glob_lines_vm.out" 'NOPE' "unmatched glob has zero iterations"

# Command and environment helpers with controlled PATH.
seed_cmd="$SEED/cmd_env"
mkdir -p "$seed_cmd/bin"
printf '#!/usr/bin/env sh\nexit 0\n' >"$seed_cmd/bin/fake-tool"
chmod +x "$seed_cmd/bin/fake-tool"
printf 'not executable\n' >"$seed_cmd/bin/not-exec"
write_fixture "$FIX/cmd_env.ds" <<'DS'
if cmd.exists("fake-tool") { echo "fake yes" }
if !cmd.exists("missing-tool") { echo "missing no" }
if !cmd.exists("not-exec") { echo "not-exec no" }
cmd.require("fake-tool")
let before = env.get("DS_V0_11_TARGET", "unset")
echo "before={before}"
env.set("DS_V0_11_TARGET", "prod value $HOME")
let after = env.get("DS_V0_11_TARGET")
echo "after={after}"
sh -c "printf 'child=%s\n' \"$DS_V0_11_TARGET\""
env.unset("DS_V0_11_TARGET")
let final = env.get("DS_V0_11_TARGET", "unset")
echo "final={final}"
DS
PATH="$seed_cmd/bin:$PATH" assert_vm_bash_parity_in_work "cmd_env" "$FIX/cmd_env.ds" "$seed_cmd" 0 ""
assert_contains "$TMP/cmd_env_vm.out" 'fake yes' "cmd.exists finds fake executable"
assert_contains "$TMP/cmd_env_vm.out" 'not-exec no' "cmd.exists ignores non-executable"
assert_contains "$TMP/cmd_env_vm.out" 'child=prod value $HOME' "env.set exports exact value"

# Functions/imports/loops integration.
write_fixture "$FIX/import-lib.ds" <<'DS'
fn print_first_line(path_name) {
  for line in lines(path_name) {
    echo "imported={line}"
  }
}

fn write_marker(path_name) {
  file.write(path_name, "marker")
}
DS
write_fixture "$FIX/import-main.ds" <<DS
import "$FIX/import-lib.ds"
print_first_line("input.txt")
write_marker("marker.txt")
DS
seed_import="$SEED/import"
mkdir -p "$seed_import"
printf 'first\nsecond\n' >"$seed_import/input.txt"
assert_vm_bash_parity_in_work "import_integration" "$FIX/import-main.ds" "$seed_import" 0 "marker.txt"
assert_contains "$TMP/import_integration_vm.out" 'imported=first' "imported function uses lines"

write_fixture "$FIX/function_loop.ds" <<'DS'
fn read_each(pattern) {
  for file in glob(pattern) {
    let text = file.read(file)
    echo "{file}:{text}"
  }
}
read_each("*.cfg")
DS
seed_func="$SEED/function_loop"
mkdir -p "$seed_func"
printf 'api' >"$seed_func/api.cfg"
printf 'web' >"$seed_func/web.cfg"
assert_vm_bash_parity_in_work "function_loop" "$FIX/function_loop.ds" "$seed_func" 0 ""
assert_contains "$TMP/function_loop_vm.out" 'api.cfg:api' "function stdlib loop output"

# Helper emission checks.
run_ok "emit_no_stdlib" "$DS" emit bash "examples/basic.ds" -o "$TMP/no_stdlib.sh"
assert_not_contains "$TMP/no_stdlib.sh" '__ds_stdlib_' "no stdlib helpers emitted when unused"
run_ok "emit_with_stdlib" "$DS" emit bash "$FIX/import-main.ds" -o "$TMP/with_stdlib.sh"
assert_contains "$TMP/with_stdlib.sh" '__ds_stdlib_lines()' "stdlib helper emitted when needed"
count_lines_helper="$(grep -cF '__ds_stdlib_lines()' "$TMP/with_stdlib.sh" || true)"
[ "$count_lines_helper" = 1 ] || fail "stdlib lines helper should be emitted once, got $count_lines_helper"
pass "stdlib helpers emitted once"
assert_not_contains "$TMP/with_stdlib.sh" ' ds ' "emitted Bash does not call ds"

# Static diagnostics.
write_fixture "$FIX/bad_unknown_helper.ds" <<'DS'
file.missing("x")
DS
assert_diag "bad_unknown_helper" "$FIX/bad_unknown_helper.ds" 'unknown standard-library helper `file.missing`'

write_fixture "$FIX/bad_arity.ds" <<'DS'
file.exists("a", "b")
DS
assert_diag "bad_arity" "$FIX/bad_arity.ds" 'expects 1 arguments but got 2'

write_fixture "$FIX/bad_type.ds" <<'DS'
file.exists(1)
DS
assert_diag "bad_type" "$FIX/bad_type.ds" 'expects string arguments'

write_fixture "$FIX/bad_stmt_only_value.ds" <<'DS'
let x = file.write("out.txt", "x")
DS
assert_diag "bad_stmt_only_value" "$FIX/bad_stmt_only_value.ds" 'statement-only'

write_fixture "$FIX/bad_env_direct.ds" <<'DS'
let x = env.HOME
DS
assert_diag "bad_env_direct" "$FIX/bad_env_direct.ds" 'direct `env.NAME` access is deferred'

write_fixture "$FIX/bad_recursive_glob.ds" <<'DS'
for file in glob("**/*.txt") {
  echo $file
}
DS
assert_diag "bad_recursive_glob" "$FIX/bad_recursive_glob.ds" 'recursive `**` glob patterns are deferred'

write_fixture "$FIX/bad_path_join_arity.ds" <<'DS'
let p = path.join()
DS
assert_diag "bad_path_join_arity" "$FIX/bad_path_join_arity.ds" 'expects 1 to'

write_fixture "$FIX/bad_env_name_static.ds" <<'DS'
env.set("BAD-NAME", "x")
DS
assert_diag "bad_env_name_static" "$FIX/bad_env_name_static.ds" 'invalid environment variable name `BAD-NAME`'

# Runtime diagnostics and Bash parity for failing helpers.
write_fixture "$FIX/fail_read_missing.ds" <<'DS'
file.read("missing.txt")
DS
# value-returning helpers cannot be standalone statements, so use a binding.
write_fixture "$FIX/fail_read_missing.ds" <<'DS'
let x = file.read("missing.txt")
echo "after"
DS
assert_runtime_failure "fail_read_missing" "$FIX/fail_read_missing.ds" "$seed_basic" 'failed to read file'

write_fixture "$FIX/fail_write_parent.ds" <<'DS'
file.write("missing-parent/out.txt", "x")
echo "after"
DS
assert_runtime_failure "fail_write_parent" "$FIX/fail_write_parent.ds" "$seed_basic" 'failed to write file'

write_fixture "$FIX/fail_lines_missing.ds" <<'DS'
for line in lines("missing.txt") {
  echo $line
}
echo "after"
DS
assert_runtime_failure "fail_lines_missing" "$FIX/fail_lines_missing.ds" "$seed_basic" 'failed to read lines'
assert_not_contains "$TMP/fail_lines_missing_vm.out" 'after' "VM lines failure is fail-fast"
assert_not_contains "$TMP/fail_lines_missing_bash.out" 'after' "Bash lines failure is fail-fast"

write_fixture "$FIX/fail_lines_dir.ds" <<'DS'
for line in lines("src") {
  echo $line
}
DS
assert_runtime_failure "fail_lines_dir" "$FIX/fail_lines_dir.ds" "$seed_basic" 'failed to read lines'

write_fixture "$FIX/fail_require.ds" <<'DS'
cmd.require("missing-tool")
echo "after"
DS
PATH="$seed_cmd/bin:$PATH" assert_runtime_failure "fail_require" "$FIX/fail_require.ds" "$seed_cmd" 'required command'

write_fixture "$FIX/fail_glob_required.ds" <<'DS'
for file in glob!("no-matches-*.txt") {
  echo $file
}
echo "after"
DS
assert_runtime_failure "fail_glob_required" "$FIX/fail_glob_required.ds" "$seed_glob" 'had no matches'
assert_not_contains "$TMP/fail_glob_required_vm.out" 'after' "VM glob! failure is fail-fast"
assert_not_contains "$TMP/fail_glob_required_bash.out" 'after' "Bash glob! failure is fail-fast"

write_fixture "$FIX/fail_env_dynamic.ds" <<'DS'
let bad = "1BAD"
let value = env.get(bad)
echo "after={value}"
DS
assert_runtime_failure "fail_env_dynamic" "$FIX/fail_env_dynamic.ds" "$seed_basic" 'invalid environment variable name'

write_fixture "$FIX/fail_recursive_glob_dynamic.ds" <<'DS'
let pattern = "**/*.txt"
for file in glob(pattern) {
  echo $file
}
DS
assert_runtime_failure "fail_recursive_glob_dynamic" "$FIX/fail_recursive_glob_dynamic.ds" "$seed_basic" 'recursive'

# Direct execution behaves like `run` for a representative fixture.
capture_status "direct_vm" bash -c "cd '$seed_basic' && '$DS' '$FIX/basic_helpers.ds'"
assert_status "direct_vm" 0
assert_same "$TMP/basic_helpers_vm.out" "$TMP/direct_vm.out" "direct execution matches run stdout"

# Makefile integration.
assert_contains "$ROOT/Makefile" '0-11' "Makefile includes v0.11 version"
assert_contains "$ROOT/Makefile" '$(TEST_TARGETS): $(BIN)' "Makefile uses normalized version targets"

printf 'v0.11.0 tests passed (%d checks)\n' "$pass_count"
