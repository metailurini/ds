#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
TMP="${TMPDIR:-/tmp}/ds_v0_6_tests.$$"
FIX="$ROOT/tests/v0_6/fixtures"
GOLD="$ROOT/tests/v0_6/golden"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# shellcheck source=tests/lib/testlib.sh
source "$ROOT/tests/lib/testlib.sh"

if [[ "${DS_SKIP_BUILD:-0}" != 1 ]]; then
  make -C "$ROOT" clean all >/dev/null
fi

# Lexer, parser, AST debug output. tokens/ast are root-file debug views.
run_ok tokens_import_basic "$DS" tokens "tests/v0_6/fixtures/imports_basic/main.ds"
assert_golden "$GOLD/import_basic.tokens" "$TMP/tokens_import_basic.out" "import token golden"
run_ok ast_import_basic "$DS" ast "tests/v0_6/fixtures/imports_basic/main.ds"
assert_golden "$GOLD/import_basic.ast" "$TMP/ast_import_basic.out" "import AST golden"
assert_contains "$TMP/ast_import_basic.out" 'ImportStmt "./lib.ds"' "AST prints import statement"
assert_not_contains "$TMP/ast_import_basic.out" 'LetStmt app' "AST root debug view does not expand imports"

# Parser invalid import syntax and placement.
declare -A parser_errors=(
  [import_inside_if.ds]='`import` is only allowed at top level'
  [import_after_statement.ds]='`import` must appear before executable statements'
  [import_non_string.ds]='expected string literal import path'
  [import_bare_path.ds]='expected string literal import path'
  [import_extra.ds]='expected end of import statement'
)
for file in "${!parser_errors[@]}"; do
  base="${file%.ds}"
  run_fail "parse_$base" "$DS" check "$FIX/imports_errors/$file"
  assert_contains "$TMP/parse_$base.err" "$FIX/imports_errors/$file:" "$base diagnostic path"
  assert_contains "$TMP/parse_$base.err" ': error:' "$base diagnostic shape"
  assert_contains "$TMP/parse_$base.err" "${parser_errors[$file]}" "$base diagnostic message"
  run_fail "emit_parse_$base" "$DS" emit bash "$FIX/imports_errors/$file" -o "$TMP/$base.sh"
  assert_file_missing_or_empty "$TMP/$base.sh" "$base leaves no Bash artifact"
done

# Basic import composition through all behavior-sensitive commands.
run_ok check_import_basic "$DS" check "$FIX/imports_basic/main.ds"
run_ok bytecode_import_basic "$DS" bytecode "tests/v0_6/fixtures/imports_basic/main.ds"
assert_golden "$GOLD/import_basic.bytecode" "$TMP/bytecode_import_basic.out" "import bytecode golden"
assert_contains "$TMP/bytecode_import_basic.out" 'imports_basic/lib.ds:1:11' "bytecode maps imported declaration"
assert_contains "$TMP/bytecode_import_basic.out" 'imports_basic/main.ds:3:1' "bytecode maps root command"
run_ok run_import_basic "$DS" run "$FIX/imports_basic/main.ds"
assert_same_text $'Deploying api to production\n3\nenabled\n' "$TMP/run_import_basic.out" "VM basic import stdout"
run_ok direct_import_basic "$DS" "$FIX/imports_basic/main.ds"
assert_same "$TMP/run_import_basic.out" "$TMP/direct_import_basic.out" "direct/run import parity"
run_ok emit_import_basic "$DS" emit bash "tests/v0_6/fixtures/imports_basic/main.ds" -o "$TMP/import_basic.sh"
assert_golden "$GOLD/import_basic.sh" "$TMP/import_basic.sh" "import Bash golden"
run_ok bash_import_basic_syntax bash -n "$TMP/import_basic.sh"
run_ok bash_import_basic env PATH="/usr/bin:/bin" bash "$TMP/import_basic.sh"
assert_same "$TMP/run_import_basic.out" "$TMP/bash_import_basic.out" "VM/Bash basic import parity"
assert_not_contains "$TMP/import_basic.sh" "$DS" "generated Bash does not reference ds binary"
assert_contains "$TMP/import_basic.sh" '# ds: tests/v0_6/fixtures/imports_basic/lib.ds:1' "Bash source comment for import"

