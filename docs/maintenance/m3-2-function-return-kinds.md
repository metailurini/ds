# M3.2 — Function return kind contract

This is a maintenance specification for function return kinds in `ds`. It is not
an implementation milestone and does not add language features. Its purpose is
to make function return kind a language-level contract before any cleanup of the
parser, lowerer, VM, Bash emitter, diagnostics, or tests.

Use this with:

- `docs/source-map.md` for file-level ownership.
- `docs/concept-map.md` for cross-cutting concept risk.
- `docs/parity-contracts.md` for VM/Bash acceptance rules.
- `docs/diagnostics.md` for phase-owned diagnostics.
- `docs/architecture.md` for the parse -> lower -> backend pipeline.
- `docs/runtime.md` for runtime value and process behavior.
- `docs/language.ds` and milestone specs for the user-facing language surface.

## Current risk

Function return kind started this pass as a Hell candidate because it is not one
isolated feature. It affects:

- function declaration and signature collection;
- return statement lowering;
- return kind inference across control flow;
- expression-call versus statement-call validation;
- scalar, collection, map, and command-result payload encoding;
- direct command interpolation and command-word restrictions;
- VM function-call execution and return unwinding;
- standalone Bash function emission and private return ABI helpers;
- diagnostics for unknown, mixed, unsupported, incomplete, or nonportable return
  forms;
- parity tests that must prove VM and emitted Bash consume the same contract.

The risk is not that the current implementation has no rules. The risk is that
the rules are spread across several files and milestones, so future work can add
a new return kind or call form by teaching one backend first and only later
finding parity or diagnostic gaps.

Implementation status after M3.2 cleanup: the concept is **Watch** in
`docs/concept-map.md`. The lowerer is the source-language validation owner, the
parser no longer emits the duplicate semantic `return`-outside-function
diagnostic, and emitted Bash helper failures are documented as internal ABI
invariants. Remaining risk is future drift when new command interpolation or
structured-return surfaces add more call/return positions.

## Current implementation trace

### Parser

Relevant files:

- `src/parse_function.c`
- `src/parse_stmt.c`
- `src/ds_ast.h`

Current role:

- Parses function declarations, parameter lists, defaults, function bodies, and
  `return` statements into AST nodes.
- Stores a return statement as `DS_STMT_RETURN` with an expression payload.
- Stores function declarations as `DS_STMT_FN` with a name, parameters, and body.
- Emits syntax diagnostics for malformed function syntax.

Current risk:

- The parser should not own return kind semantics. Any old parser diagnostics
  about deferred function returns should be treated as parse-recovery legacy or
  diagnostic pressure unless they are purely syntax-shape errors.

Desired parser boundary:

- Own function syntax shape only.
- Do not decide whether a function has a value, which kind it returns, whether a
  call can appear in expression position, or whether a return payload is portable
  across VM and Bash.

### AST

Relevant file:

- `src/ds_ast.h`

Current role:

- Represents raw function declarations and `return` statements before semantic
  validation.
- Does not store return kind metadata.

Current risk:

- AST is intentionally too early to be canonical for return kind. Using AST
  shape alone to infer language acceptance risks duplicating lowerer rules.

Desired AST boundary:

- Preserve enough syntax and spans for lowerer diagnostics.
- Do not become the source of truth for accepted return kinds.

### HIR/lowering

Relevant files:

- `src/ds_hir.h`
- `src/lower_functions.c`
- `src/lower_expr.c`
- `src/lower_stmt.c`
- `src/lower_symbols.c`
- `src/lower_internal.h`

Current role:

- `DsLowerValueKind` is the current backend-neutral value-kind enum:
  `unknown`, `bool`, `int`, `string`, `array`, `map`, and `command_result`.
- `DsLowerFn` carries function metadata:
  - name;
  - parameters;
  - required parameter count;
  - `return_kind`;
  - `has_return`;
  - `all_paths_return`;
  - `contains_plain_command`.
- `DS_LOWER_STMT_RETURN` carries the lowered return expression and return kind.
- `lower_functions.c` collects function signatures, pre-infers return kinds for
  known return expressions, computes all-paths-return state, and performs
  recursion/call graph checks.
