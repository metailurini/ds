#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
DS="$ROOT/ds"
CASE_TIMEOUT=${DS_TEST_CASE_TIMEOUT:-30}
TMP=${TMPDIR:-/tmp}/ds_v0_37_tests.$$
FIX="$TMP/fixtures"
mkdir -p "$FIX"
if [[ "${DS_KEEP_TMP:-0}" != "1" ]]; then
  trap 'rm -rf "$TMP"' EXIT
fi

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != "1" ]]; then
  make -C "$ROOT" >/dev/null
fi


capture_cmd() {
  local name="$1"
  shift
  set +e
  timeout "$CASE_TIMEOUT" "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/$name.rc"
}

run_accept() {
  local name="$1" file="$2" expected_stdout="$3"
  shift 3
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  rm -rf "$vm_work" "$bash_work"
  mkdir -p "$vm_work" "$bash_work"

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

  assert_status "${name}_vm" 0
  assert_status "${name}_bash" 0
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name VM/Bash stdout parity"
  assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name VM/Bash stderr parity"
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
}

assert_failed_artifact_absent() {
  local path="$1" name="$2"
  [ ! -s "$path" ] || { echo "--- $path" >&2; cat "$path" >&2 || true; fail "$name: expected failed emit to leave no usable artifact"; }
  pass "$name failed emit artifact absent"
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
  assert_failed_artifact_absent "$script" "$name"
}

assert_runtime_rejected() {
  local name="$1" file="$2" needle="$3" marker="$4"
  local script="$TMP/$name.sh" vm_work="$TMP/${name}_vm_work" bash_work="$TMP/${name}_bash_work"
  rm -rf "$vm_work" "$bash_work"
  mkdir -p "$vm_work" "$bash_work"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"
  set +e
  (cd "$vm_work" && timeout "$CASE_TIMEOUT" "$DS" run "$file") >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  (cd "$bash_work" && timeout "$CASE_TIMEOUT" bash "$script") >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
  local bash_rc=$?
  set -e
  printf '%s' "$vm_rc" >"$TMP/${name}_vm.rc"
  printf '%s' "$bash_rc" >"$TMP/${name}_bash.rc"
  assert_nonzero_status "${name}_vm"
  assert_nonzero_status "${name}_bash"
  assert_contains "$TMP/${name}_vm.err" "$needle" "$name VM runtime diagnostic"
  assert_contains "$TMP/${name}_bash.err" "$needle" "$name Bash runtime diagnostic"
  assert_not_contains "$TMP/${name}_vm.out" "$marker" "$name VM stops before marker"
  assert_not_contains "$TMP/${name}_bash.out" "$marker" "$name Bash stops before marker"
}

assert_direct_accept() {
  local name="$1" file="$2" expected_stdout="$3"
  shift 3
  local direct_work="$TMP/${name}_direct_work"
  rm -rf "$direct_work"
  mkdir -p "$direct_work"
  set +e
  (cd "$direct_work" && timeout "$CASE_TIMEOUT" "$DS" "$file" "$@") >"$TMP/${name}_direct.out" 2>"$TMP/${name}_direct.err"
  local direct_rc=$?
  set -e
  printf '%s' "$direct_rc" >"$TMP/${name}_direct.rc"
  assert_status "${name}_direct" 0
  assert_text "${name}_direct_stdout" "$expected_stdout" "$TMP/${name}_direct.out"
}

assert_contains Makefile '0-37' 'Makefile wires v0.37 suite'

scope_classes=$(write_fixture scope_classes <<'DS'
class Row {}
DS
)
assert_static_rejected scope_classes "$scope_classes" 'unexpected'

scope_typed_schema=$(write_fixture scope_typed_schema <<'DS'
row Row { name: string }
DS
)
assert_static_rejected scope_typed_schema "$scope_typed_schema" 'unexpected'

scope_nested=$(write_fixture scope_nested <<'DS'
let rows = [{ name: "api", tags: ["web"] }]
DS
)
assert_static_rejected scope_nested "$scope_nested" 'must be a scalar'

scope_collection_param=$(write_fixture scope_collection_param <<'DS'
fn print_rows(rows) {
  for row in rows {
    echo row.name
  }
}
print_rows([{ name: "api", loc: 1 }])
DS
)
assert_static_rejected scope_collection_param "$scope_collection_param" 'passing collection values to functions'

scope_row_assign=$(write_fixture scope_row_assign <<'DS'
let row = { name: "api", loc: 1 }
row.loc = 2
DS
)
assert_static_rejected scope_row_assign "$scope_row_assign" 'field-style map assignment'

