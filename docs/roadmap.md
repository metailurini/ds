# Roadmap

This roadmap defines the intended path from project initialization to `1.0.0`.

The roadmap is allowed to change, but changes should be deliberate. The purpose of this document is to prevent drifting away from the original goal: a shell-native scripting language that is easier than Bash, interpreted by `ds`, and able to emit every supported feature to standalone Bash.

The `docs/language.ds` file is the companion syntax catalog for this roadmap. The roadmap explains when features are expected to arrive; `docs/language.ds` keeps the complete intended syntax visible in one place.

## Version format

Versions use:

```txt
major.minor.patch
```

Before `1.0.0`:

- `0.x.0` means planned milestone work.
- `0.x.y` means bug fix, docs fix, or test fix for `0.x.0`.
- `1.0.0` means the first stable and fully functional language release.

It is acceptable to have many pre-`1.0.0` versions, including `0.100.0`, before declaring stability.

## Version wave rule

Development follows a repeating four-version wave:

```txt
0.x.0     Feature foundation
0.x+1.0   Feature expansion
0.x+2.0   Feature integration / usability
0.x+3.0   Cleanup / debt repayment
```

Example:

```txt
0.1.0  feature foundation
0.2.0  feature expansion
0.3.0  feature integration
0.4.0  cleanup

0.5.0  feature foundation
0.6.0  feature expansion
0.7.0  feature integration
0.8.0  cleanup
```

Cleanup versions are not wasted time. They prevent the project from accumulating confusing names, duplicated logic, broken abstractions, and inconsistent behavior.

## Initial roadmap

This section is intentionally detailed. Each planned version should be clear enough that a future implementation pass can create the milestone spec and test plan without rediscovering the whole direction.

Each version below includes:

- **Purpose** — why the version exists.
- **Scope** — what should be implemented.
- **Out of scope** — what should not be pulled into the version.
- **Expected outputs** — docs, commands, tests, or examples that should exist when the version is complete.

### v0.1.0 — Lexer, Parser, AST, and Diagnostics

**Purpose:** build the first `ds` frontend and make source files understandable by the tool. This version proves that the language can be read, tokenized, parsed, and diagnosed before any execution backend exists.

**Scope:**

- Implement source loading for `.ds` files, including filename tracking and line/column lookup.
- Implement the lexer for comments, identifiers, keywords, strings, integers, booleans, braces, parentheses, operators, newlines, and command words.
- Implement a parser for the initial source subset: `let`, `if`, `else`, expression statements where needed, and command statements.
- Add the `docs/language.ds` syntax catalog and keep it clearly marked as non-runnable project documentation.
- Implement an AST model that preserves enough structure and source locations for debug output and future lowering.
- Implement diagnostics with filename, line, column, message, and useful source highlighting.
- Add debug commands that print tokens and AST output in a stable human-readable format.
- Add syntax checking through `ds check file.ds`.

**Out of scope:**

- No bytecode VM.
- No command execution.
- No Bash emitter.
- No imports.
- No functions.
- No formatter.
- No advanced type checking.

**Expected outputs:**

```sh
ds tokens file.ds
ds ast file.ds
ds check file.ds
```

- Lexer/parser golden fixtures for valid and invalid input.
- Example scripts showing the tiny v0.1.0 syntax subset.
- `docs/language.ds` checked in as the initial syntax inventory for current and planned syntax.
- Diagnostics documented well enough that future versions reuse the same style.

---

### v0.2.0 — Basic Bash Emitter

**Purpose:** prove early that `ds` can emit standalone Bash. Bash emission is a core product requirement, not a later bonus.

**Scope:**

- Add `ds emit bash file.ds -o file.sh`.
- Emit standalone Bash for the full v0.1.0 syntax subset.
- Generate a Bash shebang and strict-mode header.
- Emit safe internal Bash variable names for `let` bindings.
- Emit string, integer, and boolean literals in a safe Bash representation.
- Emit simple string interpolation such as `"Hello {name}"`.
- Emit `if` and `if/else` using Bash `[[ ... ]]` where possible.
- Emit command statements in the original execution order.
- Preserve useful source markers as comments in generated Bash.
- Produce clear diagnostics when a parsed feature cannot yet be emitted.

**Out of scope:**

