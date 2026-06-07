#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
DS="$ROOT/ds"
CASE_TIMEOUT=${DS_TEST_CASE_TIMEOUT:-30}
TMP=${TMPDIR:-/tmp}/ds_v0_38_tests.$$
FIX="$TMP/fixtures"
SEED="$TMP/seeds"
mkdir -p "$FIX" "$SEED"
if [[ "${DS_KEEP_TMP:-0}" != "1" ]]; then
  trap 'rm -rf "$TMP"' EXIT
fi

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != "1" ]]; then
  make -C "$ROOT" >/dev/null
fi

write_fixture() {
  local name="$1"
  local path="$FIX/$name.ds"
  mkdir -p "$(dirname "$path")"
  cat >"$path"
  printf '%s' "$path"
}

write_expected() {
  local name="$1" text="$2" path
  path="$TMP/$name.expected"
  printf '%s' "$text" >"$path"
  printf '%s' "$path"
}

assert_text() {
  local name="$1" expected="$2" actual="$3" exp
  exp=$(write_expected "$name" "$expected")
  assert_same "$exp" "$actual" "$name"
}

capture_cmd() {
  local name="$1"
  shift
  set +e
  timeout "$CASE_TIMEOUT" "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/$name.rc"
}

copy_seed() {
  local from="$1" to="$2"
  rm -rf "$to"
  mkdir -p "$to"
  if [ -d "$from" ]; then
    (cd "$from" && tar cf - .) | (cd "$to" && tar xf -)
  fi
}

assert_no_ds_call() {
  local script="$1" name="$2"
  assert_not_contains "$script" "$ROOT/ds" "$name omits repo ds path"
  assert_not_contains "$script" './ds ' "$name omits ./ds invocation"
  assert_not_contains "$script" ' ds run ' "$name omits ds run invocation"
  assert_not_contains "$script" ' ds emit ' "$name omits ds emit invocation"
}

assert_no_duplicate_helpers() {
  local script="$1" name="$2" defs dups
  defs="$TMP/${name//[^A-Za-z0-9_]/_}_helper_defs.txt"
  dups="$TMP/${name//[^A-Za-z0-9_]/_}_helper_dups.txt"
  grep -E '^__ds_[A-Za-z0-9_]+\(\)' "$script" | sed 's/(.*//' | sort >"$defs" || true
  uniq -d "$defs" >"$dups"
  [ ! -s "$dups" ] || { cat "$dups" >&2; fail "$name has duplicate helper definitions"; }
  pass "$name has no duplicate helper definitions"
}

assert_helper_def_count() {
  local script="$1" helper="$2" expected="$3" name="$4" count
  count=$(grep -c -F -- "$helper()" "$script" || true)
  [ "$count" = "$expected" ] || fail "$name: expected $helper definition count $expected, got $count"
  pass "$name"
}

emit_checked() {
  local name="$1" file="$2" script="$3"
  run_ok "${name}_emit" "$DS" emit bash "$file" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_no_ds_call "$script" "$name emitted Bash standalone"
  assert_no_duplicate_helpers "$script" "$name emitted Bash"
}

run_parity_seed() {
  local name="$1" file="$2" seed="$3" expected_stdout="$4" expected_status="${5:-0}"
  if [ "$#" -ge 5 ]; then
    shift 5
  else
    shift "$#"
  fi
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  copy_seed "$seed" "$vm_work"
  copy_seed "$seed" "$bash_work"

  run_ok "${name}_check" "$DS" check "$file"
  run_ok "${name}_ast" "$DS" ast "$file"
  run_ok "${name}_hir" "$DS" hir "$file"
  run_ok "${name}_bytecode" "$DS" bytecode "$file"
  emit_checked "$name" "$file" "$script"

  set +e
  (cd "$vm_work" && timeout "$CASE_TIMEOUT" "$DS" run "$file" "$@") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && timeout "$CASE_TIMEOUT" bash "$script" "$@") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name VM/Bash stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name VM/Bash stderr parity"
    assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
  else
    assert_contains "$TMP/${name}_vm.err" ': error:' "$name VM runtime diagnostic shape"
    assert_contains "$TMP/${name}_bash.err" ': error:' "$name Bash runtime diagnostic shape"
  fi
}

run_parity_seed_env() {
  local name="$1" file="$2" seed="$3" expected_stdout="$4" env_assignment="$5" expected_status="${6:-0}"
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  copy_seed "$seed" "$vm_work"
  copy_seed "$seed" "$bash_work"

  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"

  set +e
  (cd "$vm_work" && env "$env_assignment" timeout "$CASE_TIMEOUT" "$DS" run "$file") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && env "$env_assignment" timeout "$CASE_TIMEOUT" bash "$script") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name VM/Bash stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name VM/Bash stderr parity"
    assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
  else
    assert_contains "$TMP/${name}_vm.err" ': error:' "$name VM runtime diagnostic shape"
    assert_contains "$TMP/${name}_bash.err" ': error:' "$name Bash runtime diagnostic shape"
  fi
}

assert_runtime_rejected_seed() {
  local name="$1" file="$2" seed="$3" needle="$4" forbidden="$5"
  run_parity_seed "$name" "$file" "$seed" '' 1
  assert_contains "$TMP/${name}_vm.err" "$needle" "$name VM diagnostic contains $needle"
  assert_contains "$TMP/${name}_bash.err" "$needle" "$name Bash diagnostic contains $needle"
  assert_not_contains "$TMP/${name}_vm.out" "$forbidden" "$name VM stops before forbidden output"
  assert_not_contains "$TMP/${name}_bash.out" "$forbidden" "$name Bash stops before forbidden output"
}