scope_custom_sort=$(write_fixture scope_custom_sort <<'DS'
fn by_loc(row) {
  return row.loc
}
let rows = [{ name: "api", loc: 1 }]
let sorted = rows.sort_by(by_loc)
DS
)
assert_static_rejected scope_custom_sort "$scope_custom_sort" 'sort_by field must be a string literal'

scope_json=$(write_fixture scope_json <<'DS'
let row = { name: "api", loc: 1 }
echo row.to_json()
DS
)
assert_static_rejected scope_json "$scope_json" 'unknown row field'

scope_cmd_result_field=$(write_fixture scope_cmd_result_field <<'DS'
let row = { name: "api", probe: run printf "ok" }
echo row.name
DS
)
assert_static_rejected scope_cmd_result_field "$scope_cmd_result_field" 'must be a scalar'

# 2. Parser, formatter, and debug visibility.
basic_parse=$(write_fixture basic_parse <<'DS'
let row = { file: "a.c", line: 1, name: "main", loc: 10 }
let rows = [row]
echo row.file
DS
)
run_accept basic_parse "$basic_parse" $'a.c\n'
assert_contains "$TMP/basic_parse_ast.out" 'ArrayExpr' 'AST remains useful for row arrays'
assert_contains "$TMP/basic_parse_hir.out" 'row_schema={file:string, line:int, name:string, loc:int}' 'HIR shows row schema'

fmt_file="$FIX/format_rows.ds"
cat >"$fmt_file" <<'DS'
let row={file:"a.c",line:1,name:"main",loc:10}
let rows=[{file:"b.c",line:2,name:"helper",loc:5}]
DS
capture_cmd fmt_before "$DS" fmt --check "$fmt_file"
assert_nonzero_status fmt_before
run_ok fmt_print "$DS" fmt "$fmt_file"
assert_contains "$TMP/fmt_print.out" 'let row = {file: "a.c", line: 1, name: "main", loc: 10}' 'formatter spaces row literal'
run_ok fmt_write "$DS" fmt --write "$fmt_file"
run_ok fmt_after "$DS" fmt --check "$fmt_file"
run_accept fmt_written "$fmt_file" $''

index_field=$(write_fixture index_field <<'DS'
let rows = [{ file: "a.c", line: 1, name: "main", loc: 10 }]
echo rows[0].name
echo "{rows[0]["file"]}"
DS
)
run_accept index_field "$index_field" $'main\na.c\n'
assert_contains "$TMP/index_field_ast.out" 'Word rows[0].name' 'AST preserves index-then-field command word'
assert_contains "$TMP/index_field_hir.out" 'Index array row_schema' 'HIR shows indexed row shape'

postfix_assign=$(write_fixture postfix_assign <<'DS'
let rows = [{ file: "a.c", line: 1, name: "main", loc: 10 }]
rows[0].loc = 20
DS
)
assert_static_rejected postfix_assign "$postfix_assign" 'field-style map assignment'

# 3. Row literals and schemas.
scalar_row=$(write_fixture scalar_row <<'DS'
let row = { file: "a.c", line: 1, name: "main", loc: 10, public: true }
echo "{row.file}:{row.line}:{row.name}:{row.loc}:{row.public}"
DS
)
run_accept scalar_row "$scalar_row" $'a.c:1:main:10:true\n'

field_ops=$(write_fixture field_ops <<'DS'
let row = { file: "a.c", line: 1, name: " main ", loc: 10, public: true }
echo "{row.file.upper()}"
echo "{row.loc + 5}"
if row.public {
  echo "{row.name.trim()}"
}
DS
)
run_accept field_ops "$field_ops" $'A.C\n15\nmain\n'

string_keys=$(write_fixture string_keys <<'DS'
let row = { "file": "a.c", "line": 1, "name": "main" }
echo "{row["file"]}"
echo row.name
DS
)
run_accept string_keys "$string_keys" $'a.c\nmain\n'

empty_field=$(write_fixture empty_field <<'DS'
let row = { "": "missing" }
echo "{row[""]}"
DS
)
assert_static_rejected empty_field "$empty_field" 'empty map keys'

nested_array=$(write_fixture nested_array <<'DS'
let row = { file: "a.c", spans: [1, 2] }
echo row.file
DS
)
assert_static_rejected nested_array "$nested_array" 'must be a scalar'

nested_map=$(write_fixture nested_map <<'DS'
let row = { file: "a.c", meta: { generated: false } }
echo row.file
DS
)
assert_static_rejected nested_map "$nested_map" 'must be a scalar'

missing_field=$(write_fixture missing_field <<'DS'
let row = { file: "a.c", line: 1 }
echo row.loc
DS
)
assert_static_rejected missing_field "$missing_field" 'unknown row field' 'loc'