- No VM yet.
- No imports.
- No functions.
- No advanced CLI argument parser.
- No arrays or maps.
- No process capture.
- No redirection sugar beyond command preservation.
- Generated Bash must not depend on the C runtime.

**Expected outputs:**

```sh
ds emit bash file.ds -o file.sh
bash -n file.sh
bash file.sh
```

- Bash emission golden tests using `input.ds` and `expected.sh` fixtures.
- Smoke tests proving generated Bash is standalone.
- Documentation explaining that every future feature must define Bash emission behavior.
- `docs/language.ds` updated if Bash emission changes the planned syntax shape.

---

### v0.3.0 — Minimal C Runtime and Bytecode VM

**Purpose:** add direct execution through `ds` while creating the minimal C runtime foundation needed by the VM and future features.

**Scope:**

- Add the first HIR lowering pass so the VM and Bash emitter can share a semantic representation.
- Define the initial bytecode format and register-based instruction model.
- Implement `DsValue` for the first runtime values: `null`, `bool`, `int`, and `string`.
- Implement minimal `DsStr` and `DsString` support for borrowed string views and owned dynamic strings.
- Implement minimal `DsArray` support where needed for internal compiler/runtime lists.
- Implement `DsMap` as the project-owned map wrapper, with raw hashmap details hidden behind the runtime boundary.
- Keep map implementation code under a project-owned runtime/core path instead of exposing a separate library subtree to the rest of the codebase.
- Implement basic process execution for command statements.
- Execute `let`, basic expressions, `if/else`, and simple command statements in VM mode.
- Add bytecode dump output for debugging.
- Add VM-vs-Bash parity tests for the supported v0.1/v0.2 behavior.

**Out of scope:**

- No advanced optimizer.
- No JIT.
- No native compiler.
- No arrays/maps as user-facing language values yet unless required internally.
- No imports.
- No command result capture yet.
- No regex user feature yet.

**Expected outputs:**

```sh
ds file.ds
ds run file.ds
ds bytecode file.ds
```

- Runtime unit tests for strings, arrays, maps, and values.
- VM integration tests for simple scripts.
- Parity tests comparing VM mode and emitted Bash mode for the same script.
- Updated `docs/runtime.md` if implementation decisions differ from the plan.

---

### v0.4.0 — Cleanup: Frontend, Runtime, and Backend Boundaries

**Purpose:** repay early technical debt before larger language features make the architecture harder to change.

**Scope:**

- Review and clean AST/HIR boundaries.
- Review and clean runtime ownership rules for strings, arrays, maps, and values.
- Review the `DsMap` wrapper over the absorbed hashmap implementation and remove direct hashmap dependency leaks from unrelated code.
- Keep the absorbed hashmap implementation private to the runtime/core boundary.
- Start the absorption if the `DsMap` API and ownership rules are stable enough; otherwise record the exact remaining blockers.
- Standardize source location and diagnostic APIs.
- Standardize golden test fixture layout.
- Remove duplicated behavior between the VM and Bash emitter.
- Clarify the compiler pipeline in architecture docs.
- Make error output consistent across `check`, `emit bash`, and `run`.

**Out of scope:**

- No major new syntax.
- No CLI args block.
- No imports.
- No command result capture.
- No user-facing arrays/maps.

**Expected outputs:**

- Cleaner internal APIs.
- Updated architecture/runtime docs.
- Regression tests proving no v0.1-v0.3 behavior was broken.
- A written list of deferred debt items for later cleanup versions.

---

### v0.5.0 — First-Class CLI Args

**Purpose:** make command-line argument parsing one of the first major advantages over Bash.

**Scope:**

- Add the `script { ... }` block.
- Support required positional args.
- Support named options with defaults.
- Support boolean flags.
- Support basic types in argument declarations: `string`, `int`, and `bool` where appropriate.
- Generate `--help` output automatically.
- Validate missing required args.
- Validate unknown options.
- Validate invalid option values, especially non-integer input for `int` options.
- Implement the same behavior in VM mode and emitted Bash mode.

**Expected syntax:**

```ds
script {
  arg app: string
  option target: string = "staging"
  option retries: int = 3
  flag force: bool = false
}
```

**Out of scope:**

- No subcommands.
- No shell completion.
- No config-file defaults.
- No environment-variable defaults unless they are trivial and explicitly added to the spec.
- No complex validation DSL.

