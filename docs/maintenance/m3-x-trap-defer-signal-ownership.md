# M3.x — Trap / Defer / Signal Behavior Ownership

## Status

Specification and test-plan pass only. Do not implement behavior changes in this
pass.

This maintenance spec narrows the current Trap/defer/signal Hell candidate into
a bounded ownership problem. The existing v0.22 implementation and tests define
the supported behavior. M3.x does not expand that surface; it documents where the
surface is owned and how later cleanup passes should protect VM/Bash parity.

## Goal

Make trap, defer, signal, foreground-command, and foreground-pipeline cleanup
behavior a language-level contract owned by parser syntax, lowerer validation,
HIR handler representation, VM execution, and Bash emission in predictable
places.

The goal is not to add new signal features. The goal is to stop the concept from
being "kind of everywhere" by naming the current homes, parity contract,
diagnostic owners, and implementation milestones.

## Current risk

Trap/defer/signal behavior is dangerous because it crosses several boundaries at
once:

- parser syntax for `defer`, `defer on:`, and `trap`;
- AST/HIR handler nodes and signal enums;
- lowerer diagnostics for supported signal literals, handler captures, and
  invalid handler control flow;
- VM bytecode registration and cleanup dispatch;
- VM foreground direct-command and pipeline process-group/signal handling;
- Bash trap setup, generated handler functions, cleanup stacks, and helper
  dispatchers;
- exit-status preservation and handler failure override behavior;
- deterministic signal harness tests that need to compare VM and emitted Bash;
- docs that currently span language, runtime, architecture, parity, diagnostics,
  and milestone specs.

The risky failure modes are:

1. Handler legality drifts between parser, lowerer, VM, and Bash.
2. Handler runtime context is accidentally VM-only or Bash-only.
3. Foreground direct commands and foreground pipelines classify `INT`/`TERM`
   differently in VM and emitted Bash.
4. Trap replacement, defer LIFO order, signal cleanup order, and `EXIT` cleanup
   can become unclear or duplicated.
5. Backend fallback diagnostics may look like source-language validation.
6. Tests may prove one backend but not the shared parity contract.

## Current implementation trace

### Parser / AST

Files:

- `src/lexer.c`
- `src/parse_stmt.c`
- `src/ds_ast.h`
- `src/ast.c`
- `src/format.c`

Current behavior:

- `lexer.c` recognizes `defer` and `trap` as keywords.
- `parse_stmt.c` parses:
  - `defer { ... }` as `EXIT` defer;
  - `defer on: "SIGNAL" { ... }`;
  - `trap "SIGNAL" { ... }`.
- The parser currently decodes and validates supported signal names while
  parsing. This is a known ownership pressure point: syntax shape belongs to the
  parser, while supported-signal policy should ultimately be lowerer-owned if a
  future cleanup can move it without destabilizing diagnostics.
- AST stores handler nodes as `DS_STMT_DEFER` / `DS_STMT_TRAP` with
  `DsHandlerSignal` and a body block.
- `ast.c` and `format.c` print/format the syntax shape.

Parser should continue to own missing-token diagnostics such as missing `:`,
missing string literal, missing `{`, and malformed block syntax. Semantic signal
support should not expand in the parser.

### Lowering / semantic diagnostics

Files:

- `src/lower_stmt.c`
- `src/lower_symbols.c`
- lowerer internals referenced through `src/lower_internal.h`

Current behavior:

- `lower_stmt.c` lowers AST handler statements to `DS_LOWER_STMT_DEFER` and
  `DS_LOWER_STMT_TRAP`.
- The lowerer tracks handler depth and handler function depth while lowering the
  handler body.
- `lower_stmt.c` rejects direct `return` from cleanup handlers.
- `lower_symbols.c` rejects cleanup handlers that capture function-local
  variables when VM/Bash parity cannot preserve the scope after the function
  returns.
- General statement/expression validation inside handlers is reused from normal
  lowering.

Desired cleanup pressure:

