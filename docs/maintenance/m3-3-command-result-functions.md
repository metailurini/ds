# M3.3 — Command-result function metadata cleanup

This is a maintenance specification for command-result functions in `ds`. It is
not an implementation milestone and does not add language features. Its purpose
is to make command-result function behavior a metadata-driven language contract
before cleanup work touches the lowerer, VM, Bash emitter, diagnostics, or tests.

Use this with:

- `docs/source-map.md` for file-level ownership.
- `docs/concept-map.md` for cross-cutting concept risk.
- `docs/parity-contracts.md` for VM/Bash acceptance rules.
- `docs/diagnostics.md` for phase-owned diagnostics.
- `docs/architecture.md` for the parse -> lower -> backend pipeline.
- `docs/runtime.md` for command-result runtime value ownership.
- `docs/language.ds` and milestone specs for the user-facing language surface.
- `docs/maintenance/m3-2-function-return-kinds.md` for adjacent user-function
  return-kind ownership.

## Current risk

Command-result functions are a Hell candidate because they are not one isolated
runtime value. They combine:

- captured command execution (`run ...`) and its inspectable fields;
- user functions whose return kind is `command_result`;
- stdlib/helper metadata that can report a command-result return kind;
- expression-position call validation;
- statement-position call validation;
- return-statement portability gates;
- field access and interpolation rules;
- VM bytecode and runtime value execution;
- Bash function return ABI, command capture helpers, and field rendering;
- diagnostics for unknown fields, unsupported temporary field access, wrong call
  position, invalid return payloads, and VM/Bash parity gates.

The current implementation has many working rules, but the rules are easy to
rediscover ad hoc. Future work can accidentally add a command-result-producing
helper or function path by updating one of stdlib, lowering, VM, or Bash first,
then discovering later that field access, return portability, diagnostics, or
parity tests disagree.

## Current implementation trace

### Parser

Relevant files:

- `src/parse_expr.c`
- `src/parse_stmt.c`
- `src/parse_command.c`
- `src/ds_ast.h`
- `src/ds_command.h`

Current role:

- Parses captured command expressions as `run ...` AST expressions.
- Parses plain command statements separately from expression calls.
- Parses field expressions such as `result.stdout` and interpolation segments
  such as `{result.stdout}` as syntax shapes.
- Parses function declarations and calls without knowing whether a call returns a
  command result.
- Uses `DsCommand` / `DsWord` syntax data for command stages, redirections, and
  word segments.

Current risk:

- Parser syntax can look command-result-like (`x.stdout`), but it is too early
  to know whether `x` is a command result, a map, or an unknown symbol.
- Parser-side captured-command restrictions should remain syntax-shape checks
  only. Semantic rules such as “this call returns a command result” or “this
  field is valid on this receiver” belong later.

Desired parser boundary:

- Own only syntax shape for `run`, field expressions, command words, calls,
  interpolation, and returns.
- Do not decide command-result value kind, valid command-result fields, call
  position, VM/Bash portability, or function return metadata.

### AST

Relevant files:

- `src/ds_ast.h`
- `src/ds_command.h`

Current role:

- Stores raw `run` expressions, field expressions, call expressions, function
  declarations, and return statements.
- Carries source spans and command syntax data needed by lowering and formatter.

Current risk:

- AST shape alone is not a command-result contract. A field expression can target
  a command result or a map; a call expression can be scalar, collection,
  command-result, statement-only, or unknown.

Desired AST boundary:

- Preserve syntax and spans.
- Do not become canonical for command-result semantics.

### Stdlib metadata

Relevant files:

- `src/ds_stdlib.h`
- `src/stdlib.c`
- `src/lower_symbols.c`

Current role:

- `DsStdlibHelper` defines helper facts: source name, Bash helper name, arity,
  `DsStdlibReturnKind`, statement-only status, string-argument restrictions,
  iterable status, env-name validation, and recursive-glob rejection.
- `DS_STDLIB_RETURN_COMMAND_RESULT` exists as a metadata value even though the
  current helper table mostly exercises command-result behavior through `run`
  and user-function returns.
- `stdlib_return_kind()` maps stdlib return metadata to lowerer symbol kinds,
  including `SYM_COMMAND_RESULT`.

Current risk:

- The metadata table is the right direction, but the full command-result function
  contract is not centralized there yet. Lowering, VM, and Bash still contain
  separate return-kind/name switches and field handling logic.
