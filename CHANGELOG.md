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
- Initial `v0.1.0` frontend implementation without tests yet.
- `Makefile` for building the early `ds` CLI.
- Source loading, diagnostics, lexer, parser, AST model, and AST printer.
- `ds tokens <file.ds>`, `ds ast <file.ds>`, and `ds check <file.ds>`.
- Manual example scripts under `examples/`.

## v0.1.0 — In progress

### Implemented so far

- Source loading.
- Lexer.
- Parser.
- AST model.
- Syntax diagnostics.
- `ds tokens <file.ds>`.
- `ds ast <file.ds>`.
- `ds check <file.ds>`.
- `docs/language.ds` syntax catalog.

### Still pending

- Golden tests for lexer, parser, AST, and diagnostics.

## v0.2.0 — Planned

### Planned scope

- Basic Bash emission backend.
- `ds emit bash <file.ds> -o <file.sh>`.
- Standalone Bash output for the `v0.1.0` language subset.
- Bash emission golden tests.
- Basic generated Bash validity and behavior tests.
