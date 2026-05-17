#!/usr/bin/env bash
# Shared shell-test helpers for versioned ds regression suites.

pass_count=${pass_count:-0}

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

capture_status() {
  local name="$1"; shift
  set +e
  "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/$name.rc"
}

assert_status() {
  local name="$1"
  local expected="$2"
  local actual
  actual="$(cat "$TMP/$name.rc")"
  [ "$actual" = "$expected" ] || {
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected exit $expected, got $actual"
  }
  pass "$name exit $expected"
}

assert_nonzero_status() {
  local name="$1"
  local actual
  actual="$(cat "$TMP/$name.rc")"
  [ "$actual" != "0" ] || {
    cat "$TMP/$name.out" >&2 || true
    cat "$TMP/$name.err" >&2 || true
    fail "$name: expected non-zero exit"
  }
  pass "$name exit non-zero"
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

assert_same_text() {
  local expected="$1"
  local actual_file="$2"
  local name="$3"
  printf '%s' "$expected" >"$TMP/$name.expected"
  assert_same "$TMP/$name.expected" "$actual_file" "$name"
}

assert_file_missing_or_empty() {
  local path="$1"
  local name="$2"
  if [ -e "$path" ] && [ -s "$path" ]; then
    echo "--- $path" >&2
    cat "$path" >&2 || true
    fail "$name: expected no output artifact or an empty file"
  fi
  pass "$name"
}

assert_golden() {
  local golden="$1"
  local actual="$2"
  local name="$3"
  [ -f "$golden" ] || fail "$name: missing golden file $golden"
  [ -f "$actual" ] || fail "$name: missing actual file $actual"
  if ! diff -u "$golden" "$actual"; then
    fail "$name: golden mismatch"
  fi
  pass "$name"
}

emit_bash() {
  local input="$1"
  local output="$2"
  (cd "$ROOT" && "$DS" emit bash "$input" -o "$output") >"$TMP/emit.out" 2>"$TMP/emit.err" || {
    cat "$TMP/emit.out" >&2 || true
    cat "$TMP/emit.err" >&2 || true
    fail "emit bash failed for $input"
  }
}
