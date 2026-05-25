# M3.x — Trap / defer / signal behavior ownership

## Problem
Trap, defer, and signal behavior spans parser keywords, handler AST/HIR nodes,
signal validation, cleanup ordering, VM bytecode/runtime state, foreground
process handling, Bash trap/helper generation, exit-status preservation,
diagnostics, and deterministic parity tests. It becomes a Hell candidate when
handler legality, signal delivery, cleanup order, or final status is decided in a
backend instead of through shared lowering/HIR contracts.

## Decision
Trap/defer/signal behavior is a language-level handler contract with
backend-specific runtime mechanics.

- Parser owns handler syntax shape only.
- Lowering owns supported signal policy, handler legality, capture rules,
  unsupported context rules, and semantic diagnostics.
- HIR handler statements are the canonical backend-neutral representation of
  accepted cleanup behavior.
- VM owns accepted handler execution, cleanup dispatch, and process/signal runtime
  mechanics.
- Bash owns rendering accepted handlers into traps, cleanup stacks, generated
  helper functions, and shell-safe process wrappers.
- Foreground direct-command and simple foreground-pipeline INT/TERM behavior must
  preserve the same observable VM/Bash cleanup/status contract.

## Ownership
- Syntax owner: parser (`src/lexer.c`, `src/parse_stmt.c`, `src/ds_ast.h`).
- Semantic owner: lowerer (`src/lower_stmt.c`, `src/lower_symbols.c`).
- Canonical representation: `DS_LOWER_STMT_DEFER`, `DS_LOWER_STMT_TRAP`, lowered
  handler bodies, and shared `DsHandlerSignal` values in HIR.
- VM owner: handler bytecode registration, runtime cleanup stacks, signal pending
  state, cleanup dispatch, foreground direct-command/pipeline process groups,
  signal forwarding/classification, and final status behavior.
- Bash owner: helper dependency detection, generated handler functions, Bash
  `trap` setup, cleanup stacks/trap slots, recursion guards, direct-command and
  pipeline cleanup wrappers, and emitted-script runtime mechanics.
- Diagnostics owner: parser for malformed handler syntax; lowerer for unsupported
  signals, invalid handler control flow, unsupported captures/context, and source
  acceptance; VM/Bash for OS/runtime/artifact/internal-invariant failures only.
- Test owner: deterministic VM/Bash parity tests for handler lifecycle, cleanup
  order, signals, foreground commands, foreground pipelines, and diagnostics.

## Accepted behavior
- `defer { ... }` as `EXIT` defer.
- `defer on: "EXIT"|"INT"|"TERM" { ... }`.
- `trap "EXIT"|"INT"|"TERM" { ... }`.
- Reaching a trap installs/replaces the one trap for that signal.
- Reaching a defer appends one handler to that signal's cleanup stack.
- Defers run in LIFO order per signal.
- On normal completion, explicit `exit`, explicit `fail`, or direct-command
  failure, `EXIT` cleanup runs.
- On supported signal cleanup, matching trap runs first, then matching defers
  LIFO, then `EXIT` trap and `EXIT` defers LIFO.
- Later traps for the same signal replace earlier traps in both VM and Bash.
- Imported defers register when imported executable code runs.
- Handler failure does not skip older applicable defers.
- Handler command failure or explicit `exit N` can replace final status according
  to the current v0.22 contract.
- `INT` defaults to status `130`; `TERM` defaults to `143` when no handler
  overrides.
- Foreground direct commands and simple foreground pipelines interrupted by
  `INT`/`TERM` are classified as DS signal cleanup events with matching VM/Bash
  observable behavior.

## Rejected behavior
- Unsupported signal names and dynamic signal expressions: lowerer diagnostic.
- Function-local captures from cleanup handlers when VM/Bash parity cannot
  preserve the scope after the function returns: lowerer diagnostic.
- Direct `return` from a cleanup handler: lowerer diagnostic.
- Portable handler context object or context variables: unsupported until HIR
  metadata exists.
- Async/job-control signal APIs beyond the current v0.22 surface: unsupported.
- Backend-specific handler legality or signal support is not allowed.

## Files touched / relevant files
- `src/lexer.c`
- `src/parse_stmt.c`
- `src/ds_ast.h`
- `src/ast.c`
- `src/format.c`
- `src/ds_hir.h`
- `src/hir.c`
- `src/lower_stmt.c`
- `src/lower_symbols.c`
- `src/vm_compile.c`
- `src/vm.c`
- `src/vm_process.c`
- `src/bash_deps.c`
- `src/bash_emit.c`
- `src/bash_stmt.c`
- `src/bash_command.c`
- `tests/v0_22/signal_runtime.sh`

## Tests
- Documentation/source-map checks for handler ownership.
- Parser syntax diagnostics for malformed `defer`, `defer on:`, and `trap`.
- Lowerer diagnostics for unsupported signals, handler captures, direct returns,
  and unsupported handler forms.
- VM behavior for trap replacement, defer LIFO order, EXIT cleanup, handler
  failure/status override, and signal-triggered cleanup.
- Bash parity for the same observable behavior.
- Foreground direct-command `INT`/`TERM` cases.
- Foreground simple-pipeline `INT`/`TERM` cases.
- Nested defer/trap cases that are currently supported.
- Focused deterministic signal runtime target: `make test-v0-22-signal-runtime`.

## Future work
- Add handler context only after defining explicit lowerer/HIR metadata and
  VM/Bash parity tests.
- Keep OS/process-group details backend-owned while preserving shared observable
  cleanup/status behavior.
- Do not add richer signal/job-control APIs during maintenance compaction.
- Keep supported-signal policy in lowering, not parser or backends.
- Continue treating this concept as Watch, not Clear, because OS and Bash runtime
  behavior remain parity-sensitive.
