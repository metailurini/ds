# M3.4 — Command words and direct function-call interpolation boundary

This is a maintenance specification and test plan for command words, word
interpolation, and direct function-call interpolation in command words. It is not
an implementation change and does not expand the language surface.

Use this with:

- `docs/source-map.md` for file ownership;
- `docs/concept-map.md` for cross-cutting concept risk;
- `docs/parity-contracts.md` for VM/Bash acceptance rules;
- `docs/diagnostics.md` for phase-owned diagnostics;
- `docs/maintenance/m3-2-function-return-kinds.md` for value-returning function
  contracts;
- `docs/maintenance/m3-3-command-result-functions.md` for command-result return
  metadata;
- `docs/language.ds` and milestone specs for the user-facing syntax surface.

## Current risk

Command words are a Hell candidate because they cross almost every phase:

- parser command-mode token collection;
- AST/HIR command payloads using `DsCommand`, `DsCommandStage`, and `DsWord`;
- lowerer command-word validation, interpolation validation, temporary binding
  materialization, function call validation, field validation, and VM/Bash parity
  gates;
- VM argv construction, string interpolation, command-result field rendering,
  redirection rendering, and subprocess execution;
- Bash command emission, quoting, helper dependency selection, interpolation
  rendering, and generated-script argv behavior;
- checker/formatter scanning and source-preserving command word output;
- diagnostics for unknown command variables, unsupported suffixes, invalid
  interpolation forms, function-call interpolation return kinds, and backend
  defensive fallbacks;
- tests spread across early command milestones, command results, function returns,
  environment variables, and v0.27 scalar interpolation.

The dangerous failure mode is VM/Bash drift. The VM can often build argv by
rendering runtime values, while Bash needs a precise quoting and helper strategy.
Any command-word feature that is accepted without a backend-neutral normalized
representation can become a VM-only behavior or a Bash-emitter semantic check.

## Definitions

### Command word

A command word is one source word inside a plain command statement or captured
`run` command. It contributes exactly one argv element after DS interpolation and
variable expansion rules are applied, unless the word is part of syntax such as a
pipe or redirection operator.

Examples:

```ds
git status
printf "%s\n" "hello world"
deploy $target
run "printf" "%s\n" "{app}"
```

### Plain command word ownership

Plain command words own only shell-native command spelling and argv shape. They
must not become arbitrary expression syntax. In the current language:

- unquoted text is preserved as command text unless it matches a supported DS
  command variable or field form;
- `$name` passes a named scalar variable as one command argument;
- `name.field` and `$name.field` are supported only for validated command-result
  fields or direct `env.NAME` where currently documented;
- quoted command words use DS string interpolation rules and remain one argv
  element;
- arrays/maps are not passed directly as command arguments;
- collection access in command arguments is deferred and should be bound first;
- redirection targets use conservative string/interpolation handling.

### Word interpolation

Word interpolation means evaluating DS interpolation inside a quoted command word
or redirection target, such as:

```ds
echo "deploying {app}"
echo "status={result.status}"
run "printf" "%s\n" "{env.APP_ENV}"
```

Current supported interpolation is the documented scalar/string interpolation
surface: named variables, command-result fields through named bindings,
`env.NAME`, supported format specifiers, arithmetic interpolation where already
implemented, and scalar expression-backed function calls where v0.27 supports
them.

Unsupported interpolation forms are not command substitution. They must be
rejected before backend selection unless explicitly documented as backend-only or
currently rejected.

### Direct function-call interpolation in command words

Direct function-call interpolation means a quoted command word contains a
function call expression directly inside `{...}`, for example:

```ds
fn artifact() {
  return "app.tar.gz"
}

tar -tf "{artifact()}"
```

Current status:

- scalar value-returning function calls in quoted command words are supported by
  the v0.27 surface;
- they are lowered by pre-materializing the call into a private temporary string
  binding before the outer command runs;
- the command word is rewritten to use that private binding, so VM argv
  construction and Bash command emission consume ordinary validated command
  words;
- collection-returning, map-returning, command-result-returning, unknown,
  mixed-return, or stdout-producing functions are rejected;
