# Runtime

This document describes the C runtime foundation that `ds` needs underneath the language frontend, bytecode VM, Bash emitter, and developer tools.

The runtime is not a separate user-facing language feature. It is the boring internal substrate that makes the rest of the project possible.

## Purpose

`ds` is written in C, so the implementation cannot rely on high-level host-language conveniences such as dynamic strings, vectors, maps, rich diagnostics, process wrappers, or safe value containers.

The project needs a small, explicit runtime layer that provides these primitives consistently.

The runtime should help `ds` stay:

- easy to debug;
- easy to test;
- easy to extend;
- memory-safe by convention;
- consistent across frontend, VM, Bash emitter, and tools;
- independent from unnecessary external complexity.

## Core rule

Runtime-backed features must still obey the main `ds` product rule:

> Every supported `ds` feature must run in the VM and emit to standalone Bash.

This means runtime APIs should not quietly create VM-only behavior.

For each runtime-backed language feature, the project should define:

- direct VM behavior;
- Bash emission behavior;
- test coverage for both;
- known limitations or parity differences.

## Runtime is not the Bash runtime

The C runtime is used by the `ds` tool itself.

Generated Bash must remain standalone and must not require `ds` or the C runtime at execution time.

Therefore:

- VM mode may call C runtime functions;
- Bash emission must generate Bash code or embedded Bash helper functions;
- emitted scripts must not dynamically call back into `ds` for normal operation.

Example:

```ds
if file.exists("package.json") {
  echo "found"
}
```

In VM mode, `file.exists` may call a C function using `stat`.

In emitted Bash, the same feature should become something like:

```bash
if [[ -e "package.json" ]]; then
  echo "found"
fi
```

## Core runtime types

The initial runtime should be small. Add only what the current milestone needs.

Expected core types:

```txt
DsStr             borrowed string view
DsString          owned dynamic string
DsArray           growable vector
DsMap             hashmap wrapper
DsArena           arena allocator
DsValue           tagged runtime value
DsSourceLoc       source location
DsSourceSpan      source span
DsDiag            diagnostic object/list
DsProcessResult   command execution result
DsRegex           regex wrapper, later
```

Not every type needs to exist in `v0.1.0`. This list defines the intended foundation.

## `DsStr`

`DsStr` is a borrowed string view.

Suggested shape:

```c
typedef struct {
    const char *ptr;
    size_t len;
} DsStr;
```

Use it for:

- token text;
- source slices;
- identifier names;
- map lookup keys;
- string comparison without copying.

Rules:

- `DsStr` does not own memory.
- `DsStr` may point into source text, interned strings, or owned dynamic strings.
- APIs must document whether they store a copy or only borrow the view temporarily.
- Length-aware comparisons must be used; do not assume NUL termination.

## `DsString`

`DsString` is an owned dynamic string.

Use it for:

- generated Bash output buffers;
- string interpolation results;
- diagnostic messages;
- command output capture;
- path construction;
- formatter output.

Needed operations:

- create/free;
- append bytes;
- append `DsStr`;
- append C string;
- append formatted text;
- reserve capacity;
- detach into owned buffer if needed.

Rules:

- ownership must be clear;
- appends must be bounds-safe;
- NUL termination may be provided for C interop, but length remains authoritative;
- avoid repeated small heap allocations in hot paths when an arena or reusable buffer is better.

## `DsArray`

`DsArray` is a growable vector.

Use it for:

- tokens;
- AST child lists;
- diagnostic lists;
- bytecode instructions;
- constants;
- command arguments;
- runtime arrays;
- test fixtures.

The implementation may be generic by macro or typed manually per use case.

Early project preference:

- keep it simple;
- avoid complex generic machinery;
- prefer explicit typed vectors if that is easier to debug in C.

## `DsMap`

`DsMap` is the internal hashmap abstraction.

Use it for:

- keyword lookup;
- symbol tables;
- lexical scopes;
- module cache;
- import cycle detection;
- builtin registry;
- Bash helper dependency tracking;
- generated symbol mapping;
- runtime map values later.

The project currently keeps the owned hashmap support code under `libs/hashmap/` as a temporary staging location. That is acceptable while `ds` is still docs-first and architecture-first, but it must not stay as a separate-feeling library forever.

Internal code should depend on `DsMap`, not directly on the hashmap API. Later, once the runtime shape is clearer, the hashmap implementation should be absorbed into `src/core/` as normal `ds` runtime code.

Suggested shape:

```c
typedef struct DsMap DsMap;

bool ds_map_init(DsMap *map);
bool ds_map_put(DsMap *map, DsStr key, void *value);
bool ds_map_get(DsMap *map, DsStr key, void **out);
bool ds_map_remove(DsMap *map, DsStr key, void **old_value);
void ds_map_free(DsMap *map);
```