- `lower_expr.c` validates user-function calls in expression position, including
  wrong arity, no-value functions, not-all-paths-return functions, plain-command
  functions, argument kind checks, and maps the function return kind back to the
  expression kind.
- `lower_stmt.c` lowers return statements, rejects mixed return kinds, rejects
  unknown return kinds, and currently owns structured-return portability gates.
- `lower_symbols.c` maps stdlib helper metadata into lowerer symbol/value kinds.

Current risk:

- Return kind inference exists in more than one shape: an AST prepass in
  `lower_functions.c`, final return statement validation in `lower_stmt.c`, and
  call-position validation in `lower_expr.c`.
- Structured return portability is now lowerer-owned, but the contract is still
  implicit in helper functions and diagnostics.
- Some return-call restrictions are expressed as function metadata booleans rather
  than a single named return-kind contract.

Desired lowerer boundary:

- Own all semantic acceptance for function return kinds.
- Own the only user-facing diagnostics for unknown, mixed, unsupported,
  nonportable, or expression-position-invalid return kinds.
- Produce HIR that is safe for both VM and Bash to consume without rediscovering
  language validity.

### VM

Relevant files:

- `src/vm_compile.c`
- `src/vm.c`
- `src/vm_scope.c`
- `src/runtime.c`
- `src/vm_stdlib.c` where stdlib return values are created

Current role:

- Compiles `DS_LOWER_STMT_RETURN` to `OP_RETURN_VALUE`.
- Executes return unwinding through VM return stacks and copies the runtime
  value back to the caller register when the call expects one.
- Represents actual payloads with runtime `DsValue` kinds, including strings,
  ints, bools, arrays, maps, and command results.
- Reports runtime failures such as return outside an active function only as a
  defensive/runtime error.

Current risk:

- The VM can naturally carry more runtime value shapes than Bash can encode.
  Without lowerer gates, the VM can accidentally accept a return payload that the
  Bash backend rejects later.

Desired VM boundary:

- Execute accepted HIR return behavior.
- Do not decide whether a function may return a given kind.
- Do not diagnose user-facing unsupported return kinds except as defensive
  internal/runtime failures that should be unreachable after lowering.

### Bash emitter

Relevant files:

- `src/bash_stmt.c`
- `src/bash_expr.c`
- `src/bash_helpers.c`
- `src/bash_emit.c`

Current role:

- Emits Bash functions from accepted HIR.
- Uses a private Bash return ABI with variables such as `__ds_return_type`,
  `__ds_return_value`, `__ds_return_array`, `__ds_return_map`,
  `__ds_return_stdout`, `__ds_return_stderr`, and status/boolean sidecars.
- Captures user-function call results through helper code and validates internal
  payload integrity.
- Emits defensive invariant diagnostics for return shapes that lowering should
  prevent.

Current risk:

- Bash is the most constrained backend, so it is the easiest place for return
  kind semantics to drift into backend diagnostics.
- Internal payload validation in emitted Bash is legitimate, but it must not be
  confused with source-language return-kind validation.

Desired Bash boundary:

- Render accepted HIR return statements and function calls.
- Own only Bash artifact generation, helper ABI implementation, and defensive
  internal payload checks.
- Do not be the first owner of user-facing unsupported return-kind diagnostics.

### Stdlib

Relevant files:

- `src/stdlib.c`
- `src/ds_stdlib.h`
- `src/lower_symbols.c`
- `src/vm_stdlib.c`
- Bash stdlib/helper emission in `src/bash_*`

Current role:

- `DsStdlibHelper.return_kind` declares helper return kinds for stdlib calls.
- `lower_symbols.c` maps stdlib return metadata into lowerer symbol/value kinds.
- VM and Bash implement helper payloads separately.

Current risk:

- Stdlib helper return kinds are metadata for helper calls, not user-function
  return-kind policy. A helper returning an array, for example, does not
  automatically mean every direct user-function return of that temporary helper
  payload is portable.

Desired stdlib boundary:

- Own helper metadata and backend helper behavior.
- Let the lowerer decide whether stdlib result shapes are valid in function
  return position.

### Diagnostics

Relevant docs/files:

