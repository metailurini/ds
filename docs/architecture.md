# Architecture

This document describes the intended architecture for `ds`.

`ds` is a C implementation of a shell-native scripting language. It is interpreted by default and should eventually use a register bytecode VM for direct execution. It must also be able to emit every supported feature to standalone Bash.

The `docs/language.ds` file is the project-wide syntax catalog. It is intentionally not a runnable script. Milestone specs are the immediate implementation contract for each version, while `docs/language.ds` keeps the broader syntax direction visible and consistent.

## High-level pipeline

```txt
source.ds
  -> source manager
  -> lexer
  -> parser
  -> AST
  -> semantic checks
  -> HIR lowering
  -> bytecode backend -> register VM
  -> Bash backend     -> standalone .sh
```

The frontend is shared by all backends.

This is important. There should not be one parser for the VM and another parser for Bash emission. That would create two languages that slowly drift apart.

## Core architectural rule

All supported features must lower to HIR.

Both execution backends consume HIR:

```txt
              ┌─────────────┐
              │    HIR      │
              └──────┬──────┘
                     │
        ┌────────────┴────────────┐
        ▼                         ▼
┌────────────────┐        ┌────────────────┐
│ Bytecode Gen   │        │ Bash Emitter   │
└───────┬────────┘        └───────┬────────┘
        ▼                         ▼
┌────────────────┐        ┌────────────────┐
│ Register VM    │        │ Standalone .sh │
└────────────────┘        └────────────────┘
```

If a feature cannot be represented in HIR and implemented by both backends, the feature is not ready.

## Core runtime substrate

`ds` is implemented in C, so the project needs a small reusable runtime substrate before the VM can become serious.

The runtime substrate is not a user-facing language layer. It is the internal foundation used by the frontend, semantic checker, bytecode generator, VM, Bash emitter, diagnostics, and tools.

Expected runtime primitives include:

- `DsStr` for borrowed string views;
- `DsString` for owned dynamic strings and output buffers;
- `DsArray` for growable vectors;
- `DsMap` for symbol tables, module caches, runtime maps, and registries;
- `DsArena` for phase-owned temporary allocation;
- `DsValue` for VM values;
- `DsProcessResult` and process helpers for command execution;
- `DsDiag`, `DsLoc`, and `DsSpan` for diagnostics;
- `DsRegex` later, only after a VM/Bash parity strategy exists.

The runtime should be boring and heavily tested. It should avoid clever abstractions that make C debugging harder.

Runtime-backed features must still obey the Bash emission rule:

> A feature may use C runtime helpers in VM mode, but emitted Bash must remain standalone and must not require `ds` or the C runtime at execution time.

For example, `file.exists("x")` may call `stat` in VM mode, but emitted Bash should use Bash syntax such as `[[ -e "x" ]]` or an embedded Bash helper.

See `docs/runtime.md` for the detailed runtime plan, including strings, arrays, maps, process execution, regex concerns, standard-library helper metadata, and the absorbed hashmap runtime boundary.

## Current backend/refactor boundaries

The current implementation keeps `include/ds.h` as a small compatibility
façade while the real declarations live in focused internal headers under
`src/`. Implementation files include the narrowest practical header instead of
the umbrella where possible:

- `src/ds_common.h` owns source spans, diagnostics, source loading, and
  allocation helpers.
- `src/ds_command.h` owns command words, redirections, captured/plain command
  metadata, and command-result field descriptors.
- `src/ds_ast.h` owns parser AST nodes and script/function/test declaration
  shapes.
- `src/frontend.h` owns token, lexer, parser, and AST-debug entrypoints.
- `src/ds_hir.h` owns the lowered HIR contract consumed by VM, Bash emission,
  formatter/checker support, and debug output.
- `src/ds_runtime.h` owns runtime values, strings, arrays, and `DsMap`.
- `src/ds_stdlib.h` owns standard-library helper metadata.
- `src/backend.h` owns formatter/checker, Bash emission, bytecode, and VM
  entrypoints.

- `src/stdlib.c` owns the table of supported standard-library helpers: public
  helper name, Bash helper name, arity, return kind, statement-only status,
  string-argument rules, iterable status, and validation flags.