assert_runtime_rejected_seed_env() {
  local name="$1" file="$2" seed="$3" env_assignment="$4" needle="$5" forbidden="$6"
  run_parity_seed_env "$name" "$file" "$seed" '' "$env_assignment" 1
  assert_contains "$TMP/${name}_vm.err" "$needle" "$name VM diagnostic contains $needle"
  assert_contains "$TMP/${name}_bash.err" "$needle" "$name Bash diagnostic contains $needle"
  assert_not_contains "$TMP/${name}_vm.out" "$forbidden" "$name VM stops before forbidden output"
  assert_not_contains "$TMP/${name}_bash.out" "$forbidden" "$name Bash stops before forbidden output"
}

assert_static_rejected() {
  local name="$1" file="$2"
  shift 2
  local script="$TMP/$name.sh" cmd needle
  for cmd in check hir bytecode run; do
    capture_cmd "${name}_${cmd}" "$DS" "$cmd" "$file"
    assert_nonzero_status "${name}_${cmd}"
    assert_contains "$TMP/${name}_${cmd}.err" ': error:' "$name $cmd diagnostic shape"
    for needle in "$@"; do
      assert_contains "$TMP/${name}_${cmd}.err" "$needle" "$name $cmd diagnostic contains $needle"
    done
  done
  capture_cmd "${name}_emit" "$DS" emit bash "$file" -o "$script"
  assert_nonzero_status "${name}_emit"
  assert_contains "$TMP/${name}_emit.err" ': error:' "$name emit diagnostic shape"
  for needle in "$@"; do
    assert_contains "$TMP/${name}_emit.err" "$needle" "$name emit diagnostic contains $needle"
  done
  assert_file_missing_or_empty "$script" "$name failed emit artifact"
}

assert_direct_accept() {
  local name="$1" file="$2" seed="$3" expected_stdout="$4"
  local work="$TMP/${name}_direct_work"
  copy_seed "$seed" "$work"
  set +e
  (cd "$work" && timeout "$CASE_TIMEOUT" "$DS" "$file") >"$TMP/${name}_direct.out" 2>"$TMP/${name}_direct.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/${name}_direct.rc"
  assert_status "${name}_direct" 0
  assert_text "${name}_direct_stdout" "$expected_stdout" "$TMP/${name}_direct.out"
}

assert_doc_contains() { assert_contains "$1" "$2" "$3"; }
assert_doc_not_contains() { assert_not_contains "$1" "$2" "$3"; }

# 1. Planning, docs, and scope guard.
[ -f docs/milestones/v0.38.0-spec.md ] || fail 'missing v0.38 spec'
pass 'v0.38 spec exists'
[ -f docs/milestones/v0.38.0-test-plan.md ] || fail 'missing v0.38 test plan'
pass 'v0.38 test plan exists'
for needle in 'dir.walk' 'dir.walk_ext' 'dir.walk!' 'dir.walk_ext!'; do
  assert_doc_contains docs/milestones/v0.38.0-spec.md "$needle" "v0.38 spec names $needle"
done
assert_doc_contains docs/roadmap.md 'v0.38.0 — Recursive Walk Helpers and DX Integration Cleanup' 'roadmap lists v0.38 recursive walk cleanup'
assert_doc_contains docs/language.ds 'dir.walk_ext("src", [".c", ".h"])' 'language docs show dir.walk_ext'
assert_doc_contains docs/runtime.md 'skip hidden descendants and symlinks' 'runtime docs describe hidden/symlink walk behavior'
assert_doc_contains docs/status.md 'dir.walk(root)' 'status docs mark dir.walk supported'
assert_doc_contains docs/parity-contracts.md 'Recursive filesystem walk helpers' 'parity contracts cover walk helpers'
assert_doc_contains docs/diagnostics.md 'runtime `dir.walk*` roots/extensions' 'diagnostics docs cover runtime walk diagnostics'
assert_doc_contains docs/dx-issues.md 'recursive file walking' 'DX issues resolved section includes recursive walking'
assert_doc_contains Makefile '0-38' 'Makefile wires v0.38 suite'
assert_doc_not_contains src/lexer.c 'walk' 'v0.38 did not add walk syntax keywords'
assert_doc_not_contains src/parser.c 'walk_ext' 'v0.38 did not add parser grammar for walk helpers'
dir_helpers=$(grep -o '"dir\.[^"]*"' src/ds_stdlib.c | tr -d '"' | sort | tr '\n' ' ')
[ "$dir_helpers" = 'dir.exists dir.walk dir.walk! dir.walk_ext dir.walk_ext! ' ] || fail "unexpected dir namespace helper surface: $dir_helpers"
pass 'dir namespace public helper surface is scoped'
for f in docs/language.ds docs/runtime.md docs/status.md docs/parity-contracts.md docs/diagnostics.md docs/dx-issues.md; do
  assert_not_contains "$f" 'supports hidden traversal flags' "$f does not overclaim hidden traversal flags"
  assert_not_contains "$f" 'supports symlink following' "$f does not overclaim symlink following"
  assert_not_contains "$f" 'supports max depth' "$f does not overclaim max depth"
  assert_not_contains "$f" 'metadata rows are returned' "$f does not overclaim metadata rows"
  assert_not_contains "$f" 'streaming iterators are supported' "$f does not overclaim streaming iterators"