- Supported-signal policy should be considered a semantic/language validity
  rule. If parser validation is later moved, lowerer should own it.
- Handler legality, capture rules, handler control-flow rules, and unsupported
  handler-context rules belong in lowering.
- Backend code should not decide whether a handler form is valid source.

### HIR

Files:

- `src/ds_hir.h`
- `src/hir.c`

Current behavior:

- HIR represents handlers as `DS_LOWER_STMT_DEFER` and `DS_LOWER_STMT_TRAP`.
- Each handler has a `DsHandlerSignal` and a lowered body.
- HIR does not currently carry richer handler context metadata such as signal
  name strings, source location context values, async/job-control flags, or
  handler capabilities.

Desired ownership:

- HIR is the canonical backend-neutral representation of accepted handler
  behavior.
- VM and Bash should consume HIR handler statements and shared signal enums, not
  parse source-level trap semantics independently.
- If future handler context values are added, they need explicit HIR metadata
  before either backend implements them.

### VM runtime

Files:

- `src/vm_compile.c`
- `src/vm.c`
- VM internals and process helpers used by `vm.c`

Current behavior:

- `vm_compile.c` emits `OP_REGISTER_HANDLER`, skips over handler bodies during
  normal execution, compiles handler bytecode, and emits `OP_END_HANDLER`.
- `vm.c` stores registered handlers at runtime.
- Trap registrations replace an existing trap for the same signal.
- Defer registrations stack and run in last-in, first-out order.
- `vm.c` installs lightweight `INT` and `TERM` signal handlers and records a
  pending signal for cooperative dispatch between bytecode instructions.
- Cleanup dispatch runs the matching trap first, then matching defers LIFO, then
  `EXIT` trap/defers for signal-triggered cleanup.
- Handler failures or explicit `exit` can replace the final process status.

VM runtime ownership:

- VM owns execution of accepted HIR handler registrations and cleanup dispatch.
- VM owns runtime/OS/process failures and internal invariants.
- VM must not become the source-language owner for unsupported handler forms.

### VM process execution

Files:

- `src/vm_process.c`
- call sites in `src/vm.c`

Current behavior:

- VM direct commands and simple foreground pipelines are process-execution
  concerns.
- v0.22 docs/tests expect foreground children/pipelines to be put in a foreground
  process group when possible.
- `INT`/`TERM` observed by the parent is forwarded/classified so ds cleanup runs
  instead of degrading into a generic command or pipeline failure.

VM process ownership:

- VM process code owns OS-level process spawning, waiting, process-group setup,
  signal observation, forwarding, and runtime command/pipeline failures.
- It does not own language support policy for traps, defers, or handler context.

### Bash emitter

Files:

- `src/bash_deps.c`
- `src/bash_emit.c`
- `src/bash_stmt.c`
- `src/bash_command.c`
- Bash helper emission files used by those modules

Current behavior:

- `bash_deps.c` detects whether handler and signal helpers are needed.
- `bash_emit.c` emits cleanup stacks, trap variables, `__ds_run_cleanup`,
  `__ds_run_signal`, and Bash `trap` dispatchers when needed.
- `bash_stmt.c` emits handler bodies as generated functions, assigns trap
  variables, and pushes defer handlers into per-signal stacks.
- Foreground direct commands and pipelines are routed through cleanup-aware Bash
  helpers when signal handlers exist.

Bash ownership:

- Bash emission owns rendering accepted HIR into standalone Bash.
- Bash owns generated helper internals, quoting, Bash `trap` setup, shell-safe
  foreground command/pipeline wrappers, and artifact/runtime failures in the
  generated script.
- Bash must not be the semantic owner for handler legality or supported signal
  policy.

### Bash runtime/helper behavior

Generated Bash owns these runtime details after emission:

- private `__ds_` cleanup stacks and trap slots;
- cleanup recursion guard;
- generated handler functions;
- `EXIT`, `INT`, and `TERM` trap dispatchers;
- final status propagation through helper variables;
- foreground process/pipeline wrappers that integrate with cleanup dispatch.