- Adding a future stdlib helper that returns a command result could require
  edits in several places unless the metadata contract becomes the only source of
  acceptance truth.

Desired stdlib boundary:

- Own stdlib helper facts only.
- Expose command-result return kind through metadata.
- Do not own language acceptance beyond helper facts such as arity and declared
  return kind.

### HIR/lowering

Relevant files:

- `src/ds_hir.h`
- `src/lower_functions.c`
- `src/lower_expr.c`
- `src/lower_stmt.c`
- `src/lower_symbols.c`
- `src/lower_internal.h`

Current role:

- `DsLowerValueKind` includes `DS_LOWER_VALUE_COMMAND_RESULT`.
- `DsLowerExpr` represents captured commands as `DS_LOWER_EXPR_RUN`.
- `DsLowerExpr.call.return_kind` carries the backend-neutral return kind for
  lowered calls.
- `lower_functions.c` discovers provisional user-function return contracts and
  can infer `DS_LOWER_VALUE_COMMAND_RESULT` from `run` expressions or forwarded
  command-result calls.
- `lower_expr.c` validates command-result field access, interpolation fields,
  expression-position function calls, stdlib call return kinds, command-result
  field kinds, and temporary field/index parity gates.
- `lower_stmt.c` validates return statements and structured-return portability,
  including command-result returns from `run`, named bindings, or forwarded
  user-function calls.
- `lower_symbols.c` maps stdlib command-result metadata to `SYM_COMMAND_RESULT`.

Current risk:

- Command-result behavior is currently spread across generic return-kind logic,
  command-result field logic, stdlib metadata mapping, structured-return gates,
  and call-position checks.
- The concept has no named “command-result function contract” helper or table;
  the lowerer uses several local checks that future code could bypass.
- Some backend code still derives type names or command-result field behavior
  from the same metadata independently, which is useful for rendering but risky
  if it becomes validation.

Desired lowerer boundary:

- Own source-language validation for command-result function behavior.
- Decide whether a call expression/statement is accepted, what value kind it
  returns, and whether its command-result fields are valid before backend
  selection.
- Reject unsupported or nonportable command-result function forms before VM or
  Bash execution/emission.
- Produce HIR in which VM and Bash can trust `return_kind`, `DS_LOWER_EXPR_RUN`,
  `DS_LOWER_EXPR_FIELD`, and structured-return statements as already valid.

### VM execution

Relevant files:

- `src/vm_compile.c`
- `src/vm.c`
- `src/vm_scope.c`
- `src/vm_process.c`
- `src/vm_stdlib.c`
- `src/runtime.c`
- `src/ds_runtime.h`

Current role:

- `vm_compile.c` compiles `DS_LOWER_EXPR_RUN` to bytecode that stores command
  syntax and compiles field access/calls from accepted HIR.
- `vm.c` executes run-capture instructions, field access instructions, stdlib
  calls, and user-function calls.
- `vm_process.c` owns process execution and creates `DsValue` command-result
  payloads with stdout, stderr, and exit code. It also reads command-result
  fields from runtime values.
- `vm_stdlib.c` executes stdlib helpers according to helper names and expected
  runtime values.
- `runtime.c` owns `DS_VALUE_COMMAND_RESULT` copying/freeing/rendering.

Current risk:

- The VM can naturally carry command-result values in more places than Bash can
  encode. If lowerer gates are missing, VM execution can accept a command-result
  function form that Bash rejects later.
- Runtime field-access diagnostics are legitimate for bad runtime state, but they
  must not become the first source-language validation for accepted HIR.

Desired VM boundary:

- Execute accepted HIR and stdlib metadata.
- Own runtime/process failures: subprocess launch failure, command exit status,
  filesystem/env/path issues, malformed internal runtime state, and defensive
  impossible states.
- Do not decide whether a source-level command-result function form is accepted.

### Bash emission

Relevant files:

- `src/bash_emit.c`
- `src/bash_deps.c`
- `src/bash_expr.c`
- `src/bash_stmt.c`
- `src/bash_command.c`
- `src/bash_quote.c`
- `src/bash_helpers.c`

Current role:

- Emits command capture helpers and field-rendering code for accepted HIR.
- Emits command-result storage variables such as `_stdout`, `_stderr`, `_code`,
  `_status`, `_ok`, and `_failed` sidecars.
