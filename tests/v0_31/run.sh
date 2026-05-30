#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
DS="$ROOT/ds"
TMP=${TMPDIR:-/tmp}/ds_v0_31_tests.$$
FIX="$TMP/fixtures"
SEED="$TMP/seeds"
mkdir -p "$FIX" "$SEED"
trap 'rm -rf "$TMP"' EXIT

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
  local name="$1" text="$2"
  local path="$TMP/$name.expected"
  printf '%s' "$text" >"$path"
  printf '%s' "$path"
}

copy_seed() {
  local from="$1" to="$2"
  mkdir -p "$to"
  if [ -d "$from" ]; then
    (cd "$from" && tar cf - .) | (cd "$to" && tar xf -)
  fi
}

capture_cmd() {
  local name="$1"
  shift
  set +e
  "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/$name.rc"
}

assert_text() {
  local name="$1" expected="$2" actual="$3"
  local exp
  exp=$(write_expected "$name" "$expected")
  assert_same "$exp" "$actual" "$name"
}

assert_no_ds_call() {
  local script="$1" name="$2"
  assert_not_contains "$script" "$ROOT/ds" "$name omits repo ds path"
  assert_not_contains "$script" './ds ' "$name omits ./ds invocation"
  assert_not_contains "$script" ' ds run ' "$name omits ds run invocation"
  assert_not_contains "$script" ' ds emit ' "$name omits ds emit invocation"
}

emit_checked() {
  local name="$1" file="$2" script="$3"
  run_ok "${name}_emit" "$DS" emit bash "$file" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_no_ds_call "$script" "$name emitted Bash standalone"
}

run_parity_in_work() {
  local name="$1" file="$2" seed="$3" expected_stdout="$4" expected_status="${5:-0}"
  shift 5 || true
  local script="$TMP/$name.sh"
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  copy_seed "$seed" "$vm_work"
  copy_seed "$seed" "$bash_work"

  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"

  set +e
  (cd "$vm_work" && "$DS" run "$file" "$@") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && bash "$script" "$@") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name VM/Bash stdout parity"
  assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name VM/Bash stderr parity"
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
}

run_parity_capture() {
  local name="$1" file="$2" seed="$3" expected_status="${4:-0}"
  shift 4 || true
  local script="$TMP/$name.sh"
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  copy_seed "$seed" "$vm_work"
  copy_seed "$seed" "$bash_work"

  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"

  set +e
  (cd "$vm_work" && "$DS" run "$file" "$@") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && bash "$script" "$@") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name VM/Bash stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name VM/Bash stderr parity"
  else
    assert_contains "$TMP/${name}_vm.err" ': error:' "$name VM runtime diagnostic shape"
    assert_contains "$TMP/${name}_bash.err" ': error:' "$name Bash runtime diagnostic shape"
  fi
}

assert_runtime_failure_in_work() {
  local name="$1" file="$2" seed="$3" message="$4"
  shift 4 || true
  run_parity_capture "$name" "$file" "$seed" 1 "$@"
  assert_contains "$TMP/${name}_vm.err" "$message" "$name VM diagnostic message"
  assert_contains "$TMP/${name}_bash.err" "$message" "$name Bash diagnostic message"
}

assert_check_fails() {
  local name="$1" file="$2" needle="$3" marker="${4:-}"
  capture_cmd "${name}_check" "$DS" check "$file"
  assert_nonzero_status "${name}_check"
  assert_contains "$TMP/${name}_check.err" ': error:' "$name check diagnostic shape"
  assert_contains "$TMP/${name}_check.err" "$needle" "$name check diagnostic message"
  if [ -n "$marker" ]; then
    [ ! -e "$marker" ] || fail "$name created side-effect marker during check"
    pass "$name no side effects during check"
  fi
}

assert_emit_fails() {
  local name="$1" file="$2" needle="$3"
  local out="$TMP/$name.sh"
  capture_cmd "${name}_emit" "$DS" emit bash "$file" -o "$out"
  assert_nonzero_status "${name}_emit"
  assert_contains "$TMP/${name}_emit.err" ': error:' "$name emit diagnostic shape"
  assert_contains "$TMP/${name}_emit.err" "$needle" "$name emit diagnostic message"
  assert_file_missing_or_empty "$out" "$name failed emit leaves no valid artifact"
}

