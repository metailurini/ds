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

## Initial documentation

The project starts with documentation only. The initial docs define the purpose, architecture, workflow, and first two milestones.

Important files:

- `docs/product-principles.md`
- `docs/roadmap.md`
- `docs/architecture.md`
- `docs/runtime.md`
- `docs/version-workflow.md`
- `docs/milestones/v0.1.0-spec.md`
- `docs/test-plans/v0.1.0-test-plan.md`
- `docs/milestones/v0.2.0-spec.md`
- `docs/test-plans/v0.2.0-test-plan.md`

## Project status

Current status: planning initialized.

No implementation code exists yet. The project intentionally begins with docs so the implementation can follow a clear path without drifting away from the original purpose.
