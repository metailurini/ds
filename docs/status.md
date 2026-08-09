# Current Status

This document is the user-facing support matrix for the current implementation.
It summarizes what users can rely on today, what is test-only or tooling-only,
and what is deliberately deferred, rejected, or out of scope for `1.0.0`.
Historical implementation sequencing belongs in `docs/milestones/` and Git.

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

There are no hidden production commands in the current surface.

`tokens` and `ast` are root-file frontend/debug views. They read only the file
passed on the command line and show the import statement as syntax instead of
composing imported files.

`check`, `hir`, `bytecode`, `run`, direct script execution, `test`, and
`emit bash` use the import-aware composed program path. Local imports are loaded
relative to the importing file, loaded once per root program, and placed before
the importing file's executable statements. `fmt` formats the root source file
that was passed to it; it does not rewrite imported files or format a workspace.

## Production language support intended for 1.0.0

The production runtime supports this language slice:

- line comments in normal parsing/checking/running/emission;
- `let` declarations with strings, integers, booleans, identifiers, unary and
  binary expressions, field access, indexing, and supported calls;
- `if`/`else` blocks and nested block scopes;
- shell-native command statements;
- shell-native plain command pipelines such as `cat log | grep ERROR | sort`;
- captured command results with `run` expressions and `stdout`, `stderr`,
  `status`, `code`, `ok`, and `failed` fields;
- captured `run` pipelines with pipefail-style status in the same
  command-result fields;
- readable redirections for plain command statements: `|>`, `|>>`, `!>`,
  `!>>`, `&>`, and `&>>`;
- top-level `fn` declarations with positional parameters and trailing literal
  defaults;
- statement-style function calls, scalar function return values, flat
  scalar-array returns, flat scalar-map/object returns, command-result returns,
  and value-returning function calls in expressions; emitted Bash now carries
  these returns through a private typed `__ds_` payload instead of untyped
  stdout;
- returned arrays/maps in emitted Bash carry the scalar kind sidecars needed for
  `in`, indexing, and map field truthiness parity;
- `return expr` inside function bodies for scalar string/int/bool values, flat
  arrays, flat maps, and command results, with conservative same-kind/all-paths
  validation before a function can be used as a value;
- array literals, map literals with string-like keys, array/map access, array
  `push`, array `for` loops, and key/value map `for` loops over named maps or
  supported flat map-returning user-function calls;
- lightweight rows and row arrays: flat row/object literals with scalar
  string/int/bool fields, same-schema row-array literals, empty-array
  first-row-push inference, row-array iteration, row-array indexing,
  copy-by-value assignment, function row-array returns, and
  `sort_by(field[, "asc"|"desc"])` with deterministic stable ordering in the
  VM and emitted Bash; row sorting is intentionally scoped to small in-memory analyzer/reporting datasets rather than table-scale workloads;
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
  `glob!`, `dir.walk`, `dir.walk!`, `dir.walk_ext`, `dir.walk_ext!`, and
  `lines` within the scoped standard-library surface;
- recursive `**` glob segments through `glob` and `glob!`, with exactly one
  complete `**` path segment per pattern, zero-or-more directory semantics,
  sorted duplicate-free string results, default hidden-path skipping, and no
  directory-symlink traversal;
- ASCII/byte-oriented string methods: `.trim()`, `.upper()`, `.lower()`,
  `.replace()`, `.contains()`, `.split()`, `.starts_with()`, `.ends_with()`,
  `.len()`, `.index_of()`, `.last_index_of()`, `.count()`, `.char_at()`, and
  `.slice()`, including known string-array elements from `split`, `lines`,
  `glob`, `glob!`, and `dir.walk*` when those values are indexed or iterated;
- kind-aware Bash `case` parity for known indexed array values and array `for`
  loop variables with known string/int/bool element kinds;
- stackable `defer { ... }` and `defer on: "EXIT"|"INT"|"TERM" { ... }` cleanup handlers, plus replacement-style `trap "EXIT"|"INT"|"TERM" { ... }`;
- formatted string interpolation with scoped string/int specifiers and
  triple-quoted multi-line string literals;
- exact `in` membership checks over scalar arrays, including array literals,
  named arrays, and known standard-library string-array results;
- conservative regex `matches` expressions with `/pattern/` and `/pattern/i`
  literals, plus runtime string patterns validated against the same portable
  subset;
- `regex.match(text, pattern[, flags])` returning a flat match-result map with
  `matched`, `full`, `"0"`, and numbered capture strings for capture groups
  present in the validated pattern, up to `"9"`, with no-match captures
  represented consistently as empty strings;
- `regex.replace(text, pattern, replacement[, flags])` performing global
  replacement with `$0`..`$9` and `$$` expansion;
- inclusive integer range loop sources, for example `for n in 1..3 { ... }`,
  with zero iterations when the start is greater than the end and with bounds
  evaluated once before the loop.

