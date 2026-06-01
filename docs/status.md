# Current Status

This document is the user-facing snapshot after the completed structured
return, direct environment/interpolation, collection mutation, recursive glob,
runtime regex feature wave, the `v0.33.0` stabilization implementation and
dedicated-test pass, and the `v0.34.0` text-literal/broken-pipe DX
implementation pass. It is a
support matrix, not a replacement for the roadmap or language catalog: it
summarizes what users can rely on today, what is test-only or tooling-only, and
what is deliberately deferred, rejected, or out of scope for `1.0.0`.

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

There are no hidden production commands in the current `v0.34.0` surface; the
cleanup milestones harden the existing CLI instead of adding new commands.

`tokens` and `ast` are root-file frontend/debug views. They read only the file
passed on the command line and show the import statement as syntax instead of
composing imported files.

`check`, `hir`, `bytecode`, `run`, direct script execution, `test`, and
`emit bash` use the import-aware composed program path. Local imports are loaded
relative to the importing file, loaded once per root program, and placed before
the importing file's executable statements. `fmt` formats the root source file
that was passed to it; it does not rewrite imported files or format a workspace.

## Production language support intended for 1.0.0

The production runtime supports the language slice implemented and stabilized
through the `v0.22.6` final v0.22 documentation pass, the completed `v0.23.0`
regex/range/membership implementation and test pass, the scoped `v0.32.0`
regex runtime-string/capture/replacement production implementation, the
no-new-syntax `v0.33.0` collection/glob/regex stabilization pass, and the
`v0.34.0` text-literal/broken-pipe DX implementation pass:

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
- recursive `**` glob segments through `glob` and `glob!`, with exactly one
  complete `**` path segment per pattern, zero-or-more directory semantics,
  sorted duplicate-free string results, default hidden-path skipping, and no
  directory-symlink traversal;
- ASCII string methods: `.trim()`, `.upper()`, `.lower()`, `.replace()`,
  `.contains()`, `.split()`, `.starts_with()`, and `.ends_with()`, including
  known string-array elements from `split`, `lines`, `glob`, and `glob!` when
  those values are indexed or iterated;
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
Since `v0.34.0`, the common closed-stdout case for uncaptured, unredirected
top-level command statements and pipelines is quieted: a status-141 broken pipe
while stdout is not a terminal is treated as successful early script completion
after supported cleanup runs. Captured `run` results still preserve observed
subprocess status fields, and unrelated command/pipeline failures remain
visible.

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
scalar value-returning calls. v0.27.0 also adds direct `env.NAME` reads,
`env.NAME = scalar` assignment, and `unset env.NAME`; missing environment
variables read as an empty string, assignments are exported to later child
commands, and unsets remove the variable from later child-command environments
in both VM and emitted Bash. Command-word interpolation supports the legacy `{name}` / `{name.field}` forms,
`{env.NAME}`, direct `env.NAME` command arguments, integer arithmetic
expressions, direct scalar value-returning function calls in quoted command
words, and v0.30 flat named collection index reads such as `{items[0]}` and
`{map[key]}`. Interpolated function calls are pre-evaluated before the outer
command launches; collection/map/command-result interpolation beyond that flat
index-read surface remains rejected. Statement-style calls may still
ignore returned values.
`examples/function-values.ds` shows the supported return, arithmetic, and
expression interpolation path.
Since `v0.34.0`, ordinary and triple-quoted strings also support strict doubled
literal braces: `{{` renders `{`, `}}` renders `}`, and `{expr}` keeps the
existing interpolation meaning. A lone `}` is rejected during lowering with a
diagnostic that points to `}}` for a literal close brace, so ambiguous strings
must be written deterministically, for example `"{{name}}"` for literal
`{name}` and `"{name}}}"` for interpolation followed by a literal `}`.
The dedicated `tests/v0_21/run.sh` suite now covers the scoped VM/Bash parity,
diagnostic, formatting, example, and generated-Bash boundary behavior.

`v0.28.0` is a cleanup-only pass over the supported `v0.25.0` through `v0.27.0`
surface. It does not add syntax. It keeps helper dependency discovery aligned
with the accepted HIR so statement-style user function calls whose arguments
contain formatted interpolation, integer arithmetic interpolation, or collection
index reads emit the same standalone Bash helpers those expressions would have
needed in `let`, assignment, return, or condition positions. At that milestone,
nested collections, recursive glob behavior, and advanced regex
captures/replacement/runtime regex strings remained deferred to their named
roadmap milestones; recursive glob was later added in `v0.31.0`, and scoped
runtime regex strings/captures/replacement were later added in `v0.32.0`.
`v0.29.0` adds deterministic key/value map iteration over
named maps and supported flat map-returning user-function calls, with ascending
bytewise/ASCII key order in both VM execution and emitted Bash. `v0.30.0` adds
named flat array/map mutation; index assignment is supported in v0.30 for named flat array/map bindings:
`items[index] = scalar` / `array[index] = scalar` replaces existing elements
only and does not append; use the existing `array.push(value)` method for
append behavior. `map[key] = scalar` inserts new keys or replaces existing
non-empty string keys. Mutation targets must be named flat collections,
RHS values stay within the flat scalar collection boundary, and generated Bash
remains standalone while VM/Bash keep scalar value kinds aligned for
interpolation, conditions, `case`, returns, and later reads. Collection variable
copies such as `let copy = original` are value copies in both VM and Bash, so
mutating `copy[index]` does not alias or mutate `original[index]`. Temporary mutation,
function-call result mutation, command-result mutation, nested mutation, sparse
arrays, slice assignment, deletion, references/aliases, compound index
assignment, and field-style map assignment remain deferred.

