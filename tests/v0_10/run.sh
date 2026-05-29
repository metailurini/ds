#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_10_tests.$$"
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

assert_single_diag() {
  local name="$1"
  local fixture="$2"
  local message="$3"
  assert_diag "$name" "$fixture" "$message"
  local count
  count="$(grep -cF "$message" "$TMP/${name}_check.err" || true)"
  [ "$count" = 1 ] || fail "$name diagnostic should appear once, got $count"
  pass "$name diagnostic appears once"
}

FIX="$TMP/fixtures"
mkdir -p "$FIX"

write_fixture "$FIX/arrays_basic.ds" <<'DS'
let services = ["api", "web", "worker"]
let first = services[0]
let second = services[1]
let third = services[2]

echo "{first}"
echo "{second}"
echo "{third}"
DS

write_fixture "$FIX/arrays_empty_push_loop.ds" <<'DS'
let services = []

for service in services {
  echo "SHOULD_NOT_PRINT"
}

services.push("api")
services.push("web service")
services.push("worker-$HOME-`nope`")

for service in services {
  echo "service={service}"
}
DS

write_fixture "$FIX/arrays_push_preserves_types.ds" <<'DS'
let values = []
let number = 1
values.push(1)
values.push(true)
values.push(number)

if 1 in values {
  echo "int yes"
} else {
  echo "int no"
}

if true in values {
  echo "bool yes"
} else {
  echo "bool no"
}

if "1" in values {
  echo "string yes"
} else {
  echo "string no"
}

if 1 in values {
  echo "number yes"
} else {
  echo "number no"
}

let first = values[0]
let second = values[1]
echo "{first}:{second}"
DS

write_fixture "$FIX/arrays_multiline_mixed.ds" <<'DS'
let values = [
  "api",
  3,
  true,
  false
]

let a = values[0]
let b = values[1]
let c = values[2]
let d = values[3]
echo "{a}:{b}:{c}:{d}"
DS

write_fixture "$FIX/maps_basic.ds" <<'DS'
let ports = {
  api: 3000,
  web: 5173
}

let api = ports.api
let web = ports["web"]
echo "{api}:{web}"
DS

write_fixture "$FIX/maps_quoted.ds" <<'DS'
let labels = {
  "api-service": "api",
  "web service": "web value"
}

let api = labels["api-service"]
let web = labels["web service"]
echo "{api}:{web}"
DS

write_fixture "$FIX/maps_special_keys.ds" <<'DS'
let labels = {
  "api's service": "api",
  "web]service": "web",
  "price$HOME": "literal"
}

let api = labels["api's service"]
let web = labels["web]service"]
let price = labels["price$HOME"]
echo "{api}:{web}:{price}"
DS

write_fixture "$FIX/loop_scope.ds" <<'DS'
let services = ["api", "web"]

for service in services {
  let inside = service
  echo "inside={inside}"
}

echo "done"
DS

write_fixture "$FIX/copy_semantics.ds" <<'DS'
let original = ["a"]
let copied = original
original.push("b")

for item in copied {
  echo "copied={item}"
}
for item in original {
  echo "original={item}"
}
DS

write_fixture "$FIX/nested_loops.ds" <<'DS'
let services = ["api", "web"]
let envs = ["dev", "prod"]

for service in services {
  for target_env in envs {
    echo "{service}:{target_env}"
  }
}
DS

write_fixture "$FIX/function_body.ds" <<'DS'
fn deploy(name) {
  let envs = ["dev", "prod"]
  for target_env in envs {
    echo "{name}:{target_env}"
  }
}

deploy("api")
DS

write_fixture "$FIX/import_lib.ds" <<'DS'
let services = ["api", "worker"]
let ports = { api: 3000, worker: 9000 }
fn announce(name) {
  echo "announce={name}"
}
DS
write_fixture "$FIX/import_main.ds" <<DS
import "$FIX/import_lib.ds"

for service in services {
  announce(service)
}
let api = ports.api
echo "api={api}"
DS

write_fixture "$FIX/commands_in_loop.ds" <<'DS'
let services = ["api", "web"]

for service in services {
  sh -c "printf '%s\\n' \"$1\"" -- $service
}
DS