These helpers are implementation details. They must reflect the HIR contract and
must not become public language semantics.

### Docs

Relevant docs:

- `docs/runtime.md` documents the current v0.22 cleanup and signal runtime.
- `docs/language.ds` documents the surface syntax.
- `docs/parity-contracts.md` defines VM/Bash parity expectations for
  trap/defer/signal behavior.
- `docs/diagnostics.md` defines phase ownership for syntax, semantic,
  unsupported, VM/runtime, and Bash emission diagnostics.
- `docs/milestones/v0.22.0-spec.md` records the original implementation scope.

This M3.x spec should become the maintenance contract for future cleanup work;
older milestone docs remain historical feature specs.

### Tests

Primary tests:

- `tests/v0_22/run.sh` is the main deterministic cleanup/signal test suite.
- Existing pipeline/process suites provide supporting parity coverage for direct
  commands, captured commands, and foreground pipelines.
- Future M3.x implementation passes should add focused tests only for changed
  ownership/diagnostics or newly protected invariants.

Current coverage includes:

- AST/HIR/bytecode shape for defer/trap;
- formatting of handler syntax;
- normal `EXIT` cleanup;
- explicit `exit`/`fail` cleanup;
- direct-command failure cleanup;
- captured-command behavior;
- handler failure and explicit handler `exit` final-status behavior;
- signal-specific normal-exit nonexecution;
- function calls from handlers;
- script args and imports with cleanup;
- deterministic imported `TERM` signal behavior;
- unsupported signal diagnostics;
- dynamic/numeric signal diagnostics;
- return-from-handler diagnostics;
- function-local capture diagnostics;
- emitted Bash helper presence/absence.

## Architectural failure

The v0.22 implementation is functional, but the concept still has too many
implicit homes:

- parser currently owns some supported-signal policy because it decodes signal
  literals directly into `DsHandlerSignal`;
- lowerer owns handler body restrictions and capture parity, but not every
  supported-signal decision;
- VM and Bash each carry nontrivial cleanup runtime state;
- foreground direct-command and pipeline signal handling live in process/runtime
  paths that are easy to change without updating handler docs/tests;
- Bash helper internals are large and can accidentally look canonical;
- tests are deterministic, but future changes need a documented matrix showing
  which ownership boundary they protect.

The desired endpoint is not "one file owns signals." The endpoint is a clear
pipeline:

```txt
source syntax
  -> parser AST handler shape
  -> lowerer semantic validation and accepted HIR handler contract
  -> VM cleanup/signal runtime and process execution
  -> Bash standalone trap/helper emission
  -> shared VM/Bash parity tests
```

## Desired ownership

### Syntax owner

Parser owns:

- recognizing `defer`, optional `on:`, `trap`, string-token positions, and body
  blocks;
- missing-token and malformed syntax diagnostics;
- AST construction for handler statements.

Parser should not grow host-specific signal semantics or handler-context policy.

### Semantic validation owner

Lowerer owns:

- supported signal set as a language acceptance rule, once/if moved out of the
  parser;
- handler placement and body legality;
- direct `return` from handlers;
- function-local capture rejection;
- unsupported handler context values;
- any future restrictions needed for VM/Bash parity.

Unsupported handler forms should fail before VM/Bash backend selection unless
explicitly documented as backend-specific runtime behavior.

### Canonical representation owner

HIR owns:

- accepted handler statement kind (`defer` vs `trap`);
- accepted signal enum;
- lowered handler body;
- future handler-context or cleanup-order metadata if support expands.

VM and Bash consume HIR. They should not reconstruct source-level handler
validity.

### Runtime execution owner

VM owns:

- runtime handler registration;
- trap replacement;
- defer LIFO stacks;
- cleanup dispatch and final status handling;
- pending signal observation;
- foreground direct-command and simple foreground-pipeline signal classification
  and forwarding where supported.

VM diagnostics here are runtime/OS/internal invariant diagnostics, not source
language validity diagnostics.