- arbitrary command substitution, nested command execution semantics,
  function-call interpolation in unquoted command words, and expression forms
  beyond the documented interpolation grammar remain unsupported unless later
  specs define them.

This means direct scalar function-call interpolation is partially supported in a
narrow, pre-materialized, quoted-word form. It is not a general command-word
expression system.

## Current implementation trace

### Parser

Relevant files:

- `src/parse_command.c`
- `src/parse_expr.c`
- `src/parse_stmt.c`
- `src/ds_command.h`
- `src/ds_ast.h`

Current behavior:

- Collects command-mode words into `DsWord` values inside `DsCommandStage`.
- Preserves word text and source spans instead of parsing each command word as a
  normal expression.
- Parses quoted strings as word text in command mode.
- Parses captured `run` commands and plain command statements into command AST
  payloads.
- Does not know whether `$name`, `name.field`, `{name}`, or `{call()}` is
  semantically valid.

Ownership rule:

- Parser owns command syntax shape: stages, pipes, redirections, word boundaries,
  and malformed command syntax.
- Parser must not own command variable resolution, command-result field validity,
  function return-kind checks, VM/Bash portability, or interpolation expression
  eligibility.

### AST and command payloads

Relevant files:

- `src/ds_command.h`
- `src/ds_ast.h`
- `src/command.c`
- `src/ast.c`
- `src/format.c`

Current behavior:

- `DsCommand` and `DsWord` are shared syntax payloads used by AST and HIR.
- Formatter and AST debug output preserve the word text instead of normalizing it
  into expression segments.
- The payload is syntax-preserving and span-carrying, not a semantic argv model.

Ownership rule:

- AST/command payloads own source-preserving command shape.
- They do not own the canonical accepted command-word semantics.

### HIR/lowering

Relevant files:

- `src/lower_expr.c`
- `src/lower_stmt.c`
- `src/lower_functions.c`
- `src/lower_internal.h`
- `src/ds_hir.h`

Current behavior:

- `validate_cmd_word()` owns command-word legality for `$name`, command-result
  field words, direct `env.NAME`, collection rejection, and command-variable
  diagnostics.
- `validate_interpolation()` owns quoted interpolation validation for named
  values, command-result fields, environment fields, format specifiers,
  arithmetic-like interpolation, and unsupported direct function-call forms.
- `lower_materialize_command_interpolation()` detects quoted command words and
  redirection targets that contain expression-call interpolation, lowers them as
  private temporary string bindings, and rewrites the command word to `$tmp` or
  the redirection target to `"{tmp}"`.
- Function-call interpolation relies on function return-kind metadata from the
  M3.2 contract and command-result metadata from the M3.3 contract.
- The lowered command payload currently remains `DsCommand`/`DsWord` rather than
  a fully segmented argv-expression HIR.

Ownership rule:

- Lowerer owns semantic acceptance of command words and interpolation.
- Lowerer owns unsupported-form diagnostics and VM/Bash parity gates.
- Lowerer owns the canonical normalized representation for the current surface:
  either validated raw command words, or pre-materialized private bindings for
  supported direct scalar function-call interpolation.
- Lowerer must reject forms that would require VM/Bash-specific command-word
  interpretation.

### Checker and formatter

Relevant files:

- `src/checker.c`
- `src/format.c`

Current behavior:

- Checker scans command words for variable-use warnings and must not become a
  semantic validator.
- Formatter prints command words and redirections from AST command payloads.

Ownership rule:

- Checker owns warnings only.
- Formatter owns presentation only.
- Neither should decide command-word language acceptance.

### VM execution

Relevant files:

- `src/vm_compile.c`
- `src/vm_process.c`
- `src/vm.c`

Current behavior:

- VM compile copies lowered command words into command instructions.
- `vm_process.c` renders each word to an argv element at runtime.
- Quoted words are decoded and interpolated.
- `$name` words look up runtime variables and stringify scalar values.
- `name.field` / command-result fields read runtime command-result values.
- VM owns process execution, redirection setup, captured stdout/stderr/status,
  and OS/runtime diagnostics.

Ownership rule:

