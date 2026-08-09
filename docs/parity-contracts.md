# VM/Bash parity contracts

This document defines the backend parity contract for `ds`. It is a maintenance
and refactoring guide, not a feature roadmap. Its purpose is to make VM/Bash
parity a first-class language acceptance rule instead of a Bash-emitter detail.

Use this with:

- `docs/architecture.md` for the end-to-end pipeline and HIR rule.
- `docs/source-map.md` for file-level ownership.
- `docs/diagnostics.md` for phase ownership of errors and warnings.
- `docs/concept-map.md` for cross-cutting concept ownership.
- `docs/language.ds` for user-facing syntax status.
- `docs/status.md` and milestone specs for the currently supported surface.

## Core parity rule

A user-facing language feature should only be accepted when it has a
backend-neutral representation and defined behavior for both VM execution and
standalone Bash emission.

If a feature does not satisfy that rule, it must be one of these explicit states:

- **VM-only**: accepted only for VM execution, documented as VM-only, and rejected
  or diagnosed for `ds emit bash`.
- **Bash-only**: accepted only for Bash emission, documented as Bash-only, and
  rejected or diagnosed for VM execution.
- **Diagnostic-only**: parsed or recognized only so `ds` can produce a clear
  diagnostic; it does not execute in either backend.
- **Currently rejected**: not part of the accepted language surface. The parser or
  lowerer rejects it before either backend receives it.

The default state for new user-facing syntax or semantics is **currently
rejected** until the feature has a backend-neutral representation and both
backend behaviors are specified.

## What parity means

VM/Bash parity means the same accepted `ds` program has the same user-observable
behavior when run through:

```sh
ds ./script.ds [args...]
ds emit bash ./script.ds -o ./script.sh
bash ./script.sh [args...]
```

Observable behavior includes:

- stdout and stderr content, except for explicitly documented debug/tracing
  surfaces;
- process exit status;
- command argv construction and word boundaries;
- command-result fields such as stdout, stderr, and status/code;
- file, directory, environment, and process side effects within the documented
  semantics;
- trap/defer/signal ordering where supported;
- diagnostics for rejected programs, including which phase rejects them.

Parity does not require identical internal implementation. The VM may use C
runtime helpers and runtime values. Emitted Bash may use shell syntax and embedded
`__ds_` helpers. Those implementations are valid only when they implement the
same HIR-defined behavior.

## Phase ownership

### Parser

Owns syntax recognition and syntax-only diagnostics.

Parser responsibilities:

- build AST shapes that preserve enough source information for lowering,
  formatter/debug output, and diagnostics;
- reject malformed syntax when the grammar itself cannot represent it;
- avoid deciding backend behavior, runtime value kinds, Bash quoting rules, or VM
  process behavior.

Parser should not accept a construct merely because one backend can execute it.

### Lowerer

Owns semantic language acceptance.

Lowerer responsibilities:

- turn AST into backend-neutral HIR;
- validate names, arity, value kinds, assignment targets, command word segments,
  handler legality, and feature-specific restrictions;
- reject forms that do not have a defined parity contract;
- mark explicitly documented VM-only, Bash-only, diagnostic-only, or rejected
  states before backend execution.

The lowerer is the normal owner for unsupported-form diagnostics because it has
both syntax and semantic context.

### HIR

Owns the backend-facing contract.

HIR responsibilities:

- represent accepted language behavior in backend-neutral structures;
- carry source spans and semantic metadata needed by both backends;
- expose shared command, interpolation, stdlib, handler, collection, regex, and
  value-kind facts without embedding VM registers or Bash text.

If behavior is not represented in HIR or shared metadata consumed by HIR, it is
not part of the accepted portable language.

### VM

Owns direct execution of accepted HIR.

VM responsibilities:

- execute HIR/bytecode according to the language contract;
- implement runtime values, scopes, function calls, command execution,
  command-result capture, traps/defers/signals, stdlib calls, and OS-facing
  effects for VM mode;
- report runtime and OS failures that cannot be known during lowering.

VM code should not reinterpret parser syntax or silently accept behavior that the
Bash backend cannot represent unless the feature is explicitly documented as
VM-only.

### Bash emitter

Owns standalone Bash rendering of accepted HIR.

Bash emitter responsibilities:

- render HIR into standalone Bash with no dependency on `ds` or the C runtime;
- select embedded helpers required to implement accepted HIR behavior;
- preserve command word boundaries, quoting, status behavior, helper semantics,
  and side effects required by the contract;
