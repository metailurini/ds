# Diagnostic ownership

This document defines which layer owns each kind of user-facing diagnostic in
`ds`. It is a maintenance/refactoring contract, not an implementation change.
The goal is to keep diagnostics aligned with the VM/Bash parity contract so
unsupported language behavior is rejected before backend selection instead of
being rediscovered by the VM or Bash emitter.

Use this with:

- `docs/parity-contracts.md` for the backend-neutral acceptance rule.
- `docs/source-map.md` for file-level ownership.
- `docs/concept-map.md` for cross-cutting concept homes.
- `docs/architecture.md` for the parse -> lower -> backend pipeline.
- `docs/language.ds` and milestone specs for the supported syntax surface.

## Core rule

A diagnostic belongs to the earliest phase that has enough information to reject
or warn about the program without guessing backend behavior.

That means:

- syntax shape errors belong to lexer/parser;
- semantic language errors belong to lowering;
- unsupported portable-language forms belong to lowering unless they are pure
  syntax errors;
- VM runtime failures belong to the VM;
- Bash emission failures belong to the Bash emitter only when the already
  accepted HIR cannot be rendered because of an output, environment, or internal
  emission problem;
- checker warnings belong to the checker and must not become hard semantic
  validation.

If a diagnostic would affect whether the program is accepted by both `ds run` and
`ds emit bash`, it normally must happen before backend selection.

## Current diagnostic-producing areas

The current source tree emits diagnostics from several layers:

- `src/lexer.c`: lexical errors and token source spans.
- `src/parse_*.c` and `src/parser.c`: malformed syntax and parse-shape errors.
- `src/lower_*.c`: semantic validation, unsupported forms, parity gates, symbol
  and value-kind checks, function/handler/test lowering checks.
- `src/ds_checker.c`: AST-level warnings for `ds check`, such as unused/shadowed
  declarations.
- `src/vm*.c`: VM runtime failures, script-argument runtime binding failures,
  process/subprocess failures, stdlib runtime failures, and defensive runtime
  checks.
- `src/bash_*.c`: rendering/emission failures for already accepted HIR, output
  file failures, and defensive errors that should be considered ownership
  pressure if they reject a user-facing language form.
- `src/source.c` and `src/cli_program.c`: source loading, import resolution,
  filesystem, import-cycle, and program-composition diagnostics.
- `src/main.c`: command-line usage and top-level command dispatch diagnostics.
- `src/diag.c`: diagnostic formatting and source-location rendering for errors
  and warnings only.

`src/diag.c` owns how diagnostics are displayed. It does not own which language
rules are errors. Use `ds_diag_report(...)` for warning-style diagnostics that
need the same location/source-line rendering as errors.

## Phase ownership

### Lexer and parser: syntax diagnostics

Owns:

- invalid tokens, unterminated literals, malformed regex literal syntax, and
  tokenization errors;
- malformed expression, statement, command, function, test, script, import,
  trap, and defer syntax;
- parse-shape errors where there is no meaningful AST shape to lower.

Does not own:

- unknown variables or functions;
- stdlib helper arity and return-kind validation;
- command-result field validity;
- value-kind or type-like checks;
- VM/Bash portability decisions;
- runtime process failures.

Parser diagnostics should say what syntax was expected. They should not say what
one backend can or cannot execute.

### Lowerer: semantic and unsupported-feature diagnostics

Owns:

- name resolution, duplicate declarations, assignment target validity, script
  declaration validation, function call shape, return-kind legality, recursion
  restrictions, handler legality, loop source eligibility, and value-kind checks;
- stdlib helper existence, arity, statement-only/expression-capable status,
  return kind, argument restrictions, and env-name validation;
- command-result field validity and command-word interpolation eligibility;
- portable collection, regex, glob, command, pipeline, trap/defer/signal, and
  environment restrictions;
- unsupported forms that parse but are not accepted by the current language;
- parity gates required by `docs/parity-contracts.md`.

Does not own:

- tokenization or parse cursor recovery;
- VM bytecode instruction selection;
- Bash text rendering and quoting mechanics;
- OS errors that happen only when executing an accepted program.

The lowerer is the default owner for diagnostics that answer: "Is this a valid
`ds` program?" or "Is this accepted portable language behavior?"

### HIR: diagnostic metadata, not diagnostic policy

Owns:

- carrying source spans and backend-neutral metadata needed to report precise
  diagnostics in later phases;
- making accepted behavior explicit enough that VM and Bash do not rediscover
  language rules independently.