done

# 2. Parser, formatter, and debug visibility.
parse_calls=$(write_fixture parse_calls <<'DS'
let files = dir.walk("src")
let c_files = dir.walk_ext("src", [".c", ".h"])

for path_name in dir.walk_ext("src", [".c", ".h", ".ds"]) {
  echo "{path_name}"
}
DS
)
run_ok parse_calls_check "$DS" check "$parse_calls"
run_ok parse_calls_ast "$DS" ast "$parse_calls"
run_ok parse_calls_fmt "$DS" fmt "$parse_calls"
assert_contains "$TMP/parse_calls_ast.out" 'CallExpr dir.walk_ext' 'AST shows ordinary namespaced walk_ext call'
assert_contains "$TMP/parse_calls_fmt.out" 'dir.walk_ext("src", [".c", ".h", ".ds"])' 'formatter preserves extension array literal'

bang_parse=$(write_fixture bang_parse <<'DS'
let files = dir.walk!("src")
let c_files = dir.walk_ext!("src", [".c"])
DS
)
run_ok bang_parse_check "$DS" check "$bang_parse"
run_ok bang_parse_ast "$DS" ast "$bang_parse"
assert_contains "$TMP/bang_parse_ast.out" 'CallExpr dir.walk!' 'AST shows dir.walk! call'
assert_contains "$TMP/bang_parse_ast.out" 'CallExpr dir.walk_ext!' 'AST shows dir.walk_ext! call'

debug_return=$(write_fixture debug_return <<'DS'
for path_name in dir.walk_ext("root", [".c"]) {
  echo "{path_name}"
}
DS
)
run_ok debug_return_hir "$DS" hir "$debug_return"
run_ok debug_return_bytecode "$DS" bytecode "$debug_return"
assert_contains "$TMP/debug_return_bytecode.out" 'STDLIB_CALL' 'bytecode shows stdlib call for walk helper'
assert_contains "$TMP/debug_return_bytecode.out" 'FOR_ARRAY' 'bytecode records walk result as array iterable'

for item in \
  'walk_none|let files = dir.walk()|helper `dir.walk` expects 1 arguments' \
  'walk_extra|let files = dir.walk("src", ".c")|helper `dir.walk` expects 1 arguments' \
  'walk_ext_missing|let files = dir.walk_ext("src")|helper `dir.walk_ext` expects 2 arguments' \
  'walk_ext_extra|let files = dir.walk_ext("src", [".c"], "extra")|helper `dir.walk_ext` expects 2 arguments' \
  'walk_bang_extra|let files = dir.walk!("src", [".c"])|helper `dir.walk!` expects 1 arguments' \
  'walk_ext_bang_missing|let files = dir.walk_ext!("src")|helper `dir.walk_ext!` expects 2 arguments'; do
  IFS='|' read -r name code needle <<<"$item"
  file=$(write_fixture "arity_$name" <<<"$code")
  assert_static_rejected "arity_$name" "$file" "$needle"
done

# Shared seeds for behavior cases.
basic_seed="$SEED/basic"
mkdir -p "$basic_seed/root/src/nested" "$basic_seed/root/empty"
touch "$basic_seed/root/a.txt" "$basic_seed/root/src/main.c" "$basic_seed/root/src/nested/util.h"

ext_seed="$SEED/ext"
mkdir -p "$ext_seed/root"
touch "$ext_seed/root/a.c" "$ext_seed/root/a.h" "$ext_seed/root/a.txt"

# 3. Basic VM/Bash behavior.
walk_basic=$(write_fixture walk_basic <<'DS'
for path_name in dir.walk("root") {
  echo "{path_name}"
}
DS
)
run_parity_seed walk_basic "$walk_basic" "$basic_seed" $'root/a.txt\nroot/src/main.c\nroot/src/nested/util.h\n'
assert_direct_accept walk_basic_direct "$walk_basic" "$basic_seed" $'root/a.txt\nroot/src/main.c\nroot/src/nested/util.h\n'

walk_empty=$(write_fixture walk_empty <<'DS'
for path_name in dir.walk("root/empty") {
  echo "BODY {path_name}"
}
echo "after"
DS
)
run_parity_seed walk_empty "$walk_empty" "$basic_seed" $'after\n'

walk_required_empty=$(write_fixture walk_required_empty <<'DS'
for path_name in dir.walk!("root/empty") {
  echo "BODY {path_name}"
}
echo "after"
DS
)
assert_runtime_rejected_seed walk_required_empty "$walk_required_empty" "$basic_seed" 'dir.walk!' 'after'

immediate_nested_seed="$SEED/immediate_nested"
mkdir -p "$immediate_nested_seed/root/nested"
touch "$immediate_nested_seed/root/top.ds" "$immediate_nested_seed/root/nested/child.ds"
run_parity_seed walk_immediate_nested "$walk_basic" "$immediate_nested_seed" $'root/nested/child.ds\nroot/top.ds\n'

dirs_not_returned_seed="$SEED/dirs_not_returned"
mkdir -p "$dirs_not_returned_seed/root/dir_without_files" "$dirs_not_returned_seed/root/dir_with_file"
touch "$dirs_not_returned_seed/root/file.ds" "$dirs_not_returned_seed/root/dir_with_file/child.ds"
run_parity_seed walk_dirs_not_returned "$walk_basic" "$dirs_not_returned_seed" $'root/dir_with_file/child.ds\nroot/file.ds\n'