write_fixture "$FIX/capture_in_loop.ds" <<'DS'
let services = ["api", "web"]

for service in services {
  let result = run sh -c "printf out-$1; printf err-$1 >&2; exit 7" -- $service
  echo "{service}:{result.stdout}:{result.stderr}:{result.code}:{result.failed}"
}
DS

write_fixture "$FIX/redirect_in_loop.ds" <<'DS'
let names = ["a", "b"]

for name in names {
  sh -c "printf '%s' \"$1\"" -- $name |>> "out.txt"
}
DS

write_fixture "$FIX/map_only.ds" <<'DS'
let ports = { api: 3000 }
let api = ports.api
echo "{api}"
DS

write_fixture "$FIX/tokens_syntax.ds" <<'DS'
let ports = { api: 3000, web: 5173 }
let services = ["api", "web"]
let first = services[0]
let api = ports.api
for service in services { echo "{service}" }
while true {}
break
continue
DS

write_fixture "$FIX/ast_syntax.ds" <<'DS'
let ports = { api: 3000, web: 5173 }
let services = ["api", "web"]
let first = services[0]
let api = ports.api
for service in services { echo "{service}" }
DS

write_fixture "$FIX/array_only.ds" <<'DS'
let services = ["api"]
let first = services[0]
echo "{first}"
DS

write_fixture "$FIX/dynamic_access.ds" <<'DS'
let services = ["api", "web"]
let idx = 1
let service = services[idx]
let ports = { api: 3000, web: 5173 }
let key = "api"
let port = ports[key]
echo "{service}:{port}"
DS

# Lexer, AST, bytecode, and check smoke coverage.
run_ok tokens_collections "$DS" tokens "$FIX/tokens_syntax.ds"
for token in LBRACKET RBRACKET LBRACE RBRACE COMMA COLON DOT FOR IN BREAK CONTINUE; do
  assert_contains "$TMP/tokens_collections.out" "$token" "lexer emits $token"
done
assert_not_contains "$TMP/tokens_collections.out" '0x' "tokens pointer-free"

run_ok ast_collections "$DS" ast "$FIX/ast_syntax.ds"
assert_contains "$TMP/ast_collections.out" 'ArrayExpr' "AST prints array literal"
assert_contains "$TMP/ast_collections.out" 'MapExpr' "AST prints map literal"
assert_contains "$TMP/ast_collections.out" 'FieldExpr api' "AST prints map field access"
assert_contains "$TMP/ast_collections.out" 'IndexExpr' "AST prints index expression"
assert_contains "$TMP/ast_collections.out" 'ForStmt service' "AST prints array loop"
assert_not_contains "$TMP/ast_collections.out" '0x' "AST pointer-free"

run_ok bytecode_collections "$DS" bytecode "$FIX/arrays_empty_push_loop.ds"
assert_contains "$TMP/bytecode_collections.out" 'ARRAY_LITERAL' "bytecode prints array literal"
assert_contains "$TMP/bytecode_collections.out" 'PUSH_ARRAY' "bytecode prints push"
assert_contains "$TMP/bytecode_collections.out" 'FOR_ARRAY' "bytecode prints array loop"
assert_not_contains "$TMP/bytecode_collections.out" '0x' "bytecode pointer-free"
run_ok bytecode_maps "$DS" bytecode "$FIX/maps_basic.ds"
assert_contains "$TMP/bytecode_maps.out" 'MAP_LITERAL' "bytecode prints map literal"
assert_contains "$TMP/bytecode_maps.out" 'GET_INDEX' "bytecode prints map/index access"

