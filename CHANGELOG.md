# v0.23.0 - Regex, Ranges, and Membership (planned)

Planned:
- Add kind-aware `in` membership checks for scalar arrays.
- Add inclusive integer range loop sources with `for n in start..end`.
- Add conservative regex literals and `matches` expressions with VM/Bash parity.
- Keep heredocs, regex captures/replacement, runtime regex patterns, range values, stepped/reverse ranges, and map membership deferred.

# v0.22.0 - Process Control and Signal Handling (planned)

Planned:
- Add EXIT-style `defer` cleanup blocks with deterministic VM/Bash ordering.
- Add signal-specific `defer on: "INT"` and `defer on: "TERM"` handlers.
- Add lower-level `trap "SIGNAL" { ... }` for the scoped `EXIT`/`INT`/`TERM` signal set.
- Preserve standalone Bash emission, cleanup final-status rules, imports, script args, and handler diagnostics for the supported subset.

# v0.21.0 - Function Values and Arithmetic

- Added `return expr` for scalar value-returning functions.
- Allowed user-defined function calls in expression positions when all known
  paths return a compatible scalar value, including supported forward calls to
  later scalar-returning functions.
- Added checked integer arithmetic operators `*`, `/`, `%`, `**`, unary `-`, and
  compound assignments `*=`, `/=`, `%=` in both the VM and emitted Bash.
- Added integer overflow diagnostics instead of silent VM/Bash wrapping for the
  scoped arithmetic operators and rejected out-of-range integer literals during
  lowering.
- Preserved standalone Bash emission for value-returning functions with a
  conservative stdout safety rule: expression-value calls reject functions that
  contain plain command statements, while statement-style calls may stream stdout
  and ignore the scalar return.
- Added expression-backed interpolation coverage for scalar calls and arithmetic,
  including command-word arithmetic interpolation while keeping direct
  command-word function calls on the bind-first diagnostic path.
- Added the dedicated `tests/v0_21/run.sh` suite and wired it into aggregate,
  ASAN, and UBSAN test targets.

# v0.20.0 - Cleanup: Wave 2 Stabilization

- Stabilized Wave 2 lowering by tracking known array element kinds for array
  literals and string-array helpers such as `string.split`, `lines`, `glob`, and
  `glob!`.
- Fixed indexed string-array composition so values like `parts[0]` from a known
  string array can call scoped string methods with VM/Bash parity.
- Fixed standalone Bash `case` parity for known indexed array selectors and
  array `for` loop variables with known int/bool/string element kinds.
- Hardened Bash helper dependency scanning so nested call arguments are scanned
  for run, pipeline, stdlib/string, collection-index, and map helper needs.
- Added the dedicated `tests/v0_20/run.sh` suite and wired it into `make test-v0-20`, aggregate `make test`, ASAN, and UBSAN paths.
- Fixed VM nested `for` loops so lexical `break` resets that loop's iterator state before the loop is re-entered.
- Inferred static kinds for function parameters with literal defaults, allowing imported/defaulted string parameters to use scoped string methods and defaulted string/int/bool parameters to preserve kind-aware Bash `case` matching while leaving untyped required parameters and explicit argument runtime tags deferred.
- Covered kind-aware exact `case` matching so mismatched literal kinds fall
  through without matching in both VM and Bash.
- Fixed VM integer formatting for large accepted widths such as `1024d` without truncating the formatted value.

# v0.19.0 - String Library and Formatted Output

- Added scoped string methods with VM and standalone Bash support: `trim`, `upper`, `lower`, `replace`, `contains`, `split`, `starts_with`, and `ends_with`.
- Added formatted interpolation specifiers for string transforms, width/alignment, integer padding, and narrow fixed-decimal integer rendering.
- Added triple-quoted multi-line string literals.
- Added `examples/strings.ds`.
- Tests for v0.19.0 now cover string methods, formatted interpolation,
  triple-quoted strings, VM/Bash parity, checker and formatter behavior,
  generated Bash standalone execution, examples, imports, script args, test
  blocks, and expected diagnostics for unsupported/deferred forms.