Does not own:

- deciding new diagnostics by itself;
- storing backend-specific error strings as semantics;
- representing rejected behavior.

If a backend needs to diagnose an accepted HIR node at runtime, HIR should carry
the span and metadata needed to make that diagnostic useful.

### VM: runtime and OS failure diagnostics

Owns:

- failures that cannot be known until VM execution, such as command exit status,
  subprocess spawn errors, redirection/file open failures, filesystem/env/glob
  runtime failures, invalid runtime script arguments, signal/process failures,
  and defensive runtime checks for corrupted or inconsistent HIR/bytecode state;
- VM-specific execution diagnostics for features explicitly documented as
  VM-only, if such features ever exist.

Does not own:

- accepting a source-language form because the VM can execute it;
- rejecting a portable unsupported form that the lowerer could have rejected;
- command syntax or AST validation.

A VM diagnostic is appropriate when the program was already accepted and the
failure depends on runtime state, OS state, process state, or user-provided CLI
arguments.

### Bash emitter: emission diagnostics only

Owns:

- output-file and artifact-writing failures;
- impossible rendering failures for already accepted HIR;
- defensive errors that indicate internal inconsistency between HIR and the Bash
  backend;
- Bash-only diagnostics only for features explicitly documented as Bash-only.

Does not own:

- normal semantic validation;
- deciding whether source syntax is part of the language;
- rejecting a user-facing form only because Bash support has not been designed.

If the Bash emitter rejects a user-facing language form that VM accepts, that is a
parity ownership bug. Prefer moving the diagnostic to lowering and adding a
diagnostic test.

### Checker and formatter: warnings and presentation

`src/ds_checker.c` owns AST-level warnings used by `ds check`, such as unused or
shadowed declarations. These warnings may help the user, but they must not become
a second semantic validator for hard errors that belong in lowering.

Checker warnings use the shared diagnostic renderer and a narrow `ds_checker.h`
entrypoint. The checker must not include the broad backend façade just to expose
its warning API.

`src/format.c` owns formatting output. Formatter diagnostics should be limited to
parse/format orchestration failures. Formatting policy should not change language
acceptance.

### CLI and source loading: orchestration diagnostics

CLI/program-loading code owns diagnostics that are outside the language grammar
itself:

- missing files and unreadable sources;
- import resolution failures;
- import cycles and invalid import composition;
- top-level command usage errors;
- output path errors that happen before or during artifact creation.

CLI diagnostics should orchestrate phases. They should not duplicate parser,
lowerer, VM, or Bash language diagnostics.

## Diagnostics before backend selection

These diagnostics must happen before VM/Bash backend selection whenever the
source can be parsed far enough to know them:

- lexical and syntax errors;
- unknown names, duplicate names, invalid assignment targets, and invalid control
  flow placement;
- helper/function arity, call form, statement-only/expression-only misuse, and
  return-kind errors;
- unsupported syntax that has no accepted AST/HIR shape, including collection
  assignment while index assignment and map field assignment are deferred;
- unsupported syntax that has an AST shape but no accepted semantics;
- unsupported VM/Bash parity forms, including forms that one backend could
  execute but the other cannot represent;
- invalid command-result fields, command-word interpolation forms, collection
  access/iteration forms, regex/glob/env restrictions, trap/defer/signal
  restrictions, and unsupported mutable collection forms.

A rejected program should fail consistently for `ds check`, `ds run`, and
`ds emit bash`, modulo command-line or file-loading errors that happen before the
program is available.

## Backend-specific diagnostics

Backend-specific diagnostics are allowed only when the program has already passed
frontend/lowering and one of these is true:

- the failure depends on VM runtime state, OS state, subprocess behavior, signal
  delivery, filesystem/environment state, or user script arguments;
- the failure depends on Bash artifact writing or an internal Bash emission
  invariant for accepted HIR;
- the behavior is explicitly documented as VM-only or Bash-only in
  `docs/parity-contracts.md` and the relevant language/status docs.

Backend-specific diagnostics are not allowed as a shortcut for unsupported
language features. If backend code contains a diagnostic like "unsupported X" for
a source-language construct, treat it as a candidate for a lowering diagnostic
unless it is defensive unreachable code or an explicitly backend-specific state.

Runtime/helper diagnostics may still reject unsupported values when those values
are not statically knowable. For example, literal recursive glob patterns and
literal empty `split`/`replace` arguments are lowerer diagnostics, while the same
values produced dynamically are VM/Bash-helper runtime data failures. Backend
wording should make that runtime/invariant ownership visible instead of sounding
like the backend is deciding language validity.