- `docs/diagnostics.md`
- `src/lower_functions.c`
- `src/lower_expr.c`
- `src/lower_stmt.c`
- defensive VM/Bash diagnostics

Current role:

- Lowering already owns many return diagnostics:
  - return outside a function;
  - return inside handler;
  - unknown return value kind;
  - mixed return kinds;
  - function without value used as expression;
  - not-all-paths-return expression calls;
  - plain-command function used as value;
  - nonportable structured return payloads.
- Bash helpers own internal emitted-script payload integrity diagnostics.
- VM owns runtime defensive failures such as return outside an active call frame.

Current risk:

- Some diagnostics are phrased by milestone/version instead of by contract.
- Some backend diagnostics may still be defensive leftovers from earlier return
  kind work and should stay only when documented as internal invariant checks.

Desired diagnostic boundary:

- Lowerer owns every source-language return-kind rejection.
- Backend diagnostics should be internal invariant/runtime/emission failures only.

### Tests

Relevant tests:

- `tests/v0_21/run.sh` for initial function returns.
- `tests/v0_25/run.sh` for scalar value-return protocol and Bash ABI integrity.
- `tests/v0_26/run.sh` for flat array/map/command-result returns and structured
  return diagnostics.
- `tests/v0_27/run.sh` for interaction with later expression/interpolation work.

Current role:

- Tests cover scalar return values, Bash emission determinism, no `ds` dependency
  in emitted scripts, payload leakage, malformed internal payload guards,
  structured returns, mixed returns, and some unsupported structured temporary
  returns.

Current risk:

- Tests are milestone-scattered. Future return-kind cleanup may miss required
  coverage unless the test ownership matrix is explicit.

Desired test boundary:

- Every accepted return kind needs VM and emitted-Bash parity tests.
- Every rejected return kind or source-position misuse needs a diagnostic test
  proving rejection before backend divergence.
- Internal Bash payload corruption tests should remain backend-internal tests and
  should not be mistaken for language acceptance tests.

## Architectural failure

The architectural failure is that "function return kind" is currently an emergent
property of several implementation facts:

- AST return expression shape;
- function collection/pre-inference;
- lowerer symbol kind;
- `DsLowerValueKind`;
- user-function call validation;
- VM runtime value support;
- Bash private return ABI;
- stdlib helper metadata;
- diagnostic tests scattered by version.

That makes it too easy to add a return kind by modifying whichever area exposes
the immediate failure. The project needs a single contract: a return kind is
accepted only when lowering can represent it in HIR, validate all call/return
positions, and both VM and Bash can execute the same observable behavior.

## Desired ownership

### Canonical representation

The canonical representation is:

1. `DsLowerValueKind` for the backend-neutral return value kind.
2. `DsLowerFn` metadata for function-level return facts:
   - `has_return`;
   - `return_kind`;
   - `all_paths_return`;
   - `contains_plain_command` until that concept gets its own explicit call-mode
     metadata.
3. `DS_LOWER_STMT_RETURN` for each lowered return statement, with the lowered
   value and its `return_kind`.
4. Shared stdlib metadata only for stdlib helper return facts, not for
   user-function return policy.

AST is not canonical for return kind. VM `DsValue` and Bash `__ds_return_*`
variables are execution representations, not language acceptance contracts.

### Validation owner

The lowerer owns return-kind validation.

Specifically:

- `lower_functions.c` owns function signature collection, return-kind inference
  and all-paths-return facts, and function graph checks.
- `lower_stmt.c` owns `return` statement validity and payload portability.
- `lower_expr.c` owns function-call validity in expression position and return
  kind propagation to expression kind.
- parser owns only syntax shape.
- checker owns warnings only.

### Execution owners

- VM owns direct execution of accepted HIR returns and runtime payload movement.
- Bash emitter owns standalone Bash rendering of accepted HIR returns and the
  private Bash return ABI.
- Runtime/stdlib own concrete runtime/helper payload construction after lowering
  accepts the form.

### Documentation owner

This maintenance spec is the focused owner for the function return kind cleanup
plan. User-facing behavior remains documented in `docs/language.ds`, milestone
specs, and `docs/runtime.md` as appropriate. VM/Bash acceptance requirements are
owned by `docs/parity-contracts.md`; phase ownership is owned by
`docs/diagnostics.md`.

