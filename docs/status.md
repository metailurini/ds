# Current Status

This document is the user-facing snapshot after the completed `v0.20.0` Wave 2
stabilization cleanup pass, the completed `v0.21.0` function-value/arithmetic
implementation, the initial `v0.22.0` process-cleanup implementation pass, and
the `v0.22.1` deterministic cleanup-core test stabilization pass. It is not a replacement for the roadmap or
language catalog; it summarizes what users can rely on today and what is still
deliberately deferred.

## Command support

The public CLI currently supports these commands:

```sh
ds <file.ds> [args...]
ds run [--trace-cmd] [--trace-vm] <file.ds> [args...]
ds test <file.ds>
ds tokens <file.ds>
ds ast <file.ds>
ds check <file.ds> [--warnings-as-errors] [--no-warnings]
ds fmt <file.ds> [--check] [--write|-w]
ds hir <file.ds>
ds bytecode <file.ds>
ds emit bash <file.ds> -o <file.sh>
```

`tokens` and `ast` are root-file frontend/debug views. They read only the file
passed on the command line and show the import statement as syntax instead of
composing imported files.

`check`, `hir`, `bytecode`, `run`, direct script execution, `test`, and
`emit bash` use the import-aware composed program path. Local imports are loaded
relative to the importing file, loaded once per root program, and placed before
the importing file's executable statements. `fmt` formats the root source file
that was passed to it; it does not rewrite imported files or format a workspace.

## Production language support

The production runtime supports the language slice implemented and stabilized
through the `v0.22.1` deterministic cleanup-core pass:

- line comments in normal parsing/checking/running/emission;
- `let` declarations with strings, integers, booleans, identifiers, unary and
  binary expressions, field access, indexing, and supported calls;
- `if`/`else` blocks and nested block scopes;
- shell-native command statements;
- shell-native plain command pipelines such as `cat log | grep ERROR | sort`;
- captured command results with `run` expressions and `stdout`, `stderr`,
  `code`, `ok`, and `failed` fields;
- captured `run` pipelines with pipefail-style status in the same
  command-result fields;
- readable redirections for plain command statements: `|>`, `|>>`, `!>`,
  `!>>`, `&>`, and `&>>`;
- top-level `fn` declarations with positional parameters and trailing literal
  defaults;
- statement-style function calls and scalar value-returning function calls in
  expressions;
- `return expr` inside function bodies for scalar string/int/bool values, with
  conservative same-kind/all-paths validation before a function can be used as a
  value;
- array literals, map literals with string-like keys, array/map access, array
  `push`, and array `for` loops;
- scalar reassignment with `name = expr` plus integer `+=`, `-=`, `*=`, `/=`, and
  `%=` updates;
- integer arithmetic `+`, `-`, `*`, `/`, `%`, `**`, and unary `-` in supported
  value expressions;
- `while` loops with normal expression conditions;
- lexical `break` and `continue` inside `for` and `while` loops;
- expression-style `case selector { ... }`, for example `case target { ... }`,
  with kind-aware exact string/int/bool literal alternatives and a final `_`
  default arm;
- `script { ... }` positional args, options, and boolean flags;
- local `import "./file.ds"` composition;
- shell-oriented helpers from `file`, `dir`, `path`, `cmd`, `env`, `glob`,
  `glob!`, and `lines` within the scoped standard-library surface;
- ASCII string methods: `.trim()`, `.upper()`, `.lower()`, `.replace()`,
  `.contains()`, `.split()`, `.starts_with()`, and `.ends_with()`, including
  known string-array elements from `split`, `lines`, `glob`, and `glob!` when
  those values are indexed or iterated;
- kind-aware Bash `case` parity for known indexed array values and array `for`
  loop variables with known string/int/bool element kinds;
- stackable `defer { ... }` and `defer on: "EXIT"|"INT"|"TERM" { ... }` cleanup handlers, plus replacement-style `trap "EXIT"|"INT"|"TERM" { ... }`;
- formatted string interpolation with scoped string/int specifiers and
  triple-quoted multi-line string literals.