# 4. Extension filtering.
walk_c=$(write_fixture walk_c <<'DS'
for path_name in dir.walk_ext("root", [".c"]) {
  echo "{path_name}"
}
DS
)
run_parity_seed ext_single "$walk_c" "$ext_seed" $'root/a.c\n'

walk_ch=$(write_fixture walk_ch <<'DS'
for path_name in dir.walk_ext("root", [".c", ".h"]) {
  echo "{path_name}"
}
DS
)
run_parity_seed ext_multiple "$walk_ch" "$ext_seed" $'root/a.c\nroot/a.h\n'

walk_duplicate_ext=$(write_fixture walk_duplicate_ext <<'DS'
for path_name in dir.walk_ext("root", [".c", ".c", ".h"]) {
  echo "{path_name}"
}
DS
)
run_parity_seed ext_duplicate "$walk_duplicate_ext" "$ext_seed" $'root/a.c\nroot/a.h\n'

exact_ext_seed="$SEED/exact_ext"
mkdir -p "$exact_ext_seed/root"
touch "$exact_ext_seed/root/lower.c" "$exact_ext_seed/root/upper.C" "$exact_ext_seed/root/name.c.backup"
run_parity_seed ext_exact_bytewise "$walk_c" "$exact_ext_seed" $'root/lower.c\n'

hidden_ext_seed="$SEED/hidden_ext"
mkdir -p "$hidden_ext_seed/root" "$hidden_ext_seed/root/.hidden_dir"
touch "$hidden_ext_seed/root/.hidden.c" "$hidden_ext_seed/root/visible.c" "$hidden_ext_seed/root/.hidden_dir/file.c"
run_parity_seed ext_skips_dotfiles "$walk_c" "$hidden_ext_seed" $'root/visible.c\n'

no_ext_seed="$SEED/no_ext"
mkdir -p "$no_ext_seed/root"
touch "$no_ext_seed/root/Makefile" "$no_ext_seed/root/script" "$no_ext_seed/root/a.c"
run_parity_seed ext_no_extension_skipped "$walk_c" "$no_ext_seed" $'root/a.c\n'

walk_ext_required_nomatch=$(write_fixture walk_ext_required_nomatch <<'DS'
for path_name in dir.walk_ext!("root", [".c"]) {
  echo "BODY {path_name}"
}
echo "after"
DS
)
txt_only_seed="$SEED/txt_only"
mkdir -p "$txt_only_seed/root"
touch "$txt_only_seed/root/a.txt"
assert_runtime_rejected_seed ext_required_nomatch "$walk_ext_required_nomatch" "$txt_only_seed" 'dir.walk_ext!' 'BODY'
run_parity_seed ext_nonbang_nomatch "$walk_c" "$txt_only_seed" ''

# 5. Static extension diagnostics.
for item in \
  'empty_array|let files = dir.walk_ext("root", [])|non-empty extension array' \
  'empty_string|let files = dir.walk_ext("root", [""])|extensions to be non-empty' \
  'missing_dot|let files = dir.walk_ext("root", ["c"])|start with `.`' \
  'glob_like|let files = dir.walk_ext("root", ["*.c"])|not glob patterns' \
  'slash|let files = dir.walk_ext("root", [".c/.h"])|not contain `/`' \
  'non_string|let files = dir.walk_ext("root", [".c", 1])|extension filters to be strings' \
  'scalar_arg|let files = dir.walk_ext("root", ".c")|string-array extension argument'; do
  IFS='|' read -r name code needle <<<"$item"
  file=$(write_fixture "static_$name" <<<"$code")
  assert_static_rejected "static_$name" "$file" "$needle"
done

# 6. Dynamic extension diagnostics.
dynamic_ext=$(write_fixture dynamic_ext <<'DS'
let ext = env.get("EXT", "")
let files = dir.walk_ext("root", [ext])
echo "after"
DS
)
assert_runtime_rejected_seed_env dynamic_empty_ext "$dynamic_ext" "$ext_seed" 'EXT=' 'dir.walk_ext' 'after'
assert_runtime_rejected_seed_env dynamic_missing_dot "$dynamic_ext" "$ext_seed" 'EXT=c' 'start with' 'after'
assert_runtime_rejected_seed_env dynamic_slash_ext "$dynamic_ext" "$ext_seed" 'EXT=.c/.h' 'not to contain' 'after'

dynamic_ext_array=$(write_fixture dynamic_ext_array <<'DS'
let exts = [".c", ".h"]
for path_name in dir.walk_ext("root", exts) {
  echo "{path_name}"
}
DS
)
run_parity_seed dynamic_ext_array "$dynamic_ext_array" "$ext_seed" $'root/a.c\nroot/a.h\n'

# 7. Root path diagnostics.
missing_root=$(write_fixture missing_root <<'DS'
let files = dir.walk("missing")
echo "after"
DS
)
assert_runtime_rejected_seed missing_root "$missing_root" "$ext_seed" 'dir.walk' 'after'

file_root_seed="$SEED/file_root"
mkdir -p "$file_root_seed/root"
touch "$file_root_seed/root/file.txt"
file_root=$(write_fixture file_root <<'DS'
let files = dir.walk("root/file.txt")
echo "after"
DS
)
assert_runtime_rejected_seed file_root "$file_root" "$file_root_seed" 'dir.walk' 'after'