- Lowering responsibilities are split by component: `src/lower.c` owns the
  orchestration entrypoints, `src/lower_expr.c` owns expression lowering and
  command-word validation, `src/lower_stmt.c` owns statement/block lowering,
  `src/lower_symbols.c` owns scope/name/vector utilities,
  `src/lower_stdlib.c` owns script declarations and literal decoding,
  `src/lower_functions.c` owns function collection/defaults/recursion checks,
  `src/lower_tests.c` owns test collection, and `src/lower_free.c` owns HIR
  cleanup. These modules consume `src/stdlib.c` metadata rather than each
  maintaining independent helper arity/name lists.
- `src/lower_internal.h` contains lowerer-private symbol/value-kind structs and
  prototypes. It is not part of the public user-facing API.
- `src/vm_internal.h` contains bytecode/VM-private structs shared only by VM
  implementation files. It is not part of the public user-facing API.
- VM responsibilities are split by component: `src/vm.c` owns the main
  interpreter loop and public VM entrypoints, `src/vm_compile.c` owns HIR to
  bytecode construction, `src/vm_dump.c` owns bytecode/debug output,
  `src/vm_args.c` owns script argument binding, `src/vm_scope.c` owns VM
  scopes/function calls, `src/vm_process.c` owns command interpolation,
  redirection, and subprocess execution, and `src/vm_test.c` owns VM-backed
  test execution setup.
- `src/vm_stdlib.c` owns VM execution for `file.*`, `dir.*`, `path.*`, `cmd.*`,
  `env.*`, `glob`, `glob!`, and `lines`.
- Bash emission responsibilities are split by component: `src/bash_emit.c` owns
  the public entrypoint, script-argument prelude, helper selection, and artifact
  writing; `src/bash_deps.c` owns helper dependency analysis;
  `src/bash_expr.c` owns expression and condition rendering;
  `src/bash_command.c` owns command words, redirections, and captured `run`
  argument rendering; `src/bash_stmt.c` owns statement/function rendering;
  `src/bash_quote.c` owns shared quoting, interpolation, buffer, and symbol
  utilities; and `src/bash_helpers.c` owns the emitted Bash helper bodies for
  command-result, collection, debug, and stdlib helpers.

This is deliberately a behavior-preserving split. `include/ds.h` still
re-exports the grouped headers for existing unit harnesses, but new internal
code should prefer the focused `src/*.h` boundary it actually needs. Emitted
Bash still remains standalone and must not depend on `ds` or on the C runtime.

## CLI commands

The `ds` tool should eventually support:

```sh
ds ./script.ds [args...]
ds run ./script.ds [args...]
ds emit bash ./script.ds -o ./script.sh
ds check ./script.ds
ds tokens ./script.ds
ds ast ./script.ds
ds hir ./script.ds
ds bytecode ./script.ds
ds fmt ./script.ds
ds test ./script.ds
```

`tokens` and `ast` are root-file frontend/debug views. They read the file passed
on the command line and preserve import statements as syntax. `check`, `hir`,
`bytecode`, `run`, direct script execution, `test`, and `emit bash` use the
composed import-aware program path. `fmt` formats only the root source passed to
it; there is no workspace formatter yet.

## Source manager

The source manager owns input files and source locations.

Responsibilities:

- read `.ds` files;
- assign stable file IDs;
- store source text;
- provide line/column lookup;
- support diagnostics with source spans;
- support import resolution later.

A source location should include:

```c
typedef struct {
    uint32_t file_id;
    uint32_t line;
    uint32_t column;
    uint32_t offset;
} DsSourceLoc;
```

A source span should include start and end locations.

## Diagnostics

Diagnostics are critical for a language project.

Every user-facing error should try to include:

- filename;
- line;
- column;
- a short message;
- source snippet;
- caret or span marker;
- optional help text.

Example:

```txt
deploy.ds:8:12 error: expected string after `import`

  import 123
         ^^^

help: imports must use a quoted relative path, such as `import "./lib.ds"`
```

Diagnostics should be shared by parser, semantic checker, VM, and Bash emitter where possible.

## Lexer

The lexer converts source text into tokens.

Responsibilities:

- keywords;
- identifiers;
- strings;
- integers;
- booleans;
- operators;
- braces;
- parentheses;
- comments;
- newlines;
- source locations.

Early keywords:

```txt
let
if
else
true
false
while
break
continue
case
```

Likely future keywords:

```txt
script
arg
option
flag
import
fn
return
for
in
run
try
defer
test
assert
```