Every supported production feature is expected to run in the VM and emit
standalone Bash. Generated Bash must not call the `ds` binary or depend on the C
runtime to execute.

`case $target { ... }` is not valid expression syntax; `$name` remains reserved
for command arguments.

## Test-only syntax

`test "name" { ... }` blocks and `assert expr` are test-runner features.
`ds test <file.ds>` lowers tests and executes them through the VM backend.
Normal `ds run`, direct script execution, and normal `emit bash` ignore test
blocks. Inside tests, `fail "message"` fails the active test, `exit 0` stops it
as a pass, and non-zero `exit` fails it.

## Formatter and checker behavior

`ds fmt` is deterministic for the currently supported formatter surface:

```sh
ds fmt file.ds
ds fmt --check file.ds
ds fmt --write file.ds
ds fmt -w file.ds
```

The formatter uses two-space indentation, same-line opening braces, stable
spacing around operators, and stable blank lines between top-level groups.

Comment-preserving formatting remains deferred in this implementation pass. The
lexer/parser accept comments for normal language behavior, but `ds fmt` rejects
comment-bearing files with a clear diagnostic rather than silently dropping
trivia. Inline trailing comments are also rejected by the formatter for the same
reason. This is the deliberate `v0.16.0` decision: retaining trivia safely needs
a larger parser/formatter design than the cleanup implementation should risk.

`ds check` emits conservative warnings for supported cases, including unused
locals/parameters, unreachable test statements, and shadowing. Warnings are
non-fatal by default, become fatal with `--warnings-as-errors`, and are hidden by
`--no-warnings`. Combining `--warnings-as-errors` and `--no-warnings` is invalid.
Checking and formatting are static tooling paths; they must not execute user
commands in the source file.

## Examples

The current examples are the public tour of implemented behavior:

- `examples/basic.ds`
- `examples/args.ds`
- `examples/import-main.ds` and `examples/import-lib.ds`
- `examples/command-result.ds`
- `examples/redirection.ds`
- `examples/functions.ds`
- `examples/collections.ds`
- `examples/control-flow.ds`
- `examples/pipeline.ds`
- `examples/strings.ds`
- `examples/stdlib.ds`
- `examples/vm.ds`
- `examples/function-values.ds`
- `examples/bad.ds` for an intentionally invalid diagnostic example

Use `ds check`, `ds run`, direct script execution, `ds test` where applicable,
and `ds emit bash` to inspect parity. Bash output is intended to be standalone.

## Deferred language features

The following remain intentionally unsupported or backend-limited until later
milestones:

- recursive-call semantics;
- collection/command-result function returns and typed return annotations;
- `until`, loop `else`, and labeled/depth-based `break`/`continue`;
- regex/glob/destructuring/fallthrough `case` behavior;
- string binary `+` concatenation; use interpolation instead;
- regex and membership operators;
- ranges, slices, index assignment, and nested collections;
- map iteration;
- passing whole collection values directly to functions or commands;
- direct collection access inside command words without first binding a scalar;
- empty map literal inference and empty map keys;
- direct `env.NAME` access or assignment;
- recursive `**` glob patterns;
- binary file helpers and streaming `lines`;
- formatter configuration, warning suppression comments, workspace formatting,
  and range formatting;
- editor/LSP integration;
- arbitrary signal names, numeric signal syntax, handler removal, and job-control/process-group APIs;
- additional shell backends or native compilation.

Unsupported forms should fail clearly instead of partially working in only one
backend.

## Internal cleanup state

`v0.16.0` splits CLI source/import composition into `src/cli_program.c` and
`src/cli_program.h`, leaving `src/main.c` focused on argument parsing and public
command dispatch. The new boundary owns source loading, root-file lex/parse,
composed import-aware parse, lowering, import cycle/load-once diagnostics, and
cleanup of loaded units.

Raw hashmap implementation details remain hidden under `src/runtime/` behind the
`DsMap` runtime abstraction. `include/ds.h` remains a compatibility umbrella,
while internal code should prefer focused headers.

## Next wave