# Nested imports, importer-relative resolution, and CWD independence.
run_ok bytecode_import_nested "$DS" bytecode "tests/v0_6/fixtures/imports_nested/main.ds"
assert_golden "$GOLD/import_nested.bytecode" "$TMP/bytecode_import_nested.out" "nested import bytecode golden"
run_ok run_import_nested "$DS" run "$FIX/imports_nested/main.ds"
assert_same_text $'b sees COMMON\na sees B and COMMON\nroot sees A B COMMON\n' "$TMP/run_import_nested.out" "nested VM stdout"
run_ok emit_import_nested "$DS" emit bash "tests/v0_6/fixtures/imports_nested/main.ds" -o "$TMP/import_nested.sh"
assert_golden "$GOLD/import_nested.sh" "$TMP/import_nested.sh" "nested import Bash golden"
run_ok bash_import_nested bash "$TMP/import_nested.sh"
assert_same "$TMP/run_import_nested.out" "$TMP/bash_import_nested.out" "nested VM/Bash parity"
run_ok cwd_independent bash -c "cd '$TMP' && '$DS' run '$FIX/imports_nested/main.ds'"
assert_same "$TMP/run_import_nested.out" "$TMP/cwd_independent.out" "imports resolve from importer not cwd"

# Duplicate load-once behavior.
run_ok run_duplicate_import "$DS" run "$FIX/imports_basic/duplicate_main.ds"
assert_same_text $'side once\nroot done\n' "$TMP/run_duplicate_import.out" "duplicate normalized import executes once"
run_ok bytecode_duplicate_import "$DS" bytecode "$FIX/imports_basic/duplicate_main.ds"
if [ "$(grep -c 'side once' "$TMP/bytecode_duplicate_import.out")" != 1 ]; then
  cat "$TMP/bytecode_duplicate_import.out" >&2
  fail "duplicate import bytecode should include side effect once"
fi
pass "duplicate import bytecode load-once"
run_ok emit_duplicate_import "$DS" emit bash "$FIX/imports_basic/duplicate_main.ds" -o "$TMP/duplicate.sh"
if [ "$(grep -c '^echo "side once"' "$TMP/duplicate.sh")" != 1 ]; then
  cat "$TMP/duplicate.sh" >&2
  fail "duplicate import Bash should include side effect once"
fi
pass "duplicate import Bash load-once"
run_ok bash_duplicate_import bash "$TMP/duplicate.sh"
assert_same "$TMP/run_duplicate_import.out" "$TMP/bash_duplicate_import.out" "duplicate import VM/Bash parity"

run_ok run_fanout "$DS" run "$FIX/imports_nested/fanout_main.ds"
assert_same_text $'shared fan once\nfan a fan\nfan b fan\nfanout done\n' "$TMP/run_fanout.out" "fan-out shared import executes once"