Every supported production feature is expected to run in the VM and emit
standalone Bash. Generated Bash must not call the `ds` binary or depend on the C
runtime to execute. Generated helper functions use the reserved `__ds_`
namespace, are emitted deterministically, and should not be duplicated in a
single generated script.

`case $target { ... }` is not valid expression syntax; `$name` remains reserved
for command arguments.

## Test-only syntax

`test "name" { ... }` blocks and `assert expr` are test-runner features.
`ds test <file.ds>` lowers tests and executes them through the VM backend.
Normal `ds run`, direct script execution, and normal `emit bash` ignore test
blocks. Inside tests, `fail "message"` fails the active test, `exit 0` stops it
as a pass, and non-zero `exit` fails it.

Test blocks are not a production scripting construct and are not emitted as a
runtime test harness in standalone Bash.

## Tooling-only and debug views

`tokens`, `ast`, `hir`, and `bytecode` are development/debug views. They are
stable enough for project tests, but they are not a user-facing runtime API.
`tokens` and `ast` are root-file views; `hir` and `bytecode` use the composed
import-aware path.

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

Comment-preserving formatting remains deferred. The
lexer/parser accept comments for normal language behavior, but `ds fmt` rejects
comment-bearing files with a clear diagnostic rather than silently dropping
trivia. Inline trailing comments are also rejected by the formatter for the same
reason. Retaining trivia safely needs a larger parser/formatter design.

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
- `examples/filtering.ds`
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
- nested collection function returns, collection-valued parameters, and typed
  return annotations;
- public row/schema/type declarations, typed row or row-array parameters,
  row-field assignment, nested row mutation, row-array replacement/deletion, and
  row fields containing arrays/maps/rows or command-result objects;
- `until`, loop `else`, and labeled/depth-based `break`/`continue`;
- regex/glob/destructuring/fallthrough `case` behavior;
- string binary `+` concatenation; use interpolation instead;
- regex split, first-class regex values, named captures, lookaround, pattern
  backreferences, replace-first/count APIs, and regex/glob case patterns;
- first-class range values, stepped/reverse ranges, slices, nested collections,
  sparse array assignment, deletion, compound index assignment, and field-style
  map assignment;
- passing whole collection values directly to functions or commands;
- direct collection access inside command words without first binding a scalar;
- empty map literal inference and empty map keys;
- environment append/prepend shorthand and scoped environment blocks;
- multiple recursive `**` glob segments, partial `**` segments, custom glob
  flags, hidden traversal flags, symlink-following traversal, brace expansion,
  extglob, shell variable expansion, and `~` expansion in glob patterns;
- binary file helpers and streaming `lines`;
- formatter configuration, warning suppression comments, workspace formatting,
  and range formatting;
- editor/LSP integration;
- arbitrary signal names, numeric signal syntax, handler removal, handler
  context objects/line numbers, and job-control/process-group APIs;
- additional shell backends or native compilation.

Unsupported forms should fail clearly instead of partially working in only one
backend.

## Rejected and out-of-scope behavior

Some syntax is intentionally not part of the `1.0.0` boundary unless a later
roadmap change says otherwise:

- heredocs, here-strings, process substitution, and command substitution syntax;
- command-level `&&`/`||`, background jobs, async/wait primitives, and public
  job-control APIs;
- alternate shell backends such as zsh, fish, POSIX sh, or native compilation;
- classes, inheritance, macros, packages, remote imports, and broad application
  framework features.

Expression-level `&&` and `||` are supported inside ds expressions; command
operators with the same spelling remain unsupported shell syntax.

## Host environment assumptions

The VM executes commands using the host process environment. Examples and tests
assume a Unix-like environment with `sh`, `printf`, `grep`, `sort`, `tr`, and
standard file utilities available. Emitted scripts target Bash; generated Bash
uses Bash 4+ guards when associative arrays or other Bash-4-only behavior is
required. Regex behavior is constrained to the conservative VM/Bash-compatible
subset documented in `docs/runtime.md`.

## 1.0.0 checklist

The executable release checklist lives in `docs/release-checklist.md`. It covers
support-matrix sign-off, VM/Bash parity, generated-Bash standalone behavior,
examples, docs, diagnostics, formatter/checker behavior, sanitizer/ownership
confidence, deferred/rejected feature sign-off, packaging/release notes, and
known limitations accepted for `1.0.0`.

## Internal cleanup state

CLI source/import composition lives in `src/cli_program.c` and
`src/cli_program.h`, leaving `src/main.c` focused on argument parsing and public
command dispatch. This boundary owns source loading, root-file lex/parse,
composed import-aware parse, lowering, import cycle/load-once diagnostics, and
cleanup of loaded units.

Raw hashmap implementation details remain hidden under `src/runtime/` behind the
`DsMap` runtime abstraction. `include/ds.h` remains a compatibility umbrella,
while internal code should prefer focused headers.
