# ds DX issues observed while writing scripts

This document records developer-experience issues observed while writing a pure `ds` approximation of a C function analyzer in `scripts/analyze_c_functions_pure.ds`.

The goal is not to decide syntax yet. These are notes for future language/runtime design.

## Current priority order

The `v0.34.0` through `v0.38.0` DX wave has addressed the script-writing issues
that were blocking a pure-`ds` analyzer flow: literal braces, common
broken-pipe quieting/noise reduction, direct indexing for read-only string
helper results, core byte-oriented string parsing helpers, function parameter kind inference
for scalar helpers, lightweight row arrays with `sort_by`, and recursive file walking
by root and extension.

The remaining open analyzer DX priority is regex capture ergonomics. Deeper
Unicode/string semantics, richer row schemas, metadata-oriented filesystem APIs,
and streaming iterators stay on the broader post-wave roadmap rather than being
implicitly pulled into the current surface.

## Resolved DX-wave items

The sections below describe the original pain points and the milestone that
closed each one.

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

## No native recursive directory walking — addressed in v0.38.0 implementation

The script had to shell out to `find`:

```ds
find '{root}' -type f \( -name '*.c' -o -name '*.h' \)
```

`v0.38.0` adds `dir.walk(root)`, `dir.walk!(root)`,
`dir.walk_ext(root, extensions)`, and `dir.walk_ext!(root, extensions)` for the
portable common case. The helpers return deterministic string arrays of regular
files, skip hidden descendants and symlinks, validate exact extensions such as
`.c`, and work in both VM execution and standalone emitted Bash.

## No native sort-by-field helper — addressed in v0.37.0 implementation

To sort analyzer results by LOC descending, the script shells out to `sort`:

```sh
sort -t ':' -k4,4nr
```

`v0.37.0` adds `rows.sort_by(field[, "asc"|"desc"])` for same-schema row
arrays. External `sort` remains useful for shell-native text pipelines, but the
common in-memory analyzer/reporting shape no longer needs to leave `ds`.

## No lightweight structured records for rows — addressed in v0.37.0 implementation

The analyzer naturally wants to store rows like:

```ds
{ file: file, line: start_line, name: func_name, loc: loc }
```

Instead, the script writes colon-separated text lines to a temporary file and sorts externally.

`v0.37.0` treats flat scalar map literals as lightweight rows when they are
buffered in same-schema row arrays, iterated, copied, returned, and sorted. Public
row declarations, row parameters, nested fields, mutation, and serialization APIs
remain deferred.

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

## Temporary files were needed for buffering — addressed in v0.37.0 implementation

Because results need to be sorted after collection, the script writes intermediate output to:

```text
.ds-c-functions.out
```

and removes it with `defer`.

`v0.37.0` row arrays plus `sort_by` cover this in-memory buffering/sorting use
case. Temporary files remain a normal shell-native option for very large streams,
but they are no longer required for the scoped analyzer-style row buffer.

## Open DX issues

### Regex capture ergonomics

The current `matches` support can answer yes/no questions, and `v0.32.0` added
the practical `regex.match(text, pattern[, flags])` map API for numbered captures
up to the current flat-map boundary. This task still wanted a smoother extraction
shape such as method-style match objects or named captures:

```ds
# illustrative only
let m = sig.match(/([A-Za-z_][A-Za-z0-9_]*)\s*\()/
let name = m.group(1)
```

Capture ergonomics beyond the current flat map remain an open design item.