### Bash emission owner

Bash emitter owns:

- helper dependency selection from accepted HIR;
- generated handler functions;
- private cleanup stacks and trap slots;
- Bash `trap` dispatchers;
- cleanup-aware direct-command and pipeline wrappers;
- artifact/rendering errors for accepted HIR.

Bash diagnostics should not decide whether a source handler form is supported.

### Diagnostics owner

- Parser: malformed syntax only.
- Lowerer: unsupported signals, unsupported handler contexts, handler capture
  legality, direct handler `return`, and parity-gate diagnostics.
- VM: runtime signal/process failures and internal invariant failures.
- Bash emitter/generated Bash: artifact failures and generated-script runtime
  failures for accepted HIR only.
- Tests: assert unsupported forms fail at the earliest documented owner.

### Parity-test owner

The version test suites own VM/Bash parity. For trap/defer/signal behavior, tests
must compare stdout, stderr where meaningful, exit status, and side effects.
Signal tests must use deterministic harnesses rather than racy self-signaling
fixtures.

## VM/Bash parity contract

### Declaring traps

- `trap "EXIT"`, `trap "INT"`, and `trap "TERM"` are the supported trap
  declarations.
- Reaching a trap statement installs/replaces the one trap handler for that
  signal.
- Later traps for the same signal replace earlier traps in both VM and Bash.
- Unsupported signal names and dynamic signal expressions must fail before VM or
  Bash execution/emission, at the documented syntax/semantic owner.

### Registering defers

- `defer { ... }` is equivalent to `defer on: "EXIT" { ... }`.
- Reaching a defer statement appends one handler to that signal's cleanup stack.
- Defers run in last-in, first-out order for the matching signal.
- Imported defers register when the imported executable code runs.

### Running handlers

- On normal completion, explicit `exit`, explicit `fail`, or direct-command
  failure, `EXIT` cleanup runs.
- For a cleanup signal, the matching trap runs before matching defers.
- `EXIT` cleanup runs after `INT`/`TERM` cleanup.
- Cleanup must run at most once per triggering event; recursion guards are
  backend implementation details.

### Defer execution

- Defer execution order is LIFO per signal.
- Handler failure does not skip older applicable defers.
- A handler's explicit `exit N` can replace the final status while still allowing
  remaining safe cleanup to run according to the current v0.22 contract.

### Signal delivery during foreground direct commands

- Supported foreground direct-command signal behavior is limited to `INT` and
  `TERM`.
- VM and Bash must both classify interrupted foreground direct commands as ds
  signal cleanup events where the supported contract applies.
- Child process-group setup/forwarding details are backend runtime mechanics, but
  observable stdout, stderr, side effects, cleanup order, and exit status should
  match.

### Signal delivery during foreground pipelines

- Supported foreground pipeline signal behavior is limited to simple foreground
  pipelines in the v0.22 surface.
- VM and Bash must both classify interrupted foreground pipelines as ds signal
  cleanup events where the supported contract applies.
- Pipeline signal tests must compare the same observable outcomes as direct
  command tests.

### Handler context restrictions

- No portable handler context object is currently exposed.
- Function-local captures from cleanup handlers are rejected.
- Direct `return` from a cleanup handler is rejected.
- `break` and `continue` follow normal lexical loop restrictions inside handler
  bodies.
- Future context values require explicit lowerer/HIR representation before VM or
  Bash support.

### Exit status behavior

- Normal cleanup success preserves the triggering status.
- `INT` defaults to `130`; `TERM` defaults to `143` when no handler overrides.
- Handler command failure can replace the final status.
- Explicit handler `exit N` can replace the final status.
- VM and Bash must agree on final process status for supported cases.

### Cleanup ordering

For supported signal-triggered cleanup:

```txt
matching trap, if any
matching defers in LIFO order
EXIT trap, if any
EXIT defers in LIFO order
```

For normal exit/fail/direct-command failure:

```txt
EXIT trap, if any
EXIT defers in LIFO order
```

