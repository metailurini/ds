# Changelog

## v0.5.0 — Implementation in progress

- Implemented first-class `script { ... }` argument contracts for `arg`, `option`, and `flag` declarations.
- Added parser, AST debug output, lowering, VM argv binding, generated help, and standalone Bash argv parser emission for the scoped v0.5.0 feature set.
- Added `examples/args.ds` and updated CLI help to show `ds <file.ds> [args...]` and `ds run <file.ds> [args...]`.
- Fixed direct script invocation with arguments so path-like missing files, such as `ds /tmp/nope.ds arg`, report source/file diagnostics instead of being treated as unknown commands.
- Documented that v0.5.0 treats option values beginning with `--` as option tokens; richer `--name=value` or escaped option-value handling remains deferred.
- New v0.5.0 tests are intentionally not added yet; existing v0.1.0 through v0.4.0 suites continue to run.


All notable changes to this project will be documented in this file.

The project uses semantic versioning once stable, but during pre-`1.0.0` development the minor version is used for planned milestone work.

## Unreleased

### Added

- Initial project documentation.
- Product principles for avoiding language drift.
- Roadmap and version workflow.
- Architecture plan for the C implementation, frontend, HIR, bytecode VM, and Bash emitter.
- Runtime plan for core C primitives used by the frontend, VM, Bash emitter, diagnostics, and tools.
- Milestone spec and test plan for `v0.1.0`.
- Milestone spec and test plan for `v0.2.0`.
- `docs/language.ds` syntax catalog for tracking the full planned language surface.
- Roadmap update so `v0.3.0` is **Minimal C Runtime and Bytecode VM**.
- Hashmap support code included under temporary `libs/hashmap/` staging and documented as eventually absorbed into `src/core` behind `DsMap`.
- Initial `v0.1.0` frontend implementation.
- `Makefile` for building the early `ds` CLI.
- Source loading, diagnostics, lexer, parser, AST model, and AST printer.
- `ds tokens <file.ds>`, `ds ast <file.ds>`, and `ds check <file.ds>`.
- Manual example scripts under `examples/`.
- `v0.1.0` executable test runner, fixtures, and golden outputs.
- Stable escaped token debug output for golden tests.
- Missing-file diagnostics now include the requested path.
- Invalid and trailing string escapes now produce lexer diagnostics.
- Initial `v0.2.0` Bash emitter implementation.
- `ds emit bash <file.ds> -o <file.sh>` for the `v0.1.0` source subset.
- Standalone generated Bash with shebang, strict mode, prefixed variables, source comments, command emission, simple string interpolation, and `if`/`else` emission.
- `v0.2.0` Bash emitter test suite with golden output, `bash -n` validation, runtime behavior checks, quoting/safety coverage, diagnostics, and CLI edge cases.
- Expanded `v0.2.0` edge-case coverage for no-input CLI usage, invalid source failures before emission, no trailing newline, deeper nested conditionals, long strings, many variables, interpolation next to punctuation, and future syntax rejection.
- Milestone spec and comprehensive regression-focused test plan for `v0.4.0` cleanup of frontend, runtime, and backend boundaries.
- Initial `v0.3.0` VM implementation with `ds <file.ds>`, `ds run <file.ds>`, and `ds bytecode <file.ds>`.
- Minimal runtime primitives for owned strings, tagged values, internal arrays, and `DsMap` symbol/value storage.
- Shared lowered program representation consumed by both bytecode generation/VM execution and Bash emission, including known variable checks, interpolation names, supported expressions, duplicate declarations, and block-local scope boundaries.
- VM runtime scope push/pop behavior for lowered blocks, so branch-local declarations do not leak into outer or sibling scopes.
- Deterministic bytecode dump output and a small direct execution VM for the supported `v0.1.0` / `v0.2.0` subset.
- `v0.3.0` runtime, lowering, bytecode, VM execution, diagnostics, command execution, sanitizer, and VM/Bash parity tests.
- Expanded `v0.3.0` strict-plan coverage for direct lowered-tree assertions, source locations after blank lines/comments, empty-block jumps, explicit command exit status, and long command output.
- `make asan` and `make ubsan` sanitizer test targets for runtime/VM ownership checks where the local compiler supports them.
- `v0.4.0` cleanup implementation for shared CLI parse/lower plumbing and backend entrypoints that consume lowered programs directly.
- Runtime container clear APIs for reuse-friendly ownership boundaries: `ds_array_clear()` and `ds_map_clear()`.
- Shared diagnostic location formatting helper for source-tied errors.
- Shared shell test helper for versioned regression runners.
- Consistent version-owned test layout: each milestone runner and unit source now
  lives under `tests/v0_*/`, with shared helpers under `tests/lib/`.