- report only impossible/backend emission failures for already accepted HIR.

The Bash emitter should not become a second semantic checker. If it discovers an
unsupported language form, prefer moving the rejection to lowering and adding a
diagnostic test.

### Diagnostics

Owns phase-appropriate user-facing rejection. The detailed ownership model lives
in `docs/diagnostics.md`.

Diagnostic responsibilities:

- parser diagnostics for malformed syntax;
- lowerer diagnostics for semantically invalid or parity-unsupported language
  forms;
- VM diagnostics for runtime/OS failures in VM execution;
- Bash emitter diagnostics only for emission failures that cannot be expressed as
  normal language validation;
- diagnostic tests for unsupported forms so unsupported behavior does not drift
  into one backend by accident.

### Tests

Own proof of the parity contract.

Required test categories for an accepted feature:

- VM execution tests for direct `ds` behavior;
- Bash emission/parity tests for the same source program through emitted Bash;
- diagnostic tests for unsupported, rejected, or backend-specific forms;
- source/formatter/checker tests when syntax, formatting, or static checking is
  part of the user-facing contract.

A feature is not fully accepted until both VM and Bash observable behavior are
covered, unless it is explicitly documented as VM-only, Bash-only,
diagnostic-only, or currently rejected.

## Concept-specific parity contracts

### Function returns

Focused maintenance contract: see
`docs/maintenance/m3-2-function-return-kinds.md`.

Canonical representation: function metadata plus HIR return statements and value
kind facts.

Validation owner: lowerer and function collection. The lowerer decides whether a
function can be called as a statement, called as an expression, return a scalar,
return a command-result value, return collections, or be rejected.

Execution owner: VM scope/function-call execution and Bash function emission.
Both backends must agree on call form, default arguments, return value encoding,
stdout behavior, and invalid return diagnostics.

Function metadata carries inferred/defaulted scalar parameter kinds. The
lowerer owns inference and call-site validation; VM execution and
emitted Bash consume the same metadata and keep defensive runtime checks for
inferred/defaulted string/int/bool parameters. Do not accept a new parameter
kind or public typed-parameter syntax by changing only one backend.

Tests: VM function tests, emitted-Bash parity tests, and diagnostics for invalid
return positions, unsupported return kinds, and expression/statement call misuse.

Current maintenance rule: do not add a new return kind by teaching only VM or
only Bash how to pass it. Add the HIR/value-kind contract first or reject it.
Structured return payloads must have a portable backend representation before
the function is accepted: literals, named values, `run` captures, and forwarded
user-function calls are the current supported shapes. Other temporary structured
values, such as direct stdlib array calls, are rejected by lowering until a
portable VM/Bash return ABI exists.

### Command-result functions

Canonical representation: shared stdlib/helper metadata plus HIR call
expressions/statements that describe command-result behavior.

Validation owner: lowerer through stdlib metadata. It owns arity, statement-only
versus expression-capable status, return kind, string argument restrictions, and
field validity.

Execution owner: VM stdlib/process code and Bash helpers/native shell constructs.
Both backends must agree on stdout/stderr/status capture, non-fatal captured
non-zero exits, and field access semantics.

Tests: VM command-result tests, Bash parity tests, wrong-arity diagnostics,
invalid-field diagnostics, and captured non-zero status tests.

Current maintenance rule: command-result helper semantics must live in shared
metadata/HIR first. Backend helper names or VM helper functions are
implementations, not the source of truth. Field access on a command-result
value currently requires a named binding; direct field access on a temporary
function call is rejected by lowering because Bash does not yet have a
backend-neutral temporary representation for it.

### Command words and interpolation

Canonical representation: command AST payload lowered to HIR command word
segments, including literal parts, variable substitutions, and supported
interpolation forms.

Validation owner: parser for command syntax shape; lowerer for word segment
legality, expression eligibility, and unsupported interpolation forms.

Execution owner: VM process execution for argv construction and Bash command
emission/quoting for standalone scripts.

Tests: VM command argv tests, Bash parity tests that prove word boundaries and
quoting, and diagnostics for unsupported command interpolation or return kinds.

Current maintenance rule: command words are portable only when the HIR segment
model preserves enough information for both VM argv construction and Bash
quoting. Do not encode command semantics as backend-specific strings. The
current direct function-call interpolation boundary is documented in
`docs/maintenance/m3-4-command-words-interpolation-boundary.md`: scalar
function calls and scalar string method chains in quoted command-word
interpolation are accepted, and they are portable because lowering
pre-materializes runtime-work expressions into private string bindings before
either backend sees the command. Flat named collection index
reads in command-word interpolation (`{items[0]}` / `{map[key]}`) because
lowering validates the named collection and index/key shape while VM/Bash render
the accepted read through the same collection access helpers. Other direct
command-word interpolation forms remain rejected until they have an equally
backend-neutral representation.