## Non-goals

M3.x does not add or require:

- new signal names beyond `EXIT`, `INT`, and `TERM`;
- numeric signals;
- dynamic signal expressions;
- multiple signals per handler declaration;
- removing handlers;
- handler priority annotations;
- handler context values such as line number or signal object;
- background jobs, async jobs, coprocesses, job-control APIs, or wait groups;
- broad process-tree management beyond the supported foreground subset;
- command timeouts, cancellation tokens, or subprocess handles;
- continuing normal execution after `INT`/`TERM` beyond the existing documented
  behavior;
- a parser rewrite;
- a Bash emitter rewrite;
- a VM runtime rewrite;
- changes to command-result capture semantics.

## Implementation milestones

### M3.x.1 — Handler/trap/defer lifecycle docs only

- This document.
- Cross-link from `docs/concept-map.md`.
- No code changes.
- Exit when the ownership model and test plan are reviewable.

### M3.x.2 — Diagnostics ownership cleanup

- Audit parser/lowerer diagnostics for handler syntax vs semantic policy.
- Decide whether supported-signal validation should move from parser to lowerer.
- Move only obvious semantic diagnostics if safe.
- Add focused diagnostic tests proving invalid forms fail at the intended phase.
- Leave parser syntax diagnostics in parser.

### M3.x.3 — VM foreground direct-command signal cleanup

- Audit VM direct-command signal handling and process-group forwarding.
- Reword backend diagnostics as runtime/internal failures where needed.
- Add focused deterministic VM/Bash parity tests only if current coverage misses
  the cleaned path.

### M3.x.4 — VM foreground pipeline signal cleanup

- Audit VM pipeline signal handling separately from direct commands.
- Ensure pipeline interruption is not treated as generic pipeline failure when it
  should trigger ds signal cleanup.
- Add focused parity tests for supported pipeline cases if missing.

### M3.x.5 — Bash trap/defer parity cleanup

- Audit generated Bash cleanup helpers and trap dispatchers.
- Clarify helper comments around private `__ds_` state and ordering invariants.
- Keep Bash as emitter/runtime implementation for accepted HIR, not semantic
  owner.

### M3.x.6 — Deterministic tests and final docs

- Re-run the full v0.22 deterministic harness.
- Add any missing parity/diagnostic regression tests identified by earlier
  cleanup passes.
- Update `docs/runtime.md`, `docs/parity-contracts.md`, or
  `docs/diagnostics.md` only if implementation changed/clarified the contract.
- Move concept-map status from Hell candidate to Watch only after implementation
  proves the ownership model.

## Test plan

### Documentation/source-map tests

- `docs/concept-map.md` links to this maintenance spec.
- `docs/runtime.md` still documents the supported v0.22 cleanup subset.
- `docs/parity-contracts.md` continues to name trap/defer/signal behavior as a
  parity-sensitive contract.
- `docs/diagnostics.md` continues to say parser owns syntax, lowerer owns
  semantic/unsupported/parity diagnostics, and VM/Bash own backend failures.

### Parser / syntax diagnostics

Fixtures should cover:

- `defer { ... }` parses;
- `defer on: "EXIT"`, `"INT"`, and `"TERM"` parse;
- `trap "EXIT"`, `"INT"`, and `"TERM"` parse;
- missing `:` after `defer on`;
- missing signal string after `defer on:`;
- missing signal string after `trap`;
- missing `{` after handler declaration;
- malformed multiple-signal syntax remains rejected;
- formatter preserves supported handler syntax.

### Lowerer / semantic diagnostics

Fixtures should cover:

- unsupported signal names such as `HUP`, `QUIT`, `KILL`, `STOP`, empty strings,
  and lowercase names;
- dynamic signal expressions such as `defer on: sig` and `trap sig`;
- numeric signal expressions;
- direct `return` from a cleanup handler;
- function-local capture from a cleanup handler;
- handler context values remain rejected/deferred if syntax ever parses;
- unsupported handler forms fail before VM or Bash backend selection.

