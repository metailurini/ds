#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DS="$ROOT/ds"
FIX="$ROOT/tests/v0_1/fixtures"
GOLD="$ROOT/tests/v0_1/golden"
TMP="${TMPDIR:-/tmp}/ds_v0_1_tests.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

pass_count=0

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

pass() {
  pass_count=$((pass_count + 1))
  echo "ok $pass_count - $*"
}

run_ok() {
  local name="$1"; shift
  "$@" >"$TMP/$name.out" 2>"$TMP/$name.err" || {
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected success"
  }
  pass "$name"
}

run_fail() {
  local name="$1"; shift
  if "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"; then
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected failure"
  fi
  pass "$name"
}

assert_contains() {
  local file="$1"
  local text="$2"
  local name="$3"
  grep -F -- "$text" "$file" >/dev/null || {
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected to contain [$text]"
  }
  pass "$name"
}

assert_not_contains() {
  local file="$1"
  local text="$2"
  local name="$3"
  if grep -F -- "$text" "$file" >/dev/null; then
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected not to contain [$text]"
  fi
  pass "$name"
}

assert_same() {
  local expected="$1"
  local actual="$2"
  local name="$3"
  if ! diff -u "$expected" "$actual"; then
    fail "$name: output mismatch"
  fi
  pass "$name"
}

write_fixture() {
  local path="$1"
  shift
  cat >"$path" <<EOF_INNER
$*
EOF_INNER
}

# Build first so all tests exercise the local executable.
if [[ "${DS_SKIP_BUILD:-0}" != 1 ]]; then
  make -C "$ROOT" clean all >/dev/null
fi

# Lexer golden tests.
run_ok tokens_basic "$DS" tokens "$FIX/tokens_basic.ds"
assert_same "$GOLD/tokens_basic.tokens" "$TMP/tokens_basic.out" "lexer golden: basic tokens/comments/strings/locations"
assert_not_contains "$TMP/tokens_basic.out" "leading comment" "lexer ignores full-line comment text"
assert_not_contains "$TMP/tokens_basic.out" "trailing comment" "lexer ignores trailing comment text"
assert_contains "$TMP/tokens_basic.out" 'STRING         "\"Hello # not comment\""' "lexer keeps # inside strings"
assert_contains "$TMP/tokens_basic.out" 'DOLLAR_IDENT   "$name"' "lexer tokenizes command variable word"

# Lexer token coverage.
cat >"$TMP/operators.ds" <<'EOF_OPS'
let x = (1 + 2) * 3 / 4 - 5
if x != 0 {
  echo ok
}
if x > 1 {
  echo gt
}
if x >= 1 {
  echo ge
}
if x < 10 {
  echo lt
}
if x <= 10 {
  echo le
}
EOF_OPS
run_ok tokens_operators "$DS" tokens "$TMP/operators.ds"
for token in LET IDENT EQUAL LPAREN INT PLUS RPAREN STAR SLASH MINUS IF BANG_EQUAL GREATER GREATER_EQUAL LESS LESS_EQUAL LBRACE RBRACE EOF; do
  assert_contains "$TMP/tokens_operators.out" "$token" "lexer tokenizes $token"
done

cat >"$TMP/identifiers.ds" <<'EOF_IDS'
let lower = 1
let UPPER = 2
let with_under_score = 3
let name123 = 4
let split = 1abc
EOF_IDS
run_ok tokens_identifiers "$DS" tokens "$TMP/identifiers.ds"
assert_contains "$TMP/tokens_identifiers.out" 'IDENT          "lower"' "lexer lowercase identifier"
assert_contains "$TMP/tokens_identifiers.out" 'IDENT          "UPPER"' "lexer uppercase identifier"
assert_contains "$TMP/tokens_identifiers.out" 'IDENT          "with_under_score"' "lexer underscore identifier"
assert_contains "$TMP/tokens_identifiers.out" 'IDENT          "name123"' "lexer identifier with digits after first char"
assert_contains "$TMP/tokens_identifiers.out" 'INT            "1"' "lexer splits invalid leading-digit identifier as int"
assert_contains "$TMP/tokens_identifiers.out" 'IDENT          "abc"' "lexer splits invalid leading-digit identifier tail"

