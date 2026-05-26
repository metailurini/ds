#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_5_tests.$$"
FIX="$ROOT/tests/v0_5/fixtures"
GOLD="$ROOT/tests/v0_5/golden"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != 1 ]]; then
  make -C "$ROOT" clean all >/dev/null
fi

cc -std=c99 -Wall -Wextra -Wpedantic -I"$ROOT/include" \
  "$ROOT/tests/v0_5/unit/lower.c" \
  "$ROOT/src/source.c" "$ROOT/src/diag.c" "$ROOT/src/lexer.c" "$ROOT/src/ast.c" "$ROOT/src/parser.c" "$ROOT/src/parse_expr.c" "$ROOT/src/parse_command.c" "$ROOT/src/parse_script.c" "$ROOT/src/parse_function.c" "$ROOT/src/parse_stmt.c" "$ROOT/src/lower.c" "$ROOT/src/lower_symbols.c" "$ROOT/src/lower_expr.c" "$ROOT/src/lower_interpolation.c" "$ROOT/src/lower_collection.c" "$ROOT/src/lower_command.c" "$ROOT/src/lower_stmt.c" "$ROOT/src/lower_stdlib.c" "$ROOT/src/lower_functions.c" "$ROOT/src/lower_tests.c" "$ROOT/src/lower_free.c" "$ROOT/src/ds_command.c" "$ROOT/src/ds_interpolation.c" "$ROOT/src/runtime.c" "$ROOT/src/ds_stdlib.c" "$ROOT/src/runtime/hashmap.c" \
  -o "$TMP/test_v0_5_lower"
run_ok lower_contract_unit "$TMP/test_v0_5_lower"
assert_contains "$TMP/lower_contract_unit.out" "v0.5 lowering unit checks passed" "lower unit reports checks"

# Lexer, AST, bytecode, help, and Bash golden stability.
run_ok tokens_args_basic "$DS" tokens "tests/v0_5/fixtures/args_basic.ds"
assert_golden "$GOLD/args_basic.tokens" "$TMP/tokens_args_basic.out" "args token golden"
run_ok ast_args_basic "$DS" ast "tests/v0_5/fixtures/args_basic.ds"
assert_golden "$GOLD/args_basic.ast" "$TMP/ast_args_basic.out" "args AST golden"
run_ok bytecode_args_basic "$DS" bytecode "tests/v0_5/fixtures/args_basic.ds"
assert_golden "$GOLD/args_basic.bytecode" "$TMP/bytecode_args_basic.out" "args bytecode golden"
run_ok help_args_basic "$DS" "tests/v0_5/fixtures/args_basic.ds" --help
assert_golden "$GOLD/args_basic.help" "$TMP/help_args_basic.out" "args help golden"
run_ok emit_args_basic "$DS" emit bash "tests/v0_5/fixtures/args_basic.ds" -o "$TMP/args_basic.sh"
assert_golden "$GOLD/args_basic.sh" "$TMP/args_basic.sh" "args Bash golden"
run_ok emitted_args_basic_syntax bash -n "$TMP/args_basic.sh"
assert_contains "$TMP/args_basic.sh" "#!/usr/bin/env bash" "emitted Bash has shebang"
assert_contains "$TMP/args_basic.sh" "set -euo pipefail" "emitted Bash keeps strict mode"
assert_not_contains "$TMP/args_basic.sh" "$DS" "emitted Bash does not reference ds binary"

# Parser/frontend source edge cases.
run_ok tokens_comments_utf8 "$DS" tokens "$FIX/comments_crlf_source.ds"
assert_contains "$TMP/tokens_comments_utf8.out" "SCRIPT" "script token after utf8 comments"
assert_contains "$TMP/tokens_comments_utf8.out" "ARG" "arg token after comments"
run_ok ast_comments_utf8 "$DS" ast "$FIX/comments_crlf_source.ds"
assert_contains "$TMP/ast_comments_utf8.out" "ScriptBlock" "AST includes script block after comments"
run_ok crlf_check "$DS" check "$FIX/crlf.ds"
run_ok crlf_run "$DS" run "$FIX/crlf.ds" api
assert_same_text $'api\n' "$TMP/crlf_run.out" "CRLF runtime stdout"
run_ok no_trailing_check "$DS" check "$FIX/no_trailing_newline.ds"
run_ok no_trailing_run "$DS" run "$FIX/no_trailing_newline.ds" api
assert_same_text $'api\n' "$TMP/no_trailing_run.out" "no trailing newline runtime stdout"