- Emits user-function return ABI handling for command-result return kinds.
- Scans accepted HIR to decide which Bash helpers are required.
- Contains defensive invariant diagnostics for command-result return shapes that
  lowering should already have validated.

Current risk:

- Bash is the constrained backend and is therefore the most likely place for
  source-language command-result restrictions to creep in as backend errors.
- Bash expression/statement emission currently repeats some return-kind and
  field-kind mappings for rendering. That is acceptable only if it remains a
  consumer of lowerer/HIR metadata, not a validator.

Desired Bash boundary:

- Render accepted command-result HIR and helper calls.
- Own standalone Bash artifact generation, helper selection, quoting, private
  return ABI implementation, and internal ABI invariant checks.
- Do not be the first owner of user-facing unsupported command-result function
  diagnostics.

### Diagnostics

Current role:

- Lowering emits diagnostics for invalid field access, unknown command-result
  fields, unsupported temporary field/index access, call-position errors,
  structured-return portability, and parity gates.
- VM emits runtime diagnostics for command/process failures and defensive
  runtime field failures.
- Bash emits artifact/invariant diagnostics when accepted HIR cannot be rendered
  due to internal/backend conditions.

Current risk:

- Some diagnostics mention command-result behavior from several layers. Without a
  named contract, future tests may accidentally rely on VM/Bash fallback errors
  instead of lowerer-owned source-language diagnostics.

Desired diagnostic boundary:

- Source-language invalidity is lowerer-owned.
- Backend diagnostics are allowed only for runtime/process/emission failure or
  defensive invariant checks after accepted HIR reaches a backend.

### Tests

Relevant areas:

- `tests/v0_7` for captured command-result runtime values, fields, interpolation,
  bytecode, and VM/Bash parity.
- `tests/v0_10` for map/field overlap and command-result field access in loops.
- `tests/v0_11` and `tests/v0_12` for stdlib command helpers such as
  `cmd.exists` and `cmd.require`.
- `tests/v0_18` and later for pipeline and command-result parity.
- `tests/v0_21` for function return fields and scalar field returns.
- `tests/v0_25` and `tests/v0_26` for function value returns, structured returns,
  and command-result function returns.
- `tests/v0_27` for newer environment/direct-access interactions with existing
  command-result behavior.

Current risk:

- Tests cover many behaviors, but the maintenance contract should add focused
  assertions that unsupported command-result function forms fail at `check` time
  and do not depend on VM-only or Bash-only fallback failures.

## Architectural failure

The architectural failure is not that command-result functions are broken. The
failure is that the concept is represented as a collection of local facts:

- `run` expressions imply command-result values;
- stdlib metadata can declare command-result returns;
- user-function return metadata can declare command-result returns;
- field descriptors live in command metadata;
- VM and Bash each know how to materialize/read command-result payloads;
- structured-return portability gates know special command-result shapes.

Those facts are correct individually, but no single contract currently says:

> This call produces a command-result value; these fields are valid; this form is
> portable across VM and Bash; this backend code may execute/render it without
> rediscovering source-language validity.

M3.3 should make that contract explicit in code without changing the language.

## Desired ownership

- **Parser**: syntax only for calls, field expressions, interpolation, `run`, and
  function/return statements.
- **Stdlib metadata**: helper facts, including declared command-result return
  kind where applicable.
- **Lowerer**: canonical source-language validation and HIR contract creation for
  command-result-producing expressions/functions.
- **HIR**: backend-neutral command-result representation through
  `DS_LOWER_VALUE_COMMAND_RESULT`, `DS_LOWER_EXPR_RUN`, `DS_LOWER_EXPR_CALL`, and
  `DS_LOWER_EXPR_FIELD`.
- **VM**: execute accepted command-result HIR and create/read runtime
  `DS_VALUE_COMMAND_RESULT` values.
- **Bash emitter**: render accepted command-result HIR using standalone helper
  code and private ABI storage.
- **Diagnostics**: lowerer owns source-language invalidity; VM/Bash own only
  runtime/emission/internal failures.
- **Tests**: prove accepted command-result function behavior has VM/Bash parity
  and unsupported forms fail before backend selection.

## Canonical representation

The implementation pass should make these the explicit canonical pieces:

