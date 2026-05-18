# ds

`ds` is an experimental shell-native scripting language. It is designed to keep the useful parts of Bash—commands, pipes, environment interaction, and scriptability—while replacing Bash's confusing syntax with a simpler, more readable language model.

The working name is temporary. The project is currently in pre-`1.0.0` planning and early implementation mode.

## Purpose

Bash is powerful, but many common tasks are painful or fragile:

- argument parsing is repetitive and error-prone;
- conditionals use confusing forms like `[ ... ]`, `[[ ... ]]`, `then`, and `fi`;
- redirection syntax such as `2>&1` is difficult to read;
- arrays and maps are awkward;
- quoting and word splitting are easy to get wrong;
- debugging failed scripts can be painful;
- refactoring large scripts is risky.

`ds` aims to provide a better scripting experience while remaining close to the shell.

## Core goals

- Run scripts directly with `ds ./script.ds`.
- Implement the `ds` tool in C.
- Interpret scripts rather than compile them to native binaries.
- Use an internal register bytecode VM for direct execution.
- Emit every supported feature to standalone Bash.
- Keep generated Bash independent: users should not need `ds` installed to run emitted scripts.
- Make command execution feel shell-native.
- Make programming logic feel like a normal readable language.
- Make CLI argument parsing a first-class feature.
- Make scripts easy to debug during both language development and normal usage.
- Keep the language simple, boring, testable, and easy to extend.

## Example direction

This is an example of the style `ds` is expected to move toward:

```ds
script {
  arg app: string
  option target: string = "staging"
  flag force: bool = false
}

cmd.require("git")
cmd.require("npm")

if !file.exists("package.json") {
  fail "package.json not found"
}

echo "Deploying {app} to {target}"

npm install
npm run build &> "build.log"

if target == "production" {
  npm test
}

./deploy.sh $app $target
```

The emitted Bash should be standalone and equivalent in behavior:

```sh
ds emit bash deploy.ds -o deploy.sh
bash deploy.sh api --target production --force
```

## Development model

Development follows pre-`1.0.0` milestone versions:

- `0.x.0` is a planned feature, integration, or cleanup version.
- `0.x.y` is reserved for bug fixes, documentation fixes, or test fixes for `0.x.0`.
- `1.0.0` is the first stable release where the supported language surface is expected to remain reliable.

Every planned version should have:

- a milestone spec;
- a test plan;
- implementation work;
- tests based on the test plan;
- docs and examples updates;
- completion review.

For the initial project, the first two planned versions are:

- `v0.1.0` — Lexer, parser, AST, diagnostics, and frontend debug commands.
- `v0.2.0` — Basic standalone Bash emitter for the `v0.1.0` language subset.

## Project layout

The project started with documentation only, then `v0.1.0` added the first frontend implementation.

Current implementation directories:

- `include/` — public internal C declarations for the early implementation;
- `src/` — source loading, diagnostics, lexer, parser, AST printer, shared lowering, bytecode/VM runtime, Bash emitter, and CLI entrypoint;
- `examples/` — small scripts used for manual frontend smoke checks;
- `libs/hashmap/` — temporary staging location for the owned hashmap code that should later be absorbed into `src/core` behind `DsMap`.

Important planning files:

Important files:

- `docs/language.ds`
- `docs/product-principles.md`
- `docs/roadmap.md`
- `docs/architecture.md`
- `docs/runtime.md`
- `docs/version-workflow.md`
- `docs/editor.md`
- `docs/milestones/v0.1.0-spec.md`
- `docs/milestones/v0.1.0-test-plan.md`
- `docs/milestones/v0.2.0-spec.md`
- `docs/milestones/v0.2.0-test-plan.md`
- `docs/milestones/v0.3.0-spec.md`
- `docs/milestones/v0.3.0-test-plan.md`
- `docs/milestones/v0.4.0-spec.md`
- `docs/milestones/v0.4.0-test-plan.md`
- `docs/milestones/v0.5.0-spec.md`
- `docs/milestones/v0.5.0-test-plan.md`
- `docs/milestones/v0.6.0-spec.md`
- `docs/milestones/v0.6.0-test-plan.md`
- `docs/milestones/v0.7.0-spec.md`
- `docs/milestones/v0.7.0-test-plan.md`
- `docs/milestones/v0.8.0-spec.md`
- `docs/milestones/v0.8.0-test-plan.md`

## Editor / LSP setup

`compile_flags.txt` is checked in so `clangd` can resolve project headers and use
the same baseline flags as the Makefile build when the workspace is opened in an
IDE. See `docs/editor.md` for notes on `clangd` and local Neovim `lua_ls` setup.

## Project status

Current status: `v0.9.0` implementation and tests are complete for the scoped user-defined functions pass; `v0.10.0` implementation and tests are complete for the scoped arrays, maps, and array-loop pass.