# 4. Row-array literals.
row_array_basic=$(write_fixture row_array_basic <<'DS'
let rows = [
  { file: "a.c", line: 1, name: "main", loc: 10 },
  { file: "b.c", line: 8, name: "helper", loc: 3 }
]
for row in rows {
  echo "{row.file}:{row.line}:{row.name}:{row.loc}"
}
DS
)
run_accept row_array_basic "$row_array_basic" $'a.c:1:main:10\nb.c:8:helper:3\n'

field_order=$(write_fixture field_order <<'DS'
let rows = [
  { file: "a.c", line: 1, name: "main" },
  { name: "helper", file: "b.c", line: 2 }
]
for row in rows {
  echo "{row.file}:{row.line}:{row.name}"
}
DS
)
run_accept field_order "$field_order" $'a.c:1:main\nb.c:2:helper\n'

missing_literal=$(write_fixture missing_literal <<'DS'
let rows = [
  { file: "a.c", line: 1, loc: 10 },
  { file: "b.c", line: 2 }
]
DS
)
assert_static_rejected missing_literal "$missing_literal" 'row-array elements must have the same field names and scalar kinds'

extra_literal=$(write_fixture extra_literal <<'DS'
let rows = [
  { file: "a.c", line: 1 },
  { file: "b.c", line: 2, loc: 10 }
]
DS
)
assert_static_rejected extra_literal "$extra_literal" 'row-array elements must have the same field names and scalar kinds'

kind_literal=$(write_fixture kind_literal <<'DS'
let rows = [
  { file: "a.c", loc: 10 },
  { file: "b.c", loc: "10" }
]
DS
)
assert_static_rejected kind_literal "$kind_literal" 'row-array elements must have the same field names and scalar kinds'

mixed_literal=$(write_fixture mixed_literal <<'DS'
let rows = [
  { file: "a.c", loc: 10 },
  "b.c"
]
DS
)
assert_static_rejected mixed_literal "$mixed_literal" 'row-array literals cannot mix rows with scalar values'

# 5. Empty arrays and push inference.
first_push=$(write_fixture first_push <<'DS'
let rows = []
rows.push({ file: "a.c", line: 1, name: "main", loc: 10 })
rows.push({ file: "b.c", line: 2, name: "helper", loc: 5 })
for row in rows {
  echo "{row.file}:{row.loc}"
}
DS
)
run_accept first_push "$first_push" $'a.c:10\nb.c:5\n'

push_schema=$(write_fixture push_schema <<'DS'
let rows = []
rows.push({ file: "a.c", loc: 10 })
rows.push({ file: "b.c", name: "helper", loc: 5 })
DS
)
assert_static_rejected push_schema "$push_schema" 'pushed row must match' 'name'

push_kind=$(write_fixture push_kind <<'DS'
let rows = []
rows.push({ file: "a.c", loc: 10 })
rows.push({ file: "b.c", loc: "5" })
DS
)
assert_static_rejected push_kind "$push_kind" 'pushed row must match' 'loc'

scalar_array=$(write_fixture scalar_array <<'DS'
let names = []
names.push("api")
names.push("web")
for name in names {
  echo "{name}"
}
DS
)
run_accept scalar_array "$scalar_array" $'api\nweb\n'

scalar_then_row=$(write_fixture scalar_then_row <<'DS'
let xs = []
xs.push("api")
xs.push({ name: "web" })
DS
)
assert_static_rejected scalar_then_row "$scalar_then_row" 'cannot push a row into a scalar array'

row_then_scalar=$(write_fixture row_then_scalar <<'DS'
let rows = []
rows.push({ name: "api" })
rows.push("web")
DS
)
assert_static_rejected row_then_scalar "$row_then_scalar" 'cannot push a scalar value into a row array'

sort_untyped=$(write_fixture sort_untyped <<'DS'
let rows = []
let sorted = rows.sort_by("name")
DS
)
assert_static_rejected sort_untyped "$sort_untyped" 'row-array with known schema'

sort_known_empty=$(write_fixture sort_known_empty <<'DS'
let rows = []
if false {
  rows.push({ name: "api", loc: 1 })
}
let sorted = rows.sort_by("loc", "desc")
for row in sorted {
  echo row.name
}
echo "done"
DS
)
run_accept sort_known_empty "$sort_known_empty" $'done\n'

# 6/7. Field access, iteration, transformation, and copy behavior.
loop_fields=$(write_fixture loop_fields <<'DS'
let rows = [
  { file: "a.c", line: 1, name: "main", loc: 10 },
  { file: "b.c", line: 2, name: "helper", loc: 5 }
]
for row in rows {
  echo "{row.name.upper()}"
  echo "{row.loc + 1}"
}
DS
)
run_accept loop_fields "$loop_fields" $'MAIN\n11\nHELPER\n6\n'

