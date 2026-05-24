# M3.5 — Regex captures, replacement, and runtime regex strings boundary

This is a maintenance specification and test plan for regex behavior in `ds`.
It does not implement new regex features. Its purpose is to keep the currently
supported conservative regex surface VM/Bash-parity-safe while making advanced
regex behavior clearly rejected until a portable representation exists.

Use this with:

- `docs/source-map.md` for file-level ownership;
- `docs/concept-map.md` for cross-cutting concept risk;
- `docs/parity-contracts.md` for VM/Bash acceptance rules;
- `docs/diagnostics.md` for phase-owned diagnostics;
- `docs/runtime.md` and `docs/language.ds` for the user-facing regex subset;
- `tests/v0_23/run.sh` for the current regex/range/membership coverage.

## Current risk

Regex is a Hell candidate because it can easily become backend-specific:

- the lexer has a special regex-literal mode after `matches`;
- AST and HIR carry regex literals as strings;
- the lowerer validates supported pattern syntax, operand kinds, flags, and
  unsupported forms;
- the VM uses the C POSIX regex runtime;
- Bash emission uses `[[ string =~ regex ]]` plus shell options such as
  `nocasematch`;
- Bash and POSIX regex dialects can differ in escaping, grouping, anchors,
  character classes, locale behavior, backreferences, and match side effects;
- capture groups are currently usable only as grouping in a predicate pattern,
  not as source-language capture values;
- regex replacement, split, and runtime-built patterns do not have a portable
  HIR or value model;
- if unsupported forms are accepted by one backend and rejected by another,
  Bash/VM parity becomes a backend accident instead of a language contract.

The safe maintenance direction is: keep `matches` with conservative regex
literals as the accepted surface, and keep captures-as-values, replacement,
splitting, and runtime regex strings explicitly rejected until a future feature
milestone defines canonical HIR/value semantics and parity tests.

## Current implementation trace

### Lexer / tokens

Relevant files:

- `src/lexer.c`
- `src/token.h`

Current role:

- Recognizes `matches` as a keyword/operator token.
- Enables regex-literal scanning only when the previous token requires a regex
  literal, currently after `matches`.
- Produces regex literal tokens of the form `/pattern/` or `/pattern/i`.
- Emits lexical diagnostics for unterminated and empty regex literals.

Current risk:

- Lexer-owned diagnostics must remain lexical only. It may reject malformed
  token shape, but it should not decide whether a regex construct is portable or
  semantically supported.

Desired boundary:

- Own tokenization and malformed literal shape.
- Do not own regex dialect policy, capture semantics, runtime-pattern legality,
  or VM/Bash parity.

### Parser / AST

Relevant files:

- `src/parse_expr.c`
- `src/ds_ast.h`
- `src/ast.c`
- `src/format.c`

Current role:

- Parses `matches` as a binary expression.
- Parses regex literal syntax as `DS_EXPR_REGEX`.
- Preserves raw regex literal text and source spans in AST.
- Formatter emits `matches` and regex literals as source syntax.

Current risk:

- Parser can see `matches`, but it does not know operand value kinds or backend
  parity constraints.
- Regex literals outside the accepted position may parse as syntax but should be
  rejected by lowering, not by parser semantics.

Desired boundary:

- Parser owns expression shape only: `left matches right`, literal payloads, and
  spans.
- Parser does not own supported regex dialect, runtime pattern rejection,
  capture access, replacement semantics, or match truthiness.

### HIR / lowering

Relevant files:

- `src/ds_hir.h`
- `src/lower_expr.c`
- `src/hir.c`

Current role:

- Represents accepted regex literals as `DS_LOWER_EXPR_REGEX`.
- Represents `matches` as a lowered binary expression returning `bool`.
- Validates that the left operand of `matches` is string-compatible.
- Validates that the right operand of `matches` is a regex literal, not a runtime
  string or arbitrary expression.
- Validates the supported regex literal surface:
  - `/pattern/`;
  - `/pattern/i` for case-insensitive matching;
  - conservative POSIX-like constructs used by the tests.
- Rejects unsupported flags and dangerous/nonportable constructs such as
  lookaround, inline flags, lazy quantifiers, Unicode classes, and backreference
  escapes.
- Rejects regex literals outside the right side of `matches`.

Current risk:

- This is the correct ownership layer, but the current representation is just a
  raw literal string. If future captures/replacement/runtime strings are added
  without changing HIR, backends may invent their own behavior.
- Grouping parentheses are currently allowed for match predicates, but the
  language does not expose capture values. Future capture APIs must not infer
  capture semantics from today’s predicate grouping support.

