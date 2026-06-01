# v0.35.0 - Core String Parsing Helpers

- Added byte-oriented string helpers `.len()`, `.index_of(needle)`,
  `.last_index_of(needle)`, `.count(needle)`, `.char_at(index)`, and
  `.slice(start, end)` with VM and standalone Bash parity.
- Preserved explicit empty-needle behavior for search/count helpers:
  `index_of("") == 0`, `last_index_of("") == len()`, and
  `count("") == len() + 1`.
- Kept `char_at` and `slice` strict: negative, out-of-range, and reversed slice
  ranges are rejected instead of clamped or wrapped.
- Accepted read-only `string.split(...)[index]` chains so parsing expressions
  like `sig.split("(")[0].trim()` work without temporary variables.
- Tightened emitted-Bash helper selection so string-only programs emit only the
  specific string helper bodies they use, plus scalar stdlib capture when a
  direct assignment needs it, instead of emitting the full string-helper block.
- Updated language/status/runtime/diagnostics/DX docs for the implemented
  v0.35.0 surface. Dedicated regression tests remain for the follow-up test
  implementation pass.

# v0.34.0 - Text Literal and Broken-Pipe DX

- Bumped the CLI help banner to `ds v0.34.0` for the new DX-priority
  implementation pass.
- Added doubled-brace literal spelling in ordinary and triple-quoted strings:
  `{{` renders a literal `{`, `}}` renders a literal `}`, and `{expr}` remains
  the existing interpolation form.
- Applied the same literal-brace rendering to VM execution, emitted standalone
  Bash, quoted command words, arrays/maps/function returns, and interpolation
  fragments lowered around supported scalar function calls.
- Rejected lone `}` in interpolated strings with a clear lowerer diagnostic
  that points users to `}}` for a literal close brace, while preserving existing
  malformed interpolation diagnostics for unmatched `{` and bad expressions.
- Quieted the common closed-stdout broken-pipe case for uncaptured,
  unredirected top-level command statements and pipelines, so commands such as
  `ds run script.ds | head` and generated Bash equivalents exit successfully
  without noisy status-141 diagnostics after supported cleanup runs.
- Tightened the follow-up broken-pipe contract: VM execution now quiets only raw direct-command/final-pipeline-stage
  `SIGPIPE` terminations with pipe-like stdout; generated Bash uses a pipe-like
  stdout check for its documented standalone status-141 heuristic and exits after
  cleanup for quiet pipeline cases instead of continuing to later statements.
- Preserved command-result capture semantics for broken-pipe-like subprocess
  statuses: captured `run` commands still expose `code`, `ok`, and `failed`
  instead of being converted into top-level success.
- Added the dedicated `tests/v0_34/run.sh` suite and wired it into aggregate
  version tests after `v0.33.0`, covering the scoped literal-brace and
  broken-pipe contracts across VM execution and standalone emitted Bash.
- Expanded the v0.34 regression suite and emitted-Bash helpers so source-level
  redirected commands/pipelines do not inherit the quiet broken-pipe heuristic;
  explicit status `141` under a DS redirection remains visible even when the
  generated script stdout is piped to `head`.
- Fixed an exposed diagnostic bug where unclosed interpolation in command-word
  strings could fall through to an unknown-variable diagnostic instead of the
  dedicated unclosed-interpolation error.
- Refreshed historical test guards that intentionally track the current CLI
  banner, and fixed the v0.33 root-runner permission fallback so its restricted
  directory stays unreadable after making the harness runnable by `nobody`.
- Updated current docs and DX notes for the implemented brace-literal and
  broken-pipe contracts plus the now-completed focused v0.34 regression pass.

# v0.33.0 - Collection, Glob, and Regex Stabilization

- Bumped the CLI help banner to `ds v0.33.0` so the executable reports the
  current cleanup milestone.
- Stabilized `regex.match` no-match capture maps so VM execution and emitted
  Bash both expose present capture-group keys as empty strings when a pattern
  does not match.
- Fixed checker warning discovery for supported indexed and direct-call
  interpolation forms such as `{items[i]}` and `{double(n)}`.
- Reconciled current docs around the completed collection/glob/regex feature
  wave, including the capture-map contract: `regex.match` materializes capture
  keys for groups present in the validated pattern, up to nine captures, rather
  than guaranteeing absent keys for all digits `1` through `9`.
- Recorded technical-debt findings for helper scanning, VM stdlib ownership,
  generated Bash helper bodies, collection portability gates, and duplicated
  regex validation.
- Added the dedicated `tests/v0_33/run.sh` stabilization suite and wired it into
  aggregate test targets after `v0.32.0`.
- Rejected structured collection/map/command-result interpolation in quoted
  command words unless the interpolation is a supported scalar field or flat
  index read.
- Expanded the `v0.33.0` stabilization suite around the final audit gaps:
  dynamic recursive-glob validation, directory symlink non-traversal, empty
  dynamic glob parity, collection assignment rejection variants, imported
  collection/regex composition, runtime regex sources, test-block execution, and
  cleanup-handler helper discovery.