### VM behavior tests

Fixtures should cover:

- normal `EXIT` defer execution;
- multiple defers LIFO;
- trap replacement;
- `EXIT` trap before `EXIT` defers;
- explicit `exit` still runs cleanup;
- explicit `fail` still runs cleanup;
- direct-command failure still runs cleanup;
- captured command failure does not trigger fail-fast cleanup until normal exit;
- handler command failure updates final status but does not skip remaining safe
  cleanup;
- explicit handler `exit N` updates final status;
- imported defers/traps register in execution order;
- test-block cleanup does not leak into later tests.

### Bash parity tests

For each supported VM behavior case above, emitted Bash should match:

- stdout;
- stderr where deterministic and meaningful;
- exit status;
- file/environment side effects;
- helper emission expectations such as no cleanup helpers when no handlers exist;
- standalone Bash does not call `ds`.

### Foreground direct-command signal cases

Use deterministic process-session harness fixtures for:

- cooperative `TERM` during a foreground direct command;
- non-cooperative `TERM` during a foreground direct command;
- non-cooperative `INT` during a foreground direct command;
- matching signal trap/defers followed by `EXIT` cleanup;
- default status `130`/`143` when no handler overrides;
- explicit handler `exit N` override;
- command output before signal remains deterministic.

### Foreground pipeline signal cases

Use deterministic process-session harness fixtures for:

- `TERM` during a simple foreground pipeline;
- `INT` during a simple foreground pipeline;
- matching signal cleanup followed by `EXIT` cleanup;
- no generic pipeline-failure diagnostic replacing supported signal cleanup;
- parity of final status and output between VM and Bash.

### Nested defer/trap cases if currently supported

Fixtures should cover current supported nesting only:

- defers registered inside functions that are called and then return;
- defers/traps in imported files;
- defers inside test blocks;
- handlers that call functions;
- loops and conditionals inside handler bodies.

Do not add tests for unsupported new handler-context, async, or job-control
features except as explicit rejection tests.

### Unsupported handler forms rejected at correct semantic layer

For each unsupported form, test `check`, `emit bash`, and `run` when applicable.
The expected diagnostic should come from parser for malformed syntax or lowerer
for semantic unsupported forms. Bash and VM should not be the first owner of
source-language rejection.

### Regression cases for currently supported cleanup behavior

Keep coverage for:

- no helper emission when no handlers exist;
- helper emission when handlers exist;
- signal-specific defers ignored during normal exit;
- `EXIT` cleanup after signal cleanup;
- cleanup recursion guard behavior as observable non-duplication;
- imported cleanup order;
- direct command vs captured command fail-fast distinction.

## Exit criteria

M3.x implementation cleanup can move Trap/defer behavior, Signal handling, and
Handler context from Hell candidate to Watch only when:

- syntax vs semantic diagnostic ownership is explicitly implemented or
  intentionally documented;
- lowerer owns unsupported handler/source-language policy;
- HIR is the only accepted-handler contract consumed by VM and Bash;
- VM and Bash foreground direct-command signal behavior has focused parity tests;
- VM and Bash foreground pipeline signal behavior has focused parity tests;
- backend diagnostics are classified as runtime/emission/internal invariant
  failures, not source validation;
- v0.22 deterministic tests pass after cleanup;
- docs are updated only where the implementation proves them inaccurate.

Clear status is not expected in M3.x. Watch is the realistic target because
signals and process behavior remain OS-sensitive and parity-sensitive.

## Deferred items vs out-of-scope items

Deferred to implementation milestones:

- moving supported-signal policy from parser to lowerer if safe;
- rewording backend diagnostics that currently look semantic;
- adding missing focused parity tests found by implementation audit;
- clarifying comments around VM/Bash cleanup ordering invariants.

Out of scope for M3.x:

- new signal names;
- background/job-control APIs;
- handler context variables;
- async process management;
- changing the user-facing syntax;
- broad parser, VM, or Bash emitter rewrites.