`v0.31.0` adds recursive `**` glob support through the existing `glob` and
`glob!` helpers. Recursive matching is accepted only when `**` is a complete path
segment and appears once in the pattern. It means zero or more directory
segments, so `src/**/*.ds` can match both `src/main.ds` and
`src/nested/deep.ds`. Results are sorted in bytewise/ASCII order, duplicate-free,
and string-kind preserving in both VM execution and emitted Bash. Hidden path
components are not matched or traversed unless named explicitly by a non-`**`
segment such as `.config`, directory symlinks are not traversed, `glob` no-match
returns an empty collection, and `glob!` no-match fails before loop bodies run.
Literal invalid recursive patterns fail in lowering; dynamically produced
invalid recursive patterns fail as runtime data errors in VM/Bash helpers.
Multiple recursive segments, partial `**` segments, custom glob flags, hidden
traversal flags, symlink following, brace expansion, extglob, shell variable
expansion, and `~` expansion remain deferred.

`v0.32.0` adds the practical regex API on top of the conservative `v0.23.0`
regex subset. Runtime string patterns are accepted by `matches` and by
`regex.match` / `regex.replace`; direct string literals are statically validated
when lowering can see them, while dynamic strings, flags, replacements, and
capture counts are validated by VM/Bash runtime helpers before later side
effects. Regex helpers accept optional flags `""` or `"i"`, expose capture data
through flat map values for present capture groups up to nine, populate
no-match capture entries consistently, and keep replacement text data-only
instead of shell code. Unsupported PCRE-style features remain rejected so VM
execution and standalone Bash do not diverge.

`v0.22.0` adds process-level cleanup registration. Plain `defer` is an `EXIT` cleanup and runs in LIFO order. `defer on:` supports the literal signals `EXIT`, `INT`, and `TERM`; repeated `trap` statements use replacement semantics per signal. `v0.22.1` stabilizes the deterministic non-signal cleanup core with VM/Bash parity tests for normal completion, explicit `exit`, explicit `fail`, direct command failure, captured command failure, `trap "EXIT"` replacement, handler failure continuation, handler `exit` status override, imports, script args, and function calls from handlers. `v0.22.2` stabilizes the `INT`/`TERM` syntax and diagnostic surface with tests for parser/token output, AST/HIR/bytecode visibility, formatter normalization, emitted-Bash helper structure, and unsupported or malformed signal diagnostics without sending real OS signals. `v0.22.3` adds the deterministic signal harness: VM and emitted-Bash scripts run in isolated process sessions, tests wait for a `ready` marker, signal the process group, capture stdout/stderr/status through files, and clean up leftovers on timeout. It proves the smallest cooperative `TERM` direct-command fixture. `v0.22.4` extends that harness to non-cooperative foreground direct commands for both `INT` and `TERM`, preserving statuses `130` and `143`, running signal-specific cleanup before `EXIT` cleanup, and avoiding generic command-failure diagnostics or Bash job-control noise. `v0.22.5` extends the same runtime contract to simple foreground pipelines and verifies the harness does not hang when pipeline children inherit stdout/stderr handles. `v0.22.6` finalizes the v0.22 documentation contract: supported behavior is process-scope cleanup for `EXIT`/`INT`/`TERM`, rejected behavior includes function-local handler captures and direct handler `return`, handler context values such as line numbers remain deferred, and broad job-control behavior remains out of scope. The final v0.22 test-plan audit fills deterministic coverage gaps for cleanup side effects, imported signal handlers and diagnostics, function-registered handlers, handler control flow, arithmetic/return interaction, test-block isolation, and malformed/dynamic/numeric/empty signal diagnostics. On signal dispatch the trap runs first, then matching defers in LIFO order, then `EXIT` cleanup. The VM installs lightweight `INT`/`TERM` handlers, checks for pending signals between bytecode instructions, and treats interrupted foreground commands/pipelines as signal cleanup events while forwarding observed `INT`/`TERM` to the foreground child process group when possible. Emitted Bash installs standalone traps. Background jobs, public job-control/process-group APIs, asynchronous pipelines, handler context objects/line numbers, and broad signal-forwarding semantics outside foreground commands and simple foreground pipelines remain out of scope.

Because required function parameters remain untyped until the function-value wave, string methods and formatted interpolation require a statically known compatible value kind. Parameters with literal defaults use that default kind in the lowered function body, and emitted Bash assigns the matching type tag for both defaulted and explicit arguments that are validated against the default kind, so defaulted parameters can participate in kind-aware `case` matching without VM/Bash coercion drift. Required unknown-kind parameters remain unknown until typed parameters or a broader runtime type-tag design is deliberately added.

The cleaned CLI program boundary, existing block/function/test scoping rules,
array-loop lowering model, scalar return transport, and process-level cleanup
model are the safe pieces to build on. The latest feature wave adds scoped
`v0.23.0` regex, ranges, and membership. `v0.24.0` hardens documentation,
examples, diagnostics, sanitizer expectations, and generated-Bash helper hygiene
without adding production syntax. `v0.29.0` adds map iteration, `v0.30.0`
adds named flat array/map index assignment, `v0.31.0` adds scoped recursive
glob patterns, `v0.32.0` adds runtime regex strings, capture maps, and regex
replacement, `v0.33.0` stabilizes the combined collection/glob/regex surface
without adding syntax, and `v0.34.0` adds the first DX-priority text-literal and
closed-stdout broken-pipe cleanup. Nested collections, formatter trivia
preservation, warning suppression, command-level shell logical operators, deeper
job-control behavior, and advanced pipeline forms remain out of scope unless
their own milestones explicitly pull them in.
