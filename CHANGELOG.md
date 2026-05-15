# Changelog

All notable changes to this project will be documented in this file.

The project uses semantic versioning once stable, but during pre-`1.0.0` development the minor version is used for planned milestone work.

## Unreleased

### Added

- Initial project documentation.
- Product principles for avoiding language drift.
- Roadmap and version workflow.
- Architecture plan for the C implementation, frontend, HIR, bytecode VM, and Bash emitter.
- Milestone spec and test plan for `v0.1.0`.
- Milestone spec and test plan for `v0.2.0`.

## v0.1.0 — Planned

### Planned scope

- Source loading.
- Lexer.
- Parser.
- AST model.
- Syntax diagnostics.
- `ds tokens <file.ds>`.
- `ds ast <file.ds>`.
- `ds check <file.ds>`.
- Golden tests for lexer, parser, AST, and diagnostics.

## v0.2.0 — Planned

### Planned scope

- Basic Bash emission backend.
- `ds emit bash <file.ds> -o <file.sh>`.
- Standalone Bash output for the `v0.1.0` language subset.
- Bash emission golden tests.
- Basic generated Bash validity and behavior tests.