# VM/Bash parity for supported behavior.
assert_vm_bash_parity v0_10_arrays_basic "$FIX/arrays_basic.ds" 0 ""
assert_same_text $'api\nweb\nworker\n' "$TMP/v0_10_arrays_basic_vm.out" "array indexing output"
assert_vm_bash_parity v0_10_arrays_empty_push_loop "$FIX/arrays_empty_push_loop.ds" 0 ""
assert_same_text $'service=api\nservice=web service\nservice=worker-$HOME-`nope`\n' "$TMP/v0_10_arrays_empty_push_loop_vm.out" "empty array and push loop output"
assert_vm_bash_parity v0_10_arrays_push_preserves_types "$FIX/arrays_push_preserves_types.ds" 0 ""
assert_same_text $'int yes\nbool yes\nstring no\nnumber yes\n1:true\n' "$TMP/v0_10_arrays_push_preserves_types_vm.out" "push preserves element type metadata"
assert_vm_bash_parity v0_10_arrays_multiline_mixed "$FIX/arrays_multiline_mixed.ds" 0 ""
assert_same_text $'api:3:true:false\n' "$TMP/v0_10_arrays_multiline_mixed_vm.out" "mixed scalar array output"
assert_vm_bash_parity v0_10_maps_basic "$FIX/maps_basic.ds" 0 ""
assert_same_text $'3000:5173\n' "$TMP/v0_10_maps_basic_vm.out" "map field/bracket access output"
assert_vm_bash_parity v0_10_maps_quoted "$FIX/maps_quoted.ds" 0 ""
assert_same_text $'api:web value\n' "$TMP/v0_10_maps_quoted_vm.out" "quoted map key output"
assert_vm_bash_parity v0_10_maps_special_keys "$FIX/maps_special_keys.ds" 0 ""
assert_same_text $'api:web:literal\n' "$TMP/v0_10_maps_special_keys_vm.out" "special quoted map keys preserve VM/Bash parity"
assert_vm_bash_parity v0_10_dynamic_access "$FIX/dynamic_access.ds" 0 ""
assert_same_text $'web:3000\n' "$TMP/v0_10_dynamic_access_vm.out" "dynamic index and map key output"
assert_vm_bash_parity v0_10_loop_scope "$FIX/loop_scope.ds" 0 ""
assert_vm_bash_parity v0_10_copy_semantics "$FIX/copy_semantics.ds" 0 ""
assert_same_text $'copied=a\noriginal=a\noriginal=b\n' "$TMP/v0_10_copy_semantics_vm.out" "collection assignment is value-copy"
assert_vm_bash_parity v0_10_nested_loops "$FIX/nested_loops.ds" 0 ""
assert_same_text $'api:dev\napi:prod\nweb:dev\nweb:prod\n' "$TMP/v0_10_nested_loops_vm.out" "nested array loop output"
assert_vm_bash_parity v0_10_function_body "$FIX/function_body.ds" 0 ""
assert_same_text $'api:dev\napi:prod\n' "$TMP/v0_10_function_body_vm.out" "collections inside function body output"
assert_vm_bash_parity v0_10_import "$FIX/import_main.ds" 0 ""
assert_same_text $'announce=api\nannounce=worker\napi=3000\n' "$TMP/v0_10_import_vm.out" "imported collections compose with functions"
assert_vm_bash_parity v0_10_commands_in_loop "$FIX/commands_in_loop.ds" 0 ""
assert_same_text $'api\nweb\n' "$TMP/v0_10_commands_in_loop_vm.out" "command args inside loop are quoted"
assert_vm_bash_parity v0_10_capture_in_loop "$FIX/capture_in_loop.ds" 0 ""
assert_contains "$TMP/v0_10_capture_in_loop_vm.out" 'api:out-api:err-api:7:true' "captured command result inside loop"
assert_vm_bash_parity v0_10_redirect_in_loop "$FIX/redirect_in_loop.ds" 0 "out.txt"
assert_same_text 'ab' "$TMP/parity_v0_10_redirect_in_loop_vm_work/out.txt" "loop redirection append output"

# Bash emission inspections.
run_ok emit_array_only "$DS" emit bash "$FIX/array_only.ds" -o "$TMP/array_only.sh"
run_ok emit_array_only_syntax bash -n "$TMP/array_only.sh"
assert_not_contains "$TMP/array_only.sh" 'Bash 4 or newer is required' "array-only script has no Bash 4 map guard"
assert_not_contains "$TMP/array_only.sh" "$DS" "array-only Bash is standalone"
run_ok emit_map_only "$DS" emit bash "$FIX/map_only.ds" -o "$TMP/map_only.sh"
run_ok emit_map_only_syntax bash -n "$TMP/map_only.sh"
assert_contains "$TMP/map_only.sh" 'maps require Bash 4 or newer' "map script emits Bash 4 guard"
assert_contains "$TMP/map_only.sh" 'declare -A' "map script uses associative array"
assert_not_contains "$TMP/map_only.sh" "$DS" "map Bash is standalone"