The current implementation supports the `v0.1.0` frontend, the `v0.2.0` Bash emission path, the first `v0.3.0` direct VM execution path, the `v0.4.0` internal cleanup pass, the first `v0.5.0` script argument contract, the initial `v0.6.0` local import composition path, the initial `v0.7.0` command-result/redirection path, the `v0.8.0` command-model/process-wrapper cleanup, the scoped `v0.9.0` user-defined functions pass, and the initial `v0.10.0` collection/array-loop implementation:

Local imports use simple quoted paths resolved relative to the importing file:

```ds
import "./lib.ds"
```

Command results capture stdout, stderr, and exit status without making non-zero
captured exits fatal:

```ds
let result = run npm test

if result.failed {
  echo result.stderr
  exit result.code
}
```

Plain command statements also support readable redirection syntax:

```ds
npm run build &> "build.log"
```

Functions are top-level reusable procedures with positional parameters, trailing literal defaults, local function scopes, and statement-style calls:

```ds
fn greet(name = "world") {
  echo "hello {name}"
}

greet()
greet("ds")
```

Collections support array literals, array indexing, `push`, string-keyed map literals, map access, and array iteration:

```ds
let services = ["api", "web"]
services.push("worker")

for service in services {
  echo "service={service}"
}

let ports = { api: 3000, web: 5173 }
let api_port = ports.api
```

`v0.10.0` deliberately defers map iteration, `while`, `break`/`continue`, ranges, index assignment, empty map literals, nested collections, and passing whole collection values to functions or commands. Bind/index scalar values instead.

```sh
make
./ds tokens examples/basic.ds
./ds ast examples/basic.ds
./ds check examples/basic.ds
./ds bytecode examples/basic.ds
./ds run examples/basic.ds
./ds examples/basic.ds
./ds examples/vm.ds
./ds examples/args.ds api --target production --retries 5 --force
./ds examples/args.ds --help
./ds examples/import-main.ds
./ds examples/command-result.ds
./ds examples/redirection.ds
./ds examples/functions.ds
./ds examples/collections.ds
./ds emit bash examples/basic.ds -o /tmp/basic.sh
./ds emit bash examples/args.ds -o /tmp/args.sh
./ds emit bash examples/import-main.ds -o /tmp/import-main.sh
./ds emit bash examples/command-result.ds -o /tmp/command-result.sh
./ds emit bash examples/redirection.ds -o /tmp/redirection.sh
./ds emit bash examples/functions.ds -o /tmp/functions.sh
./ds emit bash examples/collections.ds -o /tmp/collections.sh
bash -n /tmp/basic.sh
bash /tmp/basic.sh
bash /tmp/args.sh api --target production --retries 5 --force
bash /tmp/args.sh --help
bash /tmp/import-main.sh
bash /tmp/command-result.sh
bash /tmp/redirection.sh
bash /tmp/functions.sh
bash /tmp/collections.sh
```

The CLI now centralizes source loading, import resolution, lexing, parsing, and lowering so `check`, `emit bash`, `run`, direct script execution, and `bytecode` all share the same composed parse/lower path. `script { ... }` declarations introduce first-class positional args, options with defaults, and boolean flags for VM execution and standalone emitted Bash. The VM and Bash emitter consume the same lowered program representation for the conservative language subset: `let`, strings, integers, booleans, simple interpolation, comparisons, `if`/`else`, nested blocks, simple command statements, captured `run` commands, command-result fields, plain command redirections, top-level function declarations/calls, array/map literals, array/map access, array `push`, and array `for` loops. The VM also maintains runtime block scopes for lowered blocks, function invocations, and loop iterations. The v0.7.0 implementation supports `result.stdout`, `result.stderr`, `result.code`, `result.ok`, and `result.failed`; captured non-zero commands are inspectable instead of fatal, while plain commands remain fail-fast. The v0.8.0 cleanup centralizes lowered command ownership and command-result field metadata without changing that language surface. The v0.9.0 implementation supports untyped function parameters, trailing literal defaults, calls before declaration, imported function declarations through the existing composition path, and local function variables; typed parameters, return values, and recursive calls remain deferred. The v0.10.0 implementation uses value-copy collection assignment semantics, emits a Bash 4+ guard when maps require associative arrays, gives explicit Bash runtime failures for missing map keys and out-of-range array indexes, and deliberately keeps collection function arguments, map iteration, and `while` loops deferred until call-boundary/reassignment semantics are designed.

Known `v0.2.0` Bash-emission limitation, now mirrored by the first VM: comparison operators intentionally use string-style semantics and do not perform type-aware numeric dispatch yet. Type-aware numeric dispatch remains deferred until the language has a fuller semantic model.

## Syntax catalog

`docs/language.ds` is the project-wide syntax inventory.

It is intentionally not a runnable script. It exists to keep the full planned language surface visible in one place and to make syntax drift obvious. Milestone specs define what is actually implemented in each version; `docs/language.ds` shows the broader direction and marks syntax by planned version/status.

When syntax changes, update `docs/language.ds` together with the relevant milestone spec, test plan, roadmap entry, and examples.