# VM argv success matrix.
run_ok vm_required_default "$DS" run "$FIX/args_basic.ds" api
assert_same_text $'Deploying api to staging\nretries=3\nforce disabled\n' "$TMP/vm_required_default.out" "VM required/default stdout"
run_ok vm_all_options "$DS" run "$FIX/args_basic.ds" api --target production --retries 5 --force
assert_same_text $'Deploying api to production\nretries=5\nforce enabled\n' "$TMP/vm_all_options.out" "VM all options stdout"
run_ok vm_direct_all_options "$DS" "$FIX/args_basic.ds" api --target production --retries 5 --force
assert_same "$TMP/vm_all_options.out" "$TMP/vm_direct_all_options.out" "direct VM stdout matches run"
run_ok vm_option_before_positional "$DS" run "$FIX/args_basic.ds" --target production api
assert_contains "$TMP/vm_option_before_positional.out" "Deploying api to production" "VM option before positional"
run_ok vm_option_after_positional "$DS" run "$FIX/args_basic.ds" api --target production
assert_contains "$TMP/vm_option_after_positional.out" "Deploying api to production" "VM option after positional"
run_ok vm_double_dash "$DS" run "$FIX/args_double_dash.ds" first -- --literal
assert_same_text $'first=first\nsecond=--literal\n' "$TMP/vm_double_dash.out" "VM double dash data"
run_ok vm_int_arg_bool_option "$DS" run "$FIX/args_types.ds" 42 --dry true --label custom
assert_same_text $'count=42\ndry=true\nlabel=custom\n' "$TMP/vm_int_arg_bool_option.out" "VM int arg bool option stdout"
run_ok vm_string_spaces "$DS" run "$FIX/args_safety.ds" "hello world" --label "two words"
assert_same_text $'value=hello world\nlabel=two words\n' "$TMP/vm_string_spaces.out" "VM string values with spaces"
run_ok vm_empty_option_value "$DS" run "$FIX/args_safety.ds" value --label ""
assert_same_text $'value=value\nlabel=\n' "$TMP/vm_empty_option_value.out" "VM empty option value"
run_ok vm_help_short "$DS" run "$FIX/args_basic.ds" -h --bad
assert_golden "$GOLD/args_basic.help" "$TMP/vm_help_short.out" "VM -h help wins"
assert_same_text '' "$TMP/vm_help_short.err" "VM help stderr empty"
run_ok options_only_default "$DS" run "$FIX/options_only.ds"
assert_same_text $'target=staging\nforce=false\n' "$TMP/options_only_default.out" "options-only default run"
run_ok options_only_override "$DS" run "$FIX/options_only.ds" --force --target prod
assert_same_text $'target=prod\nforce=true\n' "$TMP/options_only_override.out" "options-only overrides"

# VM argv error matrix; body must not run.
for case in missing_arg extra_pos unknown missing_value missing_before_option duplicate_option duplicate_flag invalid_int invalid_int_suffix invalid_int_empty invalid_int_lone_minus invalid_bool; do
  : >"$TMP/$case.out"
  : >"$TMP/$case.err"