- `DsStdlibHelper.return_kind == DS_STDLIB_RETURN_COMMAND_RESULT` for stdlib
  helpers that return command-result values.
- `DsLowerValueKind == DS_LOWER_VALUE_COMMAND_RESULT` for any lowered expression,
  call, variable, field receiver, or return statement whose value is a command
  result.
- `DsLowerExprKind == DS_LOWER_EXPR_RUN` for direct captured command execution.
- `DsLowerExprKind == DS_LOWER_EXPR_CALL` with `return_kind` set to
  `DS_LOWER_VALUE_COMMAND_RESULT` for user or stdlib function calls that produce
  command-result values.
- `DsLowerExprKind == DS_LOWER_EXPR_FIELD` for validated field reads whose
  receiver kind was proven by the lowerer.
- `DsCommandResultField` descriptors as the single field-name/type catalog for
  `.stdout`, `.stderr`, `.status`, `.code`, `.ok`, and `.failed`.
- `DS_VALUE_COMMAND_RESULT` as the VM/runtime payload representation only after
  accepted HIR reaches runtime execution.
- Bash `__ds_` command-result sidecar variables as emitted-script ABI details,
  not source-language representation.

## Validation owner

The lowerer should own all of the following:

- arity and statement-only/expression-position validation for stdlib calls;
- user-function command-result call validation in expression position;
- command-result field existence and field kind validation;
- rejection of field access on non-command-result/non-map receivers;
- rejection of temporary command-result field access when VM/Bash portability
  requires a named binding;
- return-statement validation when a function returns command results;
- structured-return portability for command-result returns;
- consistency between provisional user-function return metadata and concrete
  return statements.

Parser, VM, and Bash should not be the first layer to reject these source
language forms.

## VM execution owner

The VM should own:

- process execution for `run` expressions;
- construction, copy, free, and runtime field reads of `DS_VALUE_COMMAND_RESULT`;
- VM stdlib helper execution for accepted helpers;
- user-function call/return execution for accepted command-result return kinds;
- runtime/process diagnostics such as command launch failure, nonzero command
  status where appropriate, and internal runtime invariants.

The VM should consume `return_kind` / HIR metadata and should not infer command-result source acceptance from helper names or field names except where executing
already-accepted HIR requires runtime payload handling.

## Bash emission owner

The Bash emitter should own:

- helper dependency selection for accepted command-result HIR;
- rendering captured commands and pipelines;
- rendering field reads against named command-result storage;
- rendering command-result function returns and user-function call captures;
- private `__ds_` command-result ABI validation inside emitted Bash;
- Bash syntax/artifact failures and internal invariant diagnostics.

The Bash emitter should not own source-language diagnostics such as “this
command-result function form is unsupported.” Those should be lowerer diagnostics
unless the form is explicitly documented as Bash-only or emission-only.

## Diagnostics ownership

- Syntax errors: parser.
- Unknown command-result field: lowerer.
- Field access on the wrong receiver kind: lowerer.
- Unsupported temporary command-result field access: lowerer.
- Command-result function call in an invalid position: lowerer.
- Nonportable command-result return shape: lowerer.
- Runtime process failure while executing accepted HIR: VM.
- Emitted Bash private return payload corruption: Bash helper/internal ABI
  invariant.
- Backend “unsupported command-result ...” messages should be treated as
  ownership pressure unless clearly reworded as internal invariant diagnostics.

## VM/Bash parity contract

A command-result function form is accepted only when the lowerer can produce
backend-neutral HIR with defined behavior for both VM execution and standalone
Bash emission.

Accepted forms should have:

- one canonical value kind: `DS_LOWER_VALUE_COMMAND_RESULT`;
- one field catalog: `DsCommandResultField`;
- VM behavior that creates and reads `DS_VALUE_COMMAND_RESULT` values;
- Bash behavior that creates and reads equivalent sidecar variables;
- parity tests comparing stdout/stderr/status and field values;
- diagnostics for unsupported forms before backend selection.

Unsupported forms should be rejected by lowering unless they are explicitly
specified as VM-only, Bash-only, diagnostic-only, or currently rejected.

## Refactor plan

The implementation cleanup should be small and reviewable:

1. **Name the contract in lowering.** Add a small helper or set of helpers that
   answers whether a lowered expression/call is a command-result-producing form
   and whether it has a portable VM/Bash representation. Do not add new behavior.