assert_rejected_literal() {
  local name="$1" file="$2" needle="$3" marker="${4:-}"
  assert_check_fails "$name" "$file" "$needle" "$marker"
  assert_emit_fails "$name" "$file" "$needle"
}

assert_repo_doc_contains() {
  local needle="$1" name="$2"
  grep -R -F -- "$needle" docs README.md >/dev/null || fail "$name: docs should contain [$needle]"
  pass "$name"
}

make_basic_seed() {
  local dir="$1"
  mkdir -p "$dir/src/nested" "$dir/src/nested2" "$dir/tests"
  : >"$dir/root.ds"
  : >"$dir/README.md"
  : >"$dir/src/main.ds"
  : >"$dir/src/helper.c"
  : >"$dir/src/nested/deep.ds"
  : >"$dir/src/nested/data.d"
  : >"$dir/src/nested/helper.c"
  : >"$dir/src/nested2/test-extra.ds"
  : >"$dir/tests/unit.ds"
}

# 1. Planning and docs checks.
for doc in \
  docs/milestones/v0.31.0-spec.md \
  docs/milestones/v0.31.0-test-plan.md \
  docs/roadmap.md \
  docs/status.md \
  docs/language.ds \
  docs/runtime.md \
  docs/parity-contracts.md \
  docs/diagnostics.md \
  docs/concept-map.md \
  docs/source-map.md; do
  [ -f "$doc" ] || fail "missing required doc $doc"
  pass "required doc exists: $doc"
done
assert_repo_doc_contains 'recursive glob' 'docs document recursive glob support'
assert_repo_doc_contains 'glob("src/**/*.c")' 'docs show recursive glob through glob'
assert_repo_doc_contains 'glob!' 'docs document required recursive glob through glob!'
assert_repo_doc_contains 'complete `**` path segment' 'docs document complete-segment `**` rule'
assert_repo_doc_contains 'one recursive `**` segment' 'docs document single recursive segment limit'
assert_repo_doc_contains 'zero-or-more directory' 'docs document zero-or-more directory semantics'
assert_repo_doc_contains 'sorted duplicate-free string results' 'docs document sorted duplicate-free strings'
assert_repo_doc_contains 'glob` no-match returns an empty collection' 'docs document glob no-match behavior'
assert_repo_doc_contains 'glob!` no-match fails' 'docs document glob! no-match behavior'
assert_repo_doc_contains 'hidden path components' 'docs document hidden path component policy'
assert_repo_doc_contains 'directory symlinks are not followed' 'docs document directory symlink policy'
assert_repo_doc_contains 'generated Bash remains standalone' 'docs document standalone Bash contract'
assert_repo_doc_contains 'ambient shell' 'docs document no ambient shell option reliance'
assert_repo_doc_contains 'custom glob flags' 'docs keep custom glob flags deferred'
assert_repo_doc_contains 'multiple recursive `**` glob segments' 'docs keep multiple recursive segments deferred'
assert_repo_doc_contains 'extglob' 'docs keep extglob deferred'
assert_repo_doc_contains 'brace expansion' 'docs keep brace expansion deferred'
assert_repo_doc_contains 'hidden traversal flags' 'docs keep hidden traversal flags deferred'
assert_repo_doc_contains 'symlink-following traversal' 'docs keep symlink following deferred'
assert_repo_doc_contains 'streaming' 'docs keep streaming iterators deferred'
assert_repo_doc_contains 'regex expansion' 'docs keep regex expansion deferred'

# 2. Parser, AST, HIR, bytecode, and formatting smoke tests.
smoke=$(write_fixture smoke <<'DS'
for file in glob("**/*.ds") {
  echo $file
}

for file in glob!("src/**/*.c") {
  echo $file
}
DS
)
run_ok smoke_tokens "$DS" tokens "$smoke"
assert_contains "$TMP/smoke_tokens.out" 'STRING         "\"**/*.ds\""' 'tokens preserve recursive pattern string'
run_ok smoke_ast "$DS" ast "$smoke"
assert_contains "$TMP/smoke_ast.out" 'CallExpr glob' 'AST shows glob call'
assert_contains "$TMP/smoke_ast.out" 'CallExpr glob!' 'AST shows glob! call'
run_ok smoke_fmt_check "$DS" fmt --check "$smoke"
cp "$smoke" "$TMP/smoke_fmt.ds"
run_ok smoke_fmt_write "$DS" fmt --write "$TMP/smoke_fmt.ds"
assert_contains "$TMP/smoke_fmt.ds" '**/*.ds' 'formatter preserves recursive glob pattern'
assert_contains "$TMP/smoke_fmt.ds" 'src/**/*.c' 'formatter preserves prefixed recursive glob pattern'

