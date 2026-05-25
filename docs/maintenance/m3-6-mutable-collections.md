# M3.6 — Mutable collection boundary

## Problem
Collections touch parser syntax, HIR value kinds, VM runtime containers, Bash
indexed/associative arrays, function returns, command/interpolation restrictions,
mutation semantics, diagnostics, and tests. Mutation is the Hell candidate: VM can
support shapes that Bash cannot encode unless lowering rejects or normalizes them
first.

## Decision
Collection behavior is accepted only through lowerer/HIR contracts with explicit
VM/Bash parity.

- Parser preserves collection literal, indexing, field, and assignment-target
  syntax shape.
- Lowering owns collection semantic validation, parity gates, and unsupported
  mutation diagnostics.
- There is no HIR mutation node for deferred index assignment, map field
  assignment, or nested mutation.
- VM and Bash execute/render accepted list/map literals, read-only indexing, map
  field reads, array loops, array `push`, and flat structured returns.
- Bash associative-array sidecar metadata is an implementation detail, not the
  language contract.

## Ownership
- Syntax owner: parser (`src/parse_expr.c`, `src/parse_stmt.c`, `src/ds_ast.h`).
- Semantic owner: lowerer (`src/lower_expr.c`, `src/lower_stmt.c`,
  `src/lower_functions.c`, `src/lower_symbols.c`).
- Canonical representation: HIR array/map expressions, HIR index expressions,
  accepted array-loop/push statements, value-kind metadata, and no HIR node for
  deferred mutation forms.
- VM owner: runtime arrays/maps, accepted indexing, missing-key/out-of-range
  runtime failures, array `push`, array loops, and flat collection returns.
- Bash owner: indexed arrays, associative arrays, value-type sidecars, indexing
  helpers, map-key runtime checks, array append, array loops, and emitted-script
  invariants for accepted HIR.
- Diagnostics owner: parser for malformed syntax; lowerer for semantic/parity
  rejection; VM/Bash for accepted runtime data failures, artifact failures, and
  internal invariants only.
- Test owner: VM/Bash parity for accepted collection behavior and diagnostics for
  deferred mutation/access forms.

## Accepted behavior
- List literals with portable element expressions.
- Map literals with non-empty string keys and portable value expressions.
- Duplicate map keys and empty keys rejected before backend selection.
- Named read-only list/map indexing with literal or named-variable indexes.
- Map field reads where currently supported.
- Runtime missing-key and out-of-range errors for accepted indexing forms.
- Array loops over named arrays and supported stdlib array results.
- `array.push(value)` on accepted named arrays.
- Flat array/map function returns after binding to a name, where the return
  contract proves portability.
- VM/Bash parity for list order, map keys/values, read-only index result, array
  loop order, push side effects, and accepted structured returns.

## Rejected behavior
- Empty list/map literals where current language rules reject them: lowerer
  diagnostic for semantic cases.
- Index assignment such as `xs[0] = value`: parser preserves shape; lowerer emits
  deferred index-assignment diagnostic.
- Compound index assignment and nested index assignment such as `matrix[0][1] =
  value`: lowerer diagnostic.
- Map field assignment such as `app.name = "api"`: lowerer diagnostic.
- Arbitrary mutation beyond current `array.push(value)`: lowerer diagnostic.
- Nested arrays/maps as collection elements or values: lowerer diagnostic.
- Computed collection indexes that are not literals or named variables: lowerer
  diagnostic.
- Direct field/index access on temporary collection-returning expressions: lowerer
  diagnostic; bind first.
- Collection values as direct command arguments or unsupported interpolation
  payloads: lowerer diagnostic.
- Map iteration order semantics: lowerer diagnostic; deferred until an explicit
  HIR/parity contract exists.
- Collection destructuring and collection-valued parameters: unsupported.

## Files touched / relevant files
- `src/parse_expr.c`
- `src/parse_stmt.c`
- `src/ds_ast.h`
- `src/ast.c`
- `src/format.c`
- `src/ds_hir.h`
- `src/lower_expr.c`
- `src/lower_stmt.c`
- `src/lower_functions.c`
- `src/lower_symbols.c`
- `src/vm.c`
- `src/vm_compile.c`
- `src/vm_scope.c`
- `src/vm_stdlib.c`
- `src/ds_runtime.h`
- `src/runtime/`
- `src/bash_expr.c`
- `src/bash_stmt.c`
- `src/bash_deps.c`
- `src/bash_helpers.c`
- `src/bash_emit.c`

## Tests
- Supported list/map literals and duplicate/empty-key diagnostics.
- Supported read-only indexing and runtime missing-key/out-of-range failures.
- Rejected computed, temporary, and unsupported indexing forms.
- Supported array loops and rejected map iteration.
- Rejected index assignment, map field assignment, compound assignment, and nested
  mutation.
- Supported `array.push(value)` parity.
- Command-result and function-returned collection edges.
- VM/Bash parity tests for all accepted collection behavior.
- Diagnostic tests proving deferred mutation forms fail before backend selection.

## Future work
- Design map iteration only after defining iteration order and backend parity.
- Design index assignment only after explicit HIR assignment-target and mutation
  semantics exist.
- Design nested mutation only after value/reference semantics and Bash encoding
  are specified.
- Do not add collection-valued parameters or destructuring through maintenance
  cleanup.
- Keep unsupported mutation diagnostics lowerer-owned.