# Runtime failures in VM and emitted Bash should fail clearly.
write_fixture "$FIX/bad_runtime_array_oob.ds" <<'DS'
let services = ["api"]
let missing = services[1]
echo "{missing}"
DS
run_ok oob_emit "$DS" emit bash "$FIX/bad_runtime_array_oob.ds" -o "$TMP/oob.sh"
capture_status oob_vm "$DS" run "$FIX/bad_runtime_array_oob.ds"
assert_nonzero_status oob_vm
assert_contains "$TMP/oob_vm.err" 'array index 1 out of range' "VM out-of-range diagnostic"
capture_status oob_bash bash "$TMP/oob.sh"
assert_nonzero_status oob_bash
assert_contains "$TMP/oob_bash.err" 'array index 1 out of range' "Bash out-of-range diagnostic"

write_fixture "$FIX/bad_runtime_map_missing.ds" <<'DS'
let ports = { api: 3000 }
let missing = ports["web"]
echo "{missing}"
DS
run_ok missing_map_emit "$DS" emit bash "$FIX/bad_runtime_map_missing.ds" -o "$TMP/missing_map.sh"
capture_status missing_map_vm "$DS" run "$FIX/bad_runtime_map_missing.ds"
assert_nonzero_status missing_map_vm
assert_contains "$TMP/missing_map_vm.err" 'missing map key `web`' "VM missing map key diagnostic"
capture_status missing_map_bash bash "$TMP/missing_map.sh"
assert_nonzero_status missing_map_bash
assert_contains "$TMP/missing_map_bash.err" "missing map key 'web'" "Bash missing map key diagnostic"

