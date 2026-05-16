# Architecture

This document describes the intended architecture for `ds`.

`ds` is a C implementation of a shell-native scripting language. It is interpreted by default and should eventually use a register bytecode VM for direct execution. It must also be able to emit every supported feature to standalone Bash.

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
- `DsDiag`, `DsSourceLoc`, and `DsSourceSpan` for diagnostics;
- `DsRegex` later, only after a VM/Bash parity strategy exists.

The runtime should be boring and heavily tested. It should avoid clever abstractions that make C debugging harder.

Runtime-backed features must still obey the Bash emission rule:

> A feature may use C runtime helpers in VM mode, but emitted Bash must remain standalone and must not require `ds` or the C runtime at execution time.

For example, `file.exists("x")` may call `stat` in VM mode, but emitted Bash should use Bash syntax such as `[[ -e "x" ]]` or an embedded Bash helper.

See `docs/runtime.md` for the detailed runtime plan, including strings, arrays, maps, process execution, regex concerns, and the hashmap reuse strategy.

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
ds test [path]
ds repl
ds debug ./script.ds
```

Initial versions only implement a small subset.

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
while
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

The parser should support command-mode parsing.

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
- future: type mismatch in CLI args;
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

Planned debug commands:

```sh
ds tokens file.ds
ds ast file.ds
ds hir file.ds
ds bytecode file.ds
ds imports file.ds
ds run --trace-cmd file.ds
ds run --trace-vm file.ds
```

These commands should make it easy to understand where a bug lives:

- lexer;
- parser;
- semantic checker;
- HIR lowering;
- bytecode generation;
- VM execution;
- Bash emission.

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

libs/
  hashmap/
    hashmap.c
    hashmap.h
    LICENSE
```

`libs/hashmap/` is a temporary staging location for the owned hashmap code. It is okay at the beginning, but the long-term architecture should absorb the useful implementation into `src/core/` behind the `DsMap` API so maps feel like a native part of the `ds` runtime, not a separate library subtree.

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
