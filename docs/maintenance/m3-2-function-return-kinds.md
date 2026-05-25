# M3.2 — Function return kind contract

## Problem
Function return kind is a cross-cutting Hell candidate: parser syntax, lowerer
function metadata, expression-position calls, VM call frames, Bash return ABI,
stdlib metadata, diagnostics, and tests can all drift if return behavior is added
in one backend first.

The risk is not missing behavior; it is duplicated ownership. A return kind is
accepted only when it has one backend-neutral contract and both VM and emitted
Bash can consume it with the same observable result.

## Decision
Function return kind is a lowerer/HIR contract.

- AST preserves raw function and `return` syntax only.
- `DsLowerValueKind`, `DsLowerFn`, and `DS_LOWER_STMT_RETURN` are the canonical
  backend-facing representation.
- Lowering owns all source-language return-kind validation and parity gates.
- VM and Bash execute/render accepted HIR and may keep only runtime, emission, or
  internal-invariant diagnostics.
- Stdlib helper return metadata informs helper calls, but does not by itself make
  every user-function return shape portable.

## Ownership
- Syntax owner: parser (`src/parse_function.c`, `src/parse_stmt.c`).
- Semantic owner: lowerer (`src/lower_functions.c`, `src/lower_stmt.c`,
  `src/lower_expr.c`, `src/lower_symbols.c`).
- Canonical representation: `DsLowerValueKind`, `DsLowerFn.return_kind`,
  `DsLowerFn.has_return`, `DsLowerFn.all_paths_return`,
  `DsLowerFn.contains_plain_command`, and `DS_LOWER_STMT_RETURN` in HIR.
- VM owner: VM compile/runtime call and return path (`src/vm_compile.c`,
  `src/vm.c`, `src/vm_scope.c`).
- Bash owner: Bash function emission and private return ABI (`src/bash_stmt.c`,
  `src/bash_expr.c`, `src/bash_helpers.c`, `src/bash_emit.c`).
- Diagnostics owner: lowerer for source-language rejection; parser for malformed
  syntax; VM/Bash for runtime, emission, or internal-invariant failures only.
- Test owner: version suites covering accepted VM/Bash return parity and rejected
  return diagnostics.

## Accepted behavior
- Scalar value returns with matching VM/Bash behavior.
- Flat array, map, and command-result returns where lowering proves the payload is
  portable.
- Forward value calls using lowerer provisional return-contract discovery.
- Expression-position user-function calls only when the function returns a value,
  all paths return, and stdout/plain-command behavior does not collide with value
  capture.
- Statement-position calls for supported no-value or value-returning functions
  according to current language rules.
- Stdlib helper return kinds mapped through shared metadata before HIR reaches
  either backend.
- VM/Bash parity for value kind, scalar value, structured payload, stdout/stderr,
  command-result status fields, and rejected-source diagnostics.

## Rejected behavior
- `return` outside a function: lowerer diagnostic.
- Direct `return` from unsupported handler contexts: lowerer diagnostic.
- Unknown return value kind: lowerer diagnostic.
- Mixed return kinds within one function: lowerer diagnostic.
- Function used as a value when it returns no value: lowerer diagnostic.
- Function used as a value when not all paths return: lowerer diagnostic.
- Plain-command/stdout-producing functions used where value capture would be
  ambiguous: lowerer diagnostic.
- Unsupported or nonportable structured return payloads: lowerer diagnostic.
- Unsupported direct interpolation or command-word use of returned values when the
  return kind is not safely representable: lowerer diagnostic.
- Backend "unsupported return kind" source-language errors are ownership bugs
  unless documented as defensive runtime/emission invariants.

## Files touched / relevant files
- `src/ds_hir.h`
- `src/lower_functions.c`
- `src/lower_stmt.c`
- `src/lower_expr.c`
- `src/lower_symbols.c`
- `src/ds_stdlib.h`
- `src/stdlib.c`
- `src/vm_compile.c`
- `src/vm.c`
- `src/vm_scope.c`
- `src/runtime.c`
- `src/vm_stdlib.c`
- `src/bash_stmt.c`
- `src/bash_expr.c`
- `src/bash_helpers.c`
- `src/bash_emit.c`

## Tests
- `tests/v0_21/run.sh`: initial function return behavior.
- `tests/v0_25/run.sh`: scalar value-return protocol and Bash ABI integrity.
- `tests/v0_26/run.sh`: flat array/map/command-result returns and structured
  return diagnostics.
- `tests/v0_27/run.sh`: later expression/interpolation interactions.
- Required coverage areas:
  - accepted return kind parity in VM and emitted Bash;
  - expression-position misuse diagnostics;
  - mixed, unknown, and nonportable return diagnostics;
  - Bash private return ABI corruption as backend-internal checks only.

## Future work
- Keep return-kind helper logic lowerer-private unless HIR needs explicit new
  metadata.
- When adding a return kind, update lowerer/HIR first, then VM/Bash consumers.
- Keep backend return diagnostics worded as runtime/emission/internal invariant
  failures, not source-language validation.
- Revisit `contains_plain_command` if a later call-mode contract replaces it.