indexed_fields=$(write_fixture indexed_fields <<'DS'
let rows = [
  { file: "a.c", line: 1, name: "main", loc: 10 },
  { file: "b.c", line: 2, name: "helper", loc: 5 }
]
echo rows[0].file
echo "{rows[1].loc + 2}"
DS
)
run_accept indexed_fields "$indexed_fields" $'a.c\n7\n'

bound_indexed=$(write_fixture bound_indexed <<'DS'
let rows = [{ file: "a.c", line: 1, name: "main", loc: 10 }]
let row = rows[0]
echo row.name
echo "{row.loc * 2}"
DS
)
run_accept bound_indexed "$bound_indexed" $'main\n20\n'

loop_missing=$(write_fixture loop_missing <<'DS'
let rows = [{ file: "a.c", line: 1 }]
for row in rows {
  echo row.loc
}
DS
)
assert_static_rejected loop_missing "$loop_missing" 'unknown row field' 'loc'

indexed_missing=$(write_fixture indexed_missing <<'DS'
let rows = [{ file: "a.c", line: 1 }]
echo rows[0].loc
DS
)
assert_static_rejected indexed_missing "$indexed_missing" 'unknown row field' 'loc'

insertion_order=$(write_fixture insertion_order <<'DS'
let rows = []
rows.push({ name: "third", loc: 3 })
rows.push({ name: "first", loc: 1 })
rows.push({ name: "second", loc: 2 })
for row in rows {
  echo row.name
}
DS
)
run_accept insertion_order "$insertion_order" $'third\nfirst\nsecond\n'

transform_rows=$(write_fixture transform_rows <<'DS'
let rows = [
  { file: "a.c", line: 1, name: "main", loc: 10 },
  { file: "b.c", line: 2, name: "tiny", loc: 2 },
  { file: "c.c", line: 3, name: "parse", loc: 30 }
]
let large = []
for row in rows {
  if row.loc >= 10 {
    large.push({ file: row.file, line: row.line, name: row.name.upper(), loc: row.loc })
  }
}
for row in large {
  echo "{row.name}:{row.loc}"
}
DS
)
run_accept transform_rows "$transform_rows" $'MAIN:10\nPARSE:30\n'

transform_schema_bad=$(write_fixture transform_schema_bad <<'DS'
let rows = [
  { file: "a.c", line: 1, name: "main", loc: 10 },
  { file: "b.c", line: 2, name: "tiny", loc: 2 }
]
let out = []
for row in rows {
  if row.loc > 5 {
    out.push({ name: row.name, loc: row.loc })
  } else {
    out.push({ name: row.name })
  }
}
DS
)
assert_static_rejected transform_schema_bad "$transform_schema_bad" 'pushed row must match'

loop_copy=$(write_fixture loop_copy <<'DS'
let rows = [{ name: "api", loc: 1 }]
let copied = []
for row in rows {
  copied.push({ name: row.name.upper(), loc: row.loc + 1 })
}
echo rows[0].name
echo rows[0].loc
echo copied[0].name
echo copied[0].loc
DS
)
run_accept loop_copy "$loop_copy" $'api\n1\nAPI\n2\n'

# 8. sort_by behavior.
sort_string_asc=$(write_fixture sort_string_asc <<'DS'
let rows = [{ name: "web", loc: 2 }, { name: "api", loc: 1 }, { name: "worker", loc: 3 }]
for row in rows.sort_by("name") { echo row.name }
DS
)
run_accept sort_string_asc "$sort_string_asc" $'api\nweb\nworker\n'

sort_string_desc=$(write_fixture sort_string_desc <<'DS'
let rows = [{ name: "web", loc: 2 }, { name: "api", loc: 1 }, { name: "worker", loc: 3 }]
for row in rows.sort_by("name", "desc") { echo row.name }
DS
)
run_accept sort_string_desc "$sort_string_desc" $'worker\nweb\napi\n'

sort_int_asc=$(write_fixture sort_int_asc <<'DS'
let rows = [{ name: "ten", loc: 10 }, { name: "two", loc: 2 }, { name: "one", loc: 1 }]
for row in rows.sort_by("loc") { echo "{row.name}:{row.loc}" }
DS
)
run_accept sort_int_asc "$sort_int_asc" $'one:1\ntwo:2\nten:10\n'

sort_int_desc=$(write_fixture sort_int_desc <<'DS'
let rows = [{ name: "ten", loc: 10 }, { name: "two", loc: 2 }, { name: "one", loc: 1 }]
for row in rows.sort_by("loc", "desc") { echo "{row.name}:{row.loc}" }
DS
)
run_accept sort_int_desc "$sort_int_desc" $'ten:10\ntwo:2\none:1\n'

