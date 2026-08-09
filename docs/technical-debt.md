# Technical debt

This is the current source-maintenance backlog for `ds`. It tracks code-shape
problems that still deserve attention. Completed audits and refactor narratives
belong in Git history rather than remaining in this file forever.

Use this document with:

- `docs/source-map.md` for file ownership;
- `docs/concept-map.md` for cross-cutting concepts;
- `docs/parity-contracts.md` for VM/Bash behavior requirements;
- `docs/diagnostics.md` for diagnostic ownership.

## Working rules

1. Prefer clear cohesive functions over creating more files.
2. Keep implementation logic in `.c` files. Headers should primarily declare
   interfaces and representation-level definitions.
3. Do not add a wrapper that only forwards to another wrapper. Reuse the
   canonical helper directly when the abstraction adds no policy or ownership.
4. Keep shared metadata in one canonical owner instead of repeating helper-name
   lists across lowering, VM, and Bash code.
5. Extract code only when the extracted unit has a stable responsibility.
6. Prefer deletion and consolidation over another layer of indirection.

## Standard-library metadata still spans layers

**Status:** In progress.

`src/ds_stdlib.c` and `src/ds_stdlib.h` are the canonical descriptor layer, but
helper facts can still drift into lowering, Bash dependency analysis, Bash
expression emission, VM dispatch, and function-return inference.

When a backend needs another helper fact, extend the descriptor API instead of
adding another local string-comparison list.

## `lower_functions.c` is easy to overgrow

**Status:** Watch.

The file owns signature/default validation, scalar parameter inference,
interpolation and call scanning, return-kind inference, row-schema inference,
and recursion/call-graph checks. Those concerns are related, but new work can
turn the file into a catch-all.

Do not split it by line count. Extract only a cohesive cluster when that cluster
has enough behavior to stand on its own.

## Row-array Bash ABI must remain outside statement dispatch

**Status:** Stable boundary, keep on watch.

Row materialization, row sidecars, sorting, copying, and row-array return
payloads belong in `src/bash_structured.c`. `src/bash_stmt.c` should call those
helpers without taking ownership of the structured-value ABI.

## Generated Bash temp cleanup is not fully centralized

**Status:** In progress.

Generated Bash paths that create temporary files or directories should register
them with the shared cleanup mechanism. New emission code should not introduce a
local `mktemp` lifecycle unless it is routed through the same cleanup contract.

Relevant files include `src/bash_helpers.c`, `src/bash_emit.c`,
`src/bash_command.c`, `src/bash_expr.c`, `src/bash_stmt.c`, and
`src/bash_structured.c`.

## Recursive walk edge behavior needs parity coverage

**Status:** Contract documented, keep tests aligned.

Invalid or unreadable roots fail. Hidden descendants and symlink entries are
skipped. Children that disappear during traversal may be skipped as transient
filesystem races. Bang helpers fail when there are no matches.

The VM and emitted Bash should continue to agree on those rules.

## Milestone test harnesses still repeat mechanics

**Status:** Gradual cleanup only.

Recent suites repeat parity seed setup, standalone Bash checks, helper duplicate
checks, static rejection helpers, and host-dependent filesystem skips.

Move only stable reusable mechanics into `tests/lib/testlib.sh`. Do not replace
clear milestone intent with a highly generic test DSL.

## Documentation maintenance rule

Runtime tests should verify behavior, not English prose in README or `docs/`.
`docs/status.md` remains the authoritative current support matrix and should be
updated with behavior changes, but prose wording is not an executable contract.
