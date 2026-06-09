# ds

`ds` is an experimental shell-native scripting language written in C. It keeps
commands, pipes, environment access, and standalone Bash compatibility, but gives
script logic a simpler language model than raw Bash.

The project is pre-`1.0.0`. The current implementation is the `v0.38.0` scoped
surface: direct VM execution, checking/formatting/debug views, tests, and
standalone Bash emission for the supported language subset.

## Why this exists

Bash is useful, but large scripts become fragile because quoting, arrays,
conditionals, redirection, errors, and refactors are hard to reason about. `ds`
tries to make those parts boring while still feeling like a shell.

## Tiny example

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

Emit standalone Bash:

```sh
./ds emit bash deploy.ds -o deploy.sh
bash deploy.sh api --target production --force
```

## What works today

The supported production surface includes:

- `ds <file.ds>`, `ds run`, `ds check`, `ds fmt`, `ds test`, debug views, and
  `ds emit bash`;
- strings, integers, booleans, interpolation, `if`/`else`, `while`, `case`,
  `break`, `continue`, and integer arithmetic;
- shell commands, readable redirections, plain/captured pipelines, and captured
  command-result fields;
- script args/options/flags and local `import "./file.ds"` composition;
- top-level functions with defaults, statement calls, scalar returns, arrays,
  maps, lightweight rows, and row arrays;
- shell-oriented helpers for files, dirs, paths, commands, env, globs, recursive
  file walks, lines, strings, and regex;
- VM/Bash parity for the supported subset: emitted Bash should be standalone and
  should not require the `ds` binary at runtime.

For exact guarantees, deferred items, and edge-case behavior, read
[`docs/status.md`](docs/status.md).

## Quick start

```sh
make
./ds check examples/basic.ds
./ds run examples/basic.ds
./ds emit bash examples/basic.ds -o /tmp/basic.sh
bash -n /tmp/basic.sh
bash /tmp/basic.sh
make test-v0-38
```

Useful commands:

```sh
./ds fmt --check examples/args.ds
./ds hir examples/basic.ds
./ds bytecode examples/basic.ds
./ds run --trace-cmd examples/basic.ds
DS_TRACE_CMD=1 bash /tmp/basic.sh
```

Run everything:

```sh
make test
```

## Repository map

- `src/` — lexer/parser, lowering, checker, formatter, VM, stdlib, Bash emitter,
  and CLI implementation.
- `include/` — public internal declarations.
- `examples/` — small scripts for manual smoke checks.
- `tests/` — milestone regression and parity suites.
- `docs/` — language catalog, status, architecture, runtime notes, roadmap,
  milestones, release checklist, and technical-debt DB.

## Important docs

- [`docs/status.md`](docs/status.md) — current support matrix.
- [`docs/language.ds`](docs/language.ds) — full planned syntax catalog.
- [`docs/roadmap.md`](docs/roadmap.md) — milestone direction.
- [`docs/architecture.md`](docs/architecture.md) — implementation boundaries.
- [`docs/runtime.md`](docs/runtime.md) — runtime behavior and parity contracts.
- [`docs/version-workflow.md`](docs/version-workflow.md) — milestone process.
- [`docs/technical-debt.md`](docs/technical-debt.md) — known technical debt.

## Development model

Milestones are pre-`1.0.0` slices:

- `0.x.0` is a planned feature, integration, or cleanup pass.
- `0.x.y` is a focused fix/test/doc pass for `0.x.0`.
- `1.0.0` is the first stable release boundary.

Every completed milestone should leave specs, tests, docs, VM behavior, and Bash
emission aligned for the scoped language surface.