sort_bool=$(write_fixture sort_bool <<'DS'
let rows = [
  { name: "disabled", enabled: false },
  { name: "enabled", enabled: true },
  { name: "disabled2", enabled: false }
]
for row in rows.sort_by("enabled") { echo "{row.name}:{row.enabled}" }
echo "--"
for row in rows.sort_by("enabled", "desc") { echo "{row.name}:{row.enabled}" }
DS
)
run_accept sort_bool "$sort_bool" $'disabled:false\ndisabled2:false\nenabled:true\n--\nenabled:true\ndisabled:false\ndisabled2:false\n'

sort_stable=$(write_fixture sort_stable <<'DS'
let rows = [{ name: "a", loc: 1 }, { name: "b", loc: 1 }, { name: "c", loc: 2 }, { name: "d", loc: 1 }]
for row in rows.sort_by("loc") { echo row.name }
DS
)
run_accept sort_stable "$sort_stable" $'a\nb\nd\nc\n'

sort_copy=$(write_fixture sort_copy <<'DS'
let rows = [{ name: "b", loc: 2 }, { name: "a", loc: 1 }]
let sorted = rows.sort_by("name")
echo rows[0].name
echo rows[1].name
echo sorted[0].name
echo sorted[1].name
DS
)
run_accept sort_copy "$sort_copy" $'b\na\na\nb\n'

sort_transform=$(write_fixture sort_transform <<'DS'
let rows = [
  { file: "a.c", name: "main", loc: 10 },
  { file: "b.c", name: "parse", loc: 30 },
  { file: "c.c", name: "tiny", loc: 2 }
]
let large = []
for row in rows {
  if row.loc >= 10 { large.push({ file: row.file, name: row.name, loc: row.loc }) }
}
for row in large.sort_by("loc", "desc") { echo "{row.name}:{row.loc}" }
DS
)
run_accept sort_transform "$sort_transform" $'parse:30\nmain:10\n'

for name in sort_bad_field sort_empty_field sort_bad_dir sort_dynamic_field sort_dynamic_dir sort_non_row row_sort_method; do
  case "$name" in
    sort_bad_field) code='let rows = [{ name: "api", loc: 1 }]
let sorted = rows.sort_by("missing")'; needle='unknown row field' ;;
    sort_empty_field) code='let rows = [{ name: "api", loc: 1 }]
let sorted = rows.sort_by("")'; needle='sort_by field must be non-empty' ;;
    sort_bad_dir) code='let rows = [{ name: "api", loc: 1 }]
let sorted = rows.sort_by("loc", "largest")'; needle='sort_by direction must be "asc" or "desc"' ;;
    sort_dynamic_field) code='let rows = [{ name: "api", loc: 1 }]
let key = "loc"
let sorted = rows.sort_by(key)'; needle='sort_by field must be a string literal' ;;
    sort_dynamic_dir) code='let rows = [{ name: "api", loc: 1 }]
let direction = "desc"
let sorted = rows.sort_by("loc", direction)'; needle='sort_by direction must be a string literal' ;;
    sort_non_row) code='let names = ["web", "api"]
let sorted = names.sort_by("name")'; needle='row-array with known schema' ;;
    row_sort_method) code='let row = { name: "api", loc: 1 }
let sorted = row.sort_by("name")'; needle='unknown string method `sort_by`' ;;
  esac
  f=$(write_fixture "$name" <<<"$code")
  assert_static_rejected "$name" "$f" "$needle"
done

# 9. Copy semantics and mutation limits.
copy_array=$(write_fixture copy_array <<'DS'
let rows = []
rows.push({ name: "api", loc: 1 })
let copy = rows
copy.push({ name: "web", loc: 2 })
echo "rows={rows[0].name}"
echo "copy0={copy[0].name}"
echo "copy1={copy[1].name}"
DS
)
run_accept copy_array "$copy_array" $'rows=api\ncopy0=api\ncopy1=web\n'

copy_row=$(write_fixture copy_row <<'DS'
let row = { name: "api", loc: 1 }
let copy = row
echo row.name
echo copy.name
DS
)
run_accept copy_row "$copy_row" $'api\napi\n'

elem_replace=$(write_fixture elem_replace <<'DS'
let rows = [{ name: "api", loc: 1 }]
rows[0] = { name: "web", loc: 2 }
DS
)
assert_static_rejected elem_replace "$elem_replace" 'index assignment value must be a flat scalar'

push_temp=$(write_fixture push_temp <<'DS'
fn make_rows() {
  return [{ name: "api", loc: 1 }]
}
make_rows().push({ name: "web", loc: 2 })
DS
)
assert_static_rejected push_temp "$push_temp" 'expected end of function call statement'

