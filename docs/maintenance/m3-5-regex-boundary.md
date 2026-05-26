# M3.5 — Regex captures, replacement, and runtime regex strings boundary

## Problem
Regex support can drift across lexer syntax, parser AST, lowerer parity gates, VM
regex execution, Bash `[[ =~ ]]`, string replacement helpers, diagnostics, and
tests. Captures, replacement, and runtime regex strings are especially risky
because VM and Bash have different regex engines and capture APIs.

## Decision
Regex matching remains conservative.

- Lexer/parser own regex literal syntax shape.
- Lowering owns regex feature acceptance and VM/Bash parity gates.
- HIR `matches` expressions with conservative literal metadata are the accepted
  backend-neutral contract.
- VM and Bash execute only accepted regex HIR.
- Captures, regex replacement, and runtime regex strings remain rejected until a
  canonical HIR/value model and VM/Bash parity contract exist.

## Ownership
- Syntax owner: lexer/parser (`src/lexer.c`, `src/parse_expr.c`, `src/ds_ast.h`).
- Semantic owner: lowerer (`src/lower_expr.c` and related helpers).
- Canonical representation: HIR `matches` expression plus conservative regex
  literal/source metadata.
- VM owner: runtime execution of accepted regex matches and runtime failures for
  accepted HIR.
- Bash owner: emitted Bash `[[ string =~ regex ]]`/helper rendering for accepted
  HIR and Bash artifact/runtime invariant failures.
- Diagnostics owner: lexer/parser for malformed regex syntax; lowerer for
  unsupported regex features, captures, replacement, runtime pattern strings, and
  parity gates; VM/Bash for accepted-runtime failures only.
- Test owner: accepted VM/Bash regex parity and unsupported-form diagnostics.

## Accepted behavior
- Conservative regex literal matching through the current `matches` surface.
- VM/Bash parity for accepted match success/failure.
- Literal patterns that lowerer deems portable across the VM runtime and emitted
  Bash.
- Parser-preserved literal spans so lowerer diagnostics can point at the pattern
  that caused the rejection.
- Accepted regex HIR that does not require backend-local capture state.
- String replacement only where it is not regex-capture/replacement semantics.
- Backend execution of accepted HIR without rediscovering source-language regex
  validity.

## Rejected behavior
- Regex captures and capture group access: lowerer diagnostic.
- Regex replacement semantics: lowerer diagnostic unless it is a current literal
  string replacement path outside regex replacement.
- Runtime regex strings or dynamic patterns: lowerer diagnostic.
- Advanced regex features not proven portable across VM and Bash: lowerer
  diagnostic.
- Temporary or backend-only capture state: rejected until represented in HIR and
  runtime values.
- Backend-specific acceptance of regex syntax is not allowed for source-language
  forms.
- VM-only regex syntax and Bash-only regex syntax must stay rejected unless a
  future parity contract explicitly allows backend-specific behavior.
- Diagnostics must not imply captures/replacement/runtime patterns are partly
  supported while they remain rejected.

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
- Unsupported capture group diagnostics.
- Unsupported regex replacement diagnostics.
- Unsupported runtime regex string diagnostics.
- Backend invariant cases proving accepted HIR does not rely on backend-only
  validation.
- Regression cases for malformed literals proving lexer/parser owns syntax
  errors while lowerer owns unsupported-but-parseable regex forms.

## Future work
- Design captures only with explicit HIR metadata and a runtime value model.
- Design regex replacement only with a portable replacement grammar and Bash/VM
  parity plan.
- Design runtime regex strings only after deciding compile-time vs runtime
  validation and diagnostics.
- Keep rejected regex features in lowerer diagnostics until those designs exist.
- Update `docs/parity-contracts.md` before accepting any regex feature whose
  observable VM/Bash behavior is not already covered.