cat >"$TMP/strings.ds" <<'EOF_STR'
let empty = ""
let spaced = "hello world"
let quoted = "hello \"world\""
let unicode = "xin chào"
EOF_STR
run_ok tokens_strings "$DS" tokens "$TMP/strings.ds"
assert_contains "$TMP/tokens_strings.out" 'STRING         "\"\""' "lexer empty string"
assert_contains "$TMP/tokens_strings.out" 'STRING         "\"hello world\""' "lexer string with spaces"
assert_contains "$TMP/tokens_strings.out" 'STRING         "\"hello \\\"world\\\"\""' "lexer escaped quote string"
assert_contains "$TMP/tokens_strings.out" 'STRING         "\"xin chào\""' "lexer unicode inside string"

printf 'let x = "unterminated' >"$TMP/unterminated.ds"
run_fail lexer_unterminated "$DS" tokens "$TMP/unterminated.ds"
assert_contains "$TMP/lexer_unterminated.err" "$TMP/unterminated.ds:1:9: error: unterminated string literal" "lexer unterminated string diagnostic location"

cat >"$TMP/invalid_escape.ds" <<'EOF_BAD_ESCAPE'
let bad = "hello \q"
EOF_BAD_ESCAPE
run_fail lexer_invalid_escape "$DS" tokens "$TMP/invalid_escape.ds"
assert_contains "$TMP/lexer_invalid_escape.err" 'invalid escape sequence `\q`' "lexer invalid escape diagnostic"

cat >"$TMP/trailing_escape.ds" <<'EOF_BAD_TRAIL'
let bad = "hello \
EOF_BAD_TRAIL
run_fail lexer_trailing_escape "$DS" tokens "$TMP/trailing_escape.ds"
assert_contains "$TMP/lexer_trailing_escape.err" "invalid trailing escape in string literal" "lexer trailing escape diagnostic"

# Parser and AST golden tests.
run_ok ast_mixed "$DS" ast "$FIX/ast_mixed.ds"
assert_same "$GOLD/ast_mixed.ast" "$TMP/ast_mixed.out" "AST golden: mixed statements"
run_ok ast_empty "$DS" ast "$FIX/empty.ds"
assert_same "$GOLD/empty.ast" "$TMP/ast_empty.out" "AST golden: empty file"
run_ok ast_comments_only "$DS" ast "$FIX/comments_only.ds"
assert_same "$GOLD/comments_only.ast" "$TMP/ast_comments_only.out" "AST golden: comments-only file"

cat >"$TMP/if_without_else.ds" <<'EOF_IF'
if ok {
  echo "ok"
}
EOF_IF
run_ok ast_if_without_else "$DS" ast "$TMP/if_without_else.ds"
assert_contains "$TMP/ast_if_without_else.out" "IfStmt" "parser if without else"
assert_contains "$TMP/ast_if_without_else.out" "Then" "parser if then branch"
assert_not_contains "$TMP/ast_if_without_else.out" "Else" "parser if without else has no else branch"

cat >"$TMP/expressions.ds" <<'EOF_EXPR'
let a = name
let b = "text"
let c = 123
let d = true
let e = !false
let f = name == "Danh"
let g = name != "Other"
let h = count > 1
let i = count >= 1
let j = count < 10
let k = count <= 10
let l = (count + 1) * 2
EOF_EXPR
run_ok ast_expressions "$DS" ast "$TMP/expressions.ds"
for fragment in "IdentExpr name" "StringExpr \"text\"" "IntExpr 123" "BoolExpr true" "UnaryExpr !" "BinaryExpr ==" "BinaryExpr !=" "BinaryExpr >" "BinaryExpr >=" "BinaryExpr <" "BinaryExpr <=" "BinaryExpr *" "BinaryExpr +"; do
  assert_contains "$TMP/ast_expressions.out" "$fragment" "parser expression $fragment"
done