**Expected outputs:**

```sh
ds examples/args.ds api --target production --force
ds examples/args.ds --help
ds emit bash examples/args.ds -o /tmp/args.sh
bash /tmp/args.sh api --target production --force
```

- CLI args spec and test plan.
- Examples focused on deployment-style scripts.
- VM/Bash parity tests for successful parsing and error cases.

---

### v0.6.0 — Imports / Includes

**Purpose:** allow scripts to share reusable definitions without introducing a package system too early.

**Scope:**

- Add simple file imports using `import "./file.ds"`.
- Resolve relative imports from the importing file's directory.
- Load each imported file once per program.
- Detect import cycles and report a readable import stack.
- Make imported definitions available to the importing script according to the simple v0.6 module rules.
- Bundle imported files into one standalone Bash file during Bash emission.

**Expected syntax:**

```ds
import "./lib.ds"
```

**Out of scope:**

- No package manager.
- No remote imports.
- No versioned dependencies.
- No named imports unless the milestone spec explicitly decides they are necessary.
- No public/private module visibility system.

**Expected outputs:**

```sh
ds run examples/import-main.ds
ds emit bash examples/import-main.ds -o /tmp/import-main.sh
bash /tmp/import-main.sh
```

- Implementation now covers relative local import composition, duplicate load-once behavior, missing-import diagnostics, cycles, VM execution, bytecode source mapping, and standalone Bash bundling.
- Dedicated v0.6 tests remain for the test-plan pass.

---

### v0.7.0 — Command Results and Redirection

**Purpose:** make command execution safer and clearer than Bash without losing shell-native behavior.

**Scope:**

- Add captured command execution using `run`.
- Add a command result value with `stdout`, `stderr`, `code`, `ok`, and `failed`.
- Make captured commands non-fatal by default so the script can inspect the result.
- Keep plain command statements fail-fast under strict behavior.
- Add redirection sugar for stdout, stderr, and combined output.
- Implement equivalent VM and Bash emission behavior.
- Add diagnostics for unsupported combinations or invalid redirection targets.

**Expected syntax:**

```ds
let result = run npm test

if result.failed {
  echo result.stderr
  exit result.code
}

npm run build &> "build.log"
```

**Expected redirections:**

```ds
cmd |> "out.txt"
cmd |>> "out.txt"
cmd !> "err.txt"
cmd !>> "err.txt"
cmd &> "all.txt"
cmd &>> "all.txt"
```

**Out of scope:**

- No async/background jobs.
- No pipelines as first-class result objects unless explicitly scoped.
- No command mocking framework.
- No advanced terminal control.

**Expected outputs:**

- Tests for stdout-only, stderr-only, mixed output, failed commands, empty output, and large output within reasonable limits.
- Bash emission tests showing correct conversion to `>`, `2>`, and `2>&1`.
- Parity tests for exit code behavior.

---

### v0.8.0 — Cleanup: Command Model and Bash Parity

**Purpose:** ensure command behavior is consistent before the language adds more control-flow and data features.

**Scope:**

- Audit all command execution paths: plain commands, captured commands, redirections, and emitted Bash.
- Standardize failed-command diagnostics.
- Standardize how stdout, stderr, and exit codes are represented in tests.
- Clean the C process execution wrapper.
- Clean Bash helper generation for captured commands and failure reporting.
- Strengthen VM-vs-Bash parity tests.
- Update docs to remove ambiguous command behavior.

**Out of scope:**

- No new command syntax unless required to fix an inconsistency.
- No background jobs.
- No new standard-library modules.

**Expected outputs:**

- A parity fixture suite that can run the same script in VM mode and emitted Bash mode.
- Cleaner process/runtime code.
- Updated command behavior docs.

---

### v0.9.0 — Functions

**Purpose:** add reusable script logic while keeping function behavior simple enough to emit cleanly to Bash.

**Scope:**

- Add function declarations.
- Add function calls from expression/statement contexts where appropriate.
- Support positional parameters.
- Support default parameters only if the milestone spec confirms the Bash emission remains simple.
- Validate duplicate parameter names.
- Validate wrong argument counts.
- Support local variables inside functions.
- Implement function calls in VM and Bash emission.

**Expected syntax:**