- `v0.4.0` cleanup regression suite covering pipeline boundaries, diagnostic consistency, source locations, runtime ownership, `DsMap` wrapper behavior, static backend/staged-library boundaries, generated Bash standalone behavior, command exit parity, cleanup-only future-syntax rejection, CLI usage consistency, shared golden-helper failure quality, and docs/help-command alignment.
- Milestone spec and comprehensive test plan for `v0.5.0` first-class CLI args, including VM/Bash parity, help output, shell-safety, and argument diagnostic coverage.
- Milestone spec and comprehensive test plan for `v0.6.0` imports/includes, including relative import resolution, load-once behavior, cycle diagnostics, multi-file source locations, standalone Bash bundling, and VM/Bash parity.
- Milestone spec and comprehensive test plan for `v0.7.0` command results and redirection, including captured stdout/stderr/exit-code behavior, readable redirection syntax, standalone Bash helper requirements, shell-safety, diagnostics, runtime ownership, and VM/Bash parity.

### Fixed

- `ds run` and `ds bytecode` without an input file now report usage instead of treating the subcommand name as an implicit script path.
- `ds check`, `ds emit bash`, `ds run`, direct script execution, and `ds bytecode` now share one CLI parse/lower path instead of each lowering or parsing separately.
- Source-tied diagnostics now use the standardized `file:line:column: error: message` shape across the shared diagnostics path.

## v0.4.0 — Complete

### Implemented

- Centralized CLI source-loading, parsing, and lowering helpers in the entrypoint.
- Added lowered-program backend entrypoints for Bash emission, bytecode dumping, and VM execution.
- Kept AST-only debug commands (`tokens`, `ast`) frontend-oriented while moving behavior-sensitive commands through shared lowering.
- Added explicit container clear APIs and documented runtime ownership behavior.
- Recorded that `libs/hashmap` remains staged and unused by production code; callers continue to depend only on `DsMap`.

### Tests

- Added `tests/v0_4/run.sh` and `tests/v0_4/unit/runtime.c`.
- Added `tests/lib/testlib.sh` as the first shared shell helper for reusable runner assertions.
- Expanded the v0.4.0 suite to 163 cleanup-focused checks, including missing-golden and golden-mismatch helper behavior plus README/help/docs sanity checks.
- Existing `v0.1.0`, `v0.2.0`, and `v0.3.0` regression tests continue to pass, and `make test` now runs the `v0.4.0` cleanup suite too.

## v0.3.0 — Complete

### Implemented

- Direct VM execution through `ds <file.ds>` and `ds run <file.ds>`.
- Deterministic bytecode dump output through `ds bytecode <file.ds>`.
- Shared lowered program representation consumed by both VM bytecode generation and Bash emission.
- Runtime primitives for owned strings, tagged values, growable arrays, and project-owned map storage.
- Bytecode generation and register VM execution for the supported `v0.1.0` / `v0.2.0` subset.
- Command execution through a small process-launch boundary with non-zero command exits propagated to the CLI.

### Tests

- Runtime C unit tests for `DsString`, `DsValue`, `DsArray`, and `DsMap`.
- Bytecode golden tests for empty files, comments-only files, variables, interpolation, branches, nested conditionals, and mixed scripts.
- VM integration tests for values, expressions, interpolation, conditionals, command execution, command failures, source-shape edge cases, and diagnostics.
- VM/Bash parity tests for supported success and failure fixtures.
- Sanitizer checks for runtime ownership where the local compiler supports AddressSanitizer and UndefinedBehaviorSanitizer.

## v0.1.0 — Complete

### Implemented

- Source loading.
- Lexer.
- Parser.
- AST model.
- Syntax diagnostics.
- `ds tokens <file.ds>`.
- `ds ast <file.ds>`.
- `ds check <file.ds>`.
- `docs/language.ds` syntax catalog.

### Tests

- Lexer coverage for keywords, identifiers, literals, comments, operators, strings, locations, unterminated strings, invalid escapes, and trailing escapes.
- Parser coverage for `let`, `if/else`, nested blocks, expressions, and command statements.
- AST golden tests for empty, comments-only, and mixed fixtures.
- Diagnostics tests for invalid syntax and missing files.
- CLI smoke tests for `tokens`, `ast`, `check`, and `--help`.
- Syntax catalog checks for `docs/language.ds`.

## v0.2.0 — Complete

### Implemented

- Basic Bash emission backend.
- `ds emit bash <file.ds> -o <file.sh>`.
- Standalone Bash output for the `v0.1.0` language subset.
- String interpolation in both command strings and `let` string values.
- Documented conservative Bash comparison semantics before type-aware runtime/VM behavior exists.

### Tests

- Bash emission golden tests.
- Generated Bash `bash -n` validity tests.
- Generated Bash behavior tests.
- Safety and quoting tests for strings containing spaces, quotes, dollar signs, command substitutions, backticks, and backslashes.
- Diagnostics tests for unsupported assignment expressions, unknown interpolation variables, unknown command variables, unknown condition variables, and invalid emit CLI forms.
- CLI smoke tests for missing output paths, unsupported backends, missing inputs, and unwritable outputs.
- Edge-case tests for no-input CLI usage, invalid source files failing before emission, files without trailing newlines, deeper nested conditionals, long strings, many variables, interpolation next to punctuation, and future syntax rejection.
