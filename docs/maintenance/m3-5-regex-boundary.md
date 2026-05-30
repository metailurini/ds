# M3.5 — Regex captures, replacement, and runtime regex strings boundary

## Problem
Regex support can drift across lexer syntax, parser AST, lowerer parity gates, VM
regex execution, Bash `[[ =~ ]]`, string replacement helpers, diagnostics, and
tests. Captures, replacement, and runtime regex strings are especially risky
because VM and Bash have different regex engines and capture APIs.

## Decision
Regex matching remains conservative. `v0.32.0` converts the previously rejected
capture/replacement/runtime-string bucket into a scoped accepted surface while
leaving broader regex APIs rejected.

- Lexer/parser own regex literal syntax shape.
- Lowering owns regex feature acceptance and VM/Bash parity gates.
- HIR `matches` expressions and scoped stdlib helper calls are the accepted
  backend-neutral contract.
- VM and Bash execute only accepted regex HIR/helper calls.
- Runtime string patterns, flat capture maps, and global replacement are accepted
  only through the v0.32.0 contract; regex split, first-class compiled regex
  values, named captures, lookaround, pattern backreferences, and richer
  replacement APIs remain rejected until a canonical HIR/value model and VM/Bash
  parity contract exist.

## Ownership
- Syntax owner: lexer/parser (`src/lexer.c`, `src/parse_expr.c`, `src/ds_ast.h`).
- Semantic owner: lowerer (`src/lower_expr.c` and related helpers).
- Canonical representation: HIR `matches` expression plus conservative regex
  literal/source metadata and stdlib helper calls for `regex.match` /
  `regex.replace`.
- VM owner: runtime execution of accepted regex matches/helpers and runtime
  failures for accepted HIR/helper calls.
- Bash owner: emitted Bash `[[ string =~ regex ]]`/helper rendering for accepted
  HIR/helper calls and Bash artifact/runtime invariant failures.
- Diagnostics owner: lexer/parser for malformed regex syntax; lowerer for
  unsupported regex features, static/direct string patterns, static flags and
  replacements, known capture-count failures, and parity gates; VM/Bash for
  dynamic pattern, flag, replacement, capture-count, and zero-length replacement
  runtime data failures.
- Test owner: accepted VM/Bash regex parity and unsupported-form diagnostics.

## Accepted behavior
- Conservative regex literal and runtime-string matching through the current
  `matches` surface.
- Flat capture maps through `regex.match` with `matched`, `full`, `"0"`, and
  numbered string captures through `"9"`.
- Global regex replacement through `regex.replace` with `$0`..`$9` and `$$`
  expansion.
- VM/Bash parity for accepted match success/failure.
- Literal patterns that lowerer deems portable across the VM runtime and emitted
  Bash.
- Parser-preserved literal spans so lowerer diagnostics can point at the pattern
  that caused the rejection.
- Accepted regex HIR/helper calls that expose capture state only through normal
  flat map/string values.
- Backend execution of accepted HIR without rediscovering source-language regex
  validity.

## Rejected behavior
- Regex split, named captures, first-class compiled regex values, replace-count
  APIs, callback replacements, and replace-first helpers: lowerer diagnostic.
- Advanced regex features not proven portable across VM and Bash: lowerer
  diagnostic.
- Temporary or backend-only capture state outside the v0.32 flat map/string
  helper contract: rejected until represented in HIR and runtime values.
- Backend-specific acceptance of regex syntax is not allowed for source-language
  forms.
- VM-only regex syntax and Bash-only regex syntax must stay rejected unless a
  future parity contract explicitly allows backend-specific behavior.
- Diagnostics must not imply broader regex dialect/API support beyond the scoped
  v0.32 surface.

## Files touched / relevant files
- `src/lexer.c`
- `src/parse_expr.c`
- `src/ds_ast.h`
- `src/ds_hir.h`
- `src/lower_expr.c`
- `src/vm_compile.c`
- `src/vm.c`
- `src/runtime.c`
- `src/bash_expr.c`
- `src/bash_quote.c`
- `src/bash_helpers.c`
- `src/ds_stdlib.c`
- `docs/parity-contracts.md`
- `docs/diagnostics.md`

## Tests
- Documentation/source-map checks for regex ownership.
- Accepted literal match true/false cases in VM and emitted Bash.
- Accepted no-match cases with the same boolean/result behavior in both
  backends.
- Interpolation-adjacent regex cases that are already supported.
- Capture map shape and optional/unmatched capture diagnostics.
- Regex replacement diagnostics.
- Runtime regex string diagnostics.
- Backend invariant cases proving accepted HIR does not rely on backend-only
  validation.
- Regression cases for malformed literals proving lexer/parser owns syntax
  errors while lowerer owns unsupported-but-parseable regex forms.

## Future work
- Design nested/typed capture values only with explicit HIR metadata and a
  runtime value model.
- Design regex split, replace-first/count, callback replacement, named capture,
  or first-class regex values only with a portable grammar and Bash/VM parity
  plan.
- Keep rejected regex features in lowerer diagnostics until those designs exist.
- Update `docs/parity-contracts.md` before accepting any regex feature whose
  observable VM/Bash behavior is not already covered.
