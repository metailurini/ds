# ds DX issues observed while writing scripts

This document records developer-experience issues observed while writing a pure `ds` approximation of a C function analyzer in `scripts/analyze_c_functions_pure.ds`.

The goal is not to decide syntax yet. These are notes for future language/runtime design.

## Current priority order

The roadmap now prioritizes the remaining open DX issues in this order:

1. `v0.34.0` — make literal `{` / `}` usable in strings and quiet the common
   broken-pipe case from pipelines such as `script | head`.
2. `v0.35.0` — add core string parsing helpers such as `index_of`,
   `last_index_of`, `slice`, `char_at`, and substring/character counts.
3. `v0.37.0` — design the lightweight structured-row / in-memory buffering /
   sort-by-field story for data-processing scripts.
4. `v0.38.0` — polish recursive file walking beyond the current safe
   `glob("**")` contract and reconcile solved DX notes.

The previously planned signal/job-control wave is postponed to a later roadmap
slot so these script-writing DX gaps can be addressed first.

## Literal braces in strings are awkward — addressed in v0.34.0 implementation

Writing a literal `{` or `}` in a string used to conflict with interpolation parsing.
`v0.34.0` implements strict doubled literal braces: `{{` renders `{`, `}}`
renders `}`, and `{expr}` remains interpolation. A lone `}` is rejected with a
clear diagnostic.

Example that was awkward:

```ds
line.contains("{")
```

The workaround used in the script was to shell out to `printf`:

```ds
let lbrace_result = run printf "\\173"
let rbrace_result = run printf "\\175"
let lbrace = lbrace_result.stdout
let rbrace = rbrace_result.stdout
```

This workaround is no longer needed for the supported string/interpolation
surface, and the dedicated v0.34 regression suite covers the strict
doubled-brace contract across VM execution and standalone emitted Bash.

## No indexing directly after method calls — addressed in v0.35.0 implementation

This did not work:

```ds
sig.split("(")[0].trim()
```

The script had to split it into temporary variables:

```ds
let paren_parts = sig.split("(")
let before_paren = paren_parts[0].trim()
```

That is more verbose than expected for common string/array processing.

`v0.35.0` accepts the read-only `string.split(...)[index]` chaining form needed
for common parsing pipelines, so `sig.split("(")[0].trim()` works with VM and
standalone Bash parity. This does not add temporary collection mutation or
nested collection indexing.

## Function parameters need defaults for string method inference — addressed in v0.36.0 implementation

A helper like this could not call string methods on the parameter:

```ds
fn clean_line(line) {
  let t = line.trim()
}
```

The script had to provide a default value so the parameter was known as a string:

```ds
fn clean_line(line = "") {
  let t = line.trim()
}
```

This works, but it makes ordinary helper functions feel surprising.

`v0.36.0` removes that dummy-default requirement for required scalar
parameters whose local usage clearly implies `string`, `int`, or `bool`. A
helper such as `fn clean_line(line) { return line.trim() }` now infers `line` as
`string`; `fn span(line, start, end) { return line.slice(start, end) }` infers
`line` as `string` and `start`/`end` as `int`. Wrong-kind calls are rejected at
the call boundary. Public typed-parameter syntax, implicit coercions, and
collection/row parameters remain deferred rather than solved by this DX pass.

## No native recursive directory walking

The script had to shell out to `find`:

```ds
find '{root}' -type f \( -name '*.c' -o -name '*.h' \)
```

A future standard-library helper for recursive walking/filtering may improve script readability.

## No native sort-by-field helper

To sort analyzer results by LOC descending, the script shells out to `sort`:

```sh
sort -t ':' -k4,4nr
```

This is acceptable for shell-native scripting, but it makes structured in-language data processing harder.

## No lightweight structured records for rows

The analyzer naturally wants to store rows like:

```ds
{ file: file, line: start_line, name: func_name, loc: loc }
```

Instead, the script writes colon-separated text lines to a temporary file and sorts externally.

This is workable, but limits clean data transformations inside `ds`.

## Broken pipe diagnostics are noisy — addressed in v0.34.0 implementation

Testing with `head` caused a broken-pipe style failure:

```sh
./ds run scripts/analyze_c_functions_pure.ds --root . | head
```

Observed diagnostic:

```text
command `echo` failed with exit 141
```

For CLI-style scripts, this was too noisy. `v0.34.0` quiets the common
uncaptured, unredirected closed-stdout case: VM execution uses exact raw direct-command/final-pipeline-stage
`SIGPIPE` status with pipe-like stdout, while emitted Bash uses the documented
standalone status-141/pipe-stdout heuristic. Captured command-result statuses
are preserved. Unrelated failures remain visible. Standalone emitted Bash still
documents the unavoidable explicit-`141`-under-pipe ambiguity, but source-level
redirected commands are not quieted by the inherited script stdout heuristic.
The dedicated v0.34 regression suite covers the common quiet case, captured
statuses, redirected status-141 failures, and cleanup-aware early completion.

## String parsing APIs are limited — addressed in v0.35.0 implementation

The analyzer needed common string scanning operations, including:

- `index_of`
- `last_index_of`
- `slice`
- `char_at`
- count occurrences of a substring/character

Without these, even an approximate parser required awkward workarounds.

`v0.35.0` adds the core byte-oriented helpers: `.len()`, `.index_of(needle)`,
`.last_index_of(needle)`, `.count(needle)`, `.char_at(index)`, and
`.slice(start, end)`. The implementation deliberately keeps byte offsets,
explicit empty-needle behavior, and strict bounds errors rather than adding
Unicode, regex splitting, negative indexes, or clamped slices.

## Regex support does not expose captures

The current `matches` support can answer yes/no questions, but this task wanted extraction:

```ds
# illustrative only
let m = sig.match(/([A-Za-z_][A-Za-z0-9_]*)\s*\()/
let name = m.group(1)
```

Without captures, the script approximated function-name extraction using `.split()`.

## Temporary files were needed for buffering

Because results need to be sorted after collection, the script writes intermediate output to:

```text
.ds-c-functions.out
```

and removes it with `defer`.

This works, but an ergonomic in-memory collection/sort flow would be nicer for data-processing scripts.