# v0.18.0 - Pipelines

- Added linear command pipelines for plain command statements and captured `run` expressions.
- Implemented VM pipe wiring with pipefail-style rightmost failing-stage status.
- Emitted standalone Bash pipelines and captured pipeline command-result fields without calling `ds`.
- Promoted command ownership to a pipeline-aware stage model shared by AST, HIR, formatter, checker, VM, and Bash emission.

Deferred: logical `&&`/`||`, grouping/subshells, background pipelines, process substitution, here-documents/here-strings, per-stage redirection, pipeline expressions outside command syntax, structured per-stage status arrays, multiline pipeline continuation syntax, and Windows shell semantics remain out of scope.

# Changelog

# v0.17.0 - Control Flow Completion

- Implemented scalar reassignment with `name = expr` plus integer `+=` and `-=` updates.
- Added VM and standalone Bash support for `while`, lexical `break`/`continue`, and expression-style `case`.
- Kept `case` alternatives as exact ds literals; emitted Bash uses exact comparisons instead of glob-style Bash `case` patterns.
- Added `examples/control-flow.ds` and updated language/status docs for the implemented control-flow surface.
- Deferred `until`, loop `else`, labeled/depth loop control, map iteration, index/field/env assignment, string binary `+`, function `return`, pipelines, and regex/glob/fallthrough case arms.

## v0.16.0 - Cleanup: Pre-Beta Hardening

- Added `docs/status.md` as the current support matrix for commands, production syntax, test-only syntax, formatter/checker behavior, examples, deferred language features, and the next feature wave.
- Split CLI source loading, import composition, lowering setup, and loaded-unit cleanup from `src/main.c` into `src/cli_program.c` / `src/cli_program.h`.
- Kept `tokens` and `ast` as root-file debug views while preserving the composed import-aware path for `check`, `hir`, `bytecode`, `run`, direct execution, `test`, and `emit bash`.
- Documented the formatter comment decision: comment-preserving formatting remains deferred, and `ds fmt` must reject comment-bearing files instead of silently dropping trivia.
- Updated the CLI usage banner from the stale `ds v0.6.0` label to `ds v0.16.0`.
- Updated README, architecture, and runtime docs to point at the current status document and describe the new CLI program boundary.

Deferred: comment-preserving formatting remains out of scope for this cleanup implementation because the lexer/parser pipeline still discards trivia; implementing it safely requires a dedicated attachment model rather than a small refactor.

## v0.8.0 - Cleanup: Command Model and Bash Parity

- Added shared command model regression tests under `tests/v0_8/`, including ownership/clone/free unit coverage for `DsCommand` and direct checks for the shared command-result field descriptor table.
- Added cleanup-focused VM/Bash parity coverage for captured commands, command-result field interpolation, redirection files, imports, script args with metacharacters, and plain-command fail-fast behavior.
- Tightened the shared VM/Bash parity helper so declared output files must be created by both backends before contents are compared.
- Added process-wrapper regression coverage for command-not-found handling, empty and large capture output, repeated captures, executable paths with spaces, and generated Bash helper behavior.
- Fixed plain command launch failures to report through normal source-located diagnostics while preserving exit code `127`; captured launch failures remain inspectable through the command result.
- Added diagnostic and unsupported-syntax coverage to ensure cleanup does not accidentally unlock pipelines, background jobs, stdin redirection, shell boolean operators, or future functions/loops.
- Added `make test-v0-8` and wired `tests/v0_8/run.sh` into `make test`.
- Fixed command parsing so bare `&`, `&&`, and `||` command operators are rejected instead of being treated as ordinary command words.

## v0.7.0 - Command Results and Redirection