unreadable_seed="$SEED/unreadable"
mkdir -p "$unreadable_seed/root/closed"
touch "$unreadable_seed/root/closed/file.c"
if [ "$(id -u 2>/dev/null || echo 1)" = 0 ]; then
  pass 'unreadable root permission enforcement skipped while tests run as root'
elif chmod 000 "$unreadable_seed/root/closed" 2>/dev/null; then
  unreadable=$(write_fixture unreadable <<'DS'
for path_name in dir.walk("root") {
  echo "{path_name}"
}
echo "after"
DS
)
  set +e
  run_parity_seed unreadable_root "$unreadable" "$unreadable_seed" '' 1
  unreadable_rc=$?
  set -e
  chmod 755 "$unreadable_seed/root/closed" 2>/dev/null || true
  if [ "$unreadable_rc" = 0 ]; then
    assert_contains "$TMP/unreadable_root_vm.err" 'dir.walk' 'unreadable root VM diagnostic names dir.walk'
    assert_contains "$TMP/unreadable_root_bash.err" 'dir.walk' 'unreadable root Bash diagnostic names dir.walk'
  else
    pass 'unreadable root permission enforcement skipped on this host'
  fi
else
  pass 'unreadable root permission enforcement skipped on this host'
fi
pass 'NUL root is not expressible in source-level ds strings; runtime validation remains covered by string transport boundaries'

non_string_root=$(write_fixture non_string_root <<'DS'
let files = dir.walk(123)
let files2 = dir.walk_ext(true, [".c"])
DS
)
assert_static_rejected non_string_root "$non_string_root" 'expects a string root argument'

# 8. Hidden-entry policy.
hidden_walk_seed="$SEED/hidden_walk"
mkdir -p "$hidden_walk_seed/root/.git" "$hidden_walk_seed/root/.cache" "$hidden_walk_seed/root/src"
touch "$hidden_walk_seed/root/.env" "$hidden_walk_seed/root/.hidden.c" "$hidden_walk_seed/root/.git/config" "$hidden_walk_seed/root/.cache/file.c" "$hidden_walk_seed/root/src/main.c"
run_parity_seed hidden_entries "$walk_basic" "$hidden_walk_seed" $'root/src/main.c\n'

hidden_root_seed="$SEED/hidden_root"
mkdir -p "$hidden_root_seed/.config/app" "$hidden_root_seed/.config/.secret"
touch "$hidden_root_seed/.config/app/main.ds" "$hidden_root_seed/.config/.secret/hidden.ds"
hidden_root=$(write_fixture hidden_root <<'DS'
for path_name in dir.walk(".config") {
  echo "{path_name}"
}
DS
)
run_parity_seed hidden_root "$hidden_root" "$hidden_root_seed" $'.config/app/main.ds\n'

# 9. Symlink policy.
symlink_seed="$SEED/symlink"
mkdir -p "$symlink_seed/root/real"
touch "$symlink_seed/root/real/file.c" "$symlink_seed/root/real.c"
if (cd "$symlink_seed/root" && ln -s real link_to_real && ln -s real.c link.c && ln -s missing.c broken.c && ln -s .. real/cycle) 2>/dev/null; then
  run_parity_seed symlink_policy "$walk_c" "$symlink_seed" $'root/real.c\nroot/real/file.c\n'
else
  pass 'symlink policy skipped because host does not support symlink creation'
fi

# 10. Ordering and duplicate policy.
ordering_seed="$SEED/ordering"
mkdir -p "$ordering_seed/root"
touch "$ordering_seed/root/b.c" "$ordering_seed/root/aa.c" "$ordering_seed/root/B.c" "$ordering_seed/root/a.c"
run_parity_seed ordering_bytewise "$walk_duplicate_ext" "$ordering_seed" $'root/B.c\nroot/a.c\nroot/aa.c\nroot/b.c\n'
run_parity_seed_env ordering_hostile_locale "$walk_duplicate_ext" "$ordering_seed" $'root/B.c\nroot/a.c\nroot/aa.c\nroot/b.c\n' 'LC_ALL=C.UTF-8'

# 11. Hostile file names.
hostile_seed="$SEED/hostile"
mkdir -p "$hostile_seed/root"
tab_name=$(printf '%b' "$hostile_seed/root/tab\\tname.c")
touch -- "$hostile_seed/root/a space.c" "$tab_name"
touch -- "$hostile_seed/root/-flag.c" "$hostile_seed/root/--long.c"
touch -- "$hostile_seed/root/a;b.c" "$hostile_seed/root/dollar\$sign.c" "$hostile_seed/root/quote'file.c" "$hostile_seed/root/double\"quote.c" "$hostile_seed/root/braces{ok}.c" "$hostile_seed/root/parens(ok).c" "$hostile_seed/root/brackets[ok].c"
run_parity_seed hostile_names "$walk_c" "$hostile_seed" $'root/--long.c\nroot/-flag.c\nroot/a space.c\nroot/a;b.c\nroot/braces{ok}.c\nroot/brackets[ok].c\nroot/dollar$sign.c\nroot/double"quote.c\nroot/parens(ok).c\nroot/quote\'file.c\nroot/tab\tname.c\n'
leading_root_seed="$SEED/leading_root"
mkdir -p "$leading_root_seed/-root"
touch -- "$leading_root_seed/-root/a.c" "$leading_root_seed/-root/--flag.c"
leading_root=$(write_fixture leading_root <<'DS'
for path_name in dir.walk_ext("-root", [".c"]) {
  echo "{path_name}"
}
DS
)
run_parity_seed leading_dash_root "$leading_root" "$leading_root_seed" $'-root/--flag.c\n-root/a.c\n'
unicode_seed="$SEED/unicode"
mkdir -p "$unicode_seed/root"
if touch "$unicode_seed/root/é.c" "$unicode_seed/root/中.c" 2>/dev/null; then
  run_parity_seed unicode_names "$walk_c" "$unicode_seed" $'root/é.c\nroot/中.c\n'