# 10/11. Function returns and imports.
return_row=$(write_fixture return_row <<'DS'
fn make_row() {
  return { file: "a.c", line: 1, name: "main", loc: 10 }
}
let row = make_row()
echo "{row.file}:{row.line}:{row.name}:{row.loc}"
DS
)
run_accept return_row "$return_row" $'a.c:1:main:10\n'

return_rows=$(write_fixture return_rows <<'DS'
fn make_rows() {
  let rows = []
  rows.push({ file: "a.c", line: 1, name: "main", loc: 10 })
  rows.push({ file: "b.c", line: 2, name: "helper", loc: 5 })
  return rows
}
let rows = make_rows()
for row in rows.sort_by("loc") { echo "{row.name}:{row.loc}" }
DS
)
run_accept return_rows "$return_rows" $'helper:5\nmain:10\n'

forward_return_row=$(write_fixture forward_return_row <<'DS'
let row = make_row()
echo "{row.name}:{row.loc}"

fn make_row() {
  return { name: "api", loc: 1 }
}
DS
)
run_accept forward_return_row "$forward_return_row" $'api:1\n'

forward_return_rows=$(write_fixture forward_return_rows <<'DS'
let rows = make_rows()
for row in rows.sort_by("loc") {
  echo "{row.name}:{row.loc}"
}

fn make_rows() {
  return [{ name: "b", loc: 2 }, { name: "a", loc: 1 }]
}
DS
)
run_accept forward_return_rows "$forward_return_rows" $'a:1\nb:2\n'

direct_return_sort=$(write_fixture direct_return_sort <<'DS'
fn make_rows() {
  return [{ name: "b", loc: 2 }, { name: "a", loc: 1 }]
}
for row in make_rows().sort_by("loc") {
  echo row.name
}
DS
)
run_accept direct_return_sort "$direct_return_sort" $'a\nb\n'

return_schema_bad=$(write_fixture return_schema_bad <<'DS'
fn make_rows(flag = true) {
  if flag { return [{ name: "api", loc: 1 }] }
  return [{ name: "web" }]
}
let rows = make_rows()
echo rows[0].name
DS
)
assert_static_rejected return_schema_bad "$return_schema_bad" 'row-array returns in a function must have the same field names and scalar kinds'

return_kind_bad=$(write_fixture return_kind_bad <<'DS'
fn make_rows(flag = true) {
  if flag { return [{ name: "api", loc: 1 }] }
  return [{ name: "web", loc: "1" }]
}
let rows = make_rows()
echo rows[0].name
DS
)
assert_static_rejected return_kind_bad "$return_kind_bad" 'row-array returns in a function must have the same field names and scalar kinds'

return_bytes=$(write_fixture return_bytes <<'DS'
fn make_rows() {
  return [{ name: "a b", note: "x\ny", loc: 1 }]
}
let rows = make_rows()
echo rows[0].name
echo rows[0].note
DS
)
run_accept return_bytes "$return_bytes" $'a b\nx\ny\n'

row_param=$(write_fixture row_param <<'DS'
fn print_row(row) {
  echo row.name
}
print_row({ name: "api", loc: 1 })
DS
)
assert_static_rejected row_param "$row_param" 'passing collection values to functions'

cat >"$FIX/rows-lib.ds" <<'DS'
fn load_rows() {
  let rows = []
  rows.push({ file: "b.c", line: 2, name: "helper", loc: 5 })
  rows.push({ file: "a.c", line: 1, name: "main", loc: 10 })
  return rows
}
DS
import_main=$(write_fixture rows-import-main <<'DS'
import "./rows-lib.ds"
let rows = load_rows()
for row in rows.sort_by("file") { echo "{row.file}:{row.name}" }
DS
)
run_accept import_main "$import_main" $'a.c:main\nb.c:helper\n'
assert_direct_accept import_main "$import_main" $'a.c:main\nb.c:helper\n'
run_ok import_lib_ast "$DS" ast "$FIX/rows-lib.ds"
assert_contains "$TMP/import_main_hir.out" 'Function load_rows' 'composed HIR includes imported function'
assert_not_contains "$TMP/import_main_ast.out" 'FunctionDecl load_rows' 'root AST stays root-only for imports'

cat >"$FIX/rows-lib-bad.ds" <<'DS'
fn load_rows(flag = true) {
  if flag { return [{ name: "api", loc: 1 }] }
  return [{ name: "web" }]
}
DS
import_bad=$(write_fixture rows-import-bad <<'DS'
import "./rows-lib-bad.ds"
let rows = load_rows()
echo rows[0].name
DS
)
assert_static_rejected import_bad "$import_bad" 'row-array returns in a function must have the same field names and scalar kinds'

# 12. Runtime data failures.
oob_index=$(write_fixture oob_index <<'DS'
let rows = [{ name: "api", loc: 1 }]
let i = 1
echo rows[i].name
echo "unreachable"
DS
)
assert_runtime_rejected oob_index "$oob_index" 'array index 1 out of range' 'unreachable'

