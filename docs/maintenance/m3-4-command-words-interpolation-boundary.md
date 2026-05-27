# M3.4 — Command words and direct function-call interpolation boundary

## Problem
Command words look like shell text, but they also contain DS variables,
command-result fields, environment access, quoted interpolation, redirection
text, and now a narrow direct function-call interpolation path. If VM and Bash
parse command-word strings independently, command behavior can become backend
specific.

## Decision
Command words stay syntax-preserving until lowering validates them.

- Parser owns command shape: stages, pipes, redirections, word boundaries, and
  malformed command syntax.
- `DsCommand`/`DsWord` are syntax payloads, not arbitrary expression HIR.
- Lowering owns command-word legality, interpolation legality, unsupported-form
  diagnostics, and VM/Bash parity gates.
- Direct scalar function-call interpolation is accepted only in the current
  narrow quoted-word/redirection form by pre-materializing the call into a
  private string binding and rewriting the command word/target to that binding.
- VM and Bash consume the validated/pre-materialized command payload.

## Ownership
- Syntax owner: parser (`src/parse_command.c`, `src/parse_expr.c`,
  `src/parse_stmt.c`).
- Semantic owner: lowerer (`validate_cmd_word()`, `validate_interpolation()`, and
  `lower_materialize_command_interpolation()`).
- Canonical representation: validated `DsCommand`/`DsWord` payloads plus
  lowerer-private temporary string bindings for supported direct scalar
  function-call interpolation.
- VM owner: argv construction, runtime interpolation of accepted HIR, process
  execution, redirections, captured command results, and OS/runtime failures.
- Bash owner: quoting, helper selection, standalone script command rendering, and
  emitted-script runtime helpers for accepted HIR.
- Diagnostics owner: parser for malformed command syntax; lowerer for unknown
  variables, invalid fields, invalid interpolation, unsupported forms, and parity
  gates; VM/Bash for runtime/artifact/invariant failures only.
- Test owner: command word, interpolation, redirection, VM/Bash parity, and
  diagnostic tests.

## Accepted behavior
- Plain command words preserved as shell-native argv text unless they match a
  supported DS command variable or field form.
- `$name` as one command argument for named scalar variables.
- `name.field` and `$name.field` for validated command-result fields and direct
  `env.NAME` where currently documented.
- Quoted command words as one argv element with DS string interpolation.
- Supported interpolation for named scalar values, command-result fields via
  named bindings, `env.NAME`, documented format specifiers, and already-supported
  arithmetic-like interpolation.
- Narrow direct scalar value-returning function calls inside quoted command words
  or redirection targets; these are pre-materialized before the outer command.
- VM/Bash parity for argv shape, quoted string value, field values, redirection
  targets, stdout/stderr/status behavior, and rejection diagnostics.

## Rejected behavior
- Arrays/maps as direct command arguments: lowerer diagnostic.
- Collection indexing/access directly in command arguments when not documented as
  supported: lowerer diagnostic; bind first.
- Collection-valued, map-valued, command-result-valued, unknown, mixed-return, or
  stdout-producing direct function-call interpolation: lowerer diagnostic.
- Function-call interpolation in unquoted command words: lowerer diagnostic.
- Arbitrary command substitution or nested command execution inside command-word
  interpolation: lowerer diagnostic.
- Unsupported interpolation expressions or suffixes: lowerer diagnostic.
- Invalid command-result fields or environment names in command words: lowerer
  diagnostic.
- Backend-specific command-word acceptance is not allowed unless explicitly
  documented as backend-only.

## Files touched / relevant files
- `src/ds_command.h`
- `src/ds_ast.h`
- `src/ds_command_word.c`
- `src/ast.c`
- `src/format.c`
- `src/ds_checker.c`
- `src/lower_command.c`
- `src/lower_stmt.c`
- `src/lower_functions.c`
- `src/lower_internal.h`
- `src/ds_hir.h`
- `src/vm_compile.c`
- `src/vm_process.c`
- `src/vm.c`
- `src/bash_command.c`
- `src/bash_quote.c`
- `src/bash_stmt.c`
- `src/bash_expr.c`
- `src/bash_deps.c`
- `src/bash_helpers.c`

## Tests
- Plain command words and argv boundaries.
- Quoted command words and one-argument preservation.
- `$name`, command variables, command-result fields, and `env.NAME` words.
- Redirection interpolation.
- Direct scalar function-call interpolation in quoted command words.
- Unsupported direct interpolation forms and invalid command-word expressions.
- VM/Bash parity for command argv, output, redirection, and emitted Bash.
- Diagnostic ownership cases proving unsupported language forms fail in lowering.

## Future work
- Do not turn command words into general expression syntax without a new HIR
  representation.
- Keep the current direct function-call interpolation surface narrow until a
  future milestone defines portable non-scalar, temporary, or command-substitution
  semantics.
- Move any backend command-word validation that becomes source-language policy
  back to lowering.