The lexer should not do complex semantic work. It should only produce tokens and lexical diagnostics.

## Parser

The parser converts tokens into AST.

Recommended approach:

- recursive descent for statements;
- Pratt parser for expressions.

This should keep expression parsing simple while making statement parsing explicit.

The parser is split by grammar area so command-mode and expression-mode syntax can
evolve without turning one frontend file into another god module:

- `src/parser.c` owns the public `ds_parse` entrypoint and top-level declaration
  loop;
- `src/parser_internal.h` owns the private parser cursor, allocation, vector, and
  token-copy helpers shared across parser components;
- `src/parse_expr.c` owns Pratt expression parsing, calls, indexing, fields,
  arrays, maps, and literals;
- `src/parse_command.c` owns shell-like command words, redirection suffixes, and
  captured `run` expressions;
- `src/parse_script.c` owns `script { arg/option/flag ... }` declarations;
- `src/parse_function.c` owns function declarations and test declarations;
- `src/parse_stmt.c` owns statement/block parsing and dispatch.

The parser should support command-mode parsing, but command parsing should remain
isolated in `src/parse_command.c` because shell-like syntax has different token
joining and redirection rules from normal expressions.

## Command mode vs expression mode

`ds` has two syntactic worlds:

1. language statements;
2. shell-like command statements.

A line that starts with a language keyword is parsed as a language statement:

```ds
let name = "Danh"
if name == "Danh" {
  echo "hello"
}
```

A line that does not start with language syntax is parsed as a command statement:

```ds
git status
npm install
docker compose up
```

Inside expressions, variables are referenced by name:

```ds
let message = "Deploying to " + target
```

Inside command statements, variables are passed with `$`:

```ds
./deploy.sh $target
```

This rule keeps commands familiar while keeping logic readable.

## AST

The AST should preserve source shape.

Example input:

```ds
if target == "production" {
  npm test
}
```

Possible AST:

```txt
IfStmt
  condition:
    BinaryExpr ==
      Ident target
      String "production"
  then:
    CmdStmt
      words: ["npm", "test"]
```

AST is useful for:

- diagnostics;
- debug output;
- syntax-aware tooling;
- later formatting.

AST should not be forced to match backend execution directly. That is HIR's job.

## Semantic checks

The semantic checker should validate meaning after parsing.

Early semantic checks:

- duplicate variables in the same scope;
- invalid assignment target;
- unknown syntax not supported by current version;
- invalid command interpolation;
- future: unknown variable references;
- type mismatch in CLI args for implemented `script { ... }` declarations;
- future: invalid imports.

Semantic checks should produce diagnostics, not crashes.

## HIR

HIR means High-level Intermediate Representation.

HIR should be simpler than AST and closer to execution.

The parser may preserve syntactic sugar. HIR should lower it away.

Example:

```ds
npm run build &> "build.log"
```

HIR could represent this as:

```txt
RunCommand
  argv: ["npm", "run", "build"]
  stdout: file("build.log")
  stderr: same_as_stdout
```

Both VM execution and Bash emission should use this form.

The first `v0.3.0` lowering layer is intentionally small. It keeps the
currently supported AST shape mostly intact as a backend-facing lowered program,
while centralizing symbol lookup, interpolation validation, duplicate
declaration checks, supported operator checks, and block-scope boundary checks.
Future milestones can simplify this representation further as syntax grows.

In `v0.3.0`, lowered blocks also compile to explicit VM scope push/pop
instructions. This keeps runtime variable storage aligned with lowering: values
declared inside a block are available to statements in that block and are
discarded when execution leaves it.

In `v0.4.0`, behavior-sensitive CLI paths were cleaned up so they share the
same source -> lexer -> parser -> lowering helper before reaching a backend.
`ds check`, `ds emit bash`, `ds run`, direct `ds <file.ds>`, and `ds bytecode`
all lower once through the same entrypoint plumbing. Backend-specific functions
now accept an already-lowered program where practical, so Bash emission,
bytecode dumping, and VM execution no longer each need to own parse/lower setup.
`ds tokens` and `ds ast` intentionally remain frontend/debug commands.

In `v0.5.0`, lowering also owns the script argument contract. `script { ... }`
declarations are lowered into ordered backend-facing argument declarations before
VM execution or Bash emission. The VM binds runtime argv into the root scope
before bytecode runs. The Bash backend emits a standalone parser before the
script body and binds the same lowered names into `__ds_` variables.