- VM owns argv construction for already accepted HIR command words.
- VM must not be the first owner of unsupported source-language command-word or
  interpolation diagnostics.
- VM fallback interpolation errors should be runtime or internal defensive checks
  for accepted HIR, not source-language feature gates.

### Bash emission

Relevant files:

- `src/bash_command.c`
- `src/bash_quote.c`
- `src/bash_stmt.c`
- `src/bash_expr.c`
- `src/bash_deps.c`
- `src/bash_helpers.c`

Current behavior:

- `bash_command.c` renders command words and captured command argv.
- `bash_quote.c` renders quoted interpolation, formatting, and arithmetic
  interpolation into standalone Bash syntax/helpers.
- `bash_deps.c` selects helpers required by interpolation and command rendering.
- Bash emission depends on lowerer pre-materialization for direct scalar
  function-call interpolation inside command words.

Ownership rule:

- Bash owns quoting and standalone script rendering for already accepted HIR.
- Bash may diagnose output/artifact failures and internal invariant failures.
- Bash must not rediscover command-word semantic acceptance or reject a
  user-facing command word that VM accepted unless the form is explicitly
  documented as Bash-only/VM-only or has already failed lowering.

### Diagnostics

Current diagnostic ownership:

- Parser diagnostics: malformed command syntax, malformed pipe/redirection shape,
  and parse recovery.
- Lowerer diagnostics: unknown command variables, invalid command-result fields,
  invalid environment names, collection-as-command-argument rejection,
  unsupported suffixes, unsupported interpolation expressions, invalid format
  specifiers, non-scalar function-call interpolation, stdout-producing function
  calls in value interpolation, and VM/Bash parity gates.
- VM diagnostics: process spawn/failure diagnostics, redirection file failures,
  runtime environment/filesystem failures, and defensive interpolation failures
  for accepted HIR.
- Bash diagnostics: artifact-writing failures, Bash rendering invariant failures,
  and helper/runtime diagnostics in generated scripts for accepted HIR.
- Checker diagnostics: warnings only.

Any diagnostic that answers “is this command word part of the accepted portable
language?” belongs to lowering.

## Architectural failure

Command words currently use syntax-preserving `DsWord` strings as the cross-phase
payload. That is workable for the current surface, but it creates pressure for
VM and Bash to parse the same strings differently. Direct scalar function-call
interpolation is especially risky because the source text looks like expression
syntax inside a command word, while VM and Bash require different execution
strategies.

The v0.27 implementation avoids the worst drift by pre-materializing direct
scalar calls before the command runs, then rewriting the command to a normal
variable word. M3.4 should make that boundary explicit before future work expands
command words.

## Desired ownership

- Parser: command syntax shape and source spans only.
- AST/command payloads: source-preserving command word text and command structure.
- Lowerer: all command-word semantics, interpolation eligibility, temporary
  binding materialization, value-kind checks, function-return checks, and parity
  gates.
- HIR: accepted command payload after lowerer normalization; today this may still
  be `DsCommand`/`DsWord`, but direct function-call interpolation must already be
  pre-materialized or rejected.
- VM: argv construction and process execution for accepted HIR.
- Bash emitter: quoting and standalone Bash rendering for accepted HIR.
- Diagnostics: syntax in parser, semantic/unsupported/parity in lowerer,
  runtime/emission/internal failures in VM/Bash.
- Tests: each supported command-word behavior needs VM and Bash parity coverage;
  unsupported forms need lowering diagnostic coverage.

## Canonical representation

Current canonical representation:

```txt
source command words
  -> parser `DsCommand` / `DsWord` syntax payload
  -> lowerer-validated `DsCommand` / `DsWord` HIR payload
  -> optional pre-command temporary string `let` bindings for direct scalar calls
  -> VM argv construction or Bash command emission
```

For M3.4 implementation cleanup, the canonical contract is:

- plain words remain literal command words;
- `$name` is a validated named scalar command argument;
- `name.field` / `$name.field` are validated field command words only when the
  receiver and field are portable;
- quoted words are validated interpolation strings and produce one argv element;
- supported direct scalar function-call interpolation must be normalized into a
  private string binding before command execution/emission;
