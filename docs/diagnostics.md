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
- `src/checker.c`: AST-level warnings for `ds check`, such as unused/shadowed
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
- `src/diag.c`: diagnostic formatting and source-location rendering only.

`src/diag.c` owns how diagnostics are displayed. It does not own which language
rules are errors.

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

`src/checker.c` owns AST-level warnings used by `ds check`, such as unused or
shadowed declarations. These warnings may help the user, but they must not become
a second semantic validator for hard errors that belong in lowering.

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
- unsupported syntax that has an AST shape but no accepted semantics;
- unsupported VM/Bash parity forms, including forms that one backend could
  execute but the other cannot represent;
- invalid command-result fields, command-word interpolation forms, collection
  access forms, regex/glob/env restrictions, trap/defer/signal restrictions, and
  unsupported mutable collection forms.

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
  pressure;
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
  Bash quoting; unsupported expression/return forms should continue moving into
  lowerer-owned diagnostics;
- function return kinds still span function collection, call lowering, VM call
  execution, and Bash function emission;
- trap/defer/signal diagnostics span syntax, lowerer legality, VM runtime signal
  handling, and Bash `trap` behavior;
- regex expansion beyond conservative literals needs lowerer-owned rejection
  until captures/replacement/runtime regex strings have a portable HIR model;
- mutable collections and map iteration need explicit HIR nodes before runtime or
  Bash helpers become canonical semantics;
- backend files still contain defensive `unsupported` diagnostics; future
  implementation passes should classify each as either legitimate backend failure
  or a diagnostic that belongs in lowering.