## Static vs dynamic runtime diagnostics

Some rules have two valid diagnostic owners depending on how much is known before
execution:

- Lowering owns statically-known helper argument kinds, helper arity, invalid
  environment names in string literals or direct `env.NAME` forms, literal empty
  `split`/`replace` separators, literal recursive glob patterns, known array
  index kinds, known `in` operands, known loop iterables, and known arithmetic
  operand kinds.
- VM/runtime and emitted Bash helpers own the same class of failure when the bad
  value is produced dynamically by an accepted program, such as script arguments,
  function returns, variables with unknown value kind, runtime environment names,
  runtime glob strings, or runtime string-helper separators.
- VM/Bash internal invariant diagnostics are for shapes that should be impossible
  after lowering, such as unknown helper/function targets, unsupported accepted
  interpolation shapes, or corrupted Bash function-return payloads.

Prefer wording such as "runtime ..." for dynamic value failures and "internal
VM/Bash invariant" for impossible accepted-HIR shapes. Do not move a runtime
data diagnostic into lowering unless the invalid value is reliably known before
execution.

## Relationship to VM/Bash parity

Diagnostics are part of the parity contract. For accepted features, VM execution
and emitted Bash should agree on observable behavior. For rejected features, the
project should also agree on the rejecting phase and diagnostic surface.

The key rule from `docs/parity-contracts.md` applies here:

> A user-facing language feature should only be accepted when it has a
> backend-neutral representation and defined behavior for both VM execution and
> standalone Bash emission, unless it is explicitly documented as VM-only,
> Bash-only, diagnostic-only, or currently rejected.

Therefore:

- parser rejection is for malformed syntax;
- lowerer rejection is for semantic invalidity and unsupported portable forms;
- VM/Bash rejection after lowering should be rare and treated as ownership
  pressure unless it is a runtime/data failure, artifact failure, or explicitly
  worded internal accepted-HIR invariant;
- tests should prove both accepted parity and rejected unsupported forms.

## Test ownership

Diagnostic tests should protect the phase boundary, not just the error string.

Minimum expectations:

- Syntax diagnostics: parser/CLI tests should cover malformed source and ensure
  no backend is required to discover the error.
- Semantic diagnostics: `ds check` or equivalent lowering-driven tests should
  cover invalid names, invalid calls, invalid return positions, invalid fields,
  invalid assignments, unsupported features, and parity gates.
- Runtime diagnostics: VM tests should cover failures that require execution,
  such as command failure policy, file/env/process failures, and script-argument
  runtime binding.
- Bash emission diagnostics: emit tests should cover artifact/output failures and
  any defensive accepted-HIR emission errors that remain.
- Parity diagnostics: when a form is rejected for portability, tests should prove
  that `check`, VM execution, and Bash emission all reject before backend-specific
  behavior can diverge.
- Warning diagnostics: checker tests should cover warnings separately from hard
  lowering errors.

When moving a diagnostic earlier, add or update the smallest test that proves the
new owner. Prefer one diagnostic test plus focused VM/Bash rejection or parity
coverage over a broad test-suite rewrite.

## Remaining ambiguities to resolve in implementation passes

These areas still need careful source review before behavior changes:

- command-word interpolation currently touches parser, lowerer, VM process, and
  Bash quoting. M3.4 specifies the intended boundary: parser owns command syntax
  shape, lowerer owns command-word/interpolation acceptance and
  pre-materialization of supported scalar function-call interpolation, and
  VM/Bash own execution/rendering of accepted command payloads. Unsupported
  expression/return forms should continue moving into lowerer-owned diagnostics;
- function return kinds still span function collection, call lowering, VM call
  execution, and Bash function emission;
- trap/defer/signal diagnostics span syntax, lowerer legality, VM runtime signal
  handling, and Bash `trap` behavior;
- regex expansion beyond conservative literals needs lowerer-owned rejection
  until captures/replacement/runtime regex strings have a portable HIR model;
  see `docs/maintenance/m3-5-regex-boundary.md` for the current boundary;
- mutable collections and map iteration need explicit HIR nodes before runtime or
  Bash helpers become canonical semantics;
- backend files still contain runtime/data-dependent `unsupported` diagnostics
  for dynamic values such as glob patterns and string helper separators; keep
  those in VM/Bash helpers, but word unreachable shape/name fallbacks as internal
  accepted-HIR invariants.
