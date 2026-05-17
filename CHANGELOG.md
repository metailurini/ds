# Changelog

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