cat >"$TMP/commands.ds" <<'EOF_CMD'
git status
docker compose up
echo "hello world"
let branch = "main"
git checkout $branch
if branch == "main" {
  ./script --flag=value path/to/file
}
EOF_CMD
run_ok ast_commands "$DS" ast "$TMP/commands.ds"
assert_contains "$TMP/ast_commands.out" "Word git" "parser command word order first word"
assert_contains "$TMP/ast_commands.out" "Word status" "parser command word order second word"
assert_contains "$TMP/ast_commands.out" "Word docker" "parser multi-word command first"
assert_contains "$TMP/ast_commands.out" "Word compose" "parser multi-word command second"
assert_contains "$TMP/ast_commands.out" "Word up" "parser multi-word command third"
assert_contains "$TMP/ast_commands.out" 'Word "hello world"' "parser quoted command arg single word"
assert_contains "$TMP/ast_commands.out" 'Word $branch' "parser dollar command arg"
assert_contains "$TMP/ast_commands.out" 'Word ./script' "parser adjacent punctuation command word"
assert_contains "$TMP/ast_commands.out" 'Word --flag=value' "parser flag assignment command word"
assert_contains "$TMP/ast_commands.out" 'Word path/to/file' "parser path command word"

run_ok ast_no_trailing_newline "$DS" ast "$FIX/no_trailing_newline.ds"
assert_contains "$TMP/ast_no_trailing_newline.out" 'Word "no newline"' "parser file without trailing newline"

# Diagnostics tests.
cat >"$TMP/missing_expr.ds" <<'EOF_BAD'
let name =
EOF_BAD
run_fail diag_missing_expr "$DS" check "$TMP/missing_expr.ds"
assert_contains "$TMP/diag_missing_expr.err" "$TMP/missing_expr.ds:1:11: error: expected expression after" "diagnostic missing expression includes file line column"
assert_contains "$TMP/diag_missing_expr.err" '^' "diagnostic missing expression includes caret"

cat >"$TMP/missing_brace.ds" <<'EOF_BAD'
if ok {
  echo bad
EOF_BAD
run_fail diag_missing_brace "$DS" check "$TMP/missing_brace.ds"
assert_contains "$TMP/diag_missing_brace.err" "expected \`}\` to close block" "diagnostic missing closing brace"

cat >"$TMP/invalid_let.ds" <<'EOF_BAD'
let = "Danh"
EOF_BAD
run_fail diag_invalid_let "$DS" check "$TMP/invalid_let.ds"
assert_contains "$TMP/diag_invalid_let.err" "expected identifier after" "diagnostic invalid let"

cat >"$TMP/missing_equal.ds" <<'EOF_BAD'
let name "Danh"
EOF_BAD
run_fail diag_missing_equal "$DS" check "$TMP/missing_equal.ds"
assert_contains "$TMP/diag_missing_equal.err" "expected \`=\` after variable name" "diagnostic missing equal"

cat >"$TMP/missing_if_condition.ds" <<'EOF_BAD'
if {
  echo bad
}
EOF_BAD
run_fail diag_missing_if_condition "$DS" check "$TMP/missing_if_condition.ds"
assert_contains "$TMP/diag_missing_if_condition.err" "expected condition after" "diagnostic missing if condition"

cat >"$TMP/missing_if_brace.ds" <<'EOF_BAD'
if ok
  echo bad
}
EOF_BAD
run_fail diag_missing_if_brace "$DS" check "$TMP/missing_if_brace.ds"
assert_contains "$TMP/diag_missing_if_brace.err" "expected \`{\` after if condition" "diagnostic missing if opening brace"

cat >"$TMP/incomplete_binary.ds" <<'EOF_BAD'
let name = other ==
EOF_BAD
run_fail diag_incomplete_binary "$DS" check "$TMP/incomplete_binary.ds"
assert_contains "$TMP/diag_incomplete_binary.err" "expected expression" "diagnostic incomplete binary expression"

cat >"$TMP/unexpected_else.ds" <<'EOF_BAD'
else {
}
EOF_BAD
run_fail diag_unexpected_else "$DS" check "$TMP/unexpected_else.ds"
assert_contains "$TMP/diag_unexpected_else.err" "unexpected \`else\` without matching \`if\`" "diagnostic unexpected else"