hir_fixture=$(write_fixture hir_smoke <<'DS'
let files = glob("**/*.ds")
for file in files {
  echo \$file
}
DS
)
run_ok hir_smoke "$DS" hir "$hir_fixture"
assert_contains "$TMP/hir_smoke.out" 'Call glob' 'HIR shows glob helper call'
assert_not_contains "$TMP/hir_smoke.out" '0x' 'HIR debug output pointer-free'
run_ok bytecode_smoke "$DS" bytecode "$hir_fixture"
assert_contains "$TMP/bytecode_smoke.out" 'STDLIB_CALL' 'bytecode includes stdlib call'
assert_not_contains "$TMP/bytecode_smoke.out" '0x' 'bytecode debug output pointer-free'

# 3. Basic recursive matching parity.
basic_seed="$SEED/basic"
make_basic_seed "$basic_seed"
root_recursive=$(write_fixture root_recursive <<'DS'
for file in glob("**/*.ds") {
  echo $file
}
DS
)
run_parity_in_work root_recursive "$root_recursive" "$basic_seed" $'root.ds\nsrc/main.ds\nsrc/nested/deep.ds\nsrc/nested2/test-extra.ds\ntests/unit.ds\n' 0

prefixed_recursive=$(write_fixture prefixed_recursive <<'DS'
for file in glob("src/**/*.ds") {
  echo $file
}
DS
)
run_parity_in_work prefixed_recursive "$prefixed_recursive" "$basic_seed" $'src/main.ds\nsrc/nested/deep.ds\nsrc/nested2/test-extra.ds\n' 0

filename_prefix=$(write_fixture filename_prefix <<'DS'
for file in glob("src/**/test-*.ds") {
  echo $file
}
DS
)
run_parity_in_work filename_prefix "$filename_prefix" "$basic_seed" $'src/nested2/test-extra.ds\n' 0

bracket_recursive=$(write_fixture bracket_recursive <<'DS'
for file in glob("src/**/*.[cd]") {
  echo $file
}
DS
)
run_parity_in_work bracket_recursive "$bracket_recursive" "$basic_seed" $'src/helper.c\nsrc/nested/data.d\nsrc/nested/helper.c\n' 0

explicit_dot=$(write_fixture explicit_dot <<'DS'
for file in glob("./**/*.ds") {
  echo $file
}
DS
)
run_parity_in_work explicit_dot "$explicit_dot" "$basic_seed" $'./root.ds\n./src/main.ds\n./src/nested/deep.ds\n./src/nested2/test-extra.ds\n./tests/unit.ds\n' 0

# 4. glob! required-match behavior.
required_success=$(write_fixture required_success <<'DS'
for file in glob!("src/**/*.c") {
  echo $file
}
DS
)
run_parity_in_work required_success "$required_success" "$basic_seed" $'src/helper.c\nsrc/nested/helper.c\n' 0

required_no_match=$(write_fixture required_no_match <<'DS'
for file in glob!("src/**/*.missing") {
  file.write("marker.txt", file)
}
echo "after"
DS
)
assert_runtime_failure_in_work required_no_match "$required_no_match" "$basic_seed" 'had no matches'
assert_not_contains "$TMP/required_no_match_vm.out" 'after' 'VM glob! no-match is fail-fast'
assert_not_contains "$TMP/required_no_match_bash.out" 'after' 'Bash glob! no-match is fail-fast'
[ ! -e "$TMP/required_no_match_vm_work/marker.txt" ] || fail 'VM glob! no-match executed loop body'
pass 'VM glob! no-match did not execute loop body side effect'
[ ! -e "$TMP/required_no_match_bash_work/marker.txt" ] || fail 'Bash glob! no-match executed loop body'
pass 'Bash glob! no-match did not execute loop body side effect'