- unsupported direct interpolation forms must not reach VM/Bash as source-level
  feature decisions.

This spec does not require introducing a new segmented command-word HIR node, but
future implementation may choose one if it is done as a behavior-preserving
refactor.

## Validation owner

Lowering owns validation for:

- unknown `$name` command variables;
- using functions as command variables;
- direct array/map command arguments;
- collection access in command arguments;
- command-result field command words;
- `env.NAME` command words and invalid environment names;
- quoted interpolation variable/field existence;
- interpolation format specifiers;
- scalar function-call interpolation eligibility;
- stdout-producing or non-value function-call interpolation rejection;
- collection/map/command-result-returning function-call interpolation rejection;
- malformed interpolation that parses into an unsupported expression-like form;
- VM/Bash parity gates for command words.

Parser owns only syntax forms that cannot produce a meaningful AST command
payload.

## VM/Bash parity contract

Accepted command-word behavior must match between VM execution and emitted Bash
for:

- argv boundaries;
- quoting and whitespace preservation;
- literal special characters;
- command-result field values;
- environment reads;
- redirection target rendering;
- stdout/stderr/exit status;
- failure ordering, especially when pre-materialized interpolation calls fail
  before the outer command launches.

Direct scalar function-call interpolation is parity-safe only because the lowerer
turns it into an ordinary private binding before the command runs. Future direct
interpolation features must either use an equally backend-neutral representation
or remain rejected.

## Refactor plan for the implementation pass

1. Audit `validate_cmd_word()`, `validate_interpolation()`, and
   `lower_materialize_command_interpolation()` for duplicated or misleading
   ownership.
2. Name the command-word contract in lowerer helpers so it is obvious which
   forms are validated raw words and which forms are normalized into temporaries.
3. Classify VM/Bash interpolation diagnostics as legitimate runtime/emission
   failures, defensive invariant checks, or lowerer-owned semantic diagnostics.
4. Move only obvious source-language rejections into lowering if any still occur
   first in VM/Bash.
5. Add comments around remaining VM/Bash defensive fallbacks explaining the
   lowerer rule that should prevent user-facing invalid forms.
6. Preserve the current pre-materialization strategy for scalar function calls;
   do not replace it with command substitution or direct Bash expression parsing.
7. Add focused tests only for ownership changes or unprotected parity risks.
8. Update docs only if implementation reveals this contract is inaccurate.

## Non-goals

M3.4 does not include:

- arbitrary expression syntax in command words;
- direct command substitution;
- unquoted function-call interpolation;
- collection, map, command-result, regex, job, handler, or stream interpolation;
- direct collection splatting into argv;
- direct command-word access to array/map indexes;
- command parser rewrite;
- full segmented command-word HIR redesign;
- Bash emitter rewrite;
- new function return kinds;
- command-word pipeline or signal redesign;
- changing user-facing syntax beyond correcting documented inconsistencies.

## Test plan

The implementation pass should preserve and, where gaps exist, add focused tests
for the following cases.

### 1. Plain command words

- Literal words execute in VM and emitted Bash with identical stdout/stderr/exit
  status.
- Literal shell metacharacters that are part of DS command word text preserve the
  current documented behavior.
- Command names and arguments remain separate argv words.
- Empty command stages remain parser/lowerer diagnostics as currently owned.

### 2. Quoted command words

- Quoted words with spaces produce one argv argument in VM and Bash.
- Quoted words containing shell-special text such as `$(...)`, backticks,
  semicolons, glob-like characters, and brackets do not execute or split.
- Quoted redirection targets render consistently.

### 3. Variable interpolation and command variables

- `$name` passes one scalar argument.
- Unknown `$missing` is rejected by lowering for `check`, `run`, and `emit bash`.
- Function names used as `$name` are rejected as variables.
- Direct array/map variables in command arguments are rejected with the existing
  collection diagnostic.
- Collection access forms in command arguments remain rejected until a portable
  argv representation exists.

### 4. Command-result and environment command words

- Named command-result field command words such as `result.stdout` or
  `$result.stdout`, where currently supported, match VM/Bash behavior.