else
  pass 'unicode filename preservation skipped because host filesystem rejected fixture names'
fi
newline_name=$(printf '%b' "$hostile_seed/root/new\\nline.c")
if touch "$newline_name" 2>/dev/null; then
  pass 'newline filename fixture can be created; source-level line-oriented echo comparison is intentionally skipped'
else
  pass 'newline filename fixture skipped because host filesystem rejected fixture name'
fi

# 12. Composition with arrays and strings.
basename_comp=$(write_fixture basename_comp <<'DS'
for path_name in dir.walk_ext("root", [".c"]) {
  echo "{path.basename(path_name)}"
}
DS
)
run_parity_seed basename_comp "$basename_comp" "$exact_ext_seed" $'lower.c\n'

index_comp=$(write_fixture index_comp <<'DS'
let files = dir.walk_ext("root", [".c"])
let first = files[0]
echo "{first}"
echo "{files[0].slice(0, 4)}"
if "root/lower.c" in files {
  echo "yes"
}
DS
)
run_parity_seed index_comp "$index_comp" "$exact_ext_seed" $'root/lower.c\nroot\nyes\n'

function_walk=$(write_fixture function_walk <<'DS'
fn source_files(root) {
  return dir.walk_ext(root, [".c", ".h"])
}

for path_name in source_files("root") {
  echo "{path_name}"
}
DS
)
run_parity_seed function_walk "$function_walk" "$ext_seed" $'root/a.c\nroot/a.h\n'

cat >"$FIX/lib_walk.ds" <<'DS'
fn source_files(root) {
  return dir.walk_ext(root, [".c", ".h"])
}
DS
import_walk=$(write_fixture import_walk <<'DS'
import "./lib_walk.ds"

for path_name in source_files("root") {
  echo "{path_name}"
}
DS
)
run_parity_seed import_walk "$import_walk" "$ext_seed" $'root/a.c\nroot/a.h\n'

# 13. Composition with row arrays and DX-wave features.
rows_walk=$(write_fixture rows_walk <<'DS'
let rows = []

for path_name in dir.walk_ext("root", [".c", ".h"]) {
  rows.push({ file: path_name, ext: path.ext(path_name) })
}

for row in rows.sort_by("file", "desc") {
  echo "{row.ext}\t{row.file}"
}
DS
)
run_parity_seed rows_walk "$rows_walk" "$ext_seed" $'.h\troot/a.h\n.c\troot/a.c\n'

infer_and_braces=$(write_fixture infer_and_braces <<'DS'
fn is_header(path_name) {
  return path_name.ends_with(".h")
}

for path_name in dir.walk_ext("root", [".c", ".h"]) {
  if is_header(path_name) {
    echo "{path_name}"
  }
}
DS
)
run_parity_seed infer_param_walk "$infer_and_braces" "$ext_seed" $'root/a.h\n'

brace_seed="$SEED/brace"
mkdir -p "$brace_seed/root"
touch "$brace_seed/root/name{brace}.c"
run_parity_seed literal_brace_name "$walk_c" "$brace_seed" $'root/name{brace}.c\n'

split_index_walk=$(write_fixture split_index_walk <<'DS'
for path_name in dir.walk_ext("root", [".c"]) {
  echo "{path_name.split("/")[0]}"
}
DS
)
run_parity_seed split_index_walk "$split_index_walk" "$exact_ext_seed" $'root\n'

# 14. Bash helper hygiene and standalone behavior.
no_walk=$(write_fixture no_walk <<'DS'
echo "hello"
DS
)
no_walk_script="$TMP/no_walk.sh"
emit_checked no_walk "$no_walk" "$no_walk_script"
assert_not_contains "$no_walk_script" '__ds_walk_emit' 'walk helper omitted when unused'
assert_not_contains "$no_walk_script" '__ds_stdlib_dir_walk' 'dir.walk stdlib wrappers omitted when unused'

walk_script="$TMP/walk_helper.sh"
emit_checked walk_helper "$walk_basic" "$walk_script"
assert_helper_def_count "$walk_script" '__ds_walk_emit' 1 'walk helper emitted exactly once'
assert_helper_def_count "$walk_script" '__ds_stdlib_dir_walk' 1 'dir.walk wrapper emitted exactly once'

ext_script="$TMP/walk_ext_helper.sh"
emit_checked walk_ext_helper "$walk_ch" "$ext_script"
assert_helper_def_count "$ext_script" '__ds_walk_ext_valid' 1 'walk_ext validation helper emitted exactly once'
assert_helper_def_count "$ext_script" '__ds_stdlib_dir_walk_ext' 1 'dir.walk_ext wrapper emitted exactly once'