- Fixed emitted Bash `glob("")` parity so empty non-recursive patterns produce
  zero matches like VM execution instead of falling through to shell `compgen`.

# v0.32.0 - Regex Runtime Strings, Captures, and Replacement

- Added runtime string regex patterns for `matches`, with static validation for
  direct string literals and VM/Bash runtime validation for dynamic strings.
- Added the `regex.match(text, pattern[, flags])` helper, returning a flat map
  with `matched`, `full`, `"0"`, and numbered string captures for groups
  present in the validated pattern, up to `"9"`.
- Added the `regex.replace(text, pattern, replacement[, flags])` helper with
  global replacement, `$0`..`$9` capture expansion, and `$$` literal-dollar
  expansion.
- Added shared regex validation so literals, static string patterns, VM runtime
  execution, and emitted Bash helpers reject the same unsupported regex forms.
- Kept regex split, named captures, lookaround, pattern backreferences,
  replace-first/count APIs, callback replacements, first-class regex values, and
  broader PCRE-style compatibility deferred.

# v0.24.0 - Cleanup: Pre-1.0 Hardening

- Added `docs/release-checklist.md` as the executable `1.0.0` release boundary
  for support-matrix, VM/Bash parity, generated-Bash standalone behavior,
  examples, docs, diagnostics, formatter/checker, sanitizer/ownership,
  deferred/rejected features, and release-note sign-offs.
- Updated the user-facing support matrix to reflect the implemented surface
  through `v0.23.0` plus the `v0.24.0` hardening pass without adding new
  production syntax.
- Hardened generated Bash helper hygiene by deduplicating common `__ds_error`
  and plain command failure helper emission across helper families.
- Reconciled the v0.24 broad parity fixture with the current v0.21 command-word
  interpolation rule: bind function-call results before interpolating them into
  command words.

# v0.23.0 - Regex, Ranges, and Membership

- Added kind-aware `in` membership checks for scalar arrays, including array
  literals, named arrays, and known standard-library string-array results such
  as `.split(...)`, `lines(...)`, `glob(...)`, and `glob!(...)`.
- Added inclusive integer range loop sources with `for n in start..end`,
  including variable, arithmetic, and supported scalar function-call bounds.
- Added conservative regex literals and `matches` expressions with VM/Bash
  parity, including trailing `/i` case-insensitive matching.
- Added scoped expression-level `&&` and `||` so v0.23 filter predicates can be
  composed without falling back to shell command operators.
- Kept heredocs, regex captures/replacement, runtime regex patterns, range
  values, stepped/reverse ranges, and map membership deferred.

# v0.22.7 - Final v0.22 Test-Plan Audit

- Re-audited `tests/v0_22/run.sh` against the original v0.22.0 test plan and
  filled deterministic coverage gaps for cleanup side effects, imported signal
  handlers, imported signal diagnostics, function-registered handlers,
  handler control flow, v0.21 arithmetic/return interaction, test-block handler
  isolation, empty/numeric/dynamic/malformed signal diagnostics, and background
  command rejection.
- Fixed emitted Bash cleanup stack iteration so helper-local loop variables no
  longer shadow user variables used inside cleanup handlers.
- Updated the v0.22 spec and test-plan status to reflect that the focused suite
  is now implemented and wired into `make test-v0-22`.

# v0.22.6 - Handler Context and Final v0.22 Documentation

- Finalized the staged v0.22 cleanup/signal documentation after the cleanup
  core, signal syntax surface, deterministic harness, direct-command signal
  runtime, and simple foreground-pipeline signal runtime were stabilized.
- Documented the final supported subset: process-scope `defer`/`trap` handlers
  for `EXIT`, `INT`, and `TERM`; VM/Bash parity for normal completion,
  explicit `exit`, explicit `fail`, direct command failure, captured command
  failure, foreground direct-command signals, and simple foreground-pipeline
  signals.
- Reaffirmed rejected behavior for function-local handler captures and direct
  `return` from cleanup handlers.
- Explicitly deferred handler context values such as a `$LINENO`-equivalent
  until VM execution and emitted Bash share a portable source-location model.
- Kept background jobs, async/wait primitives, public job-control APIs,
  arbitrary signals, handler removal APIs, and shell-grade process-tree
  management out of scope.

# v0.22.5 - Foreground Pipeline Signal Runtime

- Extended foreground signal-runtime coverage from direct commands to simple
  foreground pipelines for both VM execution and emitted standalone Bash.
- Verified VM/Bash parity for `INT` and `TERM` pipelines, preserving statuses
  `130` and `143`.
- Verified pipeline cleanup ordering matches direct commands: matching signal
  trap, matching signal defers in LIFO order, then `EXIT` trap and `EXIT`
  defers in LIFO order.
- Fixed emitted Bash pipeline signal handling so signal exits route through ds
  cleanup instead of leaking Bash job-control messages such as `Terminated` or
  degrading into generic pipeline failures.