### Pipelines

Canonical representation: HIR command/pipeline statements with stage metadata,
redirections, and status rules.

Validation owner: parser for pipeline syntax; lowerer for semantic restrictions,
command-stage legality, and unsupported combinations.

Execution owner: VM process/pipeline execution and Bash pipeline rendering.
Both backends must agree on stdout/stderr flow, exit status, signal cleanup, and
interaction with command-result capture where supported.

Tests: VM pipeline tests, Bash parity tests for output/status/redirection,
deterministic signal/cleanup tests where relevant, and diagnostics for
unsupported pipeline forms.

Current maintenance rule: pipeline behavior belongs in HIR/process semantics, not
as a Bash syntax shortcut.

### Trap/defer/signal behavior

Canonical representation: HIR handler declarations/tables with supported signal,
defer, ordering, and scope metadata.

Validation owner: parser for handler syntax; lowerer for supported signal names,
handler placement, context availability, and unsupported forms.

Execution owner: VM signal/defer runtime and Bash `trap`/helper emission. Both
backends must agree on supported signal delivery, foreground command cleanup,
LIFO defer order, trap replacement behavior, and any documented omissions.

Tests: deterministic VM/Bash signal harness tests, defer/trap ordering tests,
foreground command/pipeline cleanup parity tests, and diagnostics for unsupported
signals or handler contexts.

Current maintenance rule: if a signal or handler context cannot be made portable,
it stays rejected or explicitly documented as backend-specific before either
backend grows ad hoc behavior.

### Collections

Canonical representation: HIR collection literals, index expressions, explicit
flat index-assignment statements, iterable metadata, and runtime value kinds for
arrays/maps.

Validation owner: parser for literal/index/assignment syntax shape; lowerer for
element/key rules, iterability, duplicate/invalid keys, accepted mutation
support, same-map iteration mutation rejection, and unsupported assignment forms.

Execution owner: VM runtime values and Bash collection encodings/helpers. Both
backends must agree on construction, indexing, iteration order where specified,
mutation semantics where supported, and error behavior.

Tests: VM collection tests, Bash parity tests, diagnostics for unsupported
iteration/index-assignment forms, and edge cases for keys, bounds, and value
kinds.

Current maintenance rule: mutable collection features require explicit HIR
assignment/iteration nodes before implementation. Bash helper sidecars and VM
runtime containers are not the canonical semantics. The accepted mutation
surface is named
flat `array[index] = scalar` and `map[key] = scalar` mutation. Arrays replace
existing in-bounds elements; maps insert/replace non-empty string keys. Nested
mutation, field-style map assignment, temporary/function-result targets, sparse
arrays, deletion, aliases/references, and compound index assignment remain
rejected by lowering. Collection binding copies such as `let b = a` are value
copies, not aliases; emitted Bash must copy both the array/map payload and the
sidecar kind metadata so later mutation of `b` does not mutate `a`. Field/index reads currently require named collection
storage; indexing array literals or function-call collection results directly is
rejected by lowering until HIR/Bash have a portable temporary collection
representation. Collection index expressions are also limited to literals or
named variables; computed indexes must be bound to a variable first so the
lowerer, not the Bash emitter, owns the unsupported-form diagnostic. Array
iteration currently requires a named array or a known stdlib
array result; looping over array literals or user-function array calls directly
is rejected by lowering until there is a portable temporary iterable
representation shared by VM and Bash emission. Map iteration is accepted only
through the explicit two-name HIR loop over named maps or supported flat
map-returning user-function calls, and both VM and Bash must visit keys in
ascending bytewise/ASCII order.

### Regex

Canonical representation: HIR regex operator/helper metadata with conservative
literal flags, accepted runtime string patterns, flat match-result maps, global
replacement strings, and explicit unsupported states for split and richer regex
APIs.

Validation owner: parser for regex literal shape; lowerer for supported regex
surface, literal/direct string patterns, literal flags/replacements, operand
kinds, and rejected advanced forms. VM/Bash helpers own dynamic pattern, flag,
capture-count, replacement, and zero-length replacement-match runtime data
diagnostics.