## VM/Bash parity contract

A user function return kind is accepted only when all of the following are true:

1. The return kind has a backend-neutral `DsLowerValueKind` representation.
2. Every `return` statement in the function can be lowered to the same return
   kind.
3. Function metadata records whether the function returns a value and whether all
   control paths return for expression-position calls.
4. The return payload has a portable backend representation.
5. VM execution and emitted Bash produce the same observable call behavior:
   - same value kind;
   - same scalar value or structured payload;
   - same stdout/stderr behavior;
   - same exit/status behavior for command-result returns;
   - same diagnostics for rejected source programs.

If any item is not true, the form is currently rejected by lowering unless it is
explicitly documented as VM-only, Bash-only, diagnostic-only, or otherwise
backend-specific.

## Diagnostic ownership

Lowerer-owned diagnostics should cover:

- `return` outside a function;
- `return` from unsupported handler contexts;
- unknown return value kind;
- mixed return kinds inside one function;
- function call used as a value when the function does not return;
- function call used as a value when not all paths return;
- function call used as a value when stdout behavior would collide with value
  return capture;
- unsupported or nonportable structured return payloads;
- unsupported direct interpolation or command-word use of returned values when the
  return kind is not safely representable.

Backend diagnostics may remain only for:

- impossible VM return-frame failures;
- emitted Bash helper payload corruption;
- internal Bash invariant failures for accepted HIR shapes that should have been
  guaranteed by lowering;
- output/artifact failures while writing emitted Bash.

## Refactor plan

This plan is intentionally small-step. Do not implement it in one blast.

### Pass 1: inventory and naming

- Inventory every function-return diagnostic in parser, lowerer, VM, Bash, and
  tests.
- Rename or comment backend diagnostics that are defensive invariant checks.
- Record any parser diagnostics that mention deferred return semantics and decide
  whether they are syntax recovery or misplaced semantics.

### Pass 2: centralize return-kind contract helpers

- Add a lowerer-private helper layer, if needed, that answers:
  - whether an expression has a known return value kind;
  - whether a return payload is portable;
  - whether a function may be called as a value;
  - whether a function may be called as a statement.
- Keep helpers lowerer-private unless HIR needs explicit metadata.

### Pass 3: tighten diagnostics

- Move any remaining source-language return-kind rejection out of Bash/VM into
  lowering.
- Keep backend checks only as internal invariants.
- Ensure diagnostics use contract language rather than backend implementation
  details.

### Pass 4: test matrix

For every accepted return kind:

- add/verify VM behavior tests;
- add/verify emitted-Bash parity tests;
- add/verify HIR/bytecode dump tests where the ABI must not leak;
- add/verify diagnostic tests for expression-position misuse, mixed kinds,
  unknown kinds, and nonportable payloads.

### Pass 5: concept-map status update

After implementation cleanup, move Function return kinds from **Hell candidate**
to **Watch** only when:

- lowerer is the only source-language validation owner;
- backend diagnostics are documented as defensive/runtime/emission-only;
- tests cover accepted and rejected return kinds across VM and Bash.

Move to **Clear** only if return kinds have a compact, obvious contract with low
risk of future backend drift. That is unlikely until command interpolation and
structured returns are also less risky.

## Non-goals

This spec does not:

- add new return kinds;
- add typed function return annotations;
- add nested collection returns;
- add collection-valued parameters;
- redesign command words or interpolation;
- redesign the Bash private return ABI;
- rewrite VM call frames;
- change stdlib helper behavior;
- move code during this documentation pass.

## Exit criteria for the implementation pass

The implementation pass is complete when:

- return-kind source-language diagnostics are lowerer-owned;
- parser diagnostics are syntax-only;
- VM diagnostics are runtime/invariant-only;
- Bash diagnostics are emission/invariant-only;
- HIR/function metadata is the single backend-facing return-kind contract;
- accepted scalar, collection, map, and command-result returns have VM and Bash
  parity coverage;
- rejected unsupported forms fail during `ds check` before backend-specific
  execution/emission;
- `docs/concept-map.md` can move Function return kinds from **Hell candidate** to
  **Watch** with a narrowed remaining-risk explanation.