Desired boundary:

- Lowering owns regex semantic validation and parity gates.
- HIR must remain the accepted regex contract consumed by both VM and Bash.
- Unsupported advanced regex features should fail before backend selection.

### VM runtime

Relevant files:

- `src/vm_compile.c`
- `src/vm.c`
- `src/vm_dump.c`

Current role:

- Compiles lowered regex literals and `matches` expressions to bytecode.
- Executes matches with the C POSIX regex runtime.
- Applies case-insensitive matching through regex flags.
- Produces boolean match results.

Current risk:

- The VM can support regex runtime features that Bash cannot represent, or vice
  versa. VM runtime must not become the source-language owner for regex dialect
  expansion.
- VM errors such as invalid compiled regex should be defensive/backend runtime
  failures for already accepted HIR, not primary semantic validation.

Desired boundary:

- VM executes accepted lowered regex predicates.
- VM owns runtime/library failures only.
- VM does not add capture arrays, replacement, runtime patterns, or dialect
  extensions without lowerer/HIR support and Bash parity.

### Bash emission

Relevant files:

- `src/bash_expr.c`
- `src/bash_emit.c`

Current role:

- Emits accepted `matches` HIR using Bash `[[ string =~ regex ]]`.
- Quotes/escapes the regex literal into a generated Bash variable.
- Uses `nocasematch` around `/pattern/i` matches and restores the previous shell
  option state.
- Contains fallback diagnostics if a regex expression reaches emission in an
  unsupported shape.

Current risk:

- Bash regex rules are not identical to C POSIX regex rules in all edge cases.
- Bash exposes `BASH_REMATCH`, but `ds` does not currently expose capture values;
  Bash emission must not make capture semantics canonical accidentally.
- Bash fallback diagnostics must remain internal/invariant checks once lowering
  owns semantic acceptance.

Desired boundary:

- Bash emits accepted HIR only.
- Bash may own quoting and generated-shell mechanics.
- Bash must not become the owner of regex feature acceptance.

### Stdlib / string replacement

Relevant files:

- `src/stdlib.c`
- `src/vm_stdlib.c`
- `src/bash_helpers.c`
- `tests/v0_19/run.sh`

Current role:

- `string.replace(from, to)` is implemented as literal substring replacement,
  not regex replacement.
- Empty replacement source is rejected at runtime/helper level by existing string
  helper behavior.

Current risk:

- The presence of `replace` can be mistaken for regex replacement. It is not.
- Future regex replacement must not reuse string replacement semantics without a
  distinct regex contract.

Desired boundary:

- Keep substring replacement separate from regex.
- Any future regex replacement must get a new stdlib/helper contract, HIR/value
  representation, diagnostics, and VM/Bash parity tests.

### Diagnostics

Current role:

- Lexer diagnoses malformed literal token shape such as unterminated or empty
  regex literals.
- Lowerer diagnoses unsupported regex surface, wrong operand kinds, runtime
  patterns, unsupported flags, and regex literals outside `matches`.
- VM/Bash may emit defensive invalid-pattern or unsupported-shape diagnostics if
  accepted HIR violates an invariant.

Desired boundary:

- Syntax/token diagnostics: lexer/parser.
- Semantic/parity diagnostics: lowerer.
- Backend diagnostics: runtime/emission failures or internal invariant checks
  only.

### Docs

Relevant files:

- `docs/language.ds`
- `docs/runtime.md`
- `docs/status.md`
- `docs/parity-contracts.md`
- `docs/diagnostics.md`
- `docs/concept-map.md`

Current role:

- Document conservative regex `matches` as implemented in v0.23.0.
- Document capture values, regex replacement/splitting, runtime regex strings,
  and broad regex/glob case behavior as deferred.
- Document VM/Bash parity as the acceptance rule.

Current risk:

- Docs mention deferred regex concerns in several places. Without this spec,
  future work can interpret “regex” as a single feature and accidentally expand
  one backend.

Desired boundary:

- This spec is the maintenance home for regex expansion boundaries.
- User-facing docs should continue describing only the accepted surface.

### Tests

Relevant files:

- `tests/v0_23/run.sh`
- `tests/v0_19/run.sh` and `tests/v0_20/run.sh` for non-regex
  `string.replace` behavior.

Current role:

- Tests token/AST/HIR/bytecode shape for `matches` and regex literals.
- Tests VM/Bash parity for accepted conservative patterns, grouping, character
  classes, quantifiers, anchors, slash/backslash escaping, interpolation of bool
  match results, calls, case, and boolean composition.