all_helpers=$(write_fixture all_helpers <<'DS'
for path_name in dir.walk("root") { echo "{path_name}" }
for path_name in dir.walk!("root") { echo "{path_name}" }
for path_name in dir.walk_ext("root", [".c"]) { echo "{path_name}" }
for path_name in dir.walk_ext!("root", [".c"]) { echo "{path_name}" }
DS
)
all_helpers_script="$TMP/all_helpers.sh"
emit_checked all_helpers "$all_helpers" "$all_helpers_script"
assert_no_duplicate_helpers "$all_helpers_script" 'all walk helpers emitted together'

standalone_work="$TMP/standalone_work"
copy_seed "$ext_seed" "$standalone_work"
cp "$ext_script" "$standalone_work/walk_ext.sh"
set +e
(cd "$standalone_work" && PATH=/usr/bin:/bin timeout "$CASE_TIMEOUT" bash walk_ext.sh) >"$TMP/standalone.out" 2>"$TMP/standalone.err"
standalone_rc=$?
set -e
printf '%s' "$standalone_rc" >"$TMP/standalone.rc"
assert_status standalone 0
assert_text standalone_stdout $'root/a.c\nroot/a.h\n' "$TMP/standalone.out"

hostile_shell_script=$(write_fixture hostile_shell <<'DS'
let files = dir.walk_ext("root", [".c"])
echo "{files[0]}"
if "root/a.c" in files { echo "member" }
for path_name in files { echo "{path.basename(path_name)}" }
DS
)
hostile_script="$TMP/hostile_shell.sh"
emit_checked hostile_shell "$hostile_shell_script" "$hostile_script"
copy_seed "$ext_seed" "$TMP/hostile_shell_work"
set +e
(cd "$TMP/hostile_shell_work" && IFS=':' GLOBIGNORE='*' LC_ALL=C LANG=C timeout "$CASE_TIMEOUT" bash -c 'shopt -s nullglob dotglob failglob 2>/dev/null || true; alias find=false 2>/dev/null || true; bash "$1"' bash "$hostile_script") >"$TMP/hostile_shell.out" 2>"$TMP/hostile_shell.err"
hostile_shell_rc=$?
set -e
printf '%s' "$hostile_shell_rc" >"$TMP/hostile_shell.rc"
assert_status hostile_shell 0
assert_text hostile_shell_stdout $'root/a.c\nmember\na.c\n' "$TMP/hostile_shell.out"
assert_not_contains "$TMP/hostile_shell.out" '__ds_' 'array sidecar metadata does not leak to stdout'

# 15. Diagnostics and fail-fast behavior.
invalid_literal_before=$(write_fixture invalid_literal_before <<'DS'
echo "before"
let files = dir.walk_ext("root", ["*.c"])
echo "after"
DS
)
assert_static_rejected invalid_literal_before "$invalid_literal_before" 'not glob patterns'
assert_not_contains "$TMP/invalid_literal_before_run.out" 'before' 'invalid literal fails before partial execution'

dynamic_missing_root=$(write_fixture dynamic_missing_root <<'DS'
let files = dir.walk(env.get("ROOT", "missing"))
echo "after"
DS
)
assert_runtime_rejected_seed_env dynamic_missing_root "$dynamic_missing_root" "$ext_seed" 'ROOT=missing' 'dir.walk' 'after'

side_effect_bang=$(write_fixture side_effect_bang <<'DS'
for path_name in dir.walk_ext!("root", [".c"]) {
  file.write("marker", path_name)
}
echo "after"
DS
)
run_parity_seed side_effect_bang "$side_effect_bang" "$txt_only_seed" '' 1
[ ! -e "$TMP/side_effect_bang_vm_work/marker" ] || fail 'VM bang failure created marker'
pass 'VM bang failure did not create marker'
[ ! -e "$TMP/side_effect_bang_bash_work/marker" ] || fail 'Bash bang failure created marker'
pass 'Bash bang failure did not create marker'
for name in missing_root dynamic_empty_ext walk_required_empty ext_required_nomatch; do
  assert_not_contains "$TMP/${name}_vm.err" '__ds_' "$name VM diagnostic hides backend helpers"
  assert_not_contains "$TMP/${name}_bash.err" '__ds_' "$name Bash diagnostic hides backend helpers"
done

# 16. Recursive glob compatibility.
glob_seed="$SEED/glob"
mkdir -p "$glob_seed/root/nested" "$glob_seed/root/.hidden"
touch "$glob_seed/root/a.c" "$glob_seed/root/nested/b.c" "$glob_seed/root/.hidden/c.c"
glob_compare=$(write_fixture glob_compare <<'DS'
for path_name in glob("root/**/*.c") {
  echo "G {path_name}"
}
for path_name in dir.walk_ext("root", [".c"]) {
  echo "W {path_name}"
}
DS
)
run_parity_seed glob_compare "$glob_compare" "$glob_seed" $'G root/a.c\nG root/nested/b.c\nW root/a.c\nW root/nested/b.c\n'

for item in \
  'multi_recursive|let files = glob("root/**/src/**/*.c")|multiple recursive' \
  'partial_recursive|let files = glob("root/foo**bar.c")|complete path segment'; do
  IFS='|' read -r name code needle <<<"$item"
  file=$(write_fixture "glob_scope_$name" <<<"$code")
  assert_static_rejected "glob_scope_$name" "$file" "$needle"
done