# Syntax and lowering diagnostics.
write_fixture "$FIX/bad_array_unclosed.ds" <<'DS'
let xs = [
DS
assert_diag array_unclosed "$FIX/bad_array_unclosed.ds" 'expected `]` to close array literal'

write_fixture "$FIX/bad_array_trailing_comma.ds" <<'DS'
let xs = ["a",]
DS
assert_diag array_trailing_comma "$FIX/bad_array_trailing_comma.ds" 'expected array element after `,`'

write_fixture "$FIX/bad_array_double_comma.ds" <<'DS'
let xs = ["a",, "b"]
DS
assert_diag array_double_comma "$FIX/bad_array_double_comma.ds" 'expected expression'

write_fixture "$FIX/bad_index_empty.ds" <<'DS'
let xs = ["a"]
let x = xs[]
DS
assert_diag index_empty "$FIX/bad_index_empty.ds" 'expected index expression after `[`'

write_fixture "$FIX/bad_index_unclosed.ds" <<'DS'
let xs = ["a"]
let x = xs[0
DS
assert_diag index_unclosed "$FIX/bad_index_unclosed.ds" 'expected `]` after index expression'

write_fixture "$FIX/bad_map_missing_colon.ds" <<'DS'
let ports = { api }
DS
assert_diag map_missing_colon "$FIX/bad_map_missing_colon.ds" 'expected `:` after map key'

write_fixture "$FIX/bad_map_missing_value.ds" <<'DS'
let ports = { api: }
DS
assert_diag map_missing_value "$FIX/bad_map_missing_value.ds" 'expected map value after `:`'

write_fixture "$FIX/bad_map_missing_key.ds" <<'DS'
let ports = { : 3000 }
DS
assert_diag map_missing_key "$FIX/bad_map_missing_key.ds" 'expected map key before `:`'

write_fixture "$FIX/bad_map_double_comma.ds" <<'DS'
let ports = { api: 3000,, web: 5173 }
DS
assert_diag map_double_comma "$FIX/bad_map_double_comma.ds" 'expected map key before `:`'

write_fixture "$FIX/bad_field_missing.ds" <<'DS'
let ports = { api: 3000 }
let x = ports.
DS
assert_diag field_missing "$FIX/bad_field_missing.ds" 'expected field name after `.`'

write_fixture "$FIX/bad_for_missing_name.ds" <<'DS'
for in services {}
DS
assert_diag for_missing_name "$FIX/bad_for_missing_name.ds" 'expected loop variable after `for`'

write_fixture "$FIX/bad_for_missing_in.ds" <<'DS'
for service services {}
DS
assert_diag for_missing_in "$FIX/bad_for_missing_in.ds" 'expected `in` after loop variable'

write_fixture "$FIX/bad_for_missing_iterable.ds" <<'DS'
for service in {}
DS
assert_diag for_missing_iterable "$FIX/bad_for_missing_iterable.ds" 'expected iterable expression after `in`'

write_fixture "$FIX/bad_for_missing_block.ds" <<'DS'
let services = ["api"]
for service in services echo "missing block"
DS
assert_diag for_missing_block "$FIX/bad_for_missing_block.ds" 'expected `{` after for iterable'

write_fixture "$FIX/bad_for_missing_value_name.ds" <<'DS'
let ports = { api: 3000 }
for name, in ports {}
DS
assert_diag for_missing_value_name "$FIX/bad_for_missing_value_name.ds" 'expected value loop variable after `,`'

write_fixture "$FIX/bad_while_empty.ds" <<'DS'
while {}
DS
assert_diag while_empty "$FIX/bad_while_empty.ds" 'expected condition after `while`'

write_fixture "$FIX/bad_while_missing_block.ds" <<'DS'
while i < 3 echo "missing block"
DS
assert_diag while_missing_block "$FIX/bad_while_missing_block.ds" 'expected `{` after while condition'

write_fixture "$FIX/bad_index_type.ds" <<'DS'
let services = ["api"]
let bad = services["0"]
DS
assert_diag index_type "$FIX/bad_index_type.ds" 'array index must be an int in v0.10.0'

write_fixture "$FIX/bad_push_non_array.ds" <<'DS'
let service = "api"
service.push("web")
DS
assert_diag push_non_array "$FIX/bad_push_non_array.ds" '`push` requires an array variable in v0.10.0'

write_fixture "$FIX/bad_push_no_arg.ds" <<'DS'
let services = ["api"]
services.push()
DS
assert_diag push_no_arg "$FIX/bad_push_no_arg.ds" 'expected value argument for `push`'

write_fixture "$FIX/bad_push_too_many_args.ds" <<'DS'
let services = ["api"]
services.push("web", "worker")
DS
assert_diag push_too_many_args "$FIX/bad_push_too_many_args.ds" '`push` accepts exactly one argument in v0.10.0'

write_fixture "$FIX/bad_unknown_method.ds" <<'DS'
let services = ["api"]
services.missing("web")
DS
assert_diag unknown_method "$FIX/bad_unknown_method.ds" 'only `push` collection method is supported in v0.10.0'

write_fixture "$FIX/bad_push_literal.ds" <<'DS'
["api"].push("web")
DS
assert_diag push_literal "$FIX/bad_push_literal.ds" 'unknown command variable `["api"]`'

write_fixture "$FIX/bad_negative_index.ds" <<'DS'
let services = ["api"]
let bad = services[-1]
DS
assert_diag negative_index "$FIX/bad_negative_index.ds" 'array index must be non-negative'

write_fixture "$FIX/bad_duplicate_key_ident.ds" <<'DS'
let ports = { api: 3000, api: 3001 }
DS
assert_diag duplicate_key_ident "$FIX/bad_duplicate_key_ident.ds" 'duplicate map key `api`'

write_fixture "$FIX/bad_duplicate_key_quoted.ds" <<'DS'
let ports = { "api": 3000, api: 3001 }
DS
assert_diag duplicate_key_quoted "$FIX/bad_duplicate_key_quoted.ds" 'duplicate map key `api`'

write_fixture "$FIX/bad_field_non_map.ds" <<'DS'
let not_map = "api"
let bad = not_map.api
DS
assert_diag field_non_map "$FIX/bad_field_non_map.ds" 'field access is only supported on command results and maps in v0.10.0'

write_fixture "$FIX/bad_map_numeric_key.ds" <<'DS'
let ports = { api: 3000 }
let bad = ports[0]
DS
assert_diag map_numeric_key "$FIX/bad_map_numeric_key.ds" 'map index must be a string in v0.10.0'

write_fixture "$FIX/bad_empty_map.ds" <<'DS'
let ports = {}
DS
assert_diag empty_map "$FIX/bad_empty_map.ds" 'empty map literals are deferred in v0.10.0'

write_fixture "$FIX/bad_empty_map_key.ds" <<'DS'
let ports = { "": 3000 }
DS
assert_diag empty_map_key "$FIX/bad_empty_map_key.ds" 'empty map keys are deferred in v0.10.0 because emitted Bash cannot represent them safely'

write_fixture "$FIX/bad_loop_non_array.ds" <<'DS'
let service = "api"

for item in service {
  echo "{item}"
}
DS
assert_diag loop_non_array "$FIX/bad_loop_non_array.ds" 'for loop iterable must be an array in v0.10.0'

write_fixture "$FIX/bad_loop_array_literal.ds" <<'DS'
for item in ["api", "web"] {
  echo "{item}"
}
DS
assert_diag loop_array_literal "$FIX/bad_loop_array_literal.ds" 'for loop iterable must be a named array or known stdlib array result for VM/Bash parity in v0.10.0; bind temporary arrays to a variable first'

write_fixture "$FIX/map_iteration_supported.ds" <<'DS'
let ports = { api: 3000 }

for name, port in ports {
  echo "{name}:{port}"
}
DS
assert_vm_bash_parity v0_10_map_iteration_supported "$FIX/map_iteration_supported.ds" 0 ""
assert_same_text $'api:3000\n' "$TMP/v0_10_map_iteration_supported_vm.out" "map iteration is supported after v0.29.0"

write_fixture "$FIX/bad_loop_variable_leak.ds" <<'DS'
let services = ["api"]
for service in services {
  echo "{service}"
}
echo $service
DS
assert_diag loop_variable_leak "$FIX/bad_loop_variable_leak.ds" 'unknown command variable `service`'

write_fixture "$FIX/bad_loop_local_leak.ds" <<'DS'
let services = ["api"]
for service in services {
  let temp = service
}
echo $temp
DS
assert_diag loop_local_leak "$FIX/bad_loop_local_leak.ds" 'unknown command variable `temp`'

write_fixture "$FIX/bad_loop_shadow.ds" <<'DS'
let service = "outer"
let services = ["inner"]

for service in services {
  echo "{service}"
}
DS
run_ok loop_shadow_check "$DS" check "$FIX/bad_loop_shadow.ds"
assert_contains "$TMP/loop_shadow_check.err" 'shadows an outer declaration' 'loop shadow warning appears'

write_fixture "$FIX/bad_collection_function_arg.ds" <<'DS'
fn show(items) {
  echo "unused"
}

let services = ["api"]
show(services)
DS
assert_diag collection_function_arg "$FIX/bad_collection_function_arg.ds" 'passing collection values to functions is deferred in v0.10.0'

write_fixture "$FIX/bad_array_expression_element.ds" <<'DS'
let values = ["a" == "a", !false]
DS
assert_diag array_expression_element "$FIX/bad_array_expression_element.ds" 'collection element expressions must be scalar Bash-emittable values in v0.10.0'

write_fixture "$FIX/bad_map_expression_value.ds" <<'DS'
let values = { ok: "a" == "a" }
DS
assert_diag map_expression_value "$FIX/bad_map_expression_value.ds" 'collection element expressions must be scalar Bash-emittable values in v0.10.0'

write_fixture "$FIX/bad_push_expression_value.ds" <<'DS'
let values = []
values.push(!false)
DS
assert_diag push_expression_value "$FIX/bad_push_expression_value.ds" 'collection element expressions must be scalar Bash-emittable values in v0.10.0'

write_fixture "$FIX/bad_collection_command_arg.ds" <<'DS'
let services = ["api"]
echo $services
DS
assert_diag collection_command_arg "$FIX/bad_collection_command_arg.ds" 'cannot be passed directly as a command argument in v0.10.0'

write_fixture "$FIX/bad_collection_access_command_arg.ds" <<'DS'
let services = ["api"]
let ports = { api: 3000 }
echo $services[0]
echo $ports.api
echo $ports["api"]
DS
assert_diag collection_access_command_arg "$FIX/bad_collection_access_command_arg.ds" 'collection access command arguments are deferred in v0.10.0; bind the indexed value to a variable first'

write_fixture "$FIX/bad_break.ds" <<'DS'
break
DS
assert_diag break_deferred "$FIX/bad_break.ds" '`break` is only allowed inside a loop'

write_fixture "$FIX/bad_continue.ds" <<'DS'
continue
DS
assert_diag continue_deferred "$FIX/bad_continue.ds" '`continue` is only allowed inside a loop'

write_fixture "$FIX/bad_nested_array.ds" <<'DS'
let nested = [["api"]]
DS
assert_diag nested_array "$FIX/bad_nested_array.ds" 'nested collections are deferred in v0.10.0'

write_fixture "$FIX/bad_nested_map.ds" <<'DS'
let nested = { api: { port: 3000 } }
DS
assert_diag nested_map "$FIX/bad_nested_map.ds" 'nested collections are deferred in v0.10.0'

run_ok stale_emit_seed "$DS" emit bash "$FIX/arrays_basic.ds" -o "$TMP/stale.sh"
assert_contains "$TMP/stale.sh" '#!/usr/bin/env bash' "seed emit wrote script"
run_fail stale_emit_invalid "$DS" emit bash "$FIX/bad_empty_map.ds" -o "$TMP/stale.sh"
assert_file_missing_or_empty "$TMP/stale.sh" "failed collection emit removes stale artifact"

# Makefile and completion-review integration.
assert_contains "$ROOT/Makefile" 'TEST_VERSIONS := 0-1 0-2 0-3 0-4 0-5 0-6 0-7 0-8 0-9 0-10' "Makefile lists all suite versions once"
assert_contains "$ROOT/Makefile" 'TEST_TARGETS := $(addprefix test-v,$(TEST_VERSIONS))' "Makefile derives per-version test targets"
assert_contains "$ROOT/Makefile" '$(TEST_TARGETS): $(BIN)' "Makefile exposes generated per-version test targets"
assert_contains "$ROOT/Makefile" 'tests/v$(subst -,_,$(patsubst test-v%,%,$@))/run.sh' "Makefile maps test target names to suite directories"
assert_contains "$ROOT/docs/milestones/v0.10.0-spec.md" 'Tests added' "v0.10 spec completion review records tests"
assert_contains "$ROOT/docs/milestones/v0.10.0-spec.md" 'tests/v0_10/run.sh' "v0.10 spec names test suite"
assert_contains "$ROOT/README.md" 'v0.10.0` implementation and tests are complete' "README status is current for v0.10"

# Map runtime architecture: language maps must use the owned hashmap through the
# DsMap boundary, without leaking hashmap internals into frontend/lowering/VM/emitter code.
assert_contains "$ROOT/Makefile" 'src/runtime/hashmap.c' "owned hashmap is linked into ds"
assert_contains "$ROOT/src/runtime.c" '#include "runtime/hashmap.h"' "DsMap runtime wrapper uses hashmap implementation"
assert_contains "$ROOT/src/runtime.c" 'hm_put_len' "DsMap set uses hashmap insertion"
assert_contains "$ROOT/src/runtime.c" 'hm_get_len' "DsMap get uses hashmap lookup"
if grep -R -nE '#include[[:space:]]+[<"].*hashmap\.h|\bhm_[a-z]' "$ROOT/src" "$ROOT/include" | grep -v 'src/runtime.c' | grep -v 'src/runtime/hashmap\.[ch]' >/dev/null; then
  grep -R -nE '#include[[:space:]]+[<"].*hashmap\.h|\bhm_[a-z]' "$ROOT/src" "$ROOT/include" | grep -v 'src/runtime.c' | grep -v 'src/runtime/hashmap\.[ch]' >&2 || true
  fail "hashmap internals should stay behind the DsMap runtime wrapper"
fi
pass "hashmap internals stay behind the DsMap runtime wrapper"

echo "v0.10.0 tests passed: $pass_count checks"