optional_no_match=$(write_fixture optional_no_match <<'DS'
let count = 0
for file in glob("src/**/*.missing") {
  count += 1
}
echo $count
DS
)
run_parity_in_work optional_no_match "$optional_no_match" "$basic_seed" $'0\n' 0

# 5. Ordering, duplicate, locale, and ambient shell behavior.
order_seed="$SEED/order"
mkdir -p "$order_seed/src"
: >"$order_seed/A.ds"
: >"$order_seed/b.ds"
: >"$order_seed/a.ds"
: >"$order_seed/z.ds"
: >"$order_seed/src/A.ds"
: >"$order_seed/src/a.ds"
order_fixture=$(write_fixture order <<'DS'
for file in glob("**/*.ds") {
  echo $file
}
DS
)
run_parity_in_work order "$order_fixture" "$order_seed" $'A.ds\na.ds\nb.ds\nsrc/A.ds\nsrc/a.ds\nz.ds\n' 0
order_script="$TMP/order_hostile.sh"
emit_checked order_hostile "$order_fixture" "$order_script"
copy_seed "$order_seed" "$TMP/order_locale_work"
set +e
(cd "$TMP/order_locale_work" && LC_ALL=C bash "$order_script") >"$TMP/order_locale.out" 2>"$TMP/order_locale.err"
order_locale_rc=$?
set -e
printf '%s' "$order_locale_rc" >"$TMP/order_locale.rc"
assert_status order_locale 0
assert_text order_locale_stdout $'A.ds\na.ds\nb.ds\nsrc/A.ds\nsrc/a.ds\nz.ds\n' "$TMP/order_locale.out"
copy_seed "$order_seed" "$TMP/order_globstar_work"
set +e
(cd "$TMP/order_globstar_work" && bash -O globstar "$order_script") >"$TMP/order_globstar.out" 2>"$TMP/order_globstar.err"
order_globstar_rc=$?
set -e
printf '%s' "$order_globstar_rc" >"$TMP/order_globstar.rc"
assert_status order_globstar 0
assert_text order_globstar_stdout $'A.ds\na.ds\nb.ds\nsrc/A.ds\nsrc/a.ds\nz.ds\n' "$TMP/order_globstar.out"

# 6. Dotfile and hidden-directory behavior.
hidden_seed="$SEED/hidden"
mkdir -p "$hidden_seed/src/.hidden-dir" "$hidden_seed/.config/nested"
: >"$hidden_seed/visible.ds"
: >"$hidden_seed/.hidden.ds"
: >"$hidden_seed/src/visible.ds"
: >"$hidden_seed/src/.env"
: >"$hidden_seed/src/.hidden.ds"
: >"$hidden_seed/src/.hidden-dir/skipped.ds"
: >"$hidden_seed/.config/settings.json"
: >"$hidden_seed/.config/nested/deep.json"
hidden_default=$(write_fixture hidden_default <<'DS'
for file in glob("**/*.ds") {
  echo $file
}
DS
)
run_parity_in_work hidden_default "$hidden_default" "$hidden_seed" $'src/visible.ds\nvisible.ds\n' 0
assert_not_contains "$TMP/hidden_default_vm.out" '.hidden.ds' 'default recursive glob skips hidden files'
assert_not_contains "$TMP/hidden_default_vm.out" 'skipped.ds' 'default recursive glob skips hidden directories'

hidden_final=$(write_fixture hidden_final <<'DS'
for file in glob("**/.env") {
  echo $file
}
DS
)
run_parity_in_work hidden_final "$hidden_final" "$hidden_seed" $'src/.env\n' 0

hidden_prefix=$(write_fixture hidden_prefix <<'DS'
for file in glob(".config/**/*.json") {
  echo $file
}
DS
)
run_parity_in_work hidden_prefix "$hidden_prefix" "$hidden_seed" $'.config/nested/deep.json\n.config/settings.json\n' 0

