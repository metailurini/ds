# Refactoring Opportunities

## Current status — 2026-08-09

This file tracks **current** technical-debt opportunities. Git history already preserves
the older audit, line-number tables, intermediate recommendations, and completed
refactors, so they are intentionally not repeated here.

Keep changes only when they make ownership clearer, remove code, reduce drift, or make a
real failure mode safer. Do not add a generic abstraction merely to reduce a duplication
count.

## Still actionable

### AST/HIR traversal and ownership

Expression and statement switches still mirror one another structurally in the AST and
lowered HIR. Their payloads and ownership rules differ enough that a generic visitor has
not paid for itself. Prefer small shared operations when the behavior is genuinely
identical.

### Typed list/range rendering (FR10)

Continue extracting concrete typed helpers only when they delete repeated rendering
logic. Avoid a callback/macro join framework for short loops that are clearer inline.

### VM process execution (VMR13)

Direct commands and pipelines still have different fork topology. Capture, exec-error,
wait, signal, and cleanup behavior is substantially centralized already. Further work
should reduce code without hiding process-group or pipeline semantics.

### Runtime value operations

Copy, free, string conversion, and truthiness still switch over the same value-kind
enum. Share metadata or helpers only where the behavior and ownership rules are truly
the same.

### `ds_common.h` ownership boundary

`ds_common.h` is now a declaration/type boundary with executable common helpers in
`ds_common.c`. Keep it that way. The type-generic `DS_VEC_PUSH` macro is a deliberate
C-level exception; non-trivial helpers should live in an
existing implementation module rather than returning to the header or creating a new
single-purpose utility module.

### Vector wrapper discipline

The shared growth primitives are useful only if callers do not rebuild layers of
one-line aliases around them. Keep a domain helper when it adds ownership, validation,
state transition, representation hiding, or meaningful multi-step construction. Inline
helpers that merely rename `DS_VEC_PUSH`/`ds_grow_array()`, and avoid boolean return values
for operations whose only failure mode is the project's fatal allocation path.

### Hashmap wrapper inlining (RR9)

Still technically possible, but low priority. The expected deletion is small and moving
implementation into public headers changes API/ABI characteristics.

### String-method diagnostic name rendering

`ds_stdlib_string_method_names()` still builds a lazy fixed-size global string. This is
low risk today. Simplify it only if the replacement can consume existing stdlib metadata
without introducing a more complicated iterator/callback API.

## Completed or superseded

- Shared vector growth (`DS_VEC_PUSH`) and parser/lower/Bash pass-through push-wrapper
  removal.
- Checked shared vector/buffer growth, including overflow-safe capacity and allocation
  size calculations.
- Header-resident specialized cleanup macros were removed in favor of domain-owned
  cleanup functions in implementation files.
- Canonical signal, redirect, assignment-operator, case-pattern, string
  clone/equality, indentation, escaping, path, integer parsing, and source/string
  helper APIs.
- Parser collection framing/recovery, assignment-operator parsing, and statement
  assignment scanning.
- Bash dependency traversal consolidation, structured payload reuse, command facts, and
  helper metadata centralization.
- Runtime map iteration/init error handling and VM capture/error/wait cleanup work.
- `DsString` owns formatted append; Bash `EmitBuf` aliases the same representation.
- VM and emitted Bash script help share the same behavior and are covered by parity
  testing rather than implementation-name assertions.
- String-method diagnostics derive their method set from stdlib metadata rather than a
  parallel hard-coded list.
- POSIX/XOPEN compiler feature flags have one build/test definition with an explicit
  consistency check for `compile_flags.txt`.
- Materialized `run` lowering keeps temporary-scope ownership inside the lowering path
  that begins it; callers no longer rely on a helper returning with `Lower.scope`
  implicitly changed.
- Formatter in-place writes use exclusive temporary-file creation before atomic rename.
- Stdlib namespace names map explicitly to enum values rather than relying on enum/table
  ordering arithmetic.
- Several v0.8/v0.19 tests now assert observable behavior instead of requiring private
  helper names to remain in the source.

## Intentionally rejected / defer unless the code changes

- **One generic AST/HIR visitor:** current payload and ownership differences make the
  abstraction larger and less explicit than the duplicated switch skeletons.
- **Generic list-render callback/macro framework:** typed helpers have been clearer and
  smaller where reuse was worthwhile.
- **Broad Bash stdlib-iteration extraction (BR12):** the straightforward extraction was
  tested previously and increased code size/indirection.
- **Shared direct/pipeline process-I/O state object:** the straightforward VMR13
  extraction was tested previously and made `vm_process.c` larger without reducing its
  fork/wait complexity.

When revisiting any item, inspect the current tree first. Do not rely on historical line
numbers or duplication counts from older audits.