## Bytecode backend

`ds` is an interpreter, but it can compile internally to bytecode.

This keeps direct execution faster and easier to debug than walking the AST.

Target design:

- register bytecode VM;
- compact instruction format;
- constants table;
- function table later;
- debug/source location table;
- bytecode dump command.

Example bytecode shape:

```txt
0: LOAD_CONST   r1, "package.json"
1: CALL_BUILTIN r2, file.exists, r1
2: JUMP_IF_FALSE r2, 6
3: RUN_CMD      ["npm", "install"]
4: JUMP         6
5: NOP
6: RETURN
```

## Register VM

The VM executes bytecode.

Early value types:

```txt
null
bool
int
string
```

Later value types:

```txt
array
map
command_result
function
```

The VM should own runtime concerns:

- variables;
- scopes;
- expression evaluation;
- command execution;
- stdout/stderr handling;
- exit codes;
- runtime diagnostics.

Command execution should be carefully separated behind a process API so it can be tested.

The VM depends on the runtime substrate. At minimum, serious VM work requires:

- `DsValue` for tagged values;
- dynamic strings for runtime strings and command capture;
- arrays/vectors for stack/register data and argv lists;
- maps for globals, scopes, and builtin registries;
- process helpers for shell command execution;
- diagnostics helpers for runtime errors.

Because of this, `v0.3.0` should include the minimal runtime foundation needed by the first bytecode VM instead of treating the VM as bytecode only.

## Bash emitter

The Bash emitter converts HIR into standalone Bash.

Generated Bash should:

- start with a Bash shebang;
- use a strict header where appropriate;
- include only required helper functions;
- preserve useful source comments;
- use safe internal names;
- quote values safely;
- produce clear runtime errors where possible.

Example output shape:

```bash
#!/usr/bin/env bash
set -euo pipefail

__ds_die() {
  echo "error: $*" >&2
  exit 1
}

# ds: deploy.ds:4
__ds_target="staging"

# ds: deploy.ds:6
if [[ "$__ds_target" == "production" ]]; then
  npm test
fi
```

## Bash version target

Generated Bash may eventually require Bash 4+ because associative arrays are useful for maps.

When Bash 4+ becomes required, generated scripts should include a clear check:

```bash
if (( BASH_VERSINFO[0] < 4 )); then
  echo "error: this script requires Bash >= 4" >&2
  exit 1
fi
```

For early versions, avoid features that require Bash 4 unless necessary.

## Runtime helpers for emitted Bash

Some `ds` features should emit simple Bash directly.

Other features should use generated helper functions.

Example helpers:

```bash
__ds_die()
__ds_cmd_failed()
__ds_cmd_require()
__ds_parse_args()
__ds_file_exists()
```

The emitted Bash should remain standalone by embedding the helpers it uses.

## Builtins

Each builtin should define both VM and Bash behavior.

Conceptual C structure:

```c
typedef struct {
    const char *name;
    DsBuiltinVmFn vm_fn;
    DsBuiltinEmitFn bash_emit_fn;
} DsBuiltinSpec;
```

Example:

```ds
file.exists("package.json")
```

VM behavior:

```txt
stat(path) succeeds
```

Bash emission:

```bash
[[ -e "package.json" ]]
```

A builtin without Bash emission should not become part of the stable language.

## Imports

Imports are planned after the first VM and Bash emitter foundations.

Target syntax:

```ds
import "./lib.ds"
```

Rules:

- imports are resolved relative to the importing file;
- each file is loaded once;
- cycles are errors;
- emitted Bash bundles imported files into one standalone script.

Remote imports and package management are not early goals.

## Testing architecture

Testing should be built around golden fixtures and integration tests.

Suggested structure:

```txt
tests/
  lexer/
  parser/
  ast/
  diagnostics/
  emit-bash/
  vm/
  parity/
  integration/
```

Golden tests should be used for:

- token output;
- AST output;
- diagnostics;
- Bash emission;
- bytecode dumps.

Parity tests should compare:

- VM stdout;
- VM stderr shape where practical;
- VM exit code;
- emitted Bash stdout;
- emitted Bash stderr shape where practical;
- emitted Bash exit code.

## Debug commands

Debug commands are not just nice-to-have. They are part of making the language easy to develop.