```ds
fn deploy(target) {
  echo "Deploying {target}"
}

deploy("production")
```

**Out of scope:**

- No closures.
- No anonymous functions.
- No higher-order functions.
- No recursion guarantee unless easy and tested.
- No named arguments unless explicitly scoped.

**Expected outputs:**

- Function parser, semantic, VM, and Bash emission tests.
- Examples showing reusable deploy/cleanup helpers.
- Docs warning about intentionally unsupported advanced function features.

---

### v0.10.0 — Arrays, Maps, and Loops

**Purpose:** add practical data structures and iteration for real scripts.

**Scope:**

- Add user-facing arrays.
- Add user-facing maps with string keys.
- Add array indexing.
- Add basic map field/key access.
- Add `for item in items { ... }` loops.
- Add `for key, value in map { ... }` only if Bash emission stays manageable.
- Add `while condition { ... }` loops.
- Implement arrays/maps in `DsValue` and the VM.
- Emit arrays/maps to Bash using Bash arrays and associative arrays.
- Require Bash 4+ for generated scripts that use associative arrays.

**Expected syntax:**

```ds
let services = ["api", "web"]

for service in services {
  echo $service
}

let ports = {
  api: 3000,
  web: 5173
}
```

**Out of scope:**

- No nested destructuring.
- No advanced collection methods.
- No ordered map guarantee unless explicitly implemented.
- No generic types.

**Expected outputs:**

- Runtime tests for arrays/maps.
- Bash emission tests requiring Bash 4+ where maps are used.
- Loop behavior tests, including empty arrays/maps.

---

### v0.11.0 — Shell-Oriented Standard Library

**Purpose:** provide boring, practical helpers that make scripts shorter and safer than equivalent Bash.

**Scope:**

- Add `file` helpers such as `exists`, `is_file`, `is_dir`, `read`, `write`, and `append` as scoped by the milestone spec.
- Add `dir` helpers where useful.
- Add `path` helpers such as `cwd`, `join`, `basename`, `dirname`, and `ext` as scoped.
- Add `cmd.exists` and `cmd.require`.
- Add `env` access and simple environment mutation rules.
- Add `glob("pattern")`.
- Add `lines("file.txt")` for safe line iteration.
- Ensure every builtin has both VM behavior and Bash emission behavior.

**Expected syntax:**

```ds
cmd.require("git")

if file.exists("package.json") {
  npm install
}

for line in lines("input.txt") {
  echo $line
}
```

**Out of scope:**

- No networking stdlib.
- No JSON unless moved into this version by an explicit spec amendment.
- No TOML/dotenv.
- No complex path normalization across all operating systems.
- No package ecosystem.

**Expected outputs:**

- Builtin registry in C.
- Bash helper emission for each builtin that needs helpers.
- VM/Bash parity tests for every supported builtin.
- Standard-library docs or a clearly marked draft.

---

### v0.12.0 — Cleanup: Standard Library and Type Consistency

**Purpose:** stabilize names and behavior before the standard library grows too much.

**Scope:**

- Review builtin names for consistency.
- Review return types and error behavior for every builtin.
- Standardize naming between VM internals, Bash helpers, and user-facing APIs.
- Standardize type conversions and reject surprising implicit conversions.
- Improve docs for supported types and builtins.
- Add missing edge-case tests for files, dirs, globs, lines, env, and command helpers.
- Rename awkward APIs before they become stable.

**Out of scope:**

- No large new stdlib areas.
- No syntax expansion unless required to fix a consistency issue.
- No performance rewrites unless they address real pain.

**Expected outputs:**

- A cleaner and more consistent stdlib surface.
- Updated docs and examples.
- Regression tests proving renamed or changed APIs behave as documented.

---

### v0.13.0 — Debugging and Tracing

**Purpose:** make `ds` easy to debug both while developing the language and while using the language for scripts.

**Scope:**

- Add HIR debug output.
- Improve bytecode debug output.
- Add command tracing similar in usefulness to Bash `set -x`, but cleaner.
- Add VM instruction tracing.
- Improve runtime errors with source spans.
- Add source markers and failure helpers to emitted Bash.
- Make parser/semantic/runtime diagnostics visually consistent.

**Expected commands:**

```sh
ds hir file.ds
ds bytecode file.ds
ds run --trace-cmd file.ds
ds run --trace-vm file.ds
```