# 7. Symlink behavior, skipped cleanly where unavailable.
symlink_seed="$SEED/symlink"
mkdir -p "$symlink_seed/real"
: >"$symlink_seed/real/file.ds"
if ln -s "real/file.ds" "$symlink_seed/linked-file.ds" 2>/dev/null && ln -s "real" "$symlink_seed/linked-dir" 2>/dev/null; then
  ln -s "cycle-b" "$symlink_seed/cycle-a" 2>/dev/null || true
  ln -s "cycle-a" "$symlink_seed/cycle-b" 2>/dev/null || true
  ln -s "missing-target.ds" "$symlink_seed/broken.ds" 2>/dev/null || true
  symlink_fixture=$(write_fixture symlink <<'DS'
for file in glob("**/*.ds") {
  echo $file
}
DS
)
  run_parity_capture symlink "$symlink_fixture" "$symlink_seed" 0
  assert_contains "$TMP/symlink_vm.out" 'linked-file.ds' 'recursive glob may return file symlink entries'
  assert_contains "$TMP/symlink_vm.out" 'real/file.ds' 'recursive glob returns real file'
  assert_not_contains "$TMP/symlink_vm.out" 'linked-dir/file.ds' 'recursive glob does not traverse directory symlink'
  assert_contains "$TMP/symlink_vm.out" 'broken.ds' 'recursive glob includes matching broken symlink entry'
else
  pass 'symlink setup unavailable; skipping symlink-specific recursive glob assertions'
fi

# 8. Path quoting and special names.
special_seed="$SEED/special"
mkdir -p "$special_seed/space dir" "$special_seed/-leading" "$special_seed/brackets [x]"
: >"$special_seed/space dir/file one.ds"
: >"$special_seed/-leading/-dash.ds"
: >"$special_seed/brackets [x]/literal.ds"
special_fixture=$(write_fixture special <<'DS'
for file in glob("**/*.ds") {
  echo $file
}
DS
)
run_parity_in_work special "$special_fixture" "$special_seed" $'-leading/-dash.ds\nbrackets [x]/literal.ds\nspace dir/file one.ds\n' 0

# 9. Dynamic pattern behavior.
dynamic_valid=$(write_fixture dynamic_valid <<'DS'
let suffix = "*.ds"
let pattern = "src/**/{suffix}"
for file in glob(pattern) {
  echo $file
}
DS
)
run_parity_in_work dynamic_valid "$dynamic_valid" "$basic_seed" $'src/main.ds\nsrc/nested/deep.ds\nsrc/nested2/test-extra.ds\n' 0

dynamic_invalid=$(write_fixture dynamic_invalid <<'DS'
let bad = "src/**/tests/**/*.ds"
for file in glob(bad) {
  echo $file
}
echo "after"
DS
)
run_ok dynamic_invalid_check "$DS" check "$dynamic_invalid"
assert_runtime_failure_in_work dynamic_invalid "$dynamic_invalid" "$basic_seed" 'multiple recursive `**` glob segments are unsupported'
assert_not_contains "$TMP/dynamic_invalid_vm.out" 'after' 'VM dynamic invalid recursive glob is fail-fast'
assert_not_contains "$TMP/dynamic_invalid_bash.out" 'after' 'Bash dynamic invalid recursive glob is fail-fast'

arg_pattern=$(write_fixture arg_pattern <<'DS'
script {
  arg pattern: string
}

for file in glob(pattern) {
  echo $file
}
DS
)
run_parity_in_work arg_pattern_valid "$arg_pattern" "$basic_seed" $'src/main.ds\nsrc/nested/deep.ds\nsrc/nested2/test-extra.ds\n' 0 'src/**/*.ds'
run_ok arg_pattern_check "$DS" check "$arg_pattern"
assert_runtime_failure_in_work arg_pattern_invalid "$arg_pattern" "$basic_seed" 'multiple recursive `**` glob segments are unsupported' 'src/**/tests/**/*.ds'

arg_required=$(write_fixture arg_required <<'DS'
script {
  arg pattern: string
}

for file in glob!(pattern) {
  echo $file
}
DS
)
assert_runtime_failure_in_work arg_required_no_match "$arg_required" "$basic_seed" 'had no matches' 'src/**/*.missing'