- Unknown command-result fields are rejected in lowering.
- Field interpolation on non-command-result values is rejected in lowering.
- `env.NAME` in quoted interpolation and command words follows current v0.27
  behavior and invalid names fail before backend selection.

### 5. Direct scalar function-call interpolation in quoted command words

- A scalar string-returning function interpolated as `"{name()}"` produces one
  argv argument in VM and Bash.
- Int and bool scalar returns stringify consistently if currently supported.
- Returned spaces remain one argv argument.
- Literal shell-special content returned by the function is data, not shell code.
- Multiple interpolated calls in one command run left-to-right before the outer
  command launches.
- Failure in an interpolated function prevents the outer command from launching
  in both VM and Bash.
- Private temporary names do not leak to user scope and avoid collisions with
  user declarations.
- Helper emission for Bash interpolation remains standalone and emitted at most
  as needed.

### 6. Unsupported direct interpolation forms

- Collection-returning function interpolation is rejected in lowering.
- Map-returning function interpolation is rejected in lowering.
- Command-result-returning function interpolation is rejected in lowering.
- Mixed or unknown return-kind function interpolation is rejected in lowering.
- Functions containing plain command statements are rejected in value
  interpolation through the function return/value contract.
- Unquoted direct function-call interpolation remains rejected or unsupported
  according to the documented surface.
- Nested or arbitrary expression interpolation forms that exceed the documented
  grammar are rejected before backend selection.

### 7. Invalid command-word expressions

- Malformed interpolation such as `{name(}` reports a clear diagnostic.
- Unsupported suffixes on command variables report the lowerer diagnostic.
- Invalid command-result/environment field names report source-located
  diagnostics.
- Redirection interpolation follows the same supported/rejected boundary as
  quoted command words where current behavior requires it.

### 8. VM/Bash parity cases

For every accepted behavior above, assert:

- `ds check` succeeds;
- `ds emit bash` succeeds;
- emitted Bash passes `bash -n`;
- `ds run` and the emitted Bash script match stdout;
- stderr and exit status match when behavior is deterministic;
- generated Bash remains standalone and does not call `ds`.

### 9. Diagnostic ownership cases

For every unsupported behavior above, assert:

- `ds check` rejects;
- `ds run` rejects before executing the outer command where applicable;
- `ds emit bash` rejects with the same lowerer-owned diagnostic;
- Bash emitter and VM do not become the first owner of the source-language
  rejection.

### 10. Regression coverage

Retain coverage from earlier milestones for:

- command variable tokenization and AST command word boundaries;
- quoted command argument parsing;
- captured command arguments preserving shell-special data;
- command-result field use;
- collection command-argument rejections;
- scalar function returns;
- command-result function returns;
- v0.27 direct scalar function-call interpolation;
- formatter preserving command words.

## Exit criteria

M3.4 implementation cleanup is complete when:

- command-word and interpolation ownership is explicit in lowerer helper names or
  comments;
- unsupported command-word/interpolation forms are rejected by lowering, not by
  VM/Bash backend fallback diagnostics;
- VM and Bash consume already accepted/normalized command payloads;
- direct scalar function-call interpolation remains pre-materialized, not
  backend-specific command substitution;
- focused tests protect VM behavior, Bash parity, and diagnostics for any moved
  ownership;
- no new command-word feature is added;
- `docs/concept-map.md` can move command words/direct function-call
  interpolation from Hell candidate to Watch only after implementation proves the
  boundary in code.

## Implementation result

M3.4 cleanup implemented the documented boundary without adding command-word
features:

- lowerer command-word validators are now named as lowerer-owned contract checks;
- direct scalar function-call interpolation remains normalized through private
  temporary string bindings before VM/Bash command execution;
- malformed arithmetic interpolation in command words is rejected by lowering
  instead of surfacing first from VM/Bash interpolation fallbacks;
- VM and Bash interpolation fallbacks are documented in code as defensive
  backend invariants for accepted HIR, not source-language semantic owners.

Command words and direct scalar function-call interpolation can move to Watch,
not Clear. Remaining drift risk belongs to future expansion areas: arbitrary
command-word expressions, command substitution, collection/command-result
interpolation, and a possible segmented command-word HIR redesign.
