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

## Current value-kind contract

The v0.12.0 cleanup keeps the user-facing language surface unchanged, but makes
the internal value model explicit. Lowering and runtime currently reason about
these kinds:

```txt
null              internal absence / statement-only placeholder
bool              conditions and helper predicates
int               parsed literals, script args/options, command-result code
string            interpolation, command words, file/path/env/cmd helpers
command-result    captured `run` stdout/stderr/code/ok/failed fields
array             array literals, stdlib iterables, array `for` loops
map               map literals and string-key lookup
function          callable user declarations during lowering
unknown/error     conservative recovery after a reported diagnostic
```

`src/stdlib.c` is the source of truth for standard-library helper return kinds,
arity, statement-only status, iterable status, and validation flags. Lowering
maps those metadata return kinds into its local symbol/value-kind model; the VM
dispatch path validates the helper through the same metadata before executing
runtime behavior; Bash emission uses the metadata helper names when rendering
calls. The lowered representation remains the shared backend contract between VM
execution and standalone Bash emission.

The lowerer is split into focused private components rather than one semantic
god file. `src/lower_expr.c` owns expression-level value-kind checks, command
result field validation, collection literal rules, and command-word validation;
`src/lower_stmt.c` owns statement/block lowering; `src/lower_functions.c` owns
function signatures, defaults, body lowering, and recursion rejection;
`src/lower_tests.c` owns test metadata collection; `src/lower_stdlib.c` owns
script declarations and string/default decoding; `src/lower_symbols.c` owns
scopes and shared vector/name utilities; and `src/lower_free.c` owns lowered HIR
cleanup. `src/lower_internal.h` is the private boundary between those lowering
components.

Statement-only helpers such as `file.write`, `file.append`, `cmd.require`,
`env.set`, and `env.unset` are not values. Value-returning helpers are rejected
as bare statements unless the language explicitly documents that statement form.
Collection literals still keep the conservative Bash-emission rule: elements
must be expressions the Bash backend can assign without changing VM/Bash parity.

## Standard-library runtime boundary

VM stdlib execution now lives in `src/vm_stdlib.c`, behind the VM-private
`ds_vm_stdlib_call()` entrypoint declared in `src/vm_internal.h`. The rest of the
VM is split into focused private components: `src/vm_compile.c` for HIR to
bytecode construction, `src/vm_dump.c` for bytecode/debug output,
`src/vm_args.c` for script argument binding, `src/vm_scope.c` for scopes and
function calls, `src/vm_process.c` for command interpolation/redirection and
subprocess execution, `src/vm_test.c` for VM-backed test execution setup, and
`src/vm.c` for the main interpreter loop and public VM entrypoints. This keeps
file/path/env/cmd/glob/lines runtime implementations and other VM subsystems
separate while preserving the same spans, ownership rules, and fail-fast
diagnostics.

Standalone Bash emission is split into focused backend components:
`src/bash_emit.c` owns the public entrypoint, script-argument prelude, helper
selection, and artifact writing; `src/bash_deps.c` decides which embedded helper
bodies are needed; `src/bash_expr.c`, `src/bash_command.c`, and
`src/bash_stmt.c` render expressions, commands, and statements respectively;
`src/bash_quote.c` owns shared quoting/interpolation and emitter utilities; and
`src/bash_helpers.c` owns the embedded helper body strings. This keeps helper
body review separate from rendering logic while preserving the reserved `__ds_`
helper prefix and standalone-script requirement.

`v0.13.0` adds debugging/tracing runtime surfaces without adding source-language
syntax. The VM can trace command execution and instruction execution to stderr
through `DsVmOptions`; traces are intentionally observational and must not change
script stdout, exit status, captured-command behavior, or plain-command
fail-fast semantics. The Bash backend emits small standalone `__ds_` debug
helpers only for command-using scripts so `DS_TRACE_CMD=1 bash script.sh` can
trace expanded command argv and plain command failures can include embedded
source markers without requiring `ds` at Bash runtime.

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


## Internal header boundaries

The implementation no longer keeps every project type in one monolithic header.
`include/ds.h` is a thin façade, while internal code should include the focused
header for the layer it touches:

- `src/ds_common.h`: sources, spans, diagnostics, allocation helpers;
- `src/ds_command.h`: command words, redirections, command-result fields;
- `src/ds_ast.h`: AST nodes and script/function/test declaration shapes;
- `src/frontend.h`: lexer/parser token and frontend entrypoints;
- `src/ds_hir.h`: lowered HIR program, expressions, statements, functions, tests;
- `src/ds_runtime.h`: runtime values, strings, arrays, maps;
- `src/ds_stdlib.h`: standard-library metadata;
- `src/backend.h`: formatter/checker, Bash emitter, bytecode, and VM entrypoints.

This is a behavior-preserving boundary split. It reduces accidental coupling
without making runtime maps, VM bytecode internals, parser cursors, or Bash
emitter internals part of the user-facing language contract.

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

The owned hashmap support code has been absorbed into `src/runtime/hashmap.c` and `src/runtime/hashmap.h`. It remains a private implementation detail of `DsMap`; parser, lowering, VM, Bash emission, and public internal headers should not include the raw hashmap header or call `hm_*` APIs directly.

The frontend parser is componentized by grammar area. `src/parser.c` now owns only
the public parse entrypoint/top-level loop, with shared private cursor helpers in
`src/parser_internal.h`; expression, command, script, function/test, and general
statement parsing live in `src/parse_expr.c`, `src/parse_command.c`,
`src/parse_script.c`, `src/parse_function.c`, and `src/parse_stmt.c`. This keeps
command-mode parsing isolated from expression and statement parsing while
preserving the single shared frontend used by checking, VM execution, test
running, and Bash emission.

Internal code should depend on `DsMap`, not directly on the hashmap API. `src/runtime.c` is the bridge that translates the project-owned `DsMap` operations into raw hashmap operations and owns value cleanup rules. Runtime declarations now live in `src/ds_runtime.h`; `include/ds.h` remains a compatibility façade that re-exports the focused internal headers for existing tool/unit harnesses.

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
  copies keys, owns stored `DsValue` values, and does not expose the private
  raw hashmap API to the rest of the codebase.

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

## Implemented v0.4.0 ownership cleanup

`v0.4.0` keeps the runtime behavior intentionally small, but makes the current
ownership rules explicit enough for future features to reuse safely:

- `DsString` owns its `data` buffer. `len` is the byte length excluding the
  trailing NUL byte, and `cap` is the allocated capacity. `ds_string_free()` is
  the only destructor for a live string buffer.
- `ds_value_string_take()` transfers ownership of a `DsString` buffer into a
  `DsValue` and reinitializes the source `DsString`, so there is exactly one
  owner after the call.
- `ds_value_copy()` deep-copies string values and trivially copies `null`,
  `bool`, and `int` values. `ds_value_free()` is the single destructor for owned
  value contents.
- `DsArray` is a borrowed pointer vector. `ds_array_clear()` resets the logical
  length without freeing pointed-to items, and `ds_array_free()` releases the
  vector storage. Typed owning vectors should be introduced before storing owned
  values in it.
- `DsMap` copies keys and owns stored `DsValue` contents. Setting an existing
  key frees the previous value before storing the replacement. `ds_map_clear()`
  frees all keys and values while keeping capacity for reuse; `ds_map_free()`
  clears and releases storage.

The absorbed hashmap code is now used behind the `DsMap` runtime wrapper.
Normal frontend, lowering, VM, and Bash-emitter code should still depend only on
`DsMap` and its small API, not on the raw hashmap API directly. `src/runtime.c`
is the bridge between `DsMap` ownership rules and the private implementation in
`src/runtime/hashmap.c`.


## Implemented v0.5.0 argv binding

`v0.5.0` adds first-class script argument binding for the VM path. Lowering
produces a script argument contract for `arg`, `option`, and `flag` declarations.
Before executing bytecode, the VM parses runtime argv, applies defaults, validates
required positionals and typed integer/bool values, and stores the resulting
`DsValue`s in the root scope. Body statements then use those names like normal
top-level variables.

The emitted Bash path does not use the C runtime. It emits a standalone Bash argv
parser from the same lowered contract and binds parsed values to the existing
`__ds_` variable namespace before the emitted body.

## Hashmap absorption

The hashmap support code from the owned `hashmap` project is included in this
repository under `src/runtime/` as normal project-owned runtime code.

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

Current absorbed shape:

```txt
src/runtime/
  hashmap.c
  hashmap.h
  hashmap.LICENSE

src/runtime.c      # DsMap bridge and value ownership rules
```

Possible later split if runtime files continue to grow:

```txt
src/core/
  map.c
  map_internal.h
```

The intended layering is:

```txt
ds frontend / semantic / VM / emitter
  -> DsMap API in include/ds.h
    -> src/runtime.c
      -> src/runtime/hashmap.*
```

`ds` wraps raw hashmap operations with `DsMap` so the rest of the codebase is
protected from implementation naming, allocation, key-copying, and value-freeing
details.

Reasons to wrap it:

- consistent `ds` ownership rules;
- easier future movement between `src/runtime/` and `src/core/` if needed;
- simpler API for `ds` internals;
- integration with `DsStr`, `DsArena`, and diagnostics;
- avoiding hashmap-library naming leakage throughout the project.

The original license text is preserved in `src/runtime/hashmap.LICENSE`.

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
deploy.ds:12:1: error: command failed with exit code 1

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

The v0.14.0 test runner uses the normal composed parse/lower pipeline and the VM backend. Test blocks are lowered as test metadata rather than production statements, so normal `ds run`, direct execution, and normal standalone Bash emission do not execute assertions or print test summaries. Inside `ds test`, `assert expr` uses the VM truthiness rules, `fail "message"` fails the active test, `exit 0` stops the active test as a pass, and `exit nonzero` fails the active test. Command output from tests streams normally before each `ok`/`fail` line; the runner does not capture or hide it in this milestone.

The v0.16.0 cleanup keeps test execution on the same composed CLI program
boundary as `check`, `hir`, `bytecode`, `run`, direct execution, and
`emit bash`. The boundary lives in `src/cli_program.c`; it owns import-aware
source loading and lowering before the VM test runner receives a lowered
program. Normal execution and emitted Bash continue to skip test metadata.

The v0.17.0 control-flow implementation keeps VM and Bash behavior aligned for
scalar reassignment, `while`, lexical `break`/`continue`, and expression-style
`case`. VM bytecode uses explicit jump and scoped-pop instructions so loop
control exits only the active loop scopes. Case dispatch uses kind-aware exact
case matching: an integer `1` does not match the string `"1"`, and boolean `true`
does not match the string `"true"`. Bash emission uses native `while`, `break`,
and `continue`; case dispatch is emitted as exact `if`/`elif` tests rather than
Bash glob patterns, with small sidecar type tags for emitted variables so Bash
does not accidentally coerce unlike ds literal kinds. The same tags are also
used when an emitted `if` or `while` condition is a scalar variable whose ds
truthiness would otherwise differ from a raw shell string test.
Selectors whose scalar kind is still unknown after lowering are rejected in this
milestone. The v0.21.0 function-value work extends that metadata through scalar
function returns and through explicit arguments validated against defaulted
parameter kinds, so emitted Bash keeps kind-aware `case` parity for the supported
function-call forms.

The v0.21.0 runtime value-return path transports scalar `string`, `int`, and
`bool` results out of user functions. VM execution stores the returned value in
the caller; standalone Bash uses `__ds_return` plus an expression-capture helper
that rejects unexpected stdout from value-style calls. Plain command statements
inside value-returning functions are therefore rejected; captured `run`
expressions remain the supported way to use command results inside functions.
Integer arithmetic uses the same signed 64-bit contract in both backends: `*`,
`/`, `%`, `**`, unary `-`, and compound integer updates diagnose division by
zero, negative exponents, out-of-range integer literals, and overflow instead of
silently wrapping. Expression-backed interpolation supports scalar function
calls; command-word interpolation supports legacy variable/field interpolation
and integer arithmetic, while direct function calls in command words must be
bound first.

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


## Import execution model

As of v0.6.0, imports are deterministic local inclusion rather than modules. Imported statements execute once in composed order before the importing file's dependent statements. Root `script { ... }` argument parsing still happens once; imported files cannot declare their own script blocks in this first import milestone. Emitted Bash bundles imported statements and does not call `ds`.

## Command-result ownership

As of v0.7.0, the runtime has a command-result value used by the VM for `let result = run ...`. It owns separate stdout and stderr strings plus an integer exit code. Copies deep-copy both captured buffers, and value cleanup frees them exactly once through the normal `DsValue` ownership path.

Captured VM commands do not treat non-zero exit status as fatal. The process result is bound to the destination variable so scripts can inspect `stdout`, `stderr`, `code`, `ok`, and `failed`. Plain command statements still stream normally and preserve fail-fast exit behavior; redirection opens the target file before child execution so open failures can report the source span of the redirection target.

The `v0.8.0` cleanup keeps `DsCommandResult` as the user-visible runtime value for captured commands while tightening the internal process path. The VM now uses a small internal process spec/result wrapper shared by plain and captured commands. The spec keeps rendered argv, capture mode, command span, and redirection metadata together; the result carries normalized exit code plus captured buffers when capture is enabled. Command-result field metadata lives in one descriptor table; that table is used by lowering and by runtime field expansion so future fields cannot silently drift across VM and Bash paths.

## v0.18.0 pipeline runtime

`v0.18.0` adds linear command pipelines to the same VM/Bash parity contract as
ordinary commands. The VM does not invoke a shell to interpret `|`; it expands
each stage with the existing command-word rules, creates OS pipes between
adjacent stages, forks every stage, wires stdin/stdout with `dup2`, closes
unused file descriptors in parent and child paths, waits for all children, and
computes Bash `pipefail`-style status by returning the rightmost non-zero stage
status.

Plain pipelines are fail-fast like plain commands. Redirection suffixes apply to
the whole plain pipeline: stdout redirection targets the final stage stdout,
stderr redirection captures stderr from every stage, and combined redirection
captures final stdout plus every stage stderr. Captured `run` pipelines produce
the existing command-result fields: final-stage stdout, all observed stage
stderr, pipefail code, `ok`, and `failed`.

Generated Bash emits normal Bash pipelines under `set -euo pipefail`. Captured
pipeline emission is structured: the emitter writes the exact staged argv words
directly into a Bash pipeline, redirects that pipeline into temporary capture
files, and records stdout/stderr/code/ok/failed fields without using `eval` or
depending on the `ds` binary.

## v0.19.0 string runtime

`v0.19.0` keeps string helpers inside the normal VM/Bash parity boundary. The VM
implements ASCII `trim`, `upper`, `lower`, literal `replace`, literal
`contains`, literal `split`, `starts_with`, and `ends_with` using owned
`DsString`/`DsArray` values. `split` returns an array of owned string values that
works with existing array indexing and `for` loops.

Triple-quoted literals use the simple byte rule: every byte between the opening
`"""` and closing `"""` is literal content. They do not strip indentation.
Formatted interpolation is rendered during the existing interpolation step. The
supported subset is intentionally small: string transforms, string
width/alignment, integer padding, and fixed decimal rendering for integer values.
General floats and full printf-style formatting remain out of scope.

`v0.20.0` keeps the public language surface unchanged while making Wave 2
composition less conservative. The lowerer now remembers known array element
kinds for array literals and string-array helpers such as `string.split`,
`lines`, `glob`, and `glob!`. Indexing one of those arrays can therefore keep a
known string/int/bool kind for later checks, which lets scoped string methods
compose with indexed split/line/glob values without relying on runtime coercion
or Bash-only string behavior. The same lowered element-kind metadata is carried
into standalone Bash `case` emission, so known indexed array selectors and array
`for` loop variables are matched as strings, ints, or bools instead of being
collapsed to strings. Unknown element kinds remain unknown. Function parameters with literal defaults carry that default's static kind through the single lowered function body, and emitted Bash assigns the matching parameter type tag when the default branch is used, so defaulted string parameters can use string methods and defaulted string/int/bool parameters can be used as kind-aware `case` selectors. Required untyped parameters and explicit runtime argument kind propagation remain unknown until typed parameters or a runtime type-tag design is deliberately added.