done
capture_status missing_arg "$DS" run "$FIX/args_basic.ds"
assert_nonzero_status missing_arg
assert_contains "$TMP/missing_arg.err" "missing required argument" "missing arg diagnostic"
assert_not_contains "$TMP/missing_arg.out" "Deploying" "missing arg skips body"
capture_status extra_pos "$DS" run "$FIX/args_basic.ds" api extra
assert_nonzero_status extra_pos
assert_contains "$TMP/extra_pos.err" "unexpected extra positional argument" "extra positional diagnostic"
capture_status unknown "$DS" run "$FIX/args_basic.ds" api --unknown
assert_nonzero_status unknown
assert_contains "$TMP/unknown.err" 'unknown option `--unknown`' "unknown option diagnostic"
capture_status missing_value "$DS" run "$FIX/args_basic.ds" api --target
assert_nonzero_status missing_value
assert_contains "$TMP/missing_value.err" 'option `--target` requires a value' "missing option value diagnostic"
capture_status missing_before_option "$DS" run "$FIX/args_basic.ds" api --target --force
assert_nonzero_status missing_before_option
assert_contains "$TMP/missing_before_option.err" 'option `--target` requires a value' "missing before option diagnostic"
capture_status duplicate_option "$DS" run "$FIX/args_basic.ds" api --target a --target b
assert_nonzero_status duplicate_option
assert_contains "$TMP/duplicate_option.err" 'duplicate option `--target`' "duplicate option diagnostic"
capture_status duplicate_flag "$DS" run "$FIX/args_basic.ds" api --force --force
assert_nonzero_status duplicate_flag
assert_contains "$TMP/duplicate_flag.err" 'duplicate option `--force`' "duplicate flag diagnostic"
capture_status invalid_int "$DS" run "$FIX/args_basic.ds" api --retries abc
assert_nonzero_status invalid_int
assert_contains "$TMP/invalid_int.err" 'invalid int value `abc` for `retries`' "invalid int diagnostic"
capture_status invalid_int_suffix "$DS" run "$FIX/args_basic.ds" api --retries 1abc
assert_nonzero_status invalid_int_suffix
assert_contains "$TMP/invalid_int_suffix.err" 'invalid int value `1abc` for `retries`' "invalid int suffix diagnostic"
capture_status invalid_int_empty "$DS" run "$FIX/args_basic.ds" api --retries ""
assert_nonzero_status invalid_int_empty
assert_contains "$TMP/invalid_int_empty.err" 'invalid int value `' "invalid empty int diagnostic"
capture_status invalid_int_lone_minus "$DS" run "$FIX/args_basic.ds" api --retries -
assert_nonzero_status invalid_int_lone_minus
assert_contains "$TMP/invalid_int_lone_minus.err" 'invalid int value `-`' "invalid lone minus diagnostic"
capture_status invalid_int_overflow "$DS" run "$FIX/args_basic.ds" api --retries 999999999999999999999999999999999999
assert_nonzero_status invalid_int_overflow
assert_contains "$TMP/invalid_int_overflow.err" 'invalid int value `999999999999999999999999999999999999` for `retries`' "invalid int overflow diagnostic"
capture_status invalid_bool "$DS" run "$FIX/args_types.ds" 1 --dry maybe
assert_nonzero_status invalid_bool
assert_contains "$TMP/invalid_bool.err" 'invalid bool value `maybe` for `dry`' "invalid bool diagnostic"

# Bash runtime success/error matrix and VM parity.
emit_bash "$FIX/args_basic.ds" "$TMP/basic_emitted.sh"
run_ok bash_required_default bash "$TMP/basic_emitted.sh" api
assert_same "$TMP/vm_required_default.out" "$TMP/bash_required_default.out" "Bash default parity"
run_ok bash_all_options bash "$TMP/basic_emitted.sh" api --target production --retries 5 --force
assert_same "$TMP/vm_all_options.out" "$TMP/bash_all_options.out" "Bash all-options parity"
run_ok bash_help bash "$TMP/basic_emitted.sh" --help
assert_golden "$GOLD/args_basic.help" "$TMP/bash_help.out" "Bash help golden"
run_ok bash_double_dash_emit "$DS" emit bash "$FIX/args_double_dash.ds" -o "$TMP/double_dash.sh"
run_ok bash_double_dash bash "$TMP/double_dash.sh" first -- --literal
assert_same "$TMP/vm_double_dash.out" "$TMP/bash_double_dash.out" "Bash double dash parity"
run_ok bash_types_emit "$DS" emit bash "$FIX/args_types.ds" -o "$TMP/types.sh"
run_ok bash_types bash "$TMP/types.sh" 42 --dry true --label custom
assert_same "$TMP/vm_int_arg_bool_option.out" "$TMP/bash_types.out" "Bash int/bool parity"
run_ok bash_safety_emit "$DS" emit bash "$FIX/args_safety.ds" -o "$TMP/safety.sh"
run_ok bash_spaces bash "$TMP/safety.sh" "hello world" --label "two words"
assert_same "$TMP/vm_string_spaces.out" "$TMP/bash_spaces.out" "Bash spaces parity"

