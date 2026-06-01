# ds DX issues observed while writing scripts

This document records developer-experience issues observed while writing a pure `ds` approximation of a C function analyzer in `scripts/analyze_c_functions_pure.ds`.

The goal is not to decide syntax yet. These are notes for future language/runtime design.

## Current priority order

The roadmap now prioritizes the open DX issues in this order:

1. `v0.34.0` — make literal `{` / `}` usable in strings and quiet the common
   broken-pipe case from pipelines such as `script | head`.
2. `v0.35.0` — add core string parsing helpers such as `index_of`,
   `last_index_of`, `slice`, `char_at`, and substring/character counts.
3. `v0.36.0` — improve function parameter kind inference so helper functions do
   not need dummy defaults just to call string methods.
4. `v0.37.0` — design the lightweight structured-row / in-memory buffering /
   sort-by-field story for data-processing scripts.
5. `v0.38.0` — polish recursive file walking beyond the current safe
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

This workaround should no longer be needed once the v0.34 dedicated regression
suite is added and the milestone is fully closed.

## No indexing directly after method calls

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

## Function parameters need defaults for string method inference

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
uncaptured, unredirected closed-stdout status-141 case for VM execution and
emitted Bash, while preserving captured command-result statuses and unrelated
command failures. The dedicated regression suite is still pending in the next
test pass.

## String parsing APIs are limited

The analyzer needed common string scanning operations, including:

- `index_of`
- `last_index_of`
- `slice`
- `char_at`
- count occurrences of a substring/character

Without these, even an approximate parser required awkward workarounds.

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