negative_index=$(write_fixture negative_index <<'DS'
let rows = [{ name: "api", loc: 1 }]
let i = -1
echo rows[i].name
echo "unreachable"
DS
)
assert_runtime_rejected negative_index "$negative_index" 'array index -1 out of range' 'unreachable'

branch_schema_bad=$(write_fixture branch_schema_bad <<'DS'
let rows = []
let flag = true
if flag {
  rows.push({ name: "api", loc: 1 })
} else {
  rows.push({ name: "web" })
}
echo rows[0].name
DS
)
assert_static_rejected branch_schema_bad "$branch_schema_bad" 'pushed row must match'

# 13. Hostile data preservation.
spaces_tabs=$(write_fixture spaces_tabs <<'DS'
let rows = [
  { name: "", note: "a b", loc: 1 },
  { name: "tab", note: "x\ty", loc: 2 }
]
for row in rows.sort_by("loc") { echo "name=[{row.name}] note=[{row.note}]" }
DS
)
run_accept spaces_tabs "$spaces_tabs" $'name=[] note=[a b]\nname=[tab] note=[x\ty]\n'

newlines=$(write_fixture newlines <<'DS'
let rows = [{ name: "multi", note: "line1\nline2", loc: 1 }]
echo rows[0].note
DS
)
run_accept newlines "$newlines" $'line1\nline2\n'

metachars=$(write_fixture metachars <<'DS'
let rows = [{ name: "$(echo bad)", note: "*?[x] ; rm -rf /", loc: 1 }]
echo rows[0].name
echo rows[0].note
DS
)
run_accept metachars "$metachars" $'$(echo bad)\n*?[x] ; rm -rf /\n'

braces=$(write_fixture braces <<'DS'
let rows = [{ name: "{{parser}}", note: "has {{ and }}", loc: 1 }]
echo rows[0].name
echo rows[0].note
DS
)
run_accept braces "$braces" $'{parser}\nhas { and }\n'

hostile_sort=$(write_fixture hostile_sort <<'DS'
let rows = [
  { name: "b b", loc: 2 },
  { name: "a\na", loc: 1 },
  { name: "$(echo nope)", loc: 3 }
]
for row in rows.sort_by("name") { echo "[{row.name}]" }
DS
)
run_accept hostile_sort "$hostile_sort" $'[$(echo nope)]\n[a\na]\n[b b]\n'

# 14. Bash helper hygiene and standalone behavior.
no_rows_script="$TMP/no_rows.sh"
run_ok no_rows_emit "$DS" emit bash "$scalar_array" -o "$no_rows_script"
assert_not_contains "$no_rows_script" '__ds_row_' 'scalar array emitted Bash has no row sidecars'
row_script="$TMP/row_script.sh"
emit_checked row_script "$sort_int_asc" "$row_script"
assert_contains "$row_script" '__ds_row_' 'row emitted Bash has row sidecars'
assert_not_contains "$row_script" 'eval ' 'row emitted Bash avoids eval keyword in row path smoke'

dedup=$(write_fixture dedup <<'DS'
let rows = [{ name: "b", loc: 2 }, { name: "a", loc: 1 }]
let a = rows.sort_by("name")
let b = rows.sort_by("loc")
for row in a { echo row.name }
for row in b { echo row.loc }
DS
)
run_accept dedup "$dedup" $'a\nb\n1\n2\n'
assert_no_duplicate_helpers "$TMP/dedup.sh" 'dedup emitted Bash helper definitions'

hostile_env_script="$TMP/hostile_env.sh"
emit_checked hostile_env "$metachars" "$hostile_env_script"
set +e
env -i PATH="$PATH" __ds_rows=bad __ds_tmp=bad bash "$hostile_env_script" >"$TMP/hostile_env.out" 2>"$TMP/hostile_env.err"
hostile_env_rc=$?
set -e
printf '%s' "$hostile_env_rc" >"$TMP/hostile_env.rc"
assert_status hostile_env 0
assert_text hostile_env_stdout $'$(echo bad)\n*?[x] ; rm -rf /\n' "$TMP/hostile_env.out"

# 15. Diagnostics.
whole_row_cmd=$(write_fixture whole_row_cmd <<'DS'
let row = { name: "api", loc: 1 }
echo $row
DS
)
assert_static_rejected whole_row_cmd "$whole_row_cmd" 'collection `row` cannot be passed directly'

whole_row_interp=$(write_fixture whole_row_interp <<'DS'
let row = { name: "api", loc: 1 }
echo "row={row}"
DS
)
assert_static_rejected whole_row_interp "$whole_row_interp" 'cannot interpolate map value'

