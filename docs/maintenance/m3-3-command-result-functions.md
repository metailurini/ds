# M3.3 — Command-result function metadata cleanup

## Problem
Command-result functions combine captured commands, user-function returns,
stdlib/helper metadata, field access, interpolation, VM runtime values, Bash
sidecar variables, and diagnostics. Without one contract, future work can teach a
helper, return path, field read, VM runtime, or Bash emitter a different version
of command-result behavior.

## Decision
Command-result function behavior is metadata-driven and lowerer-owned.

- Parser/AST preserve `run`, field, call, interpolation, and return syntax only.
- Stdlib declares helper facts through `DsStdlibHelper.return_kind`.
- Lowering maps helper/user-function return facts into `DS_LOWER_VALUE_COMMAND_RESULT`.
- `DS_LOWER_EXPR_CALL.return_kind`, `DS_LOWER_EXPR_RUN`, and validated
  `DS_LOWER_EXPR_FIELD` are the backend-facing contract.
- `DsCommandResultField` is the single field-name/type catalog.
- VM `DS_VALUE_COMMAND_RESULT` and Bash `__ds_` sidecars are execution/ABI
  representations, not source-language acceptance rules.

## Ownership
- Syntax owner: parser (`src/parse_expr.c`, `src/parse_stmt.c`,
  `src/parse_command.c`).
- Semantic owner: lowerer (`src/lower_functions.c`, `src/lower_expr.c`,
  `src/lower_stmt.c`, `src/lower_symbols.c`).
- Canonical representation: `DS_LOWER_VALUE_COMMAND_RESULT`, `DS_LOWER_EXPR_RUN`,
  `DS_LOWER_EXPR_CALL.return_kind`, validated `DS_LOWER_EXPR_FIELD`, and
  `DsCommandResultField` descriptors.
- VM owner: command capture, process execution, runtime command-result values,
  field reads, and accepted user/stdlib call execution.
- Bash owner: command capture helpers, field rendering, user-function return ABI,
  helper dependency selection, and emitted-script invariant checks.
- Diagnostics owner: lowerer for field/call/return acceptance; parser for syntax;
  VM/Bash for runtime, process, artifact, or invariant failures.
- Test owner: command-result parity and diagnostic suites across captured values,
  user-function returns, stdlib metadata, and Bash helper emission.

## Accepted behavior
- `run ...` captured command expressions.
- Named command-result bindings and reads of `.stdout`, `.stderr`, `.status`,
  `.code`, `.ok`, and `.failed`.
- User functions returning command results from direct `run`, named captured
  values, or forwarded command-result calls when lowering proves portability.
- User or stdlib calls whose `return_kind` is lowered to command-result metadata.
- Interpolation/field reads on validated named command-result values.
- VM/Bash parity for stdout, stderr, status/code, ok/failed, and final observable
  behavior of accepted command-result functions.

## Rejected behavior
- Unknown command-result field: lowerer diagnostic.
- Field access on a non-command-result/non-map receiver: lowerer diagnostic.
- Temporary command-result field access such as `probe().stdout` while the named
  binding parity rule remains: lowerer diagnostic.
- Command-result function call in an invalid expression or statement position:
  lowerer diagnostic.
- Nonportable command-result return shape: lowerer diagnostic.
- Backend source-language rejections for command-result forms are ownership
  pressure unless they are documented as runtime/emission/internal invariants.

## Files touched / relevant files
- `src/ds_command.h`
- `src/ds_hir.h`
- `src/ds_stdlib.h`
- `src/stdlib.c`
- `src/lower_functions.c`
- `src/lower_expr.c`
- `src/lower_stmt.c`
- `src/lower_symbols.c`
- `src/vm_compile.c`
- `src/vm.c`
- `src/vm_process.c`
- `src/vm_stdlib.c`
- `src/runtime.c`
- `src/bash_emit.c`
- `src/bash_deps.c`
- `src/bash_expr.c`
- `src/bash_stmt.c`
- `src/bash_command.c`
- `src/bash_helpers.c`

## Tests
- Useful suites: `make test-v0-7`, `make test-v0-10`, `make test-v0-11`,
  `make test-v0-12`, `make test-v0-18`, `make test-v0-21`, `make test-v0-25`,
  `make test-v0-26`, `make test-v0-27`.
- Required coverage areas:
  - named captured command-result fields;
  - command-result returns from `run`, named bindings, and forwarded calls;
  - `.stdout`, `.stderr`, `.status`, `.code`, `.ok`, `.failed` parity;
  - imports involving command-result-returning functions;
  - temporary field access rejection;
  - field access on wrong receiver kind;
  - Bash helper selection and standalone Bash parity.

## Future work
- Keep command-result-producing classification in lowerer helpers.
- Add future command-result-producing stdlib helpers only through shared metadata
  and HIR return-kind propagation.
- Do not broaden direct temporary field access until a portable representation is
  designed.
- Keep `.code` compatibility and `.status` behavior stable unless a future
  language milestone explicitly changes them.