for payload in '$HOME' '$(echo hacked)' '`echo hacked`' '"quoted"' 'back\slash' '{braces}' 'semi;colon' 'star*glob'; do
  safe_name="safety_$(printf '%s' "$payload" | tr -c 'A-Za-z0-9' '_')"
  run_ok "vm_$safe_name" "$DS" run "$FIX/args_safety.ds" "$payload" --label "$payload"
  run_ok "bash_$safe_name" bash "$TMP/safety.sh" "$payload" --label "$payload"
  assert_same "$TMP/vm_$safe_name.out" "$TMP/bash_$safe_name.out" "safety parity $payload"
  assert_contains "$TMP/vm_$safe_name.out" "value=$payload" "safety literal value $payload"
done

for name in bash_missing_arg bash_extra_pos bash_unknown bash_missing_value bash_duplicate_option bash_duplicate_flag bash_invalid_int; do :; done
capture_status bash_missing_arg bash "$TMP/basic_emitted.sh"
assert_nonzero_status bash_missing_arg
assert_contains "$TMP/bash_missing_arg.err" "missing required argument" "Bash missing arg diagnostic"
capture_status bash_extra_pos bash "$TMP/basic_emitted.sh" api extra
assert_nonzero_status bash_extra_pos
assert_contains "$TMP/bash_extra_pos.err" "unexpected extra positional argument" "Bash extra positional diagnostic"
capture_status bash_unknown bash "$TMP/basic_emitted.sh" api --unknown
assert_nonzero_status bash_unknown
assert_contains "$TMP/bash_unknown.err" 'unknown option `--unknown`' "Bash unknown option diagnostic"
capture_status bash_missing_value bash "$TMP/basic_emitted.sh" api --target
assert_nonzero_status bash_missing_value
assert_contains "$TMP/bash_missing_value.err" 'option `--target` requires a value' "Bash missing value diagnostic"
capture_status bash_duplicate_option bash "$TMP/basic_emitted.sh" api --target a --target b
assert_nonzero_status bash_duplicate_option
assert_contains "$TMP/bash_duplicate_option.err" 'duplicate option `--target`' "Bash duplicate option diagnostic"
capture_status bash_duplicate_flag bash "$TMP/basic_emitted.sh" api --force --force
assert_nonzero_status bash_duplicate_flag
assert_contains "$TMP/bash_duplicate_flag.err" 'duplicate option `--force`' "Bash duplicate flag diagnostic"
capture_status bash_invalid_int bash "$TMP/basic_emitted.sh" api --retries abc
assert_nonzero_status bash_invalid_int
assert_contains "$TMP/bash_invalid_int.err" 'invalid int value `abc` for `retries`' "Bash invalid int diagnostic"
capture_status bash_invalid_int_overflow bash "$TMP/basic_emitted.sh" api --retries 999999999999999999999999999999999999
assert_nonzero_status bash_invalid_int_overflow
assert_contains "$TMP/bash_invalid_int_overflow.err" 'invalid int value `999999999999999999999999999999999999` for `retries`' "Bash invalid int overflow diagnostic"

