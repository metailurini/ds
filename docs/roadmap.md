# Roadmap

This roadmap defines the intended path from project initialization to `1.0.0`.

The roadmap is allowed to change, but changes should be deliberate. The purpose of this document is to prevent drifting away from the original goal: a shell-native scripting language that is easier than Bash, interpreted by `ds`, and able to emit every supported feature to standalone Bash.

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

### v0.1.0 — Lexer, Parser, AST, and Diagnostics

Build the first frontend.

Expected capabilities:

- source loading;
- lexer;
- parser;
- AST model;
- syntax diagnostics;
- token debug output;
- AST debug output;
- syntax checking command.

Primary CLI commands:

```sh
ds tokens file.ds
ds ast file.ds
ds check file.ds
```

No VM and no Bash emitter yet.

### v0.2.0 — Basic Bash Emitter

Prove that the language can emit standalone Bash early.

Expected capabilities:

- emit Bash for the `v0.1.0` subset;
- generate shebang and strict Bash header;
- emit safe variable names;
- emit simple `let`, `if`, `else`, and command statements;
- preserve source comments where useful;
- add golden tests for generated Bash.

Primary CLI command:

```sh
ds emit bash file.ds -o file.sh
```

### v0.3.0 — Minimal C Runtime and Bytecode VM

Add direct script execution through an internal register bytecode VM.

Expected capabilities:

- minimal C runtime foundation;
- `DsStr`, `DsString`, `DsArray`, and `DsMap` basics;
- `DsValue` for VM values;
- process execution wrapper for command statements;
- HIR lowering;
- bytecode format;
- bytecode dump command;
- VM execution for `let`, expressions, `if`, and command statements;
- VM/Bash parity tests for supported behavior.

Primary CLI commands:

```sh
ds file.ds
ds run file.ds
ds bytecode file.ds
```

### v0.4.0 — Cleanup: Frontend and Backend Boundaries

Repay early technical debt before adding larger features.

Expected work:

- clean AST/HIR boundaries;
- clean runtime ownership boundaries;
- review `DsMap`/hashmap wrapping decisions;
- standardize diagnostics;
- standardize source locations;
- standardize golden test format;
- document compiler pipeline;
- remove duplicated logic between VM and Bash emitter.

No major language features.

### v0.5.0 — First-Class CLI Args

Make argument parsing one of the first major advantages over Bash.

Expected syntax:

```ds
script {
  arg app: string
  option target: string = "staging"
  option retries: int = 3
  flag force: bool = false
}
```

Expected behavior:

- required positional args;
- options with defaults;
- boolean flags;
- generated `--help`;
- VM and Bash emitter parity.

### v0.6.0 — Imports / Includes

Allow scripts to share code.

Expected syntax:

```ds
import "./lib.ds"
```

Expected behavior:

- resolve imports relative to the importing file;
- detect cycles;
- load each imported file once;
- bundle imports into emitted Bash.

### v0.7.0 — Command Results and Redirection

Improve command execution ergonomics.

Expected syntax:

```ds
let result = run npm test

if result.failed {
  echo result.stderr
  exit result.code
}

npm run build &> "build.log"
```

Expected redirections:

```ds
cmd |> "out.txt"
cmd |>> "out.txt"
cmd !> "err.txt"
cmd !>> "err.txt"
cmd &> "all.txt"
cmd &>> "all.txt"
```

### v0.8.0 — Cleanup: Command Model and Bash Parity

Ensure all command behavior is consistent across VM mode and emitted Bash.

Expected work:

- compare stdout/stderr/exit codes;
- improve failed command diagnostics;
- clean process execution code;
- clean Bash helper generation;
- add parity tests.

### v0.9.0 — Functions

Add reusable script logic.

Expected syntax:

```ds
fn deploy(target) {
  echo "Deploying {target}"
}

deploy("production")
```

Non-goals:

- no closures;
- no anonymous functions;
- no higher-order functions.

### v0.10.0 — Arrays, Maps, and Loops

Add useful data structures and iteration.

Expected syntax:

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

Generated Bash may require Bash 4+ for associative arrays.

### v0.11.0 — Shell-Oriented Standard Library

Add practical helpers.

Expected modules or namespaces:

- `file`
- `dir`
- `path`
- `cmd`
- `env`
- `glob`
- `lines`

Example:

```ds
cmd.require("git")

if file.exists("package.json") {
  npm install
}

for line in lines("input.txt") {
  echo $line
}
```

### v0.12.0 — Cleanup: Standard Library and Type Consistency

Stabilize names and semantics before the standard library grows.

Expected work:

- review builtin names;
- standardize error messages;
- standardize Bash helper names;
- add stdlib docs;
- add VM/Bash parity tests for builtins;
- rename awkward APIs before they become stable.

### v0.13.0 — Debugging and Tracing

Make `ds` easy to debug.

Expected commands:

```sh
ds hir file.ds
ds bytecode file.ds
ds run --trace-cmd file.ds
ds run --trace-vm file.ds
```

Expected behavior:

- command traces;
- VM instruction traces;
- runtime errors with source spans;
- emitted Bash source markers.

### v0.14.0 — Test Runner

Allow `ds` projects to test scripts.

Expected syntax:

```ds
test "target defaults to staging" {
  assert "staging" == "staging"
}
```

Primary command:

```sh
ds test
```

### v0.15.0 — Formatter and Checker

Improve maintainability.

Expected commands:

```sh
ds fmt file.ds
ds check file.ds
```

Expected behavior:

- consistent indentation;
- consistent brace formatting;
- stable output;
- warnings for suspicious patterns where useful.

### v0.16.0 — Cleanup: Pre-Beta Hardening

Review everything before broader usage.

Expected work:

- review all syntax;
- remove or rename confusing features;
- verify every supported feature has docs and tests;
- run all examples in VM and Bash mode;
- write status documentation for stable vs experimental behavior.

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