`v0.17.0` completed the scoped control-flow wave: `while`, `break`,
`continue`, scalar reassignment, and expression-style `case`, with VM/Bash
parity and clear interaction with existing block and loop scopes.

`v0.18.0` added linear command pipelines for plain command statements and
captured `run` expressions. Pipeline status uses Bash `pipefail` semantics: if
any stage fails, the pipeline status is the rightmost failing stage.

`v0.19.0` added ASCII string methods, formatted interpolation, and
triple-quoted strings. Format widths and precisions are bounded to `1..1024`.
`v0.20.0` stabilizes Wave 2 composition by keeping known array element kinds in
the lowerer and by making Bash helper dependency scanning recurse through call
arguments. This means values indexed out of known string arrays, such as
`"a,b".split(",")[0]`, can participate in scoped string methods with VM/Bash
parity and emitted helper coverage.

`v0.21.0` adds the implementation path for scalar function `return` values and
integer arithmetic: `return expr` inside functions, function calls as supported
value expressions, `*`, `/`, `%`, `**`, unary `-`, and integer `*=`, `/=`, `%=`
compound assignments. Functions used as values must have explicit compatible
scalar returns on all statically-known paths, including supported forward calls
to later value-returning functions. The VM and emitted Bash diagnose checked
integer overflow instead of silently wrapping. Functions called as expression
values reject plain command statements so expression-style calls cannot collide
with the return transport through arbitrary stdout; statement-style calls may
still stream stdout and ignore returned scalar values. Use captured `run`
expressions inside value functions when command output should participate in the
returned value. Expression-backed string interpolation can include
scalar value-returning calls. Command-word interpolation supports the legacy
`{name}`/`{name.field}` forms plus integer arithmetic expressions; direct
function-call interpolation in command words still emits a targeted diagnostic
telling callers to bind the string expression first. Statement-style calls may
still ignore returned values. `examples/function-values.ds` shows the supported
return, arithmetic, and expression interpolation path.
The dedicated `tests/v0_21/run.sh` suite now covers the scoped VM/Bash parity,
diagnostic, formatting, example, and generated-Bash boundary behavior.

`v0.22.0` adds process-level cleanup registration. Plain `defer` is an `EXIT` cleanup and runs in LIFO order. `defer on:` supports the literal signals `EXIT`, `INT`, and `TERM`; repeated `trap` statements use replacement semantics per signal. `v0.22.1` stabilizes the deterministic non-signal cleanup core with VM/Bash parity tests for normal completion, explicit `exit`, explicit `fail`, direct command failure, captured command failure, `trap "EXIT"` replacement, handler failure continuation, handler `exit` status override, imports, script args, and function calls from handlers. On signal dispatch the trap runs first, then matching defers in LIFO order, then `EXIT` cleanup. The VM installs lightweight `INT`/`TERM` handlers, checks for pending signals between bytecode instructions, and treats interrupted foreground commands/pipelines as signal cleanup events while forwarding observed `INT`/`TERM` to the foreground child process group when possible. Emitted Bash installs standalone traps. Real OS signal harnessing remains split into later v0.22 stabilization slices; background jobs, public job-control/process-group APIs, and broad signal-forwarding semantics outside foreground commands remain out of scope.

Because required function parameters remain untyped until the function-value wave, string methods and formatted interpolation require a statically known compatible value kind. Parameters with literal defaults use that default kind in the lowered function body, and emitted Bash assigns the matching type tag for both defaulted and explicit arguments that are validated against the default kind, so defaulted parameters can participate in kind-aware `case` matching without VM/Bash coercion drift. Required unknown-kind parameters remain unknown until typed parameters or a broader runtime type-tag design is deliberately added.

The cleaned CLI program boundary, existing block/function/test scoping rules,
array-loop lowering model, scalar return transport, and process-level cleanup
model are the safe pieces to build on. The next planned feature wave starts
with `v0.23.0` regex, ranges, and membership. Map iteration, nested
collections, formatter trivia preservation, warning suppression, logical shell
operators, advanced pipeline forms, and deeper job-control behavior remain out
of scope unless their own milestones explicitly pull them in.
