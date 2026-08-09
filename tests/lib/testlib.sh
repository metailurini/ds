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

write_fixture() {
  local target="$1" path
  if [[ "$target" == *.ds ]]; then
    path="$target"
  else
    path="$FIX/$target.ds"
  fi
  mkdir -p "$(dirname "$path")"
  cat >"$path"
  if [[ "$target" != *.ds ]]; then
    printf '%s' "$path"
  fi
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

capture_cmd() {
  local name="$1"
  shift
  set +e
  "$@" >"$TMP/$name.out" 2>"$TMP/$name.err"
  local rc=$?
  set -e
  printf '%s' "$rc" >"$TMP/$name.rc"
}

capture_in_dir() {
  local name="$1" dir="$2"; shift 2
  mkdir -p "$dir"
  set +e
  (cd "$dir" && "$@") >"$TMP/$name.out" 2>"$TMP/$name.err"
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

assert_matches() {
  local file="$1" regex="$2" name="$3"
  grep -E -- "$regex" "$file" >/dev/null || {
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected to match /$regex/"
  }
  pass "$name"
}

assert_not_matches() {
  local file="$1" regex="$2" name="$3"
  if grep -E -- "$regex" "$file" >/dev/null; then
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected not to match /$regex/"
  fi
  pass "$name"
}

assert_line_count() {
  local expected="$1" file="$2" pattern="$3" name="$4"
  local count
  count="$(grep -E -c -- "$pattern" "$file" || true)"
  [ "$count" = "$expected" ] || {
    echo "--- $file" >&2
    cat "$file" >&2 || true
    fail "$name: expected $expected matching lines, got $count"
  }
  pass "$name"
}

assert_diag_span() {
  local file="$1" fixture="$2" message="$3" name="$4"
  assert_contains "$file" "$fixture:" "$name path"
  assert_contains "$file" ': error:' "$name error shape"
  assert_contains "$file" "$message" "$name message"
  assert_contains "$file" '^' "$name caret"
}

assert_diag() {
  local file="$1" fragment="$2" name="$3"
  assert_contains "$file" ': error:' "$name severity"
  assert_contains "$file" "$fragment" "$name text"
  assert_contains "$file" '^' "$name caret"
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
  local expected_file="$TMP/assert_text_${pass_count}_expected"
  printf '%s' "$expected" >"$expected_file"
  assert_same "$expected_file" "$actual_file" "$name"
}

assert_text() {
  assert_same_text "$2" "$3" "$1"
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

assert_check_fails() {
  local name="$1" fixture="$2" fragment="$3"
  run_fail "${name}_check" "$DS" check "$fixture"
  assert_diag "$TMP/${name}_check.err" "$fragment" "$name check diagnostic"
}

assert_emit_fails() {
  local name="$1" fixture="$2" fragment="$3"
  run_fail "${name}_emit" "$DS" emit bash "$fixture" -o "$TMP/${name}.sh"
  assert_diag "$TMP/${name}_emit.err" "$fragment" "$name emit diagnostic"
  assert_file_missing_or_empty "$TMP/${name}.sh" "$name no partial Bash"
}

assert_rejected() {
  local name="$1" file="$2" needle="$3"
  assert_check_fails "$name" "$file" "$needle"
  assert_emit_fails "$name" "$file" "$needle"
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

emit_checked() {
  local name="$1" file="$2" script="$3"
  run_ok "${name}_emit" "$DS" emit bash "$file" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_no_ds_call "$script" "$name emitted Bash standalone"
  assert_no_duplicate_helpers "$script" "$name emitted Bash"
}

assert_vm_bash_parity() {
  local name="$1"
  local fixture="$2"
  local expected_status="$3"
  local output_files="$4"
  shift 4

  local work_vm="$TMP/parity_${name}_vm_work"
  local work_bash="$TMP/parity_${name}_bash_work"
  local script="$TMP/$name.sh"
  mkdir -p "$work_vm" "$work_bash"

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
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "VM/Bash stderr parity: $name"
  else
    assert_contains "$TMP/${name}_vm.err" ': error:' "VM non-zero diagnostic source marker: $name"
    assert_contains "$TMP/${name}_bash.err" ': error:' "Bash non-zero diagnostic source marker: $name"
  fi

  local rel
  for rel in $output_files; do
    [ -f "$work_vm/$rel" ] || fail "VM/Bash $rel parity: $name: VM did not create expected output file"
    [ -f "$work_bash/$rel" ] || fail "VM/Bash $rel parity: $name: Bash did not create expected output file"
    assert_same "$work_vm/$rel" "$work_bash/$rel" "VM/Bash $rel parity: $name"
  done
}

assert_parity() {
  local name="$1" fixture="$2" expected_status="$3" expected_stdout="$4"; shift 4
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"

  run_ok "${name}_check" "$DS" check "$fixture"
  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same_text "$expected_stdout" "$TMP/${name}_vm.out" "$name VM stdout"
  assert_same_text "$expected_stdout" "$TMP/${name}_bash.out" "$name Bash stdout"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  else
    pass "$name non-zero stderr checked by focused assertions when required"
  fi
}

assert_parity_same() {
  local name="$1" fixture="$2" expected_status="$3"; shift 3
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"

  run_ok "${name}_check" "$DS" check "$fixture"
  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same "$TMP/${name}_vm.out" "$TMP/${name}_bash.out" "$name stdout parity"
  if [ "$expected_status" = 0 ]; then
    assert_same "$TMP/${name}_vm.err" "$TMP/${name}_bash.err" "$name stderr parity"
  else
    pass "$name non-zero stderr checked by focused assertions when required"
  fi
}

assert_parity_file() {
  local name="$1" fixture="$2" expected_status="$3" expected_stdout="$4" rel_file="$5" expected_file_text="$6"; shift 6
  local vm_work="$TMP/${name}_vm_work"
  local bash_work="$TMP/${name}_bash_work"
  local script="$TMP/${name}.sh"
  mkdir -p "$vm_work" "$bash_work"

  run_ok "${name}_check" "$DS" check "$fixture"
  capture_in_dir "${name}_vm" "$vm_work" "$DS" run "$fixture" "$@"
  run_ok "${name}_emit" "$DS" emit bash "$fixture" -o "$script"
  run_ok "${name}_bash_n" bash -n "$script"
  assert_not_matches "$script" '(^|[^A-Za-z0-9_./-])ds([[:space:]]|$)' "$name emitted Bash does not call ds"
  capture_in_dir "${name}_bash" "$bash_work" bash "$script" "$@"

  assert_status "${name}_vm" "$expected_status"
  assert_status "${name}_bash" "$expected_status"
  assert_same_text "$expected_stdout" "$TMP/${name}_vm.out" "$name VM stdout"
  assert_same_text "$expected_stdout" "$TMP/${name}_bash.out" "$name Bash stdout"
  assert_same_text "$expected_file_text" "$vm_work/$rel_file" "$name VM side effect"
  assert_same_text "$expected_file_text" "$bash_work/$rel_file" "$name Bash side effect"
  assert_same "$vm_work/$rel_file" "$bash_work/$rel_file" "$name side-effect parity"
}
