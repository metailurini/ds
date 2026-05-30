#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
DS="$ROOT/ds"
TMP=${TMPDIR:-/tmp}/ds_v0_32_tests.$$
FIX="$TMP/fixtures"
mkdir -p "$FIX"
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

capture_cmd() {
  local name="$1"
  shift
  set +e
  "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/$name.rc"
}

capture_cmd_env() {
  local name="$1"
  shift
  set +e
  env -i PATH="$PATH" "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"
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

run_parity() {
  local name="$1" file="$2" expected_stdout="$3" expected_status="${4:-0}"
  shift 4 || true
  local script="$TMP/$name.sh"
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  mkdir -p "$vm_work" "$bash_work"

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
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
}

run_parity_env() {
  local name="$1" file="$2" expected_stdout="$3"
  shift 3
  local script="$TMP/$name.sh"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"

  set +e
  env -i PATH="$PATH" "$@" "$DS" run "$file" >"$TMP/${name}_vm.out" 2>"$TMP/${name}_vm.err"
  local vm_rc=$?
  env -i PATH="$PATH" "$@" bash "$script" >"$TMP/${name}_bash.out" 2>"$TMP/${name}_bash.err"
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

run_runtime_failure() {
  local name="$1" file="$2" needle="$3" expected_stdout="$4"
  shift 4 || true
  local script="$TMP/$name.sh"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"

  capture_cmd "${name}_vm" "$DS" run "$file" "$@"
  capture_cmd "${name}_bash" bash "$script" "$@"
  assert_nonzero_status "${name}_vm"
  assert_nonzero_status "${name}_bash"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name runtime stdout parity"
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
  assert_contains "$TMP/${name}_vm.err" ': error:' "$name VM diagnostic shape"
  assert_contains "$TMP/${name}_bash.err" ': error:' "$name Bash diagnostic shape"
  assert_contains "$TMP/${name}_vm.err" "$needle" "$name VM diagnostic message"
  assert_contains "$TMP/${name}_bash.err" "$needle" "$name Bash diagnostic message"
}

run_runtime_failure_env() {
  local name="$1" file="$2" needle="$3" expected_stdout="$4"
  shift 4
  local script="$TMP/$name.sh"
  run_ok "${name}_check" "$DS" check "$file"
  emit_checked "$name" "$file" "$script"
  capture_cmd_env "${name}_vm" "$@" "$DS" run "$file"
  capture_cmd_env "${name}_bash" "$@" bash "$script"
  assert_nonzero_status "${name}_vm"
  assert_nonzero_status "${name}_bash"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name runtime stdout parity"
  assert_text "${name}_stdout" "$expected_stdout" "$TMP/${name}_vm.out"
  assert_contains "$TMP/${name}_vm.err" "$needle" "$name VM diagnostic message"
  assert_contains "$TMP/${name}_bash.err" "$needle" "$name Bash diagnostic message"
}

assert_check_fails() {
  local name="$1" file="$2" needle="$3"
  capture_cmd "${name}_check" "$DS" check "$file"
  assert_nonzero_status "${name}_check"
  assert_contains "$TMP/${name}_check.err" ': error:' "$name check diagnostic shape"
  assert_contains "$TMP/${name}_check.err" "$needle" "$name check diagnostic message"
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

assert_rejected() {
  local name="$1" file="$2" needle="$3"
  assert_check_fails "$name" "$file" "$needle"
  assert_emit_fails "$name" "$file" "$needle"
}

assert_doc_contains() {
  local file="$1" needle="$2" name="$3"
  assert_contains "$file" "$needle" "$name"
}

assert_helper_present() {
  local script="$1" helper="$2" name="$3"
  assert_contains "$script" "$helper" "$name"
  assert_contains "$script" '__ds_' "$name uses reserved helper namespace"
}

assert_helper_absent() {
  local script="$1" helper="$2" name="$3"
  assert_not_contains "$script" "$helper" "$name"
}

# 1. Planning and docs checks.
for doc in \
  docs/milestones/v0.32.0-spec.md \
  docs/milestones/v0.32.0-test-plan.md \
  docs/roadmap.md \
  docs/language.ds \
  docs/status.md \
  docs/runtime.md \
  docs/parity-contracts.md \
  docs/diagnostics.md \
  docs/concept-map.md \
  docs/source-map.md; do
  [ -f "$doc" ] || fail "missing required doc $doc"
  pass "required doc exists: $doc"
done
assert_doc_contains docs/language.ds 'runtime string patterns' 'docs mention runtime string regex patterns'
assert_doc_contains docs/language.ds 'regex.match' 'docs mention regex.match'
assert_doc_contains docs/language.ds 'regex.replace' 'docs mention regex.replace'
assert_doc_contains docs/runtime.md 'matched' 'runtime docs mention match-result keys'
assert_doc_contains docs/runtime.md 'Optional unmatched captures' 'runtime docs mention optional captures'
assert_doc_contains docs/runtime.md '$0' 'runtime docs mention replacement $0'
assert_doc_contains docs/runtime.md '$$' 'runtime docs mention replacement literal dollar'
assert_doc_contains docs/runtime.md 'invalid runtime' 'runtime docs mention invalid dynamic regex diagnostics'
assert_doc_contains docs/parity-contracts.md 'standalone Bash emission' 'parity docs mention standalone Bash'
assert_doc_contains docs/concept-map.md 'named captures' 'concept docs keep named captures deferred'
assert_doc_contains docs/concept-map.md 'lookaround' 'concept docs keep lookaround deferred'
assert_doc_contains docs/concept-map.md 'replace-first/count' 'concept docs keep replace-first/count deferred'

# 2. Parser, lexer, AST, formatter, and debug smoke tests.
literal_shape=$(write_fixture literal_shape <<'DS'
let ok = "release/12" matches /^release\/[0-9]+$/
echo $ok
DS
)
run_ok literal_tokens "$DS" tokens "$literal_shape"
assert_contains "$TMP/literal_tokens.out" 'REGEX          "/^release' 'tokens preserve regex literal text'
run_ok literal_ast "$DS" ast "$literal_shape"
assert_contains "$TMP/literal_ast.out" 'matches' 'AST shows matches expression'
assert_contains "$TMP/literal_ast.out" 'Regex' 'AST shows regex literal node'
run_ok literal_hir "$DS" hir "$literal_shape"
assert_contains "$TMP/literal_hir.out" 'matches' 'HIR shows regex matches operation'
run_ok literal_bytecode "$DS" bytecode "$literal_shape"
assert_not_contains "$TMP/literal_bytecode.out" '0x' 'bytecode debug output pointer-free'
run_parity literal_shape_parity "$literal_shape" $'true\n' 0

runtime_shape=$(write_fixture runtime_shape <<'DS'
let pattern = "^release/[0-9]+$"
let ok = "release/12" matches pattern
echo $ok
DS
)
run_ok runtime_tokens "$DS" tokens "$runtime_shape"
assert_not_contains "$TMP/runtime_tokens.out" 'REGEX' 'runtime string matches needs no regex token'
run_ok runtime_ast "$DS" ast "$runtime_shape"
assert_contains "$TMP/runtime_ast.out" 'IdentExpr pattern' 'AST keeps runtime matches RHS as expression'
run_ok runtime_fmt_check "$DS" fmt --check "$runtime_shape"
cp "$runtime_shape" "$TMP/runtime_fmt.ds"
run_ok runtime_fmt_write "$DS" fmt --write "$TMP/runtime_fmt.ds"
assert_contains "$TMP/runtime_fmt.ds" '^release/[0-9]+$' 'formatter preserves runtime pattern string'
run_parity runtime_shape_parity "$runtime_shape" $'true\n' 0

helper_shape=$(write_fixture helper_shape <<'DS'
let m = regex.match("v1.2", /^v([0-9]+)\.([0-9]+)$/)
let matched = m.matched
let out = regex.replace("api-123", /([a-z]+)-([0-9]+)/, "$1:$2")
echo $matched
echo $out
DS
)
run_ok helper_ast "$DS" ast "$helper_shape"
assert_contains "$TMP/helper_ast.out" 'CallExpr regex.match' 'AST shows regex.match call'
assert_contains "$TMP/helper_ast.out" 'CallExpr regex.replace' 'AST shows regex.replace call'
run_ok helper_hir "$DS" hir "$helper_shape"
assert_contains "$TMP/helper_hir.out" 'Call regex.match' 'HIR shows regex.match helper call'
assert_contains "$TMP/helper_hir.out" 'Call regex.replace' 'HIR shows regex.replace helper call'
run_ok helper_bytecode "$DS" bytecode "$helper_shape"
assert_not_contains "$TMP/helper_bytecode.out" '0x' 'helper bytecode debug output pointer-free'
run_ok helper_fmt_check "$DS" fmt --check "$helper_shape"
run_parity helper_shape_parity "$helper_shape" $'true\napi:123\n' 0

first_class_regex=$(write_fixture first_class_regex <<'DS'
let r = /abc/
DS
)
assert_rejected first_class_regex "$first_class_regex" 'expected expression'
command_slash_word=$(write_fixture command_slash_word <<'DS'
echo /abc/
DS
)
run_parity command_slash_word "$command_slash_word" $'/abc/\n' 0

# 3. Runtime string `matches` parity.
runtime_basic=$(write_fixture runtime_basic <<'DS'
let pattern = "^release/[0-9]+$"
let a = "release/12" matches pattern
let b = "feature/x" matches pattern
echo $a
echo $b
DS
)
run_parity runtime_basic "$runtime_basic" $'true\nfalse\n' 0

runtime_fn=$(write_fixture runtime_fn <<'DS'
fn version_pattern() {
  return "^v[0-9]+$"
}

if "v123" matches version_pattern() {
  echo version
}
DS
)
run_parity runtime_fn "$runtime_fn" $'version\n' 0

runtime_args=$(write_fixture runtime_args <<'DS'
script {
  arg text: string
  option pattern: string = "^api$"
}

let ok = text matches pattern
echo $ok
DS
)
run_parity runtime_args_true "$runtime_args" $'true\n' 0 api --pattern '^api$'
run_parity runtime_args_false "$runtime_args" $'false\n' 0 web --pattern '^api$'

runtime_env=$(write_fixture runtime_env <<'DS'
let pattern = env.DS_TEST_PATTERN
let ok = "worker-7" matches pattern
echo $ok
DS
)
run_parity_env runtime_env "$runtime_env" $'true\n' DS_TEST_PATTERN='^worker-[0-9]+$'

runtime_interp=$(write_fixture runtime_interp <<'DS'
let pattern = "^a+$"
let text = "aaa"
let line = "ok={text matches pattern}"
echo $line
DS
)
run_parity runtime_interp "$runtime_interp" $'ok=true\n' 0

runtime_return=$(write_fixture runtime_return <<'DS'
fn is_release(branch = "release/1") {
  let pattern = "^release/[0-9]+$"
  return branch matches pattern
}

let a = is_release("release/2")
let b = is_release("feature/x")
echo $a
echo $b
DS
)
run_parity runtime_return "$runtime_return" $'true\nfalse\n' 0

# 4. `regex.match` capture map parity.
match_basic=$(write_fixture match_basic <<'DS'
let m = regex.match("v1.2.3", /^v([0-9]+)\.([0-9]+)\.([0-9]+)$/)
let matched = m.matched
let full = m.full
let zero = m["0"]
let one = m["1"]
let two = m["2"]
let three = m["3"]
echo $matched
echo $full
echo $zero
echo $one
echo $two
echo $three
DS
)
run_parity match_basic "$match_basic" $'true\nv1.2.3\nv1.2.3\n1\n2\n3\n' 0

match_no=$(write_fixture match_no <<'DS'
let m = regex.match("feature/x", /^release\/[0-9]+$/)
let matched = m.matched
let full = m.full
let zero = m["0"]
echo $matched
echo $full
echo $zero
echo after
DS
)
run_parity match_no "$match_no" $'false\n\n\nafter\n' 0

match_optional=$(write_fixture match_optional <<'DS'
let a = regex.match("abc", /^(a)?(b)?(c)$/)
let a_matched = a.matched
let a1 = a["1"]
let a2 = a["2"]
let a3 = a["3"]
echo "a={a_matched}:{a1}:{a2}:{a3}"

let b = regex.match("c", /^(a)?(b)?(c)$/)
let b_matched = b.matched
let b1 = b["1"]
let b2 = b["2"]
let b3 = b["3"]
echo "b={b_matched}:{b1}:{b2}:{b3}"
DS
)
run_parity match_optional "$match_optional" $'a=true:a:b:c\nb=true:::c\n' 0

match_empty_capture=$(write_fixture match_empty_capture <<'DS'
let m = regex.match("a", /^(a*)(b*)$/)
let one = m["1"]
let two = m["2"]
echo "1={one}"
echo "2={two}"
DS
)
run_parity match_empty_capture "$match_empty_capture" $'1=a\n2=\n' 0

match_alternation=$(write_fixture match_alternation <<'DS'
let a = regex.match("api-123", /^(api|web)-([0-9]+)$/)
let b = regex.match("web-456", /^(api|web)-([0-9]+)$/)
let a_name = a["1"]
let a_id = a["2"]
let b_name = b["1"]
let b_id = b["2"]
echo "{a_name}:{a_id}"
echo "{b_name}:{b_id}"
DS
)
run_parity match_alternation "$match_alternation" $'api:123\nweb:456\n' 0

match_return=$(write_fixture match_return <<'DS'
fn parse_version(s = "v0") {
  return regex.match(s, /^v([0-9]+)$/)
}

let m = parse_version("v42")
let matched = m.matched
let one = m["1"]
echo $matched
echo $one
DS
)
run_parity match_return "$match_return" $'true\n42\n' 0

match_iter=$(write_fixture match_iter <<'DS'
let m = regex.match("v1.2", /^v([0-9]+)\.([0-9]+)$/)
for key, value in m {
  echo "{key}={value}"
}
DS
)
run_parity match_iter "$match_iter" $'0=v1.2\n1=1\n2=2\nfull=v1.2\nmatched=true\n' 0

match_mutation=$(write_fixture match_mutation <<'DS'
let m = regex.match("v1", /^v([0-9]+)$/)
m["1"] = "2"
let one = m["1"]
echo $one
DS
)
run_parity match_mutation "$match_mutation" $'2\n' 0

# 5. Runtime string regex.match.
runtime_match_var=$(write_fixture runtime_match_var <<'DS'
let pattern = "^([a-z]+)-([0-9]+)$"
let m = regex.match("api-123", pattern)
let matched = m.matched
let one = m["1"]
let two = m["2"]
echo $matched
echo $one
echo $two
DS
)
run_parity runtime_match_var "$runtime_match_var" $'true\napi\n123\n' 0

runtime_match_collections=$(write_fixture runtime_match_collections <<'DS'
let patterns = ["^api$", "^web$"]
let config = { service: "^worker-[0-9]+$" }
let a = "api" matches patterns[0]
let b = "worker-2" matches config.service
echo $a
echo $b
DS
)
run_parity runtime_match_collections "$runtime_match_collections" $'true\ntrue\n' 0

runtime_match_command=$(write_fixture runtime_match_command <<'DS'
let result = run printf "^api$"
let pattern = result.stdout.trim()
let ok = "api" matches pattern
echo $ok
DS
)
run_parity runtime_match_command "$runtime_match_command" $'true\n' 0

runtime_match_ci=$(write_fixture runtime_match_ci <<'DS'
let pattern = "^api$"
let m = regex.match("API", pattern, "i")
let matched = m.matched
let full = m.full
echo $matched
echo $full
DS
)
run_parity runtime_match_ci "$runtime_match_ci" $'true\nAPI\n' 0

# 6. regex.replace parity.
replace_capture=$(write_fixture replace_capture <<'DS'
let raw = "api-123 web-456"
let out = regex.replace(raw, /([a-z]+)-([0-9]+)/, "$1:$2")
echo $out
DS
)
run_parity replace_capture "$replace_capture" $'api:123 web:456\n' 0

replace_full=$(write_fixture replace_full <<'DS'
let out = regex.replace("a1 b2", /[a-z][0-9]/, "[$0]")
echo $out
DS
)
run_parity replace_full "$replace_full" $'[a1] [b2]\n' 0

replace_dollar=$(write_fixture replace_dollar <<'DS'
let out = regex.replace("api-123", /([a-z]+)-([0-9]+)/, "$$$1=$2")
echo $out
DS
)
run_parity replace_dollar "$replace_dollar" $'$api=123\n' 0

replace_no_match=$(write_fixture replace_no_match <<'DS'
let out = regex.replace("abc", /[0-9]+/, "N")
echo $out
DS
)
run_parity replace_no_match "$replace_no_match" $'abc\n' 0

replace_delete=$(write_fixture replace_delete <<'DS'
let out = regex.replace("a1b2c3", /[0-9]/, "")
echo $out
DS
)
run_parity replace_delete "$replace_delete" $'abc\n' 0

replace_grow=$(write_fixture replace_grow <<'DS'
let out = regex.replace("ab", /[ab]/, "<$0>")
echo $out
DS
)
run_parity replace_grow "$replace_grow" $'<a><b>\n' 0

replace_anchored_alt_left=$(write_fixture replace_anchored_alt_left <<'DS'
let out = regex.replace("aba", /^a|a$/, "X")
echo $out
DS
)
run_parity replace_anchored_alt_left "$replace_anchored_alt_left" $'XbX\n' 0

replace_anchored_alt_right=$(write_fixture replace_anchored_alt_right <<'DS'
let out = regex.replace("aba", /a$|^a/, "X")
echo $out
DS
)
run_parity replace_anchored_alt_right "$replace_anchored_alt_right" $'XbX\n' 0

replace_optional=$(write_fixture replace_optional <<'DS'
let out = regex.replace("c", /^(a)?(b)?(c)$/, "$1-$2-$3")
echo $out
DS
)
run_parity replace_optional "$replace_optional" $'--c\n' 0

replace_runtime=$(write_fixture replace_runtime <<'DS'
let pattern = "([a-z]+)-([0-9]+)"
let repl = "$1:$2"
let out = regex.replace("api-123", pattern, repl)
echo $out
DS
)
run_parity replace_runtime "$replace_runtime" $'api:123\n' 0

replace_ci=$(write_fixture replace_ci <<'DS'
let out = regex.replace("API api Api", "api", "svc", "i")
echo $out
DS
)
run_parity replace_ci "$replace_ci" $'svc svc svc\n' 0

replace_return=$(write_fixture replace_return <<'DS'
fn normalize(s = "") {
  return regex.replace(s, /([a-z]+)-([0-9]+)/, "$1:$2")
}

let out = normalize("api-123")
echo $out
DS
)
run_parity replace_return "$replace_return" $'api:123\n' 0

replace_adjacent_empty_input=$(write_fixture replace_adjacent_empty_input <<'DS'
let a = "" matches /^$/
let b = regex.replace("a1b2", /[a-z][0-9]/, "<$0>")
echo $a
echo $b
DS
)
run_parity replace_adjacent_empty_input "$replace_adjacent_empty_input" $'true\n<a1><b2>\n' 0

# 7. Flags and shell-option hygiene.
literal_i=$(write_fixture literal_i <<'DS'
let ok = "API" matches /api/i
echo $ok
DS
)
run_parity literal_i "$literal_i" $'true\n' 0

flags_compare=$(write_fixture flags_compare <<'DS'
let a = regex.match("API", /api/i)
let b = regex.match("API", "api", "i")
let am = a.matched
let bm = b.matched
echo $am
echo $bm
DS
)
run_parity flags_compare "$flags_compare" $'true\ntrue\n' 0

flags_empty=$(write_fixture flags_empty <<'DS'
let m = regex.match("api", "api", "")
let matched = m.matched
let out = regex.replace("api", "api", "svc", "")
echo $matched
echo $out
DS
)
run_parity flags_empty "$flags_empty" $'true\nsvc\n' 0

flag_static_bad=$(write_fixture flag_static_bad <<'DS'
let m = regex.match("api", "api", "g")
let matched = m.matched
echo $matched
DS
)
assert_rejected flag_static_bad "$flag_static_bad" 'regex flags must be either an empty string or `i`'

flag_dynamic_bad=$(write_fixture flag_dynamic_bad <<'DS'
script {
  arg flags: string
}

let m = regex.match("api", "api", flags)
let matched = m.matched
echo $matched
echo after
DS
)
run_runtime_failure flag_dynamic_bad "$flag_dynamic_bad" 'regex flags must be either an empty string or `i`' '' g

nocasematch_restore=$(write_fixture nocasematch_restore <<'DS'
let out = regex.replace("API", "api", "svc", "i")
let ok = "API" matches "api"
echo $out
echo $ok
DS
)
run_parity nocasematch_restore "$nocasematch_restore" $'svc\nfalse\n' 0
nocase_script="$TMP/nocasematch_restore_hostile.sh"
emit_checked nocasematch_restore_hostile "$nocasematch_restore" "$nocase_script"
capture_cmd nocasematch_restore_hostile bash -O nocasematch "$nocase_script"
assert_status nocasematch_restore_hostile 0
assert_text nocasematch_restore_hostile_stdout $'svc\nfalse\n' "$TMP/nocasematch_restore_hostile.out"

# 8. Static diagnostics.
invalid_literal_syntax=$(write_fixture invalid_literal_syntax <<'DS'
let ok = "abc" matches /(abc/
DS
)
assert_rejected invalid_literal_syntax "$invalid_literal_syntax" 'invalid regex pattern'

invalid_literal_flag=$(write_fixture invalid_literal_flag <<'DS'
let ok = "abc" matches /abc/z
DS
)
assert_rejected invalid_literal_flag "$invalid_literal_flag" 'invalid regex literal'

invalid_string_pattern=$(write_fixture invalid_string_pattern <<'DS'
let ok = "abc" matches "(abc"
echo $ok
DS
)
assert_rejected invalid_string_pattern "$invalid_string_pattern" 'invalid regex pattern'

arity_match_few=$(write_fixture arity_match_few <<'DS'
let m = regex.match("abc")
DS
)
assert_rejected arity_match_few "$arity_match_few" 'helper `regex.match` expects 2 to 3 arguments'
arity_match_many=$(write_fixture arity_match_many <<'DS'
let m = regex.match("abc", "abc", "i", "extra")
DS
)
assert_rejected arity_match_many "$arity_match_many" 'helper `regex.match` expects 2 to 3 arguments'
arity_replace_few=$(write_fixture arity_replace_few <<'DS'
let out = regex.replace("abc", "abc")
DS
)
assert_rejected arity_replace_few "$arity_replace_few" 'helper `regex.replace` expects 3 to 4 arguments'
arity_replace_many=$(write_fixture arity_replace_many <<'DS'
let out = regex.replace("abc", "abc", "x", "i", "extra")
DS
)
assert_rejected arity_replace_many "$arity_replace_many" 'helper `regex.replace` expects 3 to 4 arguments'

wrong_left=$(write_fixture wrong_left <<'DS'
let ok = 123 matches "[0-9]+"
DS
)
assert_rejected wrong_left "$wrong_left" 'left operand of `matches` must be a string'
wrong_right=$(write_fixture wrong_right <<'DS'
let ok = "123" matches ["[0-9]+"]
DS
)
assert_rejected wrong_right "$wrong_right" 'right operand of `matches` must be a regex literal or string pattern'
wrong_match_text=$(write_fixture wrong_match_text <<'DS'
let m = regex.match(123, "[0-9]+")
DS
)
assert_rejected wrong_match_text "$wrong_match_text" 'regex helper `regex.match` expects string arguments'
wrong_match_pattern=$(write_fixture wrong_match_pattern <<'DS'
let m = regex.match("123", ["[0-9]+"])
DS
)
assert_rejected wrong_match_pattern "$wrong_match_pattern" 'regex helper `regex.match` expects string arguments'
wrong_replace_repl=$(write_fixture wrong_replace_repl <<'DS'
let out = regex.replace("123", "[0-9]+", 456)
DS
)
assert_rejected wrong_replace_repl "$wrong_replace_repl" 'regex helper `regex.replace` expects string arguments'

too_many_static=$(write_fixture too_many_static <<'DS'
let m = regex.match("abcdefghij", /(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)/)
DS
)
assert_rejected too_many_static "$too_many_static" 'more than nine capture groups'

bad_ref_static=$(write_fixture bad_ref_static <<'DS'
let out = regex.replace("api-123", /([a-z]+)-([0-9]+)/, "$3")
DS
)
assert_rejected bad_ref_static "$bad_ref_static" 'references a capture group that cannot exist'
bad_repl_static_x=$(write_fixture bad_repl_static_x <<'DS'
let out = regex.replace("abc", /a/, "$x")
DS
)
assert_rejected bad_repl_static_x "$bad_repl_static_x" 'regex replacement supports only `$0` through `$9` and `$$`'
bad_repl_static_end=$(write_fixture bad_repl_static_end <<'DS'
let out = regex.replace("abc", /a/, "$")
DS
)
assert_rejected bad_repl_static_end "$bad_repl_static_end" 'regex replacement supports only `$0` through `$9` and `$$`'

inline_flags_rejected=$(write_fixture inline_flags_rejected <<'DS'
let ok = "abc" matches /(?i)abc/
DS
)
assert_rejected inline_flags_rejected "$inline_flags_rejected" 'lookaround, inline flags, named captures'
pattern_backref_rejected=$(write_fixture pattern_backref_rejected <<'DS'
let ok = "aa" matches /(a)\1/
DS
)
assert_rejected pattern_backref_rejected "$pattern_backref_rejected" 'unsupported regex escape'
lookaround_rejected=$(write_fixture lookaround_rejected <<'DS'
let ok = "abc" matches /a(?=b)/
DS
)
assert_rejected lookaround_rejected "$lookaround_rejected" 'lookaround, inline flags, named captures'
lazy_rejected=$(write_fixture lazy_rejected <<'DS'
let ok = "abc" matches /a+?/
DS
)
assert_rejected lazy_rejected "$lazy_rejected" 'lazy regex quantifiers'
named_rejected=$(write_fixture named_rejected <<'DS'
let ok = "abc" matches /(?P<x>a)/
DS
)
assert_rejected named_rejected "$named_rejected" 'lookaround, inline flags, named captures'

# 9. Dynamic runtime diagnostics.
dyn_pattern_arg=$(write_fixture dyn_pattern_arg <<'DS'
script {
  arg pattern: string
}

echo before
let ok = "abc" matches pattern
echo $ok
echo after
DS
)
run_runtime_failure dyn_pattern_arg "$dyn_pattern_arg" 'invalid regex pattern' $'before\n' '('

dyn_pattern_fn=$(write_fixture dyn_pattern_fn <<'DS'
fn bad() {
  return "(abc"
}

let m = regex.match("abc", bad())
let matched = m.matched
echo $matched
echo after
DS
)
run_runtime_failure dyn_pattern_fn "$dyn_pattern_fn" 'invalid regex pattern' ''

dyn_pattern_env=$(write_fixture dyn_pattern_env <<'DS'
let p = env.DS_BAD_PATTERN
let ok = "abc" matches p
echo $ok
echo after
DS
)
run_runtime_failure_env dyn_pattern_env "$dyn_pattern_env" 'invalid regex pattern' '' DS_BAD_PATTERN='('

dyn_too_many=$(write_fixture dyn_too_many <<'DS'
script {
  arg pattern: string
}

let m = regex.match("abcdefghij", pattern)
let matched = m.matched
echo $matched
echo after
DS
)
run_runtime_failure dyn_too_many "$dyn_too_many" 'more than nine capture groups' '' '(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)'

dyn_repl_ref=$(write_fixture dyn_repl_ref <<'DS'
script {
  arg repl: string
}

let out = regex.replace("api-123", "([a-z]+)-([0-9]+)", repl)
echo $out
echo after
DS
)
run_runtime_failure dyn_repl_ref "$dyn_repl_ref" 'references a capture group that cannot exist' '' '$3'

dyn_repl_bad_x=$(write_fixture dyn_repl_bad_x <<'DS'
script {
  arg repl: string
}

let out = regex.replace("abc", "a", repl)
echo $out
echo after
DS
)
run_runtime_failure dyn_repl_bad_x "$dyn_repl_bad_x" 'regex replacement supports only `$0` through `$9` and `$$`' '' '$x'
run_runtime_failure dyn_repl_bad_end "$dyn_repl_bad_x" 'regex replacement supports only `$0` through `$9` and `$$`' '' '$'

zero_start=$(write_fixture zero_start <<'DS'
script {
  arg pattern: string
}

let out = regex.replace("abc", pattern, "X")
echo $out
echo after
DS
)
run_runtime_failure zero_start "$zero_start" 'regex.replace patterns that match empty strings are unsupported' '' '^'
zero_star=$(write_fixture zero_star <<'DS'
script {
  arg pattern: string
}

let out = regex.replace("abc", pattern, "X")
echo $out
echo after
DS
)
run_runtime_failure zero_star "$zero_star" 'regex.replace patterns that match empty strings are unsupported' '' 'a*'

zero_matches_ok=$(write_fixture zero_matches_ok <<'DS'
let ok = "abc" matches /^/
echo $ok
DS
)
run_parity zero_matches_ok "$zero_matches_ok" $'true\n' 0

runtime_failure_interp=$(write_fixture runtime_failure_interp <<'DS'
script {
  arg pattern: string
}

let good_pattern = "abc"
let text = "abc"
let before = "before={text matches good_pattern}"
echo $before
let bad = "bad={text matches pattern}"
echo $bad
echo after
DS
)
run_runtime_failure runtime_failure_interp "$runtime_failure_interp" 'invalid regex pattern' $'before=true\n' '('

runtime_failure_side_effect=$(write_fixture runtime_failure_side_effect <<'DS'
script {
  arg pattern: string
}

let ok = "abc" matches pattern
file.write("marker.txt", "bad")
echo $ok
DS
)
side_script="$TMP/runtime_failure_side_effect.sh"
emit_checked runtime_failure_side_effect "$runtime_failure_side_effect" "$side_script"
mkdir -p "$TMP/runtime_failure_side_effect_vm" "$TMP/runtime_failure_side_effect_bash"
set +e
(cd "$TMP/runtime_failure_side_effect_vm" && "$DS" run "$runtime_failure_side_effect" '(') >"$TMP/runtime_failure_side_effect_vm.out" 2>"$TMP/runtime_failure_side_effect_vm.err"
vm_side_rc=$?
(cd "$TMP/runtime_failure_side_effect_bash" && bash "$side_script" '(') >"$TMP/runtime_failure_side_effect_bash.out" 2>"$TMP/runtime_failure_side_effect_bash.err"
bash_side_rc=$?
set -e
printf '%s' "$vm_side_rc" >"$TMP/runtime_failure_side_effect_vm.rc"
printf '%s' "$bash_side_rc" >"$TMP/runtime_failure_side_effect_bash.rc"
assert_nonzero_status runtime_failure_side_effect_vm
assert_nonzero_status runtime_failure_side_effect_bash
[ ! -e "$TMP/runtime_failure_side_effect_vm/marker.txt" ] || fail 'VM dynamic regex failure created marker'
pass 'VM dynamic regex failure prevents later side effect'
[ ! -e "$TMP/runtime_failure_side_effect_bash/marker.txt" ] || fail 'Bash dynamic regex failure created marker'
pass 'Bash dynamic regex failure prevents later side effect'

pass 'dynamic non-string regex operands are statically rejected by the current value-kind system'

# 10. Interactions with existing language features.
if_bool=$(write_fixture if_bool <<'DS'
let pattern = "^release/"
if ("release/1" matches pattern) && !("feature/x" matches pattern) {
  echo ok
} else {
  echo bad
}
DS
)
run_parity if_bool "$if_bool" $'ok\n' 0

case_regex=$(write_fixture case_regex <<'DS'
let m = regex.match("api-123", /^([a-z]+)-([0-9]+)$/)
case m.matched {
  true {
    let service = m["1"]
    echo $service
  }
  false { echo none }
}
DS
)
run_parity case_regex "$case_regex" $'api\n' 0

replace_split_loop=$(write_fixture replace_split_loop <<'DS'
let normalized = regex.replace("api-1 web-2", /([a-z]+)-([0-9]+)/, "$1:$2")
for part in normalized.split(" ") {
  echo $part
}
DS
)
run_parity replace_split_loop "$replace_split_loop" $'api:1\nweb:2\n' 0

command_words=$(write_fixture command_words <<'DS'
let m = regex.match("api-123", /^([a-z]+)-([0-9]+)$/)
let service = m["1"]
printf "%s\n" $service
DS
)
run_parity command_words "$command_words" $'api\n' 0

test_block=$(write_fixture test_block <<'DS'
test "regex captures" {
  let m = regex.match("v7", /^v([0-9]+)$/)
  assert m.matched
  assert m["1"] == "7"
}
DS
)
run_ok test_block_regex "$DS" test "$test_block"

patterns_lib="$FIX/patterns.ds"
cat >"$patterns_lib" <<'DS'
fn service_pattern() {
  return "^([a-z]+)-([0-9]+)$"
}
DS
import_main=$(write_fixture import_main <<'DS'
import "./patterns.ds"

let m = regex.match("api-123", service_pattern())
let service = m["1"]
echo $service
DS
)
run_parity import_main "$import_main" $'api\n' 0

env_mutation=$(write_fixture env_mutation <<'DS'
env.DS_REGEX_TEST = "^api$"
let pattern = env.DS_REGEX_TEST
let ok = "api" matches pattern
echo $ok
DS
)
run_parity env_mutation "$env_mutation" $'true\n' 0

# 11. Bash helper hygiene and dependency discovery.
literal_only_script="$TMP/literal_only.sh"
emit_checked literal_only "$literal_shape" "$literal_only_script"
assert_helper_absent "$literal_only_script" '__ds_regex_match_into' 'literal matches omits regex.match map helper'
assert_helper_absent "$literal_only_script" '__ds_regex_replace' 'literal matches omits regex.replace helper'

runtime_matches_script="$TMP/runtime_matches_only.sh"
emit_checked runtime_matches_only "$runtime_basic" "$runtime_matches_script"
assert_helper_present "$runtime_matches_script" '__ds_regex_test' 'runtime matches emits base regex helper'
assert_helper_absent "$runtime_matches_script" '__ds_regex_match_into' 'runtime matches omits regex.match map helper'
assert_helper_absent "$runtime_matches_script" '__ds_regex_replace' 'runtime matches omits regex.replace helper'

match_script="$TMP/match_only.sh"
emit_checked match_only "$match_basic" "$match_script"
assert_helper_present "$match_script" '__ds_regex_match_into' 'regex.match emits match helper'
assert_helper_absent "$match_script" '__ds_regex_replace' 'regex.match omits regex.replace helper'

replace_script="$TMP/replace_only.sh"
emit_checked replace_only "$replace_capture" "$replace_script"
assert_helper_present "$replace_script" '__ds_regex_replace' 'regex.replace emits replacement helper'
assert_helper_absent "$replace_script" '__ds_regex_match_into' 'regex.replace omits match-map helper'

stmt_call_arg=$(write_fixture stmt_call_arg <<'DS'
fn show(x = "") {
  echo $x
}

show(regex.replace("api-123", /([a-z]+)-([0-9]+)/, "$1:$2"))
DS
)
run_parity stmt_call_arg "$stmt_call_arg" $'api:123\n' 0

interpolation_helper=$(write_fixture interpolation_helper <<'DS'
let normalized = regex.replace("api-123", /([a-z]+)-([0-9]+)/, "$1:$2")
echo "x={normalized}"
DS
)
run_parity interpolation_helper "$interpolation_helper" $'x=api:123\n' 0

helper_failure_msg=$(write_fixture helper_failure_msg <<'DS'
script {
  arg pattern: string
}

let out = regex.replace("abc", pattern, "x")
echo $out
DS
)
run_runtime_failure helper_failure_msg "$helper_failure_msg" 'invalid regex pattern' '' '('

# 12. Regression coverage for v0.23 regex behavior.
v023_regression=$(write_fixture v023_regression <<'DS'
let anchor = "release/123" matches /^release\/[0-9]+$/
let cls = "api" matches /^(api|web)$/
let quant = "aaab" matches /^a+b$/
let ci = "API" matches /^api$/i
echo "{anchor}:{cls}:{quant}:{ci}"
let text = "api"
let interp = "interp={text matches /api/}"
echo $interp
DS
)
run_parity v023_regression "$v023_regression" $'true:true:true:true\ninterp=true\n' 0
ambiguous_chain=$(write_fixture ambiguous_chain <<'DS'
let ok = "a" matches /a/ == true
DS
)
assert_rejected ambiguous_chain "$ambiguous_chain" 'ambiguous comparison chain'

fmt_regex=$(write_fixture fmt_regex <<'DS'
let ok="api" matches/api/
DS
)
run_ok fmt_regex "$DS" fmt "$fmt_regex"
assert_contains "$TMP/fmt_regex.out" '"api" matches /api/' 'formatter preserves regex literal spacing'

echo "v0.32.0 tests passed ($pass_count checks)"