2. **Use metadata as the source of truth.** Ensure stdlib command-result returns
   are recognized through `DsStdlibHelper.return_kind` and mapped once into
   lowerer/HIR value kind metadata.
3. **Centralize field validation.** Keep `DsCommandResultField` as the field
   catalog and make lowerer field validation the first user-facing diagnostic for
   command-result fields.
4. **Audit backend fallbacks.** Reword or comment VM/Bash command-result fallback
   errors as runtime/internal invariants where lowering should already prevent
   source-language invalidity.
5. **Protect call positions.** Verify command-result functions are accepted in
   expression positions and rejected from invalid statement/value positions using
   lowerer diagnostics, not backend diagnostics.
6. **Protect returns.** Verify command-result function returns use the M3.2 return
   contract and structured-return portability gates.
7. **Add focused tests only where gaps exist.** Prefer check/emit/run assertions
   proving unsupported forms fail at the documented layer, plus VM/Bash parity
   tests for accepted forms.
8. **Update docs only if implementation finds the spec inaccurate.** Do not use
   implementation cleanup as an excuse to rewrite language docs.

## Non-goals

- Do not add new command-result fields.
- Do not remove the `.code` compatibility alias or change `.status` behavior.
- Do not add new stdlib helpers that return command results.
- Do not expand direct `probe().stdout` support; keep the existing named-binding
  parity rule unless a later feature milestone designs portable temporaries.
- Do not redesign command words, pipelines, redirection, trap/defer/signal
  behavior, or process execution.
- Do not redesign the Bash `__ds_` return ABI.
- Do not change function syntax or return-kind syntax.
- Do not broaden into mutable collections, regex captures, or env direct access.

## Test plan

Implementation cleanup should run or add focused tests for:

- command-result field access on named captured values;
- command-result function returns from direct `run`;
- command-result function returns from named captured values;
- forwarded command-result user-function returns;
- imports involving command-result-returning functions;
- `.stdout`, `.stderr`, `.status`, `.code`, `.ok`, and `.failed` parity;
- temporary command-result field access rejection, such as `probe().stdout`;
- unknown command-result field rejection;
- field access on non-command-result/non-map values;
- wrong call-position diagnostics for statement-only helpers or no-value
  functions;
- Bash emission helper selection for command-result programs;
- VM/Bash parity for stdout, stderr, and status on successful and nonzero
  captured commands.

Useful existing suites include:

- `make test-v0-7`
- `make test-v0-10`
- `make test-v0-11`
- `make test-v0-12`
- `make test-v0-18`
- `make test-v0-21`
- `make test-v0-25`
- `make test-v0-26`
- `make test-v0-27`

Run the full `make test` when practical, but targeted suites are acceptable when
full-suite runtime is too large for the maintenance pass.

## Exit criteria

M3.3 implementation cleanup can be considered complete when:

- command-result functions have one clearly named lowerer/HIR contract;
- stdlib command-result return facts flow through metadata instead of ad hoc
  backend checks;
- lowerer owns all user-facing source-language diagnostics for command-result
  function acceptance;
- VM and Bash consume accepted HIR/function metadata without rediscovering
  source-language validity;
- backend command-result diagnostics are legitimate runtime/emission/internal
  invariant checks;
- accepted command-result function behavior has VM/Bash parity tests;
- unsupported command-result function forms have check/emit/run diagnostics that
  fail before backend-specific divergence;
- `docs/concept-map.md` can move Command-result functions from **Hell
  candidate** to **Watch**, not **Clear**, because command words and future
  portable temporary support can still affect the concept.

## Implementation cleanup result

The M3.3 cleanup pass moved this concept to **Watch** by making the existing
metadata contract more explicit in code without changing language behavior:

- stdlib return metadata maps once into lowered value-kind metadata;
- `DS_LOWER_EXPR_CALL.return_kind` is the backend-facing call result contract for
  both stdlib and user-function calls;
- lowerer helpers classify command-result-producing expressions and portable
  command-result return shapes;
- Bash expression/statement type rendering consumes lowered call metadata instead
  of re-deriving stdlib call result types from helper names.

This is intentionally not **Clear**. Future command-result-producing helpers,
direct temporary command-result field support, and command-word interpolation
work must continue to update the lowerer/HIR contract first and then let VM/Bash
consume accepted metadata.