Implemented and planned debug commands:

```sh
ds tokens file.ds
ds ast file.ds
ds hir file.ds
ds bytecode file.ds
ds run --trace-cmd file.ds
ds run --trace-vm file.ds
ds imports file.ds
```

These commands should make it easy to understand where a bug lives:

- lexer;
- parser;
- semantic checker;
- HIR lowering;
- bytecode generation;
- VM execution;
- Bash emission.

As of `v0.13.0`, `ds hir` prints the composed lowered program, `ds bytecode`
includes script/function/constant/instruction metadata with source markers, and
`ds run --trace-cmd` / `ds run --trace-vm` emit deterministic trace lines to
stderr without changing script stdout or normal execution semantics. Emitted
Bash remains standalone and supports command tracing with `DS_TRACE_CMD=1` plus
source-located command failure messages for plain command statements. `ds
imports` remains a future debug command. As of `v0.14.0`, `ds test <file.ds>` uses the same composed parse/lower path and VM execution machinery for test blocks while normal VM execution and normal Bash emission skip those test declarations.

## Suggested C project layout

When implementation begins, a possible source layout is:

```txt
src/
  main.c

  core/
    arena.c
    string.c
    array.c
    ds_map.c
    diag.c
    source.c

  runtime/
    value.c
    process.c
    regex.c

  frontend/
    lexer.c
    parser.c
    ast.c
    ast_print.c

  semantic/
    resolver.c
    checker.c
    imports.c

  ir/
    hir.c
    lower.c
    lower_expr.c
    lower_stmt.c
    lower_symbols.c
    lower_stdlib.c
    lower_functions.c
    lower_tests.c
    lower_free.c
    lower_internal.h

  bytecode/
    bc.c
    bc_gen.c
    bc_dump.c

  vm/
    vm.c
    builtins.c

  emit/
    emit_bash.c
    emit_helpers.c

  tools/
    fmt.c
    test_runner.c
    repl.c

src/runtime/
  hashmap.c
  hashmap.h
  hashmap.LICENSE

docs/language.ds
```

The owned hashmap implementation is absorbed into `src/runtime/` and is private to the `DsMap` bridge in `src/runtime.c`. Other compiler, VM, and emitter code should use `DsMap` only, so maps feel like a native part of the `ds` runtime rather than a separate library subtree.

This layout is only a starting point. The actual implementation may evolve.

## Architecture risks

### Risk: VM and Bash emitter drift apart

Mitigation:

- all features lower to HIR;
- parity tests compare VM and emitted Bash;
- completion checklist requires both backends.

### Risk: too many features too early

Mitigation:

- use milestone specs;
- keep non-goals explicit;
- have cleanup versions every fourth milestone.

### Risk: Bash emission becomes ugly

Mitigation:

- design syntax with Bash emission in mind;
- delay features that need a large runtime;
- keep generated helpers small and explicit.

### Risk: debugging is added too late

Mitigation:

- expose `tokens`, `ast`, and `check` in `v0.1.0`;
- add `bytecode` early with the VM;
- preserve source locations everywhere.


## v0.6.0 import composition

Behavior-sensitive CLI commands now share a source/import loader before lowering. Local `import "./file.ds"` statements are resolved relative to the importing file, loaded once per root program, composed before the importing file's executable statements, and then lowered into the same backend-facing program used by the VM and Bash emitter. `tokens` and `ast` remain root-file debug views.

As of `v0.16.0`, that loader/composer lives in `src/cli_program.c` behind
`src/cli_program.h`. `src/main.c` remains responsible for usage text, public
argument parsing, command dispatch, and command-specific flags, while the CLI
program boundary owns source loading, root-file lex/parse, composed import-aware
parse, lowering, import cycle/load-once diagnostics, and cleanup of loaded
units. This is a behavior-preserving cleanup boundary.

## v0.17.0 control flow

The `v0.17.0` implementation extends the shared AST/HIR path with scalar
reassignment, `while`, lexical `break`/`continue`, and expression-style `case`.
The parser keeps these constructs as explicit statement nodes; lowering
validates assignment targets, loop-control placement, duplicate/default case
arms, and scalar case selectors before either backend runs.