- Kept support limited to foreground pipeline statements; background pipelines,
  async/wait primitives, and shell-grade process-tree management remain out of
  scope.

# v0.22.4 - Foreground Direct-Command Signal Runtime

- Extended the v0.22 signal coverage to non-cooperative foreground direct
  commands for both `INT` and `TERM` in VM execution and emitted standalone
  Bash.
- Verified conventional signal statuses: `130` for `INT` and `143` for
  `TERM`.
- Verified cleanup ordering for direct-command signals: matching signal trap,
  matching signal defers in LIFO order, then `EXIT` trap and `EXIT` defers in
  LIFO order.
- Fixed emitted Bash direct-command handling so signal exits route through ds
  cleanup instead of leaking Bash job-control messages such as `Terminated`.
- Hardened the signal harness to launch runners through Python in a new process
  session instead of a shell background job, keeping `INT` trappable for
  emitted Bash tests.
- Fixed emitted Bash cleanup stack iteration and direct-command helper failure
  handling so non-exiting cleanup handler failures still allow older cleanup
  handlers to run.

# v0.22.3 - Deterministic Signal Harness

- Added a reusable v0.22 signal-test harness that runs VM and emitted-Bash
  scripts in isolated process sessions, waits for a deterministic `ready`
  marker, signals the process group, captures stdout/stderr/status through
  files, and kills leftovers on timeout.
- Proved the harness with the smallest direct-command `TERM` fixture in both
  VM execution and emitted standalone Bash.
- Kept the fixture cooperative so this slice validates harness determinism
  without expanding the broader signal-runtime matrix assigned to v0.22.4 and
  later slices.
- Fixed emitted Bash signal cleanup under `set -u` by avoiding same-command
  `local` initialization that referenced the signal name before assignment.

# v0.22.2 - Signal Syntax and Diagnostic Surface

- Extended the v0.22 focused suite with deterministic `INT`/`TERM` syntax
  coverage without adding real OS signal-delivery tests.
- Covered tokens, AST, HIR, bytecode/debug output, formatter normalization, and
  emitted-Bash helper structure for `defer on: "INT"`, `defer on: "TERM"`,
  `trap "INT"`, and `trap "TERM"`.
- Added diagnostic coverage for unsupported `defer on:` signals, unsupported
  `trap` signals, lowercase signal names, and malformed non-string signal
  syntax.
- Verified emitted Bash declares and registers signal-specific defer stacks,
  replacement trap slots, and standalone `INT`/`TERM` shell traps without
  calling back into `ds`.
- Kept foreground child interruption, process-group signaling, and asynchronous
  signal harnessing assigned to later v0.22 slices.

# v0.22.1 - Cleanup Core Test Stabilization

- Added the deterministic `tests/v0_22/run.sh` cleanup-core suite and wired it
  into aggregate version tests.
- Covered parser/token/AST/HIR/bytecode shape, formatter normalization,
  checker/emitter diagnostics, VM/Bash parity, imports, script args, function
  calls from handlers, and Bash-emission helper boundaries for the non-signal
  cleanup contract.
- Covered normal completion, explicit `exit`, explicit `fail`, direct command
  failure, captured command failure, `trap "EXIT"` replacement, `defer` LIFO
  ordering, handler failure continuation, handler `exit` status override, and
  normal completion skipping `INT`/`TERM` handlers.
- Fixed VM cleanup final-status preservation so explicit `exit N` is not
  overwritten by successful cleanup handlers.
- Fixed emitted Bash cleanup helper status tracking so an explicit handler
  `exit 0` is distinct from ordinary handler success and can intentionally
  override an earlier non-zero status.
- Kept real `SIGINT`/`SIGTERM` delivery tests out of this slice; those remain
  assigned to later v0.22 signal-harness/runtime slices.

# v0.22.0 - Process Control and Signal Handling

- Implemented process-level `defer` cleanup blocks for VM execution and emitted
  standalone Bash.
- Added `defer on: "EXIT"`, `defer on: "INT"`, and `defer on: "TERM"` for the
  scoped portable signal set.
- Added replacement-style `trap "EXIT"`, `trap "INT"`, and `trap "TERM"`
  handlers that run before matching defers.
- Preserved deterministic cleanup ordering: signal trap, signal defers in LIFO
  order, then EXIT cleanup.
- Hardened VM foreground command/pipeline interruption so `INT`/`TERM` during a
  long-running child process runs signal-specific cleanup and forwards the
  observed signal to the child process group when possible.
- Fixed explicit `fail`/`exit` parity for emitted Bash when cleanup handlers are
  present, including `exit 0` stopping execution before later statements.
- Rejected unsupported function-local handler captures and direct `return` from
  cleanup handlers before VM execution or Bash emission.
- Kept remaining cleanup running after a non-exiting handler fails, while
  preserving the failure status as the final status.
- Added VM bytecode/runtime support and standalone Bash cleanup helpers without
  adding calls back to `ds`.
- Updated docs for the implemented scope. The dedicated real-signal harness
  remains assigned to later v0.22 stabilization slices.

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