row_condition=$(write_fixture row_condition <<'DS'
let row = { name: "api", loc: 1 }
if row { echo "bad" }
DS
)
assert_static_rejected row_condition "$row_condition" 'condition expression must be bool'

row_array_condition=$(write_fixture row_array_condition <<'DS'
let rows = [{ name: "api", loc: 1 }]
if rows { echo "bad" }
DS
)
assert_static_rejected row_array_condition "$row_array_condition" 'condition expression must be bool'

# 16. Integration with existing features.
string_helpers=$(write_fixture string_helpers <<'DS'
let rows = [{ sig: "  parse(int x)  ", loc: 10 }]
let sig = rows[0].sig.trim()
let before = sig.split("(")[0]
let idx = rows[0].sig.index_of("parse")
let chunk = rows[0].sig.slice(2, 7)
echo "{before}"
echo "{idx}"
echo "{chunk}"
DS
)
run_accept string_helpers "$string_helpers" $'parse\n2\nparse\n'

param_infer=$(write_fixture param_infer <<'DS'
fn clean(name) {
  return name.trim().upper()
}
let rows = [{ name: " api ", loc: 1 }]
let cleaned = clean(rows[0].name)
echo "{cleaned}"
DS
)
run_accept param_infer "$param_infer" $'API\n'

case_selector=$(write_fixture case_selector <<'DS'
let rows = [{ name: "api", loc: 1, enabled: true }]
if rows[0].enabled {
  echo "on"
} else {
  echo "off"
}
DS
)
run_accept case_selector "$case_selector" $'on\n'

membership=$(write_fixture membership <<'DS'
let rows = [{ name: "api", loc: 1 }]
let allowed = ["api", "web"]
if rows[0].name in allowed { echo "allowed" }
DS
)
run_accept membership "$membership" $'allowed\n'

regex_row=$(write_fixture regex_row <<'DS'
let rows = [{ name: "ds_parse", loc: 1 }]
if rows[0].name matches /^ds_/ { echo "public" }
DS
)
run_accept regex_row "$regex_row" $'public\n'

script_args=$(write_fixture script_args <<'DS'
script {
  arg file: string
  option line: int = 1
  flag public: bool = false
}
let row = { file: file, line: line, public: public }
echo "{row.file}:{row.line}:{row.public}"
DS
)
run_accept script_args "$script_args" $'src/main.c:7:true\n' src/main.c --line 7 --public

# 17/18. Analyzer-style smoke and edge matrix.
analyzer=$(write_fixture analyzer <<'DS'
let rows = []
let files = ["src/a.c", "src/b.c", "src/c.c"]
let names = ["parse", "run", "emit"]
let locs = [20, 5, 20]
let i = 0
for file in files {
  rows.push({ file: file, line: i + 1, name: names[i], loc: locs[i] })
  i += 1
}
let sorted = rows.sort_by("loc", "desc")
for row in sorted { echo "{row.loc}\t{row.file}:{row.line}\t{row.name}" }
DS
)
run_accept analyzer "$analyzer" $'20\tsrc/a.c:1\tparse\n20\tsrc/c.c:3\temit\n5\tsrc/b.c:2\trun\n'

edge_matrix=$(write_fixture edge_matrix <<'DS'
let rows = [
  { name: "", note: "colon:a:b", loc: 0, enabled: false },
  { name: "Case", note: "tab\tvalue", loc: -2, enabled: true },
  { name: "case", note: "café", loc: -10, enabled: false }
]
let copy = rows
let sorted = copy.sort_by("loc")
for row in sorted { echo "{row.name}|{row.note}|{row.loc}|{row.enabled}" }
DS
)
run_accept edge_matrix "$edge_matrix" $'case|café|-10|false\nCase|tab\tvalue|-2|true\n|colon:a:b|0|false\n'

huge="$FIX/huge_rows.ds"
{
  echo 'let rows = []'
  for i in $(seq 1 100); do
    printf 'rows.push({ name: "n%03d", loc: %d })\n' "$i" "$((101 - i))"
  done
  echo 'let sorted = rows.sort_by("loc")'
  echo 'echo sorted[0].name'
  echo 'echo sorted[99].name'
} >"$huge"
run_accept huge_rows "$huge" $'n100\nn001\n'

test_blocks=$(write_fixture row_test_blocks <<'DS'
test "row smoke" {
  let rows = [{ name: "api", loc: 1 }]
  assert rows[0].loc == 1
}
DS
)
run_ok row_test_blocks "$DS" test "$test_blocks"
assert_contains "$TMP/row_test_blocks.out" '1 tests, 1 passed, 0 failed' 'ds test covers row fixture'

echo "v0.37 tests passed ($pass_count assertions)"