- Tests diagnostics for non-string left operand, runtime pattern strings,
  unsupported flags, unterminated/empty literals, lookahead, backreferences, lazy
  quantifiers, Unicode classes, and newline literals.

Current risk:

- Current tests cover the existing boundary well, but implementation cleanup
  should add focused regression names around advanced regex rejection if any
  diagnostics move earlier or backend fallback messages are reworded.

## Currently supported behavior

Supported now:

- `string matches /pattern/` as a boolean predicate.
- `string matches /pattern/i` as a case-insensitive boolean predicate.
- Conservative regex literals that both VM POSIX regex and Bash `[[ =~ ]]` can
  evaluate consistently in the supported test environment.
- Regex literals only as the right operand of `matches`.
- Grouping parentheses in predicates for alternation/precedence, without
  exposing capture values.
- Boolean use of match results in `let`, `if`, `while`, `case`, function returns,
  interpolation, and function arguments through ordinary bool semantics.

## Unsupported / rejected behavior

Rejected or deferred now:

- runtime-built regex patterns, such as `let pat = "api"; x matches pat`;
- regex literals used as first-class values;
- capture group access or capture arrays;
- backreferences and replacement references;
- regex replacement/substitution APIs;
- regex split APIs;
- named captures;
- lookaround;
- inline flags other than trailing `/i`;
- lazy quantifiers;
- Unicode character classes such as `\p{L}`;
- multiline regex literals;
- exposing Bash `BASH_REMATCH` or VM `regmatch_t` as language values;
- accepting backend-specific regex dialect features unless both VM and Bash have
  a documented parity story.

These forms should remain rejected until a future feature milestone defines a
canonical representation and parity tests.

## Architectural failure to avoid

Regex becomes Hell when either backend owns expansion:

- VM accepts a C regex feature that Bash cannot emit;
- Bash accepts a `[[ =~ ]]` behavior that VM POSIX regex does not match;
- capture values exist in Bash via `BASH_REMATCH` but not in VM values;
- runtime strings are compiled dynamically in VM but quoted/emitted differently
  in Bash;
- string replacement is confused with regex replacement;
- unsupported constructs are rejected by backend fallbacks instead of the lowerer.

The fix is not to broaden regex now. The fix is to keep supported regex as an
accepted HIR predicate and keep advanced features explicitly rejected until their
runtime value model exists.

## Desired ownership

### Canonical representation owner

- HIR/lowering owns the canonical representation for accepted regex predicates:
  a `matches` expression with a string-compatible left operand and a validated
  regex literal right operand.
- Future capture/replacement/runtime-pattern support must introduce explicit HIR
  and value metadata before any backend accepts it.

### Validation owner

- Lexer/parser validate literal token shape and expression syntax.
- Lowerer validates operand kinds, allowed literal position, flags, supported
  regex surface, and all unsupported-feature diagnostics.

### VM execution owner

- VM executes accepted regex predicates with the runtime regex library.
- VM owns only backend runtime failures for accepted HIR.

### Bash emission owner

- Bash emitter renders accepted regex HIR into standalone shell code.
- Bash owns quoting, helper variables, `nocasematch` mechanics, and internal
  emission invariants.

### Diagnostics owner

- Lexer/parser: malformed literal syntax.
- Lowerer: runtime-string rejection, unsupported flags/forms, non-string left
  operand, non-literal right operand, literal outside `matches`, and future
  capture/replacement/runtime-pattern rejection.
- VM/Bash: internal invariant or backend runtime/emission failures only.

### Test owner

- `tests/v0_23/run.sh` remains the primary regex boundary suite.
- Future implementation cleanup should add tests only when diagnostics or
  backend fallback ownership changes.

## VM/Bash parity contract

Accepted regex programs must have the same observable behavior in VM execution
and emitted Bash:

- same boolean match result;
- same case-sensitive vs case-insensitive behavior;
- same stdout/stderr and exit status for scripts using match results;
- same rejection before backend selection for unsupported forms;
- no capture values observable in either backend;
- no backend-specific regex extension accepted by only one backend.

If a regex construct cannot be represented safely in both VM and Bash, it must
remain rejected by the lowerer or documented as explicitly backend-specific. No
currently supported regex feature is backend-specific.

## Maintenance direction

Regex match can move from vague Hell to Watch because the accepted surface is
already bounded: conservative literal `matches` with VM/Bash parity tests.

Regex captures/replacement/runtime regex strings should move to Clear while
rejected: they are not supported, they should remain rejected, and this spec
states what representation work is required before they can become features.

Remaining Watch risk:

- subtle regex dialect drift between C POSIX regex and Bash `[[ =~ ]]`;
- escaping/quoting regressions;
- future helper additions that blur substring replacement with regex
  replacement;
