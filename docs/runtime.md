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
int               parsed literals, script args/options, command-result status/code
string            interpolation, command words, file/path/env/cmd helpers
command-result    captured `run` stdout/stderr/status/code/ok/failed fields
array             array literals, stdlib iterables, array `for` loops
map               map literals and string-key lookup
function          callable user declarations during lowering
unknown/error     conservative recovery after a reported diagnostic
```

`src/ds_stdlib.c` is the source of truth for standard-library helper return kinds,
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
`src/lower.c` owns the small test metadata collection pass; `src/lower_stdlib.c` owns
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
subprocess execution, `src/vm.c` for VM-backed test execution setup, and
`src/vm.c` for the main interpreter loop and public VM entrypoints. This keeps
file/path/env/cmd/glob/lines runtime implementations and other VM subsystems
separate while preserving the same spans, ownership rules, and fail-fast
diagnostics.

Standalone Bash emission is split into focused backend components:
`src/bash_emit.c` owns the public entrypoint, script-argument prelude, helper
selection, and artifact writing; `src/bash_deps.c` decides which embedded helper
bodies are needed by walking accepted HIR expression payloads, including
statement-style user function call arguments; `src/bash_expr.c`,
`src/bash_command.c`, and
`src/bash_stmt.c` render expressions, commands, and statements respectively;
`src/bash_quote.c` owns shared quoting/interpolation and emitter utilities; and
`src/bash_helpers.c` owns the embedded helper body strings. This keeps helper
body review separate from rendering logic while preserving the reserved `__ds_`
helper prefix and standalone-script requirement.

String helpers are dependency-scanned by helper name. A script that only uses
`.len()` emits the scalar capture helper when assignment needs it and
`__ds_string_len`, but does not emit unrelated string helpers such as
`__ds_string_slice`, `__ds_string_char_at`, or `__ds_string_split`.

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

Regex is useful, but it can easily create VM/Bash parity problems. The supported
surface is deliberately conservative and is restricted to the VM/Bash portable
subset documented below.

Supported boolean syntax:

```ds
if text matches /error|failed/i {
  echo "bad log found"
}
```

`v0.32.0` also adds runtime string pattern matching and the `regex.match` /
`regex.replace` helpers:

```ds
let pattern = "^release/[0-9]+$"
let ok = branch matches pattern

