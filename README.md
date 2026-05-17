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

## Editor / LSP setup

`compile_flags.txt` is checked in so `clangd` can resolve project headers and use
the same baseline flags as the Makefile build when the workspace is opened in an
IDE. See `docs/editor.md` for notes on `clangd` and local Neovim `lua_ls` setup.

## Project status

Current status: `v0.4.0` implementation is complete and tests are complete for the scoped cleanup/refactor pass. Existing `v0.1.0` through `v0.3.0` regression suites continue to pass, and `v0.4.0` adds cleanup-focused regression coverage for pipeline boundaries, diagnostics, source locations, runtime ownership, `DsMap`, generated Bash standalone behavior, and staged-library boundaries.

The current implementation supports the `v0.1.0` frontend, the `v0.2.0` Bash emission path, the first `v0.3.0` direct VM execution path, and the `v0.4.0` internal cleanup pass:

```sh
make
./ds tokens examples/basic.ds
./ds ast examples/basic.ds
./ds check examples/basic.ds
./ds bytecode examples/basic.ds
./ds run examples/basic.ds
./ds examples/basic.ds
./ds examples/vm.ds
./ds emit bash examples/basic.ds -o /tmp/basic.sh
bash -n /tmp/basic.sh
bash /tmp/basic.sh
```

The CLI now centralizes source loading, lexing, parsing, and lowering so `check`, `emit bash`, `run`, direct script execution, and `bytecode` all share the same parse/lower path. The VM and Bash emitter consume the same lowered program representation for the conservative `v0.1.0` / `v0.2.0` language subset: `let`, strings, integers, booleans, simple interpolation, comparisons, `if`/`else`, nested blocks, and simple command statements. The VM also maintains runtime block scopes for lowered blocks. The v0.3 test suite covers runtime primitives, direct lowering shape checks, bytecode dumps, source-map/jump edges, VM execution, command failures, diagnostics, and VM/Bash parity.

Known `v0.2.0` Bash-emission limitation, now mirrored by the first VM: comparison operators intentionally use string-style semantics and do not perform type-aware numeric dispatch yet. Type-aware numeric dispatch remains deferred until the language has a fuller semantic model.

## Syntax catalog

`docs/language.ds` is the project-wide syntax inventory.

It is intentionally not a runnable script. It exists to keep the full planned language surface visible in one place and to make syntax drift obvious. Milestone specs define what is actually implemented in each version; `docs/language.ds` shows the broader direction and marks syntax by planned version/status.

When syntax changes, update `docs/language.ds` together with the relevant milestone spec, test plan, roadmap entry, and examples.