# Import error graph handling.
declare -A graph_errors=(
  [missing.ds]='failed to open imported file'
  [nested_missing_main.ds]='failed to open imported file'
  [directory_import_main.ds]='failed to read imported file'
  [cycle_self.ds]='import cycle detected'
  [cycle_a.ds]='import cycle detected'
  [cycle_3_a.ds]='import cycle detected'
  [duplicate_decl_main.ds]='duplicate variable `app`'
  [duplicate_across_imports_main.ds]='duplicate variable `dup`'
  [imported_script_main.ds]='imported files cannot declare `script` blocks in v0.6.0'
  [unknown_in_import_main.ds]='unknown command variable `missing`'
  [future_syntax_main.ds]='nested collections are deferred in v0.10.0'
)
for file in "${!graph_errors[@]}"; do
  base="${file%.ds}"
  run_fail "check_graph_$base" "$DS" check "$FIX/imports_errors/$file"
  assert_contains "$TMP/check_graph_$base.err" ': error:' "$base source diagnostic shape"
  assert_contains "$TMP/check_graph_$base.err" "${graph_errors[$file]}" "$base diagnostic message"
  run_fail "run_graph_$base" "$DS" run "$FIX/imports_errors/$file"
  assert_contains "$TMP/run_graph_$base.err" "${graph_errors[$file]}" "$base run diagnostic message"
  run_fail "bytecode_graph_$base" "$DS" bytecode "$FIX/imports_errors/$file"
  assert_contains "$TMP/bytecode_graph_$base.err" "${graph_errors[$file]}" "$base bytecode diagnostic message"
  run_fail "emit_graph_$base" "$DS" emit bash "$FIX/imports_errors/$file" -o "$TMP/$base.sh"
  assert_file_missing_or_empty "$TMP/$base.sh" "$base invalid graph leaves no Bash artifact"
done
assert_contains "$TMP/check_graph_cycle_a.err" 'cycle_a.ds' "cycle stack includes first file"
assert_contains "$TMP/check_graph_cycle_a.err" 'cycle_b.ds' "cycle stack includes second file"
assert_contains "$TMP/check_graph_nested_missing_main.err" 'nested_missing_lib.ds' "nested missing points at importer file"
assert_contains "$TMP/check_graph_directory_import_main.err" 'directory_import_main.ds:1:1' "directory import points at import site"
assert_contains "$TMP/check_graph_directory_import_main.err" 'imported_dir' "directory import diagnostic names path"
assert_contains "$TMP/check_graph_duplicate_decl_main.err" 'duplicate_decl_main.ds' "root/import duplicate points at root declaration"
assert_contains "$TMP/check_graph_duplicate_across_imports_main.err" 'duplicate_b.ds' "import/import duplicate points at second import declaration"
assert_contains "$TMP/check_graph_unknown_in_import_main.err" 'unknown_in_import_lib.ds' "imported lowering error path preserved"
assert_contains "$TMP/check_graph_future_syntax_main.err" 'future_syntax_lib.ds' "imported parse error path preserved"

# Imported command failure preserves exit status for VM and Bash.
capture_status import_command_exit "$DS" run "$FIX/imports_errors/failing_command_main.ds"
assert_status import_command_exit 7
run_ok emit_failing_import "$DS" emit bash "$FIX/imports_errors/failing_command_main.ds" -o "$TMP/failing_import.sh"
capture_status bash_import_command_exit bash "$TMP/failing_import.sh"
assert_status bash_import_command_exit 7

# Interaction with v0.5 script args. Help must not execute imported top-level commands.
run_ok args_import_default "$DS" run "$FIX/imports_args/main.ds" api
assert_same_text $'lib sees api\nroot api staging done\n' "$TMP/args_import_default.out" "imported code can read root script args"
run_ok args_import_all "$DS" "$FIX/imports_args/main.ds" api --target prod --force
assert_same_text $'lib sees api\nroot api prod done\nforce\n' "$TMP/args_import_all.out" "direct import args stdout"
run_ok args_import_help "$DS" "$FIX/imports_args/main.ds" --help
assert_contains "$TMP/args_import_help.out" 'Usage:' "import args help shown"
assert_not_contains "$TMP/args_import_help.out" 'lib sees' "help does not execute imported commands"
run_ok emit_args_import "$DS" emit bash "$FIX/imports_args/main.ds" -o "$TMP/args_import.sh"
run_ok bash_args_import bash "$TMP/args_import.sh" api --target prod --force
assert_same "$TMP/args_import_all.out" "$TMP/bash_args_import.out" "import args VM/Bash parity"
if [ "$(grep -c '__ds_usage()' "$TMP/args_import.sh")" -ne 1 ]; then
  cat "$TMP/args_import.sh" >&2
  fail "generated Bash should emit one root argument parser"
fi
pass "generated Bash emits one arg parser"