**Out of scope:**

- No interactive debugger yet.
- No breakpoints.
- No step-through UI.
- No explain-mode natural language summary unless explicitly scoped later.

**Expected outputs:**

- Trace output tests where stable enough.
- Manual smoke examples showing how to debug a failed script.
- Updated architecture docs explaining debug surfaces.

---

### v0.14.0 — Test Runner

**Purpose:** allow `ds` users to test their scripts without needing an external test framework for simple cases.

**Scope:**

- Add `test "name" { ... }` blocks.
- Add `assert` for simple boolean checks.
- Add `ds test`.
- Ensure tests do not run during normal `ds script.ds` execution unless intentionally requested.
- Report passing/failing test names clearly.
- Support tests in imported files only according to simple, documented rules.

**Expected syntax:**

```ds
test "target defaults to staging" {
  assert "staging" == "staging"
}
```

**Out of scope:**

- No complex mocking framework.
- No snapshot testing.
- No property-based testing.
- No parallel test runner.
- No coverage tool.

**Expected outputs:**

```sh
ds test path/to/file.ds
```

Directory/no-argument discovery may be added later; the initial implementation
requires an explicit file path.

- Test runner docs.
- Self-test examples.
- Tests proving normal execution ignores test blocks.

---

### v0.15.0 — Formatter and Checker

**Purpose:** improve maintainability and consistency for real projects.

**Scope:**

- Add `ds fmt file.ds`.
- Add format rules for indentation, braces, spacing, blank lines, and simple expression layout.
- Keep formatter output stable and deterministic.
- Improve `ds check` with semantic warnings where practical.
- Add warnings for suspicious patterns, such as unused variables, unreachable branches, or impossible argument declarations, only if reliable.
- Add formatter golden tests.

**Expected commands:**

```sh
ds fmt file.ds
ds fmt --check file.ds
ds check file.ds
```

**Out of scope:**

- No configurable style system at first.
- No auto-fix for complex semantic issues.
- No linter plugin system.

**Expected outputs:**

- Formatter fixtures.
- Checker diagnostics tests.
- Updated contributing/development docs if needed.

---

### v0.16.0 — Cleanup: Pre-Beta Hardening

**Purpose:** pause feature work and make the language boring, consistent, documented, and safe enough for broader experimentation.

**Scope:**

- Review every syntax feature added so far.
- Remove or rename confusing features before users depend on them.
- Verify every supported feature has VM behavior, Bash emission behavior, tests, and docs.
- Run every example in both VM mode and emitted Bash mode.
- Review memory ownership in the runtime.
- Review parser and VM performance for obvious avoidable problems.
- Write a status document describing stable, experimental, and deferred areas.
- Update `1.0.0` readiness criteria based on what has been learned.

**Out of scope:**

- No major new language features.
- No backend expansion to zsh/fish yet.
- No native compiler.

**Expected outputs:**

- `docs/status.md` or equivalent.
- Cleaned docs and examples.
- Full regression/parity test pass.
- A clear next-wave plan.

## Later waves

Possible future areas:

- better module system;
- shell completion generation;
- JSON support;
- TOML or dotenv support;
- richer path APIs;
- REPL;
- interactive debugger;
- explain mode;
- zsh or fish emission backend.

These should be added only after the core VM/Bash architecture is proven.

## Not now

The following are intentionally out of early scope:

- classes;
- inheritance;
- async/await;
- package manager;
- native compiler;
- remote imports;
- complex macro system;
- advanced type system;
- plugin ABI;
- POSIX `sh` emission;
- Windows PowerShell backend.

They may be reconsidered later, but they should not distract from the initial goal.

## 1.0.0 criteria

`1.0.0` means the language is stable enough for real scripts.

Before `1.0.0`, `ds` should have:

- direct script execution with `ds ./script.ds`;
- standalone Bash emission;
- stable CLI argument syntax;
- imports;
- functions;
- commands, pipes, and redirections;
- useful filesystem and command standard-library helpers;
- clear diagnostics;
- debugging/tracing tools;
- formatter/checker;
- VM/Bash parity tests;
- documentation for all stable syntax;
- realistic example scripts that pass in both VM and Bash mode.

`1.0.0` should not mean "first working version." It should mean "safe enough to depend on."