- future capture or runtime-pattern work bypassing HIR/lowering.

## Refactor / implementation plan

This spec does not require feature work. The implementation pass should be a
small audit and cleanup only:

1. Grep regex diagnostics in `src/lexer.c`, `src/lower_expr.c`, `src/vm.c`, and
   `src/bash_expr.c`.
2. Classify each diagnostic:
   - lexical syntax shape;
   - lowerer semantic/parity rejection;
   - backend runtime/emission invariant.
3. Move only obvious semantic regex diagnostics out of VM/Bash if they can still
   be reached by source programs.
4. Reword backend fallback diagnostics as internal invariants when lowering
   should prevent them.
5. Add focused tests only for changed diagnostics or newly protected rejection
   ownership.
6. Keep the accepted regex surface unchanged.

Likely touched files during implementation:

- `src/lower_expr.c` for semantic regex validation helpers;
- `src/bash_expr.c` for defensive fallback wording/comments;
- `src/vm.c` for defensive runtime invariant wording/comments;
- `tests/v0_23/run.sh` for focused diagnostic ownership tests;
- `docs/concept-map.md` only if implementation changes risk status further.

## Non-goals

Out of scope for this maintenance line:

- regex capture values;
- capture arrays or maps;
- named captures;
- backreferences;
- regex replacement/substitution;
- regex split;
- runtime-built regex patterns;
- first-class regex values;
- command-word regex interpolation;
- Bash-only or VM-only regex extensions;
- changing `string.replace` into regex replacement;
- switching regex engines;
- parser or Bash emitter rewrites.

## Test plan

### Shape and docs checks

- Token stream preserves `matches` and regex literal text.
- AST preserves `matches` as a binary expression and regex literal payload.
- HIR preserves accepted regex literal and bool result.
- Bytecode exposes regex match operation.
- Formatter preserves spacing around `matches` and regex literals.
- Docs mention conservative regex literals and deferred capture/replacement /
  runtime strings.

### Accepted VM/Bash parity cases

- Simple literal match.
- Anchored match.
- Alternation/grouping used only for predicate semantics.
- Character classes.
- Quantifiers supported by both backends.
- Slash escaping.
- Backslash escaping.
- Case-sensitive mismatch.
- Case-insensitive `/i` match.
- `nocasematch` state restored after `/i` emission.
- Match result used in:
  - `let` binding;
  - `if`;
  - `while` or loop conditions where currently supported;
  - `case` over bool;
  - interpolation as a bool;
  - function return;
  - function argument;
  - imported function call.

### Unsupported diagnostic cases

Each should fail at `ds check` and `ds emit bash` with the same lowerer-owned
message where parsing succeeds:

- non-string left operand;
- non-literal right operand / runtime pattern string;
- regex literal outside the right side of `matches`;
- unsupported trailing flag such as `/g`;
- lookahead/lookbehind;
- inline flags such as `(?i:...)`;
- lazy quantifier;
- Unicode class such as `\p{L}`;
- backreference such as `(a)\1`;
- named capture syntax;
- first-class regex value binding if parseable;
- capture-group access if parseable;
- regex replacement/split helper names if parseable, with existing unknown
  helper/method diagnostics unless future syntax is introduced.

Lexer-owned diagnostics should remain lexer/parser diagnostics:

- unterminated regex literal;
- empty regex literal;
- newline inside regex literal.

### Backend invariant cases

- Emitted Bash for accepted regex should not call `ds`.
- Bash emission should use `nocasematch` only for `/i` patterns.
- VM runtime should not report invalid regex for lowerer-accepted test patterns.
- Backend fallback diagnostics, if reachable only by corrupted HIR, should be
  documented as internal invariants and not asserted as source-language errors.

### Regression cases

- Existing `string.replace` remains literal substring replacement, not regex
  replacement.
- Empty `string.replace` source behavior remains unchanged and is not treated as
  regex behavior.
- Membership `in` and ranges continue to parse independently of regex `matches`.
- Command-word interpolation cleanup from M3.4 remains unaffected.

## Exit criteria

The implementation pass can move regex from Hell to Watch / Clear while rejected
when:

- accepted regex `matches` behavior is represented in HIR and tested for VM/Bash
  parity;
- unsupported captures, replacement, runtime regex strings, and dialect-specific
  constructs are rejected before backend selection;
- VM/Bash regex diagnostics are runtime/invariant-only;
- docs and tests clearly distinguish substring replacement from regex
  replacement;
- no backend accepts regex behavior not represented by lowering/HIR.
