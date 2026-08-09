# Roadmap

`ds` is currently pre-1.0 and reports the `v0.38.0` implementation surface.
This file describes future direction. Completed milestone detail belongs in
`docs/milestones/` and `CHANGELOG.md`.

## Current baseline

The implemented baseline already includes the core shell-native language shape,
VM execution, standalone Bash emission, functions, control flow, flat
collections, lightweight rows, regex helpers, recursive globs and walks,
formatter/checker tooling, diagnostics, and VM/Bash parity coverage.

For exact supported and deferred behavior, see `docs/status.md`.

## Near-term priorities

### Regex capture ergonomics

The current flat numbered-capture map is useful but awkward for extraction-heavy
scripts. Explore a clearer capture API without widening the portable regex
subset or creating VM/Bash differences.

### Signal and job-control follow-up

The existing cleanup and signal surface is intentionally limited. Future work
may include clearer handler context, scoped background jobs, wait primitives,
and foreground job-control integration.

Any addition must have explicit process ownership and deterministic tests.

### Structured values beyond lightweight rows

Rows solve a useful small-data reporting case, but nested collections and richer
schemas are still outside the current boundary. Expand only when there is a
portable representation for both VM and standalone Bash.

## Later possibilities

- a better module system;
- shell completion generation;
- JSON support;
- TOML or dotenv support;
- richer path and filesystem APIs;
- streaming iterators;
- a REPL;
- an interactive debugger;
- explain mode;
- additional shell emission backends such as zsh or fish.

These should follow proven VM/Bash architecture rather than expanding the
surface faster than it can be maintained.

## Not now

The following are intentionally outside the early scope:

- classes and inheritance;
- async/await;
- a package manager;
- a native compiler;
- remote imports;
- a complex macro system;
- an advanced type system;
- a plugin ABI;
- POSIX `sh` emission;
- a Windows PowerShell backend.

## 1.0 criteria

Before `1.0.0`, the project should have a coherent and documented scripting
surface that is safe enough to depend on. In particular:

- direct script execution and standalone Bash emission;
- stable script argument syntax and imports;
- functions with portable supported return values;
- deterministic collection behavior;
- commands, pipelines, and redirections;
- practical filesystem, path, environment, string, and regex helpers;
- a documented cleanup and signal subset;
- clear diagnostics and useful debugging tools;
- formatter and checker behavior that does not destroy source information;
- VM/Bash parity for every supported production feature;
- realistic examples that pass through both execution modes;
- documentation that clearly separates supported, deferred, and rejected
  behavior.

`1.0.0` should mean stable enough for real scripts, not merely the first version
that works.

## Historical milestones

Detailed completed milestone specs and test plans remain under
`docs/milestones/`. They are historical records, not a second current-status
document and not executable test fixtures.