- Implemented `run` expressions for captured command execution in both VM mode and emitted standalone Bash.
- Added command-result fields: `stdout`, `stderr`, `code`, `ok`, and `failed`.
- Added `{result.stdout}`-style command-result field interpolation in strings for VM and Bash parity.
- Captured command failures are now inspectable without aborting the script, while plain command statements remain fail-fast.
- Added readable redirection suffixes for plain command statements: `|>`, `|>>`, `!>`, `!>>`, `&>`, and `&>>`.
- Added stable token, AST, and bytecode shapes for captured commands, field access, and redirected commands.
- Added portable examples under `examples/command-result.ds` and `examples/redirection.ds`.
- Added `tests/v0_7/run.sh`, capture/redirection/diagnostic/parity fixtures, golden token/AST/bytecode/Bash outputs, and C unit tests for command-result ownership and lowered command-result/redirection shape.
- Fixed unsupported pipeline and legacy shell-redirection diagnostics so v0.7.0 rejects out-of-scope command forms before execution or Bash emission.
- Improved command-word field diagnostics for missing fields and known non-result receivers.
- Fixed VM redirection-open failures to use normal source-located diagnostics and failed Bash emission to remove stale output artifacts.
- Expanded v0.7.0 edge coverage for command-not-found capture, executable paths with spaces, block scoping, field interpolation, redirection-open locations, and stale emit artifacts.
- `make test` now runs the v0.7.0 suite in addition to prior suites.

## v0.6.0 - Imports / Includes

- Implemented initial local `import "./file.ds"` parsing and composition for `check`, `bytecode`, `run`, direct execution, and `emit bash`.
- Added deterministic load-once import resolution relative to the importing file, cycle diagnostics, missing-import diagnostics, and standalone Bash bundling of imported statements.
- Fixed top-level import ordering so multiple imports before executable statements are accepted and composed in order.
- Added `tests/v0_6/run.sh`, import fixtures, and golden outputs covering parser/AST import syntax, relative and nested resolution, duplicate load-once behavior, cycle, missing-file, and directory-import diagnostics, cross-file lowering errors, bytecode source mapping, VM execution, standalone Bash bundling, VM/Bash parity, script-argument interaction, and older-version regressions.
- Fixed import read-failure handling so directory imports and other import-source read failures surface source-tied diagnostics at the import site and cannot be silently ignored.
- `make test` now runs the v0.6.0 suite; current count is `v0.6.0 tests passed: 231 checks`.
- Added import examples under `examples/import-main.ds` and `examples/import-lib.ds`.

## v0.5.0 — Complete

- Implemented first-class `script { ... }` argument contracts for `arg`, `option`, and `flag` declarations.
- Added parser, AST debug output, lowering, VM argv binding, generated help, and standalone Bash argv parser emission for the scoped v0.5.0 feature set.
- Added `examples/args.ds` and updated CLI help to show `ds <file.ds> [args...]` and `ds run <file.ds> [args...]`.
- Fixed direct script invocation with arguments so path-like missing files, such as `ds /tmp/nope.ds arg`, report source/file diagnostics instead of being treated as unknown commands.
- Fixed one-argument unknown command handling so `ds frob` remains a usage error while readable/path-like script arguments still use direct execution.
- Added `tests/v0_5/run.sh`, `tests/v0_5/unit/lower.c`, fixtures, and golden files for lexer/parser/AST output, lowering, VM argv parsing, standalone Bash argv parsing, help output, bytecode arg-contract dumps, diagnostics, shell safety, CLI integration, integer overflow parity, and older regression coverage.
- `make test` now runs the v0.5.0 suite; current count is `v0.5.0 tests passed: 249 checks`.
- Fixed v0.5.0 integer overflow handling so integer defaults fail during lowering and VM/emitted Bash argv parsing both reject values outside the supported signed 64-bit range.
- Updated sanitizer targets to avoid rebuilding inside each versioned test runner and to run the AddressSanitizer suite with leak detection disabled for reliable full-suite completion in the tool environment.
- Documented that v0.5.0 treats option values beginning with `--` as option tokens; richer `--name=value` or escaped option-value handling remains deferred.


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