# 10. Literal invalid pattern diagnostics.
invalid_marker="$TMP/static-marker.txt"
for entry in \
  'partial_prefix|src/**file.ds|complete path segment' \
  'partial_middle|src/foo**bar.ds|complete path segment' \
  'partial_triple|src/***.ds|complete path segment' \
  'multiple_nested|src/**/tests/**/*.ds|multiple recursive `**` glob segments are unsupported' \
  'multiple_adjacent_path|src/**/**/file.ds|multiple recursive `**` glob segments are unsupported' \
  'multiple_only|**/**|multiple recursive `**` glob segments are unsupported'; do
  IFS='|' read -r name pattern needle <<<"$entry"
  fixture=$(write_fixture "bad_$name" <<DS
for file in glob("$pattern") {
  file.write("$invalid_marker", file)
}
DS
)
  assert_rejected_literal "bad_$name" "$fixture" "$needle" "$invalid_marker"
done

# 11. Existing non-recursive glob regression and helper selection.
shallow_seed="$SEED/shallow"
mkdir -p "$shallow_seed"
: >"$shallow_seed/a.txt"
: >"$shallow_seed/b.txt"
: >"$shallow_seed/c.ds"
: >"$shallow_seed/q1.txt"
nonrecursive=$(write_fixture nonrecursive <<'DS'
for file in glob("*.txt") {
  echo "txt={file}"
}
for file in glob!("*.ds") {
  echo "ds={file}"
}
for file in glob("q?.txt") {
  echo "q={file}"
}
DS
)
run_parity_in_work nonrecursive "$nonrecursive" "$shallow_seed" $'txt=a.txt\ntxt=b.txt\ntxt=q1.txt\nds=c.ds\nq=q1.txt\n' 0
nonrecursive_script="$TMP/nonrecursive.sh"
emit_checked nonrecursive_helper "$nonrecursive" "$nonrecursive_script"
assert_contains "$nonrecursive_script" '__ds_stdlib_glob()' 'non-recursive glob helper is emitted'
assert_not_contains "$nonrecursive_script" '__ds_glob_recursive()' 'recursive-only helper absent for non-recursive literal glob'

# 12. Collection and value-kind integration.
index_result=$(write_fixture index_result <<'DS'
let files = glob("src/**/*.ds")
let first = files[0]
echo $first
DS
)
run_parity_in_work index_result "$index_result" "$basic_seed" $'src/main.ds\n' 0

string_method=$(write_fixture string_method <<'DS'
for src_path in glob("src/**/*.ds") {
  if src_path.ends_with(".ds") {
    echo $src_path
  }
}
DS
)
run_parity_in_work string_method "$string_method" "$basic_seed" $'src/main.ds\nsrc/nested/deep.ds\nsrc/nested2/test-extra.ds\n' 0

function_return=$(write_fixture function_return <<'DS'
fn sources() {
  let files = glob("src/**/*.ds")
  return files
}

let files = sources()
for file in files {
  echo $file
}
DS
)
run_parity_in_work function_return "$function_return" "$basic_seed" $'src/main.ds\nsrc/nested/deep.ds\nsrc/nested2/test-extra.ds\n' 0

lib="$FIX/lib.ds"
cat >"$lib" <<'DS'
fn sources() {
  let files = glob("src/**/*.ds")
  return files
}
DS
import_main=$(write_fixture import_main <<DS
import "$lib"

let files = sources()
for file in files {
  echo \$file
}
DS
)
run_parity_in_work import_main "$import_main" "$basic_seed" $'src/main.ds\nsrc/nested/deep.ds\nsrc/nested2/test-extra.ds\n' 0

# 13. Interaction with control flow and test blocks.
control_seed="$SEED/control"
mkdir -p "$control_seed/src/nested"
: >"$control_seed/src/main.ds"
: >"$control_seed/src/skip.ds"
: >"$control_seed/src/nested/deep.ds"
control_flow=$(write_fixture control_flow <<'DS'
for src_path in glob("src/**/*.ds") {
  if src_path.contains("skip") {
    continue
  }
  echo $src_path
  break
}
DS
)
run_parity_in_work control_flow "$control_flow" "$control_seed" $'src/main.ds\n' 0

return_loop=$(write_fixture return_loop <<'DS'
fn first_source() {
  for file in glob("src/**/*.ds") {
    return file
  }
  return "none"
}

let first = first_source()
echo $first
DS
)
run_parity_in_work return_loop "$return_loop" "$basic_seed" $'src/main.ds\n' 0

