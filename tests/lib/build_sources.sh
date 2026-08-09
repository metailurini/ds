#!/usr/bin/env bash
# Shared C unit-test build helpers.
#
# These helpers keep direct unit-test source lists out of individual versioned
# harnesses. The Makefile remains the canonical source list for the project;
# tests choose a small named source group only when they intentionally link a
# focused internal unit executable instead of the full ds binary.

ds_project_source_rels() {
  local root="$1"
  awk '
    /^SRC[[:space:]]*:=/ {
      sub(/^SRC[[:space:]]*:=/, "")
      print
      next
    }
  ' "$root/Makefile"
}

ds_project_library_sources() {
  local root="$1" rel
  for rel in $(ds_project_source_rels "$root"); do
    case "$rel" in
      src/main.c) continue ;;
    esac
    printf '%s/%s\n' "$root" "$rel"
  done
}

ds_runtime_unit_sources() {
  local root="$1"
  printf '%s\n' \
    "$root/src/runtime.c" \
    "$root/src/source.c" \
    "$root/src/diag.c" \
    "$root/src/runtime/hashmap.c"
}

ds_command_model_unit_sources() {
  local root="$1"
  printf '%s\n' \
    "$root/src/ds_command.c" \
    "$root/src/ds_command_facts.c" \
    "$root/src/source.c" \
    "$root/src/diag.c"
}

ds_command_result_unit_sources() {
  local root="$1"
  printf '%s\n' \
    "$root/src/ds_command_facts.c" \
    "$root/src/source.c" \
    "$root/src/diag.c"
}

ds_source_group() {
  local root="$1" group="$2"
  case "$group" in
    runtime) ds_runtime_unit_sources "$root" ;;
    library) ds_project_library_sources "$root" ;;
    command_model) ds_command_model_unit_sources "$root" ;;
    command_result) ds_command_result_unit_sources "$root" ;;
    *)
      echo "unknown ds unit source group: $group" >&2
      return 1
      ;;
  esac
}

ds_compile_unit() {
  local root="$1" group="$2" unit="$3" output="$4"
  local -a sources feature_flags
  mapfile -t sources < <(ds_source_group "$root" "$group")
  read -r -a feature_flags <<< "$(sed -n 's/^DS_FEATURE_CPPFLAGS[[:space:]]*:=[[:space:]]*//p' "$root/config/feature_flags.mk")"
  cc -std=c99 -Wall -Wextra -Wpedantic "${feature_flags[@]}" ${DS_UNIT_EXTRA_CFLAGS:-} \
    -I"$root/include" -I"$root/src" \
    "$unit" "${sources[@]}" \
    -o "$output"
}