The exact API can change, but the ownership rules must be explicit.

Questions every map API should answer:

- Are keys copied, borrowed, or moved?
- Are values owned or borrowed?
- Can keys contain embedded NUL bytes?
- Is iteration order stable or intentionally unspecified?
- Which allocator owns internal memory?


## Implemented v0.3.0 runtime subset

`v0.3.0` adds the first concrete runtime pieces used by the direct VM path.
They are intentionally small and internal-only:

- `DsString` owns a dynamic, NUL-terminated byte buffer while still tracking
  explicit length. It supports construction from C strings or source ranges,
  append-by-range, append-by-C-string, append-by-character, and explicit free.
- `DsValue` is a tagged value for `null`, `bool`, `int`, and owned `string`.
  String values copy or take ownership explicitly so VM values do not dangle
  after source buffers are released.
- `DsArray` is a minimal growable pointer vector used for runtime/compiler
  lists in this milestone.
- `DsMap` is the project-owned map wrapper used for VM variable storage. It
  copies keys, owns stored `DsValue` values, and does not expose the staged
  `libs/hashmap` API to the rest of the codebase.

The VM stores variables in a small runtime scope stack. The root scope holds
top-level declarations. Each lowered block pushes a child scope and pops it
when the block exits, so branch-local variables remain usable inside their
block but do not leak into later statements, outer scopes, or sibling blocks.
Lowering remains conservative and rejects nested shadowing for now; sibling
blocks may reuse the same branch-local name because their scopes are distinct.

The initial VM truthiness rule is deliberately simple and mirrored by tests:
`false`, `0`, `null`, and empty strings are falsey; `true`, non-zero integers,
and non-empty strings are truthy. Comparisons render values to deterministic
strings before comparing, matching the current Bash-emission limitation. A
future semantic pass may replace this with type-aware numeric dispatch.

## Hashmap absorption plan

The hashmap support code from the owned `hashmap` project is included in this repository under `libs/hashmap/` for now.

This directory is a temporary integration stage, not the long-term architecture.

Observed useful properties:

- C99 implementation;
- string/slice keys;
- length-aware keys;
- support for embedded NUL keys;
- `void *` values;
- copied, borrowed, and moved key APIs;
- custom allocator support;
- optional key arena support;
- iterator support;
- validation/statistics helpers;
- MIT license;
- blackbox tests passed during review.

Current temporary shape:

```txt
libs/hashmap/
  hashmap.c
  hashmap.h
  LICENSE

src/core/
  ds_map.c
  ds_map.h
```

Long-term absorbed shape:

```txt
src/core/
  map.c
  map.h
  map_internal.h      # only if needed
```

The exact filenames may change, but the long-term result should feel like `ds` owns the map implementation directly, not like the runtime depends on a separate library subtree.

The transition should happen in stages:

- keep `libs/hashmap/` while the project is being bootstrapped;
- create `src/core/ds_map.*` as the only API used by the rest of `ds`;
- add tests around `DsMap` behavior, ownership, iteration, deletion, and error cases;
- migrate useful hashmap implementation pieces into `src/core/`;
- rename symbols and files to match `ds` runtime naming;
- remove direct references to `libs/hashmap/` from normal runtime code;
- keep license/attribution notes if required by the original files;
- delete `libs/hashmap/` once the absorbed `DsMap` implementation is complete.

During the temporary stage, the intended layering is:

```txt
ds frontend / semantic / VM / emitter
  -> src/core/ds_map.*
    -> libs/hashmap/*
```

After absorption, the intended layering is:

```txt
ds frontend / semantic / VM / emitter
  -> src/core/map.*
```

`ds` should wrap the current `libs/hashmap` code with `DsMap` so the rest of the codebase is protected during the transition.

Reasons to wrap it:

- consistent `ds` ownership rules;
- easier absorption into the `ds` runtime later;
- simpler API for `ds` internals;
- integration with `DsStr`, `DsArena`, and diagnostics;
- avoiding hashmap-library naming leakage throughout the project.

The hashmap code is already included under `libs/hashmap/`. Keep its license and original project files intact while it is staged there. When absorbing it into `src/core/`, do the move intentionally in a cleanup version so tests can prove that behavior did not change.

## `DsArena`

`DsArena` is a bump allocator for temporary or phase-owned allocations.

Use it for:

- AST allocation;
- HIR allocation;
- temporary parser structures;
- semantic checker scratch memory;
- import graph construction;
- short-lived compiler pipeline data.

Rules:

- arena lifetime must be obvious;
- do not store arena pointers in longer-lived runtime values;
- diagnostics that survive the phase must copy needed text;
- avoid using arenas where individual frees are required.