Execution owner: VM regex runtime and Bash regex emission/helpers. Both backends
must agree on match truthiness, case sensitivity, unsupported pattern forms,
capture numbering/empty-capture behavior, replacement expansion, stdout/stderr,
exit status, and side effects.

Tests: VM regex tests, Bash parity tests over conservative patterns and runtime
string patterns, diagnostics for unsupported flags/forms, capture-map shape,
replacement expansion, and dynamic failure cases.

Current maintenance rule: regex expansion beyond the runtime string,
capture, and replacement surface remains rejected until it has an explicit
portable HIR/helper contract and backend plan. See
`docs/maintenance/m3-5-regex-boundary.md` for the regex maintenance boundary and
test plan.

### Globs

Canonical representation: stdlib metadata plus HIR iterable/string results for
supported `glob` and `glob!` forms.

Validation owner: lowerer and stdlib validation for arity, pattern argument
rules, literal recursive `**` segment validity, and iterable eligibility.

Execution owner: VM stdlib glob implementation and Bash glob helpers/emission.
Both backends must agree on match ordering, no-match behavior, hidden-file rules,
single-`**` zero-or-more directory semantics, no directory-symlink traversal, and
dynamic recursive-pattern rejection/support.

Tests: VM glob tests, Bash parity tests with controlled fixtures, sorted output
or documented ordering tests, and diagnostics for invalid literal and dynamic
recursive patterns.

Current maintenance rule: additional glob power beyond the scoped single
recursive segment, such as custom flags, multiple `**` segments, hidden traversal
flags, or symlink following, is not accepted until VM and Bash matching semantics
can be made observably equivalent or the difference is documented and tested.

### Recursive filesystem walk helpers

Canonical representation: stdlib metadata plus HIR string-array return metadata
for `dir.walk`, `dir.walk!`, `dir.walk_ext`, and `dir.walk_ext!`. No new HIR node
or public syntax is required.

Validation owner: parser for ordinary call syntax including the existing bang
helper shape, lowerer/stdlib validation for arity and static argument kinds, and
lowerer validation for literal extension arrays. Literal extension filters must
be non-empty strings beginning with `.`, must not contain `/`, and must reject
glob-like values such as `*.c` with a helper-focused diagnostic.

Execution owner: VM stdlib walk implementation and Bash walk helpers/emission.
Both backends must agree on root validation, required-match failures, empty
non-bang results, hidden-descendant skipping, symlink skipping, exact extension
filtering, bytewise sorting, duplicate removal, and composition with normal
string-array loops/indexing/membership/function returns.

Tests: VM/Bash parity tests with controlled directory fixtures, hidden files and
directories, symlinks, duplicate extension filters, invalid literal and dynamic
extension values, invalid roots, direct helper indexing, and row-array
composition. The implementation pass may use manual smoke coverage when the
milestone is explicitly requested without adding the dedicated tests.

Current maintenance rule: hidden traversal flags, symlink following, max-depth
options, metadata rows, streaming iterators, predicate callbacks, and glob-style
extension filters remain deferred until they have explicit VM/Bash parity
contracts and tests.

### Environment variables

Canonical representation: stdlib metadata for helper-style env operations and,
for direct `env.NAME` access/assignment, explicit HIR env read/write nodes before
acceptance.

Validation owner: lowerer for env names, assignment target legality, value kind,
process-local mutation semantics, and unsupported shorthand forms.

Execution owner: VM environment runtime/process spawning and Bash environment
assignment/expansion. Both backends must agree on missing-variable behavior,
mutation scope, child-process environment inheritance, and quoting.

Tests: VM env tests, Bash parity tests, diagnostics for invalid names or
unsupported assignment forms, and process-local mutation tests.

Current maintenance rule: direct env syntax must not be implemented as parser or
backend sugar alone. It needs HIR ownership or it remains rejected/deferred.

### Diagnostics

Canonical representation: `DsDiag` with phase-owned source spans and messages.
Diagnostics are observable behavior for rejected programs.

Validation owner: the phase that owns the rule being checked. Parser owns syntax
errors; lowerer owns semantic/parity rejection; VM owns runtime/OS failures; Bash
emitter owns only emission impossibilities.

Execution owner: diagnostic rendering/source manager plus the phase that emits
the diagnostic.

Tests: diagnostic tests for each rejected or unsupported form, especially when a
feature is recognized but not accepted. Parity tests should not rely on backend
crashes or emitter failures to prove rejection.

Current maintenance rule: unsupported portable-language forms should be rejected
before backend execution whenever possible. A backend-only failure for a normal
language form is a sign the parity contract is missing or misplaced.