The VM compiles loops and cases into normal jumps plus scoped block cleanup for
`break` and `continue`. Case matching uses a dedicated kind-aware exact compare
path so integer, string, and boolean literals do not coerce into each other.
Bash emission stays standalone: `while` and loop control emit native Bash
constructs, and `case` emits exact-comparison `if`/`elif` chains instead of Bash
glob-style `case` patterns. Emitted Bash records lightweight sidecar type tags
for variables so `case x { "1" ... 1 ... }` follows ds value-kind semantics even
though shell variables are strings. Those tags also preserve ds truthiness for
scalar-variable `if` and `while` conditions in emitted Bash. This preserves the
language rule that case alternatives are exact ds literals, not shell patterns.

## v0.7.0 command results and redirection

The parser now represents captured command execution as a `run` expression and plain command redirection as command-statement metadata. Lowering is the shared command-result HIR boundary for VM execution and Bash emission: it validates command-result fields, command variables, and conservative string-literal redirection targets before either backend runs.

The VM lowers captured commands to bytecode that executes through the process boundary, captures stdout/stderr separately, stores the exit code, and exposes derived `ok`/`failed` fields. The Bash emitter consumes the same lowered shape and emits standalone `__ds_` helpers only when a program uses captured command results. Plain command statements keep their existing fail-fast behavior, with optional redirection metadata emitted as normal Bash redirection syntax.

## v0.8.0 command cleanup

The first `v0.8.0` cleanup pass makes simple command data explicit in the lowered representation. Parsed AST nodes may still preserve syntax-oriented command fields, but lowering now uses a shared `DsCommand` shape for both plain command statements and captured `run` commands. That shape owns ordered words, per-word spans, optional redirection metadata, command kind, and the source span of the command as a whole.

Command-result field knowledge is centralized behind one descriptor table for `stdout`, `stderr`, `code`, `ok`, and `failed`. Lowering, VM field reads, VM string interpolation, VM command-word expansion, and Bash condition emission now consult that shared model instead of each backend maintaining an independent list of known fields.

The VM command process path also has a small internal cleanup boundary around a VM-local process spec/result pair. The wrapper owns rendered argv, keeps the command span and redirection metadata together, opens redirection targets with source-located diagnostics, launches the child, normalizes wait status, and fills captured stdout/stderr only for `run` commands. Plain commands still stream and fail fast; captured commands still collect stdout/stderr and return inspectable status. Generated Bash remains standalone and keeps using `__ds_` helper names instead of depending on the C runtime.

Command-heavy regression suites now share a VM/Bash parity helper from `tests/lib/testlib.sh` instead of carrying a version-local copy. The helper emits standalone Bash, validates it with `bash -n`, runs VM and Bash modes from isolated working directories, compares stdout/stderr/status, and optionally compares generated output files.

## v0.18.0 pipeline command model

`v0.18.0` promotes command representation from a single word vector to a small
pipeline-aware model. A `DsCommand` now owns one or more stages, each stage owns
the existing `DsWordVec`, and the redirect remains a whole-command suffix. The
AST, HIR, formatter, checker, VM compiler, VM runtime, bytecode dumper, and Bash
emitter traverse command stages through that shared model instead of exposing
Bash-specific pipeline state through the frontend.

The VM compiler flattens stage words into bytecode instruction storage while
retaining per-stage word counts. The process runtime reconstructs argv vectors
per stage, wires real OS pipes, and computes pipefail status. The Bash backend
emits ordinary Bash pipelines for plain commands and a standalone capture helper
for captured `run` pipelines.

## v0.19.0 string helper boundary

String methods lower through the same helper-call path as the shell-oriented
standard library, with the receiver passed as the first helper argument. This
keeps parser syntax (`value.trim()`) separate from backend implementation
details while allowing the VM and Bash emitter to share arity, receiver, and
return-kind metadata. Generated Bash emits helper functions only when the
lowered program needs string helpers or interpolation trimming.

Triple-quoted strings remain regular string literals in the AST/HIR; decode
helpers in lowering, VM compilation, and Bash emission agree that the content is
all bytes between the delimiters.

`v0.20.0` keeps helper emission at the existing dependency-class granularity but
hardens the dependency scan: call arguments are now traversed when deciding
whether emitted Bash needs run, pipeline, collection-index, map, or stdlib/string
helper support. This preserves standalone Bash for compositions such as a string
method called on an indexed `split` result, where the outer call needs string
helpers and the receiver argument also needs collection-index helpers.