cat >"$TMP/unexpected_rbrace.ds" <<'EOF_BAD'
}
EOF_BAD
run_fail diag_unexpected_rbrace "$DS" check "$TMP/unexpected_rbrace.ds"
assert_contains "$TMP/diag_unexpected_rbrace.err" "unexpected \`}\`" "diagnostic unexpected closing brace"

# Multiple invalid files should each fail cleanly without global-state corruption.
run_fail diag_repeat_1 "$DS" check "$TMP/missing_expr.ds"
run_fail diag_repeat_2 "$DS" check "$TMP/invalid_let.ds"
run_fail diag_repeat_3 "$DS" check "$TMP/unexpected_rbrace.ds"

# CLI behavior.
run_ok cli_check_valid "$DS" check "$FIX/ast_mixed.ds"
if [ -s "$TMP/cli_check_valid.out" ]; then
  fail "cli_check_valid: expected check stdout to be empty"
fi
pass "cli check valid prints no stdout"
run_fail cli_check_invalid "$DS" check "$TMP/missing_expr.ds"
run_fail cli_tokens_missing "$DS" tokens "$TMP/missing-file.ds"
assert_contains "$TMP/cli_tokens_missing.err" "$TMP/missing-file.ds:1:1: error: failed to open source file" "cli missing file diagnostic includes requested path"
run_fail cli_ast_invalid "$DS" ast "$TMP/missing_expr.ds"
run_fail cli_unknown_command "$DS" nope "$FIX/ast_mixed.ds"
assert_contains "$TMP/cli_unknown_command.err" "unknown command" "cli unknown command diagnostic"
run_ok cli_help "$DS" --help
assert_contains "$TMP/cli_help.out" "ds v0.5.0" "cli help output"

# Edge cases.
cat >"$TMP/many_blank_lines.ds" <<'EOF_BLANK'


let x = 1


EOF_BLANK
run_ok edge_many_blank_lines "$DS" check "$TMP/many_blank_lines.ds"

printf 'let x = "%05000d"\n' 0 >"$TMP/long_string.ds"
run_ok edge_long_string "$DS" check "$TMP/long_string.ds"

python3 - <<'PY' >"$TMP/long_line.ds"
print('echo ' + 'arg ' * 300)
PY
run_ok edge_long_line "$DS" check "$TMP/long_line.ds"

cat >"$TMP/tabs_spaces.ds" <<'EOF_WS'
	let tabbed = true
  let spaced = false
EOF_WS
run_ok edge_tabs_spaces "$DS" check "$TMP/tabs_spaces.ds"

cat >"$TMP/nested_blocks.ds" <<'EOF_NEST'
if a {
  if b {
    if c {
      echo deep
    }
  }
}
echo after
EOF_NEST
run_ok edge_nested_blocks "$DS" ast "$TMP/nested_blocks.ds"
assert_contains "$TMP/edge_nested_blocks.out" "Word after" "parser command after closing brace"

# Syntax catalog checks.
[ -f "$ROOT/docs/language.ds" ] || fail "docs/language.ds should exist"
pass "syntax catalog exists"
head -20 "$ROOT/docs/language.ds" | grep -F "syntax catalog" >/dev/null || fail "syntax catalog should identify itself near the top"
pass "syntax catalog explains purpose"
head -40 "$ROOT/docs/language.ds" | grep -F "not a runnable script" >/dev/null || fail "syntax catalog should say it is not runnable"
pass "syntax catalog says not runnable"
for marker in "stable" "candidate" "deferred" "rejected"; do
  grep -F "$marker" "$ROOT/docs/language.ds" >/dev/null || fail "syntax catalog missing marker $marker"
  pass "syntax catalog documents marker $marker"
done
for syntax in "let name" "if" "else" "git status" "true" "false"; do
  grep -F "$syntax" "$ROOT/docs/language.ds" >/dev/null || fail "syntax catalog missing v0.1.0 subset $syntax"
  pass "syntax catalog includes $syntax"
done

# The full syntax catalog intentionally contains future syntax and is not a v0.1.0 parser acceptance fixture.
run_fail syntax_catalog_not_v0_1_fixture "$DS" check "$ROOT/docs/language.ds"

make -C "$ROOT" check >/dev/null
pass "make check passes"

echo "v0.1.0 tests passed: $pass_count checks"
