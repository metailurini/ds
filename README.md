# ds

`ds` is an experimental shell-native scripting language written in C. It is
designed for scripts that still need to feel like shell programs, but should be
easier to read, validate, test, and refactor than large Bash scripts.

The project is pre-1.0. The current implementation reports itself as `v0.38.0`.
The supported surface can run directly in the VM or emit standalone Bash for
the same supported language subset.

## What ds is for

Use `ds` when a script needs normal shell operations such as commands, pipes,
redirections, environment access, and filesystem helpers, but also benefits
from clearer control flow and structured values.

The project aims to keep these properties:

- shell commands remain first-class language statements;
- common scripting mistakes are rejected before execution when practical;
- the VM and Bash emitter consume the same lowered program model;
- emitted Bash is standalone and does not require the `ds` binary at runtime;
- language and runtime behavior stay intentionally small before 1.0.

## Example

```ds
script {
  arg app: string
  option target: string = "staging"
}

cmd.require("git")

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

Run it directly:

```sh
./ds deploy.ds api --target production
```

Or emit standalone Bash:

```sh
./ds emit bash deploy.ds -o deploy.sh
bash deploy.sh api --target production
```

## Build and test

The project builds with `make` and a C compiler:

```sh
make
make test
```

For a quick smoke check:

```sh
./ds examples/basic.ds
./ds check examples/basic.ds
./ds emit bash examples/basic.ds -o /tmp/basic.sh
bash -n /tmp/basic.sh
bash /tmp/basic.sh
```

The Makefile also exposes per-milestone regression targets such as
`make test-v0-38`.

## CLI

The current public command surface includes:

```text
ds <file.ds> [args...]
ds run [--trace-cmd] [--trace-vm] <file.ds> [args...]
ds test <file.ds>
ds check <file.ds> [--warnings-as-errors] [--no-warnings]
ds fmt <file.ds> [--check] [--write|-w]
ds tokens <file.ds>
ds ast <file.ds>
ds hir <file.ds>
ds bytecode <file.ds>
ds emit bash <file.ds> -o <file.sh>
```

`tokens`, `ast`, `hir`, and `bytecode` are development and debugging views.
For exact command behavior, supported syntax, deferred features, and edge cases,
use the current-status documentation rather than milestone history.

## Supported language shape

The current implementation includes shell commands and pipelines, command
results, readable redirections, script arguments, local imports, functions,
control flow, arrays, maps, lightweight rows, integer arithmetic, filesystem and
path helpers, environment helpers, string helpers, globs, recursive walks,
ranges, regex matching and replacement, cleanup handlers, and a test runner.

Support is deliberately scoped. Some syntax in the language catalog is planned
or deferred rather than implemented. The formatter also rejects comment-bearing
files instead of silently dropping comments.

See [`docs/status.md`](docs/status.md) for the authoritative support matrix.

## Repository layout

- `src/` contains the lexer, parser, lowering, checker, formatter, VM, Bash
  emitter, standard library, and CLI implementation.
- `include/` contains shared declarations used across implementation phases.
- `examples/` contains small scripts for manual and regression checks.
- `tests/` contains executable behavior and VM/Bash parity regressions.
- `docs/` contains current language, runtime, architecture, diagnostics, and
  contributor references, plus historical milestone records.

## Documentation

Start with these documents:

- [`docs/status.md`](docs/status.md): what works now and what is deferred.
- [`docs/language.ds`](docs/language.ds): syntax catalog and language direction.
- [`docs/architecture.md`](docs/architecture.md): compiler and backend boundaries.
- [`docs/runtime.md`](docs/runtime.md): runtime representation and behavior.
- [`docs/diagnostics.md`](docs/diagnostics.md): diagnostic ownership and rules.
- [`docs/source-map.md`](docs/source-map.md): source-file responsibilities.
- [`docs/concept-map.md`](docs/concept-map.md): cross-cutting concept ownership.
- [`docs/roadmap.md`](docs/roadmap.md): future direction and release criteria.

Files under `docs/milestones/` are historical implementation records. Runtime
tests do not treat documentation wording as an executable contract.

## Contributing

Keep behavior changes aligned across lowering, VM execution, Bash emission, and
tests. Prefer putting implementation logic in `.c` files and keeping headers
focused on declarations and small representation-level definitions.

Before adding a new helper or wrapper, check whether the existing phase owner
already provides the needed operation. Prefer one clear reusable implementation
over layers of pass-through wrappers.

Run `make check` and the relevant regression suites before considering a change
complete.