# Source diagnostics and no output artifact for invalid source.
declare -A invalid_expect=(
  [invalid_duplicate.ds]='duplicate variable `app`'
  [invalid_default.ds]='default for `retries` must be an int'
  [invalid_int_overflow_default.ds]='default for `retries` must be an int'
  [invalid_flag_true.ds]='flag `force` default `true`'
  [invalid_conflict_let.ds]='duplicate variable `app`'
  [invalid_after_body.ds]='script` block must appear before executable statements'
  [invalid_bool_arg.ds]='bool positional args are not supported'
  [invalid_nested_script.ds]='script` block is only allowed at top level'
  [invalid_missing_brace.ds]='expected `{` after `script`'
  [invalid_missing_type.ds]='expected type name `string`, `int`, or `bool`'
  [invalid_option_no_default.ds]='expected `=` after option type'
  [invalid_flag_type.ds]='flag `force` must have type `bool`'
)
for file in "${!invalid_expect[@]}"; do
  base="${file%.ds}"
  run_fail "check_$base" "$DS" check "$FIX/$file"
  assert_contains "$TMP/check_$base.err" "$FIX/$file:" "$base source path in diagnostic"
  assert_contains "$TMP/check_$base.err" ": error:" "$base source diagnostic shape"
  assert_contains "$TMP/check_$base.err" "${invalid_expect[$file]}" "$base diagnostic message"
  run_fail "emit_$base" "$DS" emit bash "$FIX/$file" -o "$TMP/$base.sh"
  assert_file_missing_or_empty "$TMP/$base.sh" "$base leaves no Bash artifact"
done

# CLI integration and older behavior compatibility.
run_ok help_top "$DS" --help
for text in 'ds <file.ds> [args...]' 'ds run <file.ds> [args...]' 'ds tokens <file.ds>' 'ds ast <file.ds>' 'ds check <file.ds>' 'ds bytecode <file.ds>' 'ds emit bash <file.ds> -o <file.sh>'; do
  assert_contains "$TMP/help_top.out" "$text" "top help lists $text"
done
run_ok old_tokens "$DS" tokens examples/basic.ds
run_ok old_ast "$DS" ast examples/basic.ds
run_ok old_check "$DS" check examples/basic.ds
run_ok old_bytecode "$DS" bytecode examples/basic.ds
run_ok old_run "$DS" run examples/basic.ds
run_ok old_direct "$DS" examples/basic.ds
run_ok old_emit "$DS" emit bash examples/basic.ds -o "$TMP/old_basic.sh"
run_ok old_emit_syntax bash -n "$TMP/old_basic.sh"
run_ok old_bash bash "$TMP/old_basic.sh"
assert_same "$TMP/old_run.out" "$TMP/old_direct.out" "old direct/run stdout parity"
assert_same "$TMP/old_run.out" "$TMP/old_bash.out" "old VM/Bash stdout parity"
run_fail run_no_file "$DS" run
assert_contains "$TMP/run_no_file.err" 'expected `ds run [--trace-cmd] [--trace-vm] <file.ds> [args...]`' "run no file usage"
run_fail bytecode_no_file "$DS" bytecode
assert_contains "$TMP/bytecode_no_file.err" 'expected a command and <file.ds>' "bytecode no file usage"
run_fail emit_no_output "$DS" emit bash "$FIX/args_basic.ds"
assert_contains "$TMP/emit_no_output.err" 'expected `ds emit bash <file.ds> -o <file.sh>`' "emit missing -o usage"
run_fail unknown_command "$DS" frob "$FIX/args_basic.ds"
assert_contains "$TMP/unknown_command.err" 'unknown command `frob`' "unknown command diagnostic"
run_fail missing_direct_path "$DS" "$TMP/nope.ds" api
assert_contains "$TMP/missing_direct_path.err" 'failed to open source file' "direct missing path with args stays source error"
run_fail no_contract_extra "$DS" run examples/basic.ds extra
assert_contains "$TMP/no_contract_extra.err" 'unexpected script arguments' "no-contract extra args rejected"

# Docs/status checks relevant to v0.5.
assert_contains "$ROOT/README.md" "v0.5.0" "README mentions v0.5.0"
assert_contains "$ROOT/docs/language.ds" "script" "language doc mentions script"
assert_contains "$ROOT/docs/milestones/v0.5.0-test-plan.md" "First-Class CLI Args" "v0.5 test plan title present"
assert_not_contains "$ROOT/README.md" "arrays are implemented" "README does not claim future arrays implemented"

echo "v0.5.0 tests passed: $pass_count checks"