# Edge cases.
run_ok run_import_edges "$DS" run "$FIX/imports_edges/main.ds"
assert_contains "$TMP/run_import_edges.out" 'edges ok ok' "empty/comments/no-newline/CRLF imports work"
assert_contains "$TMP/run_import_edges.out" 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789' "large imported string works"
run_ok run_import_parent "$DS" run "$FIX/imports_edges/subdir/main.ds"
assert_same_text $'parent=parent\n' "$TMP/run_import_parent.out" "parent path import works"
run_ok run_import_space "$DS" run "$FIX/imports_edges/space_main.ds"
assert_same_text $'space ok\n' "$TMP/run_import_space.out" "import path with spaces works"
run_ok run_import_many "$DS" run "$FIX/imports_many/main.ds"
assert_same_text $'many 1 12\n' "$TMP/run_import_many.out" "many imported files work"
run_ok emit_import_edges "$DS" emit bash "$FIX/imports_edges/main.ds" -o "$TMP/edges.sh"
run_ok bash_import_edges bash "$TMP/edges.sh"
assert_same "$TMP/run_import_edges.out" "$TMP/bash_import_edges.out" "edge imports VM/Bash parity"

# Regression for non-import programs and old commands.
run_ok old_tokens "$DS" tokens examples/basic.ds
run_ok old_ast "$DS" ast examples/basic.ds
run_ok old_check "$DS" check examples/basic.ds
run_ok old_bytecode "$DS" bytecode examples/basic.ds
run_ok old_run "$DS" run examples/basic.ds
run_ok old_direct "$DS" examples/basic.ds
run_ok old_emit "$DS" emit bash examples/basic.ds -o "$TMP/old_basic.sh"
run_ok old_bash_syntax bash -n "$TMP/old_basic.sh"
run_ok old_bash bash "$TMP/old_basic.sh"
assert_same "$TMP/old_run.out" "$TMP/old_direct.out" "old direct/run parity"
assert_same "$TMP/old_run.out" "$TMP/old_bash.out" "old VM/Bash parity"
run_ok old_args "$DS" run tests/v0_5/fixtures/args_basic.ds api --target prod --force
assert_contains "$TMP/old_args.out" 'Deploying api to prod' "v0.5 args still work"

# Static architecture boundaries.
if grep -R 'DS_STMT_IMPORT' "$ROOT/src/vm.c" "$ROOT/src/bash_emit.c" >/dev/null; then
  fail "VM/Bash backends should not consume raw import AST nodes"
fi
pass "VM/Bash backends do not consume raw import AST nodes"
assert_contains <(grep -R 'load_composed_file' "$ROOT/src" || true) 'src/main.c' "import composition lives in shared CLI pipeline"
if grep -R 'hashmap_' "$ROOT/src" "$ROOT/include" >/dev/null; then
  fail "staged hashmap API should not leak into production src/include"
fi
pass "staged hashmap API remains outside production API"
assert_not_contains "$TMP/import_basic.sh" ' ds ' "generated Bash does not call ds"

# Documentation/status checks.
assert_contains "$ROOT/README.md" 'import "./lib.ds"' "README documents import syntax"
assert_contains "$ROOT/docs/language.ds" 'import "./lib.ds"' "language catalog documents imports"
assert_contains "$ROOT/docs/roadmap.md" 'v0.6.0' "roadmap mentions v0.6.0"
assert_contains "$ROOT/docs/milestones/v0.6.0-spec.md" 'Imports / Includes' "v0.6 spec title present"
assert_contains "$ROOT/docs/milestones/v0.6.0-test-plan.md" 'make test-v0-6' "v0.6 test plan has target"
assert_contains "$ROOT/docs/runtime.md" 'imported files cannot declare their own script blocks' "runtime docs mention script/import interaction"
[ -f "$ROOT/examples/import-main.ds" ] || fail "README import example exists"
pass "import example exists"

# Suite-level make targets are runnable.
run_ok make_check make -C "$ROOT" check

echo "v0.6.0 tests passed: $pass_count checks"