test_block=$(write_fixture test_block <<'DS'
test "recursive glob finds sources" {
  let files = glob("src/**/*.ds")
  assert files[0] == "src/main.ds"
}
DS
)
copy_seed "$basic_seed" "$TMP/test_block_work"
run_ok test_block_check "$DS" check "$test_block"
run_ok test_block_run bash -c "cd '$TMP/test_block_work' && '$DS' test '$test_block'"
assert_contains "$TMP/test_block_run.out" 'ok   recursive glob finds sources' 'ds test uses recursive globs in test blocks'

# 14. Bash helper emission and hygiene.
recursive_script="$TMP/recursive_helper.sh"
emit_checked recursive_helper "$root_recursive" "$recursive_script"
assert_contains "$recursive_script" '__ds_glob_recursive()' 'recursive glob helper emitted when needed'
assert_contains "$recursive_script" '__ds_stdlib_glob()' 'stdlib glob helper emitted when recursive glob used'
no_glob=$(write_fixture no_glob <<'DS'
echo "no glob"
DS
)
no_glob_script="$TMP/no_glob.sh"
emit_checked no_glob "$no_glob" "$no_glob_script"
assert_not_contains "$no_glob_script" '__ds_glob_recursive()' 'recursive helper absent when no glob is used'
assert_not_contains "$no_glob_script" '__ds_stdlib_glob()' 'glob helper absent when no glob is used'

shell_option=$(write_fixture shell_option <<'DS'
for file in glob("**/*.ds") {
  echo $file
}
echo "literal ** remains data"
DS
)
shell_option_script="$TMP/shell_option.sh"
emit_checked shell_option "$shell_option" "$shell_option_script"
copy_seed "$basic_seed" "$TMP/shell_option_work"
set +e
(cd "$TMP/shell_option_work" && bash -O globstar "$shell_option_script") >"$TMP/shell_option.out" 2>"$TMP/shell_option.err"
shell_option_rc=$?
set -e
printf '%s' "$shell_option_rc" >"$TMP/shell_option.rc"
assert_status shell_option 0
assert_text shell_option_stdout $'root.ds\nsrc/main.ds\nsrc/nested/deep.ds\nsrc/nested2/test-extra.ds\ntests/unit.ds\nliteral ** remains data\n' "$TMP/shell_option.out"

# 15. Runtime filesystem failures. Skip as root because unreadable directories are still traversable.
perm_seed="$SEED/perm"
mkdir -p "$perm_seed/src/private"
: >"$perm_seed/src/main.ds"
: >"$perm_seed/src/private/secret.ds"
chmod 000 "$perm_seed/src/private" 2>/dev/null || true
if [ ! -r "$perm_seed/src/private" ] && [ "$(id -u)" != "0" ]; then
  perm_fixture=$(write_fixture perm_failure <<'DS'
for file in glob("src/**/*.ds") {
  file.write("marker.txt", file)
}
echo "after"
DS
)
  assert_runtime_failure_in_work perm_failure "$perm_fixture" "$perm_seed" 'failed to traverse recursive glob directory'
  assert_not_contains "$TMP/perm_failure_vm.out" 'after' 'VM traversal failure is fail-fast'
  assert_not_contains "$TMP/perm_failure_bash.out" 'after' 'Bash traversal failure is fail-fast'
else
  pass 'permission traversal failure skipped for root or permissive filesystem'
fi
chmod 700 "$perm_seed/src/private" 2>/dev/null || true

# 16. Diagnostics ownership checks.
malformed=$(write_fixture malformed_call <<'DS'
for file in glob("**/*.ds" {
  echo file
}
DS
)
capture_cmd malformed_call_check "$DS" check "$malformed"
assert_nonzero_status malformed_call_check
assert_contains "$TMP/malformed_call_check.err" 'expected' 'malformed helper syntax is parser-owned'

bad_arity=$(write_fixture bad_arity <<'DS'
let files = glob()
DS
)
assert_rejected_literal bad_arity "$bad_arity" 'expects 1 arguments'

bad_type=$(write_fixture bad_type <<'DS'
let files = glob(1)
DS
)
assert_rejected_literal bad_type "$bad_type" 'expects string arguments'

printf 'v0.31.0 tests passed (%d checks)\n' "$pass_count"