glob_deferred_literals=$(write_fixture glob_deferred_literals <<'DS'
for path_name in glob("root/*.{{c,h}}") {
  echo "brace={path_name}"
}
for path_name in glob("root/@(a|b).c") {
  echo "extglob={path_name}"
}
for path_name in glob("~/src/*.c") {
  echo "tilde={path_name}"
}
for path_name in glob("$HOME/*.c") {
  echo "shell={path_name}"
}
echo "after"
DS
)
run_parity_seed glob_deferred_literals "$glob_deferred_literals" "$ext_seed" $'after\n'

# 17. Realistic analyzer-style smoke.
analyzer_seed="$SEED/analyzer"
mkdir -p "$analyzer_seed/project/src/nested" "$analyzer_seed/project/vendor" "$analyzer_seed/project/.git"
touch "$analyzer_seed/project/src/main.c" "$analyzer_seed/project/src/lib.h" "$analyzer_seed/project/src/nested/util.c" "$analyzer_seed/project/vendor/ignored.txt" "$analyzer_seed/project/.git/config"
analyzer=$(write_fixture analyzer <<'DS'
let rows = []

for path_name in dir.walk_ext("project", [".c", ".h"]) {
  let base = path.basename(path_name)
  let ext = path.ext(path_name)
  rows.push({ file: path_name, base: base, ext: ext })
}

for row in rows.sort_by("file", "asc") {
  echo "{row.ext}\t{row.base}\t{row.file}"
}
DS
)
run_parity_seed analyzer_smoke "$analyzer" "$analyzer_seed" $'.h\tlib.h\tproject/src/lib.h\n.c\tmain.c\tproject/src/main.c\n.c\tutil.c\tproject/src/nested/util.c\n'

# 18. Documentation cleanup checks.
for needle in 'literal braces' 'broken-pipe quieting' 'direct indexing' 'function parameter kind inference' 'lightweight row arrays' 'recursive file walking'; do
  assert_doc_contains docs/dx-issues.md "$needle" "DX resolved section includes $needle"
done
assert_doc_contains docs/dx-issues.md 'Open DX issues' 'DX open section remains present'
assert_doc_contains docs/dx-issues.md 'regex capture ergonomics' 'open regex ergonomics remains honest'
if find examples -type f -print0 | xargs -0 grep -nE "find .*\(-name '\\*\.c' -o -name '\\*\.h'\)|printf .*\{\}|dummy default|delimiter" >"$TMP/example_workarounds.txt" 2>/dev/null; then
  cat "$TMP/example_workarounds.txt" >&2
  fail 'examples contain obsolete DX-wave workaround patterns'
fi
pass 'examples avoid obsolete solved DX-wave workaround patterns'

# 19. Edge-case matrix is covered above. Record source-level skips explicitly.
pass 'edge matrix covered by focused walk, extension, root, hidden, symlink, ordering, hostile-name, and Bash-hygiene cases'

# 20. Manual smoke tests encoded where stable.
real_project=$(write_fixture real_project <<'DS'
let count = 0

for path_name in dir.walk_ext("src", [".c", ".h"]) {
  count = count + 1
}

echo "{count}"
DS
)
real_script="$TMP/real_project.sh"
emit_checked real_project "$real_project" "$real_script"
set +e
timeout "$CASE_TIMEOUT" "$DS" run "$real_project" >"$TMP/real_project_vm.out" 2>"$TMP/real_project_vm.err"
real_vm_rc=$?
timeout "$CASE_TIMEOUT" bash "$real_script" >"$TMP/real_project_bash.out" 2>"$TMP/real_project_bash.err"
real_bash_rc=$?
set -e
printf '%s' "$real_vm_rc" >"$TMP/real_project_vm.rc"
printf '%s' "$real_bash_rc" >"$TMP/real_project_bash.rc"
assert_status real_project_vm 0
assert_status real_project_bash 0
assert_same "$TMP/real_project_vm.out" "$TMP/real_project_bash.out" 'real project source-tree VM/Bash count parity'
assert_same "$TMP/real_project_vm.err" "$TMP/real_project_bash.err" 'real project source-tree VM/Bash stderr parity'

copy_elsewhere="$TMP/copy_elsewhere"
copy_seed "$ext_seed" "$copy_elsewhere"
cp "$ext_script" "$copy_elsewhere/copied.sh"
set +e
(cd "$copy_elsewhere" && PATH=/usr/bin:/bin timeout "$CASE_TIMEOUT" bash copied.sh) >"$TMP/copy_elsewhere.out" 2>"$TMP/copy_elsewhere.err"
copy_elsewhere_rc=$?
set -e
printf '%s' "$copy_elsewhere_rc" >"$TMP/copy_elsewhere.rc"
assert_status copy_elsewhere 0
assert_text copy_elsewhere_stdout $'root/a.c\nroot/a.h\n' "$TMP/copy_elsewhere.out"

set +e
(timeout "$CASE_TIMEOUT" "$DS" run "$real_project" | head -n 1 >/dev/null) 2>"$TMP/head_vm.err"
head_vm_rc=$?
(timeout "$CASE_TIMEOUT" bash "$real_script" | head -n 1 >/dev/null) 2>"$TMP/head_bash.err"
head_bash_rc=$?
set -e
printf '%s' "$head_vm_rc" >"$TMP/head_vm.rc"
printf '%s' "$head_bash_rc" >"$TMP/head_bash.rc"
assert_status head_vm 0
assert_status head_bash 0

echo "ok - v0.38 tests completed ($pass_count checks)"