let m = regex.match("v1.2.3", /^v([0-9]+)\.([0-9]+)\.([0-9]+)$/)
let normalized = regex.replace("api-123 web-456", /([a-z]+)-([0-9]+)/, "$1:$2")
```

The runtime does not expose first-class compiled regex values. Regex execution
is owned by the VM and standalone Bash helpers for accepted HIR only.

Important warning:

Bash `[[ string =~ regex ]]` uses Bash's regex behavior, while C libraries may use POSIX regex, PCRE, or another engine. These may not match perfectly.

Therefore accepted regex syntax is restricted to the shared POSIX-ERE-shaped
subset, and unsupported forms are rejected by the lexer/parser, lowerer, or
runtime helpers before backend behavior can silently diverge.

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

The v0.25.0 runtime value-return path transports scalar `string`, `int`, and
`bool` results out of user functions. The v0.26.0 extension adds structured
function returns for flat scalar arrays, flat scalar maps/objects, and
command-result objects through the same private function-value boundary.
The structured function returns stay inside the flat scalar collection boundary. The
flat scalar collection boundary is intentional: collection payloads may carry
only scalar string/int/bool elements or values, not nested arrays/maps or
command-result entries. VM execution stores the returned `DsValue` in the
caller; standalone Bash uses private `__ds_return_type` metadata plus
`__ds_` payload variables for scalar, array, map, and command-result shapes.
The exact Bash encoding is intentionally private, but it must preserve empty
strings, whitespace, shell metacharacters, text newlines, and scalar kind
metadata without calling the `ds` binary from generated scripts. For returned
arrays, generated Bash copies both the array payload and the per-element scalar
kind sidecar used by indexing and `in` comparisons. For returned maps, generated
Bash copies both the associative-array payload and the per-key scalar kind
sidecar used by field/index reads, so returned `false` and `0` values keep ds
truthiness instead of becoming truthy shell strings.

The v0.29.0 map-iteration path lowers `for key, value in map` to an explicit
two-name HIR loop. VM execution snapshots and sorts map keys before each loop;
standalone Bash materializes the accepted map source into a private associative
array and sorts the key list with `LC_ALL=C`. Both backends therefore expose the
same deterministic ascending bytewise/ASCII key order while keeping loop key and
value variables scoped to the loop body.

User-function calls that initialize, assign, forward-return, participate in
string-sensitive conditions, select `case` arms, or feed direct user-function
arguments use temporary assignment-by-reference materialization rather than
Bash command substitution, so string returns preserve trailing newlines exactly
in those supported call positions. Plain command statements inside
value-returning functions are therefore rejected; captured `run` expressions are
the supported way to return command-result data from functions. Nested
collections, arrays/maps inside maps, collection-valued parameters, structured
value interpolation in command words, and public access to the private payload
format remain deferred. Direct scalar value-returning function calls in quoted
command words are pre-materialized through the same private function-value ABI
used by expression calls, so the outer command is not launched when an
interpolated call fails.

Environment reads through `env.NAME` lower to runtime environment lookups and
read as an empty string when the variable is absent. Environment assignments
through `env.NAME = value` accept scalar string/int/bool values, render them to
the same string form used by interpolation, update the current VM process
environment, and export the value to later child commands. Emitted Bash uses
strict-mode-safe `${NAME:-}` reads and quoted `export NAME="$value"`
assignments, so generated scripts read the environment at Bash runtime rather
than baking values in during emission. `unset env.NAME` removes the variable
from the current script environment; later DS environment reads return an empty
string and later child commands do not receive the variable from DS unless the
parent shell already supplies it again.
Integer arithmetic uses the same signed 64-bit contract in both backends: `*`,
`/`, `%`, `**`, unary `-`, and compound integer updates diagnose division by
zero, negative exponents, out-of-range integer literals, and overflow instead of
silently wrapping. Expression-backed interpolation supports scalar function
calls and checked scalar string method chains such as `{s.len()}` or
`{s.slice(0, 3)}`. Quoted command words use the same scalar interpolation
surface for direct value-returning function calls and string method chains by
pre-materializing each interpolated call or scalar method-chain expression that
needs runtime work into a private string variable before the command launches,
preserving quoting and avoiding generated Bash calls back into `ds`. Those compiler-generated variables
are visible only to the lowered command/run operation: they are not added to the
source symbol scope, they do not collide with user variables, and later source
code cannot read them as normal variables.

`v0.34.0` keeps that interpolation surface but adds one literal-brace spelling
inside ordinary and triple-quoted strings: `{{` renders `{`, `}}` renders `}`,
and `{expr}` continues to mean interpolation. Lowering rejects a lone `}` with a
source diagnostic that tells users to write `}}` for a literal close brace.
Consequently `"{{name}}"` renders literal `{name}`, while `"{name}}}"` renders
the interpolated `name` followed by a literal `}`. The VM string interpolator,
generated Bash string quoting, command-word interpolation validation, and Bash
expression fragments all share this contract so accepted source has VM/Bash
parity.


## Cleanup and signal runtime

The v0.22.0 cleanup model is process-level and registration-based. Reaching a `defer` statement registers a stackable handler for a signal; plain `defer` is the same signal class as `defer on: "EXIT"`. Reaching a `trap` statement installs one replacement handler for that signal, so a later `trap "EXIT" { ... }` replaces an earlier one. Supported signal names are the string literals `"EXIT"`, `"INT"`, and `"TERM"`.

Handler registration is process-scope, not RAII-style function-scope cleanup.
Function-local handler captures are rejected because cleanup may run after the
function has returned and VM/Bash parity cannot safely preserve that local scope
yet. Direct `return` from cleanup handlers is rejected; use `exit` when a
handler must choose a process-level final status.

Handler context values such as a `$LINENO`-equivalent are explicitly deferred in
the finalized v0.22 supported subset. The VM and emitted Bash do not currently
expose a portable cleanup-context object or line-number variable to handlers;
adding one needs a shared source-location model that remains stable across
lowering, imports, formatting, and generated Bash.

Cleanup runs for normal completion, explicit `exit`, explicit `fail`, command
failure, and the supported `INT`/`TERM` paths. The deterministic v0.22.1 cleanup
core covers the non-signal cases first, v0.22.2 locks down the `INT`/`TERM`
syntax, diagnostics, formatting, lowering visibility, and emitted-Bash helper
shape without sending real OS signals, v0.22.3 adds the deterministic
process-session signal harness with the smallest cooperative `TERM`
direct-command proof for VM and emitted Bash, v0.22.4 extends that
harness to non-cooperative foreground direct commands for both `INT` and
`TERM`, and v0.22.5 extends the same supported signal contract to simple
foreground pipelines. v0.22.6 closes the milestone by documenting the final
supported, rejected, deferred, and out-of-scope cleanup/signal behavior without
adding new handler-context syntax.

On normal completion, explicit `exit`, `fail`, or direct-command failure, the runtime runs the `EXIT` trap first when present, then `EXIT` defers in last-in, first-out order. On `INT` or `TERM`, the runtime runs the matching trap first, then matching defers in last-in, first-out order, then the `EXIT` cleanup sequence. The original status is preserved when cleanup succeeds; handler failures can replace the final status, and explicit `exit N` in emitted Bash follows Bash's process exit behavior while the cleanup guard prevents recursive handler execution.

The VM implements cleanup without relying on Bash by lowering handler registration to bytecode and executing registered handler bytecode during shutdown. It installs lightweight `INT` and `TERM` signal handlers, observes pending signals between bytecode instructions, and now also classifies interrupted foreground direct commands and simple foreground pipelines while waiting for child processes. During VM command execution, child commands and pipelines are placed in a foreground process group when possible; an `INT` or `TERM` observed by the parent is forwarded to that group, and a child terminated by `INT` or `TERM` runs the matching ds cleanup path instead of degrading into a generic command-failure diagnostic. Generated Bash emits standalone trap dispatchers and handler functions under the reserved `__ds_` namespace. For foreground direct commands and simple foreground pipeline statements, generated Bash runs the command through cleanup-aware helpers so `INT` and `TERM` exits become ds signal cleanup events instead of generic command/pipeline failures or shell job-control messages. Since `v0.34.0`, the common non-interactive closed-stdout case is also quieted for uncaptured, unredirected top-level command statements and pipelines. VM execution uses raw wait status and quiets only an actual direct-command or final-pipeline-stage `SIGPIPE` termination with pipe-like stdout and no non-SIGPIPE pipeline failures; emitted Bash is standalone and therefore uses the narrow portable heuristic of status `141` with pipe-like stdout. Quiet cases are treated as successful early script completion after supported cleanup runs, avoiding noisy `script | head` diagnostics. Captured `run` commands and captured pipelines still preserve the observed command-result fields, including broken-pipe-like statuses, and unrelated command/pipeline failures remain fail-fast apart from Bash's documented inability to distinguish explicit `exit 141` from real `SIGPIPE` under an inherited pipe without unsafe probe writes or non-standalone helpers. Background child management, arbitrary job-control APIs, asynchronous pipelines, and broad signal-forwarding semantics outside the supported foreground subset remain deferred.

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

Captured VM commands do not treat non-zero exit status as fatal. The process result is bound to the destination variable so scripts can inspect `stdout`, `stderr`, `status`, `code`, `ok`, and `failed`; `status` and `code` are integer aliases for the normalized command exit status. Plain command statements still stream normally and preserve fail-fast exit behavior; redirection opens the target file before child execution so open failures can report the source span of the redirection target.

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
stderr, pipefail `status`/`code`, `ok`, and `failed`.

Generated Bash emits normal Bash pipelines under `set -euo pipefail`. Captured
pipeline emission is structured: the emitter writes the exact staged argv words
directly into a Bash pipeline, redirects that pipeline into temporary capture
files, and records stdout/stderr/status/code/ok/failed fields without using `eval` or
depending on the `ds` binary.

## v0.19.0 string runtime

`v0.19.0` keeps string helpers inside the normal VM/Bash parity boundary. The VM
implements ASCII `trim`, `upper`, `lower`, literal `replace`, literal
`contains`, literal `split`, `starts_with`, and `ends_with` using owned
`DsString`/`DsArray` values. `split` returns an array of owned string values that
works with existing array indexing and `for` loops.

`v0.35.0` keeps the same parity boundary and adds byte-oriented parsing helpers:
`len`, `index_of`, `last_index_of`, `count`, `char_at`, and `slice`. VM helpers
operate on explicit byte lengths and emitted Bash helpers force `LC_ALL=C` for
substring/length arithmetic. `char_at` and `slice` reject negative or
out-of-range indexes at runtime for dynamic values, while static argument-kind
validation remains lowerer-owned. Bash emission also supports the narrow
read-only temporary array case needed for `string.split(...)[index]` chains by
materializing the split result inside a command-substitution-local array before
calling the existing array lookup helper. The same expression path is available
inside interpolation and quoted command words for scalar method chains, so
`"len={s.len()}"` and command arguments such as `"{s.slice(0, 3)}"` have the
same VM/Bash behavior as ordinary `let` expressions.

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
collapsed to strings. Unknown element kinds remain unknown. Function parameters
with literal defaults carry that default's static kind through the single
lowered function body, and `v0.36.0` extends the same lowered metadata path to
required parameters whose local usage infers `string`, `int`, or `bool`.
Inferred/defaulted parameter kinds feed return-kind predeclaration, HIR/debug
output, VM function metadata, and emitted Bash private type sidecars. Supported
call sites are validated before function body execution/emission; standalone
Bash also emits defensive pre-body checks for inferred/defaulted scalar
signatures. Required parameters used only in neutral contexts remain `unknown`
until public typed parameters or a broader collection/type design is
deliberately added.

## v0.23.0 regex, range, and membership runtime

### Membership equality

`v0.23.0` implements `needle in array` for arrays whose element kind is one of
the supported scalar kinds: `string`, `int`, or `bool`. Membership uses exact
ds value equality, not substring matching, Bash glob matching, or string-only
coercion. The VM evaluates the left operand once, evaluates the right operand
once, then scans the array using kind-aware comparisons, so string `"2"`, int
`2`, and bool-like strings such as `"true"` remain distinct values.

Generated Bash preserves the same contract with value-kind sidecars for arrays
and membership operands. Empty arrays are supported as always-false membership
sources. Map membership, command-result membership, range membership, regex
membership, and arbitrary iterable membership remain deferred until the language
has a broader iterable/value model.

### Range loop semantics

`v0.23.0` implements `for n in start..end { ... }` as an inclusive integer loop
source. Range expressions are not first-class values in this milestone; they are
accepted only in `for` loop-source position. Both bounds must be integers. The
VM evaluates the start expression once before the loop and the end expression
once before the loop, binds the loop variable as an `int`, and iterates upward
while `start <= end`. A range whose start is greater than its end runs zero
iterations instead of counting down.

Generated Bash uses arithmetic loop constructs with temporary bound variables
rather than brace expansion, so runtime bounds, arithmetic bounds, and supported
scalar-returning function-call bounds keep VM/Bash parity. Open-ended,
half-open, stepped, reverse-counting, non-integer, and value-position ranges are
rejected before execution or emission.

### Regex subset

`v0.23.0` implements `string matches /pattern/` and `string matches /pattern/i`
with search semantics: patterns match anywhere unless they are anchored by `^`
or `$`. `v0.32.0` accepts string-kind runtime string patterns for `matches` and
for `regex.match` / `regex.replace`; direct string literals are validated during
lowering, while dynamic strings are validated at runtime before later side
effects. The supported subset is the portable POSIX-ERE-shaped surface shared by
the VM implementation and Bash `[[ string =~ regex ]]`: literal characters
except unescaped `/` and newline in regex literals, escaped `/`, escaped
backslash, anchors, `.`, character classes, capturing groups, alternation, and
`*`, `+`, `?`, `{m}`, and `{m,n}` quantifiers.

Regex literals support trailing `/i`; `regex.match` and `regex.replace` accept an
optional flags string of either `""` or `"i"`. Runtime-string `matches` does not
have a flags slot; use `regex.match(text, pattern, "i").matched` when dynamic
flags are needed.

`regex.match(text, pattern[, flags])` returns a flat map:

- `matched`: bool;
- `full` and `"0"`: the full matched substring, or `""` when there is no match;
- `"1"` through `"9"`: numbered capture strings for capture groups present in
  the validated pattern. Optional unmatched captures and no-match captures are
  represented as `""`.

`regex.replace(text, pattern, replacement[, flags])` performs global
left-to-right replacement over non-overlapping matches. Replacement text supports
`$0` for the full match, `$1` through `$9` for captures, and `$$` for a literal
`$`. A lone `$`, unknown replacement escape, capture reference that cannot
exist, too many capture groups, invalid runtime flags, invalid runtime patterns,
and zero-length replacement matches fail with clear diagnostics.

Unsupported regex constructs such as lookaround, pattern backreferences, named
captures, inline flags, lazy quantifiers, Unicode classes, word-boundary
shortcuts, non-capturing groups, and multiline literals are rejected during
checking/lowering or runtime validation so VM execution and standalone Bash do
not silently diverge.

## v0.24.0 pre-1.0 hardening runtime notes

`v0.24.0` does not add production syntax. It hardens the existing runtime and
emission contract before the `1.0.0` release checklist:

- VM execution and emitted Bash remain the required parity pair for every
  supported production feature.
- Generated Bash is standalone and must not invoke `ds` or depend on the C
  runtime after emission.
- Generated helper functions remain in the reserved `__ds_` namespace. The
  common `__ds_error` and plain-command failure helpers are emitted once when
  needed instead of being repeated by each helper family.
- Bash-version guards remain required for Bash-4-only behavior such as
  associative-array-backed maps and map iteration.
- Unsupported or deferred constructs should fail in checking/lowering/emission
  before user commands run or invalid Bash is written.
- Sanitizer-backed aggregate runs are part of the release checklist for source
  buffers, diagnostics, HIR/bytecode, VM values, command captures, cleanup
  handler state, and Bash emission buffers.

## v0.30.0 flat index-assignment runtime notes

`v0.30.0` adds mutation only for named flat collections represented by an
explicit HIR index-assignment statement. Arrays replace existing elements at
integer indexes; out-of-range, negative, and non-integer indexes are runtime data
failures in both VM execution and emitted Bash. Assignment never appends and does
not create sparse arrays.

Maps insert or replace entries at non-empty string keys. Empty or non-string keys
are rejected by the lowerer when statically known and by VM/Bash helpers when the
key value is only known at runtime. Emitted Bash updates the value-kind sidecar
for every accepted assignment so later map reads, interpolation, conditions,
`case`, returns, and map iteration preserve VM/Bash scalar-kind parity. Quoted
command-word interpolation accepts flat named collection index reads such as
`{items[0]}` and `{map[key]}` plus checked scalar method chains that build on
accepted scalar values; formatting those indexed segments directly remains
deferred, so bind the indexed value first when a format specifier is needed.

Collection bindings copy by value, not by reference. `let b = a` where `a` is a
flat array or map deep-copies the collection payload and its scalar-kind sidecar
metadata in emitted Bash, matching the VM's `ds_value_copy()` behavior. Later
`b[index] = value` or `b[key] = value` mutations therefore do not mutate `a`.

Nested mutation, field-style map assignment, temporary/function-result mutation,
compound index assignment, deletion, aliases/references, and same-map mutation
during map iteration remain unsupported language forms rather than backend
runtime behavior.

## v0.37.0 row-array runtime notes

`v0.37.0` keeps lightweight rows inside the existing flat map/array value
model. A row is a flat map whose fields are scalar `string`, `int`, or `bool`
values, and a row-array is an array whose elements share one lowered row schema.
The schema is lowerer-owned metadata: it records field names and scalar kinds so
field access, row-array `push`, loop variables, indexed rows, returned rows, and
`sort_by(field[, direction])` have the same checks before VM execution and Bash
emission.

The VM stores rows as copied `DsValue` maps inside copied `DsValue` arrays. Row
and row-array assignment follows the existing collection copy-by-value rule, so
later `push` operations on a copied row array do not mutate the original. The VM
sorts row arrays by the requested string/int/bool field using deterministic,
stable ordering and returns a sorted copy rather than mutating the receiver.

Generated Bash remains standalone. It represents row arrays with reserved
`__ds_` sidecar arrays: one array tracks row positions and element kind, and one
field-specific sidecar array stores each scalar field's bytes. Function returns
and assignments copy those sidecars so row-array payloads preserve spaces, tabs,
newlines, shell metacharacters, braces, and scalar kind metadata without calling
back into `ds`. Runtime row-array index failures are fail-fast in both VM and
emitted Bash, including row field reads inside command arguments.

Rows are intentionally not public schema/type declarations. Row and row-array
parameters, nested row fields, row-field assignment, row-array element
replacement/deletion, custom comparators, serialization helpers, and public Bash
payload compatibility remain deferred.

## v0.31.0 recursive glob runtime notes

`v0.31.0` extends the existing `glob` and `glob!` helpers with scoped recursive
`**` support. Recursive `**` support accepts exactly one path segment equal to
`**`; the recursive `**` contract is not Bash globstar passthrough. Partial
segments such as `foo**bar`, multiple recursive segments, and
recursive patterns containing `..` segments are rejected by the lowerer when the
pattern is a literal and by VM/Bash helpers when the pattern is produced
dynamically.

In the VM, recursive globbing traverses the process current working directory
without invoking a shell. It treats `**` as zero or more directory segments,
collects matching path strings, sorts them bytewise, removes duplicates, and
returns a string array. Hidden child directories are not traversed by recursive
`**`, hidden path components are matched only when the corresponding pattern
segment begins with `.`, and directory symlinks are not followed; symlink entries
may still be returned when the final path itself matches the non-recursive
suffix. `glob` no-match returns an empty array; `glob!` no-match is a runtime
failure before loop bodies run.

Emitted Bash remains standalone. Its recursive-glob helper does not require
ambient `globstar`, `dotglob`, `nullglob`, or `failglob` settings and keeps helper
names in the reserved `__ds_` namespace. It uses explicit traversal and
`LC_ALL=C sort -u` to match the VM's deterministic ordering and duplicate policy
for the supported POSIX-style path surface, including spaces and leading-dash
paths. Custom glob flags, hidden traversal flags, symlink-following traversal,
brace expansion, extglob, shell variable expansion, `~` expansion, Windows path
semantics, and streaming filesystem iterators remain deferred.