Suggested project pattern:

```txt
source lifetime       owns source text
parse arena           owns AST
semantic arena        owns HIR/checker data
bytecode heap/arena   owns bytecode/constants
VM heap               owns runtime values
```

## `DsValue`

`DsValue` is the tagged value type used by the VM and possibly by constant folding or tests.

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
builtin
```

Suggested shape:

```c
typedef enum {
    DS_VAL_NULL,
    DS_VAL_BOOL,
    DS_VAL_INT,
    DS_VAL_STRING,
    DS_VAL_ARRAY,
    DS_VAL_MAP,
    DS_VAL_RESULT,
} DsValueKind;

typedef struct {
    DsValueKind kind;
    union {
        bool boolean;
        int64_t integer;
        DsString *string;
        void *object;
    } as;
} DsValue;
```

The real implementation can differ, but it must define:

- copy behavior;
- move behavior;
- destruction behavior;
- equality behavior;
- debug formatting;
- Bash emission equivalent where relevant.

## Process execution

Command execution is a core feature, not an afterthought.

The process API should sit behind a testable boundary.

Expected responsibilities:

- spawn commands;
- pass argv safely;
- optionally capture stdout;
- optionally capture stderr;
- stream output normally for plain command statements;
- return exit code;
- support command tracing;
- eventually support environment overrides and working directory overrides.

Suggested result:

```c
typedef struct {
    int exit_code;
    DsString stdout_text;
    DsString stderr_text;
    bool was_signaled;
} DsProcessResult;
```

This powers future `ds` syntax:

```ds
let result = run npm test

if result.failed {
  echo result.stderr
  exit result.code
}
```

Bash emission must implement equivalent behavior using Bash helpers and temporary capture where necessary.

## Regex support

Regex is useful, but it can easily create VM/Bash parity problems.

Potential syntax:

```ds
if text matches /error|failed/i {
  echo "bad log found"
}
```

The runtime may eventually provide `DsRegex`, but regex support should be added only when the project can define both:

- C runtime behavior;
- Bash emission behavior.

Important warning:

Bash `[[ string =~ regex ]]` uses Bash's regex behavior, while C libraries may use POSIX regex, PCRE, or another engine. These may not match perfectly.

Therefore the first regex milestone should either:

- restrict `ds` regex to a small portable subset; or
- clearly document differences; or
- delay regex until a parity strategy exists.

Regex should not be part of the initial frontend milestones unless explicitly scoped.

## Paths and filesystem

`ds` should eventually provide shell-oriented helpers:

```ds
file.exists("x")
file.is_file("x")
file.is_dir("x")
file.read("x")
file.write("x", "hello")
path.cwd()
path.join("src", "main.ds")
```

In VM mode, these use C runtime APIs.

In Bash emission mode, they must become Bash tests, shell builtins, or embedded Bash helper functions.

## Diagnostics integration

The runtime should support diagnostics instead of printing ad hoc errors from random places.

Good diagnostic shape:

```txt
deploy.ds:12:1 error: command failed with exit code 1

  npm run build
  ^^^^^^^^^^^^^

help: run with --trace-cmd to show commands before execution
```

Diagnostics should be usable from:

- lexer;
- parser;
- semantic checker;
- importer;
- Bash emitter;
- bytecode compiler;
- VM;
- process execution;
- test runner.

## Testing strategy

Runtime primitives must have direct C tests.

Expected test groups:

```txt
tests/core/string_test.c
tests/core/array_test.c
tests/core/map_test.c
tests/core/arena_test.c
tests/core/diag_test.c
tests/vm/value_test.c
tests/vm/process_test.c
```

For each primitive, test:

- normal usage;
- empty inputs;
- ownership behavior;
- growth behavior;
- invalid input handling;
- memory cleanup expectations;
- edge cases relevant to shell scripting.

Runtime tests are not a replacement for language tests. They support language tests.

## Documentation rule

When a runtime primitive becomes important enough to affect user-visible behavior, document the user-visible behavior in the relevant milestone spec and later in stable language docs.

For example:

- `DsMap` as an internal symbol table may only need runtime docs.
- User-facing map values need syntax docs, VM behavior, Bash emission behavior, and tests.

## Initial milestone impact

The runtime should influence the roadmap as follows:

- `v0.1.0` may need minimal `DsStr`, `DsArray`, source locations, and diagnostics.
- `v0.2.0` may need `DsString` for Bash output generation, but generated Bash must not depend on the C runtime.
- `v0.3.0` should become **Minimal C Runtime + Bytecode VM**, because the VM requires values, strings, process execution, and basic runtime ownership rules.

Do not overbuild the full runtime before it is needed. Build runtime pieces just ahead of the language features that require them.
