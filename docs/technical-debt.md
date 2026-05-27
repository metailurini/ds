# Technical debt

This file is the source-code maintenance backlog for `ds`. It is intentionally
about implementation shape, not feature scope. Use it with:

- `docs/source-map.md` for current file ownership.
- `docs/concept-map.md` for cross-cutting concept pressure.
- `docs/parity-contracts.md` for VM/Bash behavior requirements.
- `docs/diagnostics.md` for diagnostic ownership.

## Current audit summary

Recent maintenance passes reduced several real duplication points, but they also
introduced a new architectural smell: **concept micro-files**. A file with one or
two tiny functions is not automatically clearer. Long files are acceptable when
functions are well named and cohesive; tiny files are harmful when they make the
reader jump between many names to understand one behavior.

The next cleanup direction should be **cohesion and naming consistency**, not
more splitting. Do not create a new `.c` file merely because a concept appears in
`docs/concept-map.md`.

## Refactor rules going forward

1. Prefer clear functions over small files.
2. A `.c` file should own a cohesive implementation cluster, not one lookup or
   one wrapper, unless it is a deliberate public façade/entrypoint.
3. If a header declares implementation-specific functions, the `.h`/`.c` stem
   should match or the mismatch must be an explicit façade pattern.
4. Shared descriptor tables are useful, but they should live in a module whose
   name says what the table is for.
5. Backend parity helpers must not become a generic dumping ground for unrelated
   policies.
6. Before extracting, identify at least one stable cluster: data model, parser,
   renderer, runtime executor, or policy gate. If the cluster is one function,
   leave it in the caller and make the function clearer.
7. Prefer net-neutral consolidation over continued file growth by new leaves.

## H1 — Concept micro-files from recent maintenance passes

**Status:** Addressed for one-function recent files; keep watching for new leaves
**Kind:** cohesion / navigation debt
**Files:**

- `src/ds_signal.c`
- `src/ds_interpolation.c`
- `src/lower_collection.c`

**Problem:**

Several files were created to give concepts a “home”, but some homes became too
small to justify the navigation cost. The one-function `src/vm_command_result.c`
has been folded back into `src/vm.c`; the one-function `src/lower_tests.c` has
been folded into `src/lower.c`; and the one-function `src/vm_test.c` has been
folded into `src/vm.c`. The remaining small files listed above are not
one-wrapper leaves: each owns a compact shared table or validation cluster. They
are acceptable only while that cluster stays cohesive and the source map remains
honest about the narrow ownership.

**Why this matters:**

Future contributors may copy the pattern and create one source file per pressure
point. That would make the codebase harder to read even if each extraction is
technically correct.

**Preferred fix:**

Keep the consolidation rule strict:

- Keep VM command-result/map field materialization in `src/vm.c` unless it grows
  into a larger VM value-access cluster.
- Keep `src/ds_interpolation.c` only while docs clearly say it owns
  **format-spec metadata**, not the entire interpolation segment model.
- Keep `src/lower_collection.c` only while it remains the collection portability
  policy cluster; otherwise fold tiny validators back into the lowering files
  with clearer section comments.
- Do not recreate test-only one-function files; keep small test setup passes near
  their owning orchestration/runtime entrypoints unless they grow into real
  clusters.
- Treat `parser.c` and `lower.c` as façade/entrypoint exceptions only if the
  source map says so.

**Do not:**

- Split more files just to make concept-map rows look owned.
- Create `*_policy.c` files for one or two predicates.
- Use docs as proof that a tiny file is a good module.

## H2 — Header/source naming does not have a consistent rule

**Status:** Addressed for shared `ds_*` modules; enforce going forward
**Kind:** naming / module boundary debt
**Files:**

- `src/ds_command.h` / `src/ds_command.c`
- `src/ds_interpolation.h` / `src/ds_interpolation.c`
- `src/ds_signal.h` / `src/ds_signal.c`
- `src/ds_stdlib.h` / `src/ds_stdlib.c`
- façade headers such as `src/backend.h`, `src/frontend.h`

**Problem:**

Shared `ds_*` headers now have matching `ds_*.c` implementations. The remaining
risk is naming breadth: `ds_interpolation.h`/`.c` and `ds_signal.h`/`.c` are
matching pairs, but their names are broader than the helper clusters they own.
The source map must keep that narrow ownership explicit.

**Preferred fix:**

Keep this naming rule enforced:

- Public/shared headers may use `ds_<name>.h`.
- The matching implementation should normally be `ds_<name>.c`, or the source
  map must identify the header as a façade with no matching `.c`.
- If the implementation is narrower than the header name, rename the pair to the
  narrower concept instead of using a broad name.

**Current outcome:**

- `ds_command.h` / `ds_command.c`, `ds_interpolation.h` /
  `ds_interpolation.c`, `ds_signal.h` / `ds_signal.c`, and `ds_stdlib.h` /
  `ds_stdlib.c` now match.
- `backend.h` and `frontend.h` remain façade headers and do not need matching
  `.c` files.
- Do not introduce new non-matching shared header/source pairs unless the source
  map explicitly marks the header as a façade.

## H3 — `vm_process.c` is long, and must stay readable by concern

**Status:** Addressed for the current maintenance loop; keep on watch
**Kind:** readability / runtime boundary debt
**File:** `src/vm_process.c`

**Resolved in this pass:**

- Replaced compressed `vip_*` arithmetic interpolation helpers with named,
  multi-line parser functions such as `arithmetic_parse_expr()`,
  `arithmetic_parse_power()`, and `checked_mul_i64()`.
- Added section comments inside `src/vm_process.c` for trace output,
  interpolation rendering, command-word/redirect materialization, process specs
  and control commands, foreground process groups/signals, direct process
  execution, and pipeline execution.
- Reused small identifier helpers in interpolation scanning instead of repeating
  long ASCII identifier predicates inline.

**Remaining pressure:**

The file owns too many runtime concerns at once: command-word materialization,
interpolation rendering, arithmetic interpolation parsing, redirection, direct
process execution, capture, pipelines, and signal interruption classification.
That is acceptable for now because the functions are clearer, but future changes
should still avoid making any one section absorb unrelated behavior.

**Next fix only if the section itself grows unclear:**

Consider one cohesive extraction such as:

- `vm_argv.c` for command-word/redirect target materialization; or
- `vm_process_exec.c` for child spawn/wait/redirection; or
- `vm_pipeline.c` for pipeline mechanics.

**Do not:**

- Split the file by arbitrary line count.
- Move signal behavior while changing process behavior.
- Change VM/Bash parity during readability cleanup.

## H4 — `bash_stmt.c` remains a broad emitter monolith

**Status:** Addressed enough for now; keep on watch
**Kind:** emitter cohesion debt
**Files:** `src/bash_stmt.c`, `src/bash_function.c`, `src/bash_return.c`, `src/bash_command.c`

**What was wrong:**

`bash_stmt.c` mixed too many Bash-emitter concerns: statement dispatch,
function definitions, user-function call materialization, structured return
payloads, captured pipeline assignment mechanics, handlers, assignments,
mutation, and control-flow rendering. The problem was not the line count by
itself; the problem was that unrelated mechanics lived beside the statement
switch, making future edits easy to misplace.

**Current shape after the H4 cleanup:**

- `bash_stmt.c` remains the accepted-HIR statement dispatcher and owns normal
  statement/control-flow/handler/assignment/mutation emission.
- `bash_function.c` owns Bash function wrapper/parameter/default binding and
  user-function call materialization, including nested user-call argument capture
  and value-call plumbing.
- `bash_return.c` owns return statement payload emission, including structured
  array/map/command-result returns.
- `bash_command.c` owns command rendering and captured pipeline assignment
  mechanics.

This is intentionally not a one-file-per-statement split. Each extracted file
has a cohesive Bash-emitter concern with enough weight to justify its existence.

**Remaining watch points:**

- `bash_stmt.c` is still long because statement dispatch and statement-local
  mechanics remain there.
- Assignment/mutation helpers may deserve clearer names if collection work grows.
- Handler emission is still inside `bash_stmt.c`; extract only if signal/trap
  behavior grows, not just because it is a switch case.

**Do not:**

- Move one or two statement cases into standalone files.
- Split by line count.
- Let helper files become generic dumping grounds.

## H5 — Shared command metadata was becoming a mixed bag

**Status:** Addressed for the current maintenance loop; keep on watch
**Kind:** module scope debt
**Files:** `src/ds_command.h`, `src/ds_command.c`, `src/ds_command_word.*`,
`src/ds_command_result.*`, `src/ds_command_pipeline.*`

**Problem:**

`ds_command.c` had grown into a mixed shared-policy bucket. It owned command
cloning/freeing, command-word shape classification, direct-call interpolation
detection, command-result fields, and pipeline shape/status helpers. Each
addition reduced a duplication point, but together they hid distinct ownership
boundaries behind one broad module name.

**Current shape after the H5 cleanup:**

- `ds_command.c` / `ds_command.h` own only command payload data lifecycle:
  words, stages, redirects, command init/clone/free.
- `ds_command_word.c` / `.h` own raw command-word shape classification and
  direct-call interpolation detection. Lowering still owns legality.
- `ds_command_result.c` / `.h` own the command-result field catalog, field
  kinds, and Bash storage aliases.
- `ds_command_pipeline.c` / `.h` own pipeline shape helpers and the shared
  VM pipefail-style status calculation.

This is a deliberate small cluster split, not a one-file-per-concept pattern:
each new file has a named command-model subdomain consumed by multiple phases.

**Keep on watch:**

- Do not put new command semantic rules into `ds_command.c`; they belong in
  lowerer code unless they are backend-neutral descriptors.
- Do not add unrelated VM/Bash parity helpers to the command modules merely
  because the feature appears inside command syntax.
- If any helper cluster shrinks to a one-wrapper file, fold it back into its
  caller and keep the function name clear.

**Do not:**

- Recreate a generic `command_model` bucket by moving unrelated helpers into one
  broad file.
- Split command modules by arbitrary line count rather than ownership cluster.

## H6 — Bash structured-value ABI is clearer but still spread across emitters

**Status:** Addressed for the current shape; keep on watch
**Kind:** backend ABI debt
**Files:**

- `src/bash_structured.c`
- `src/bash_stmt.c`
- `src/bash_expr.c`
- `src/bash_helpers.c`
- `src/bash_deps.c`

**Problem:**

Earlier passes gave `bash_structured.c` the obvious names/declarations/storage
helpers, but type-sidecar writes and structured return payloads were still
implemented through statement, expression, return, and function emitters. That
made the ABI look owned while important metadata rules were still duplicated at
call sites.

This pass moved the cohesive ABI pieces into `bash_structured.c`:

- static Bash type-name mapping for lowered expressions;
- dynamic-or-static type value emission for function arguments and sidecars;
- `__ds_type_*`, `__ds_elem_type_*`, and `__ds_value_type_*` assignment helpers;
- array/map structured return payload construction;
- command-result storage/copy helpers.

`bash_stmt.c`, `bash_expr.c`, `bash_function.c`, and `bash_return.c` now consume
those helpers rather than reimplementing type metadata or structured payload
layout.

**Preferred fix:**

Keep `bash_structured.c` as the Bash ABI owner. Do not move broad
statement/expression rendering there, and do not move generated shell helper
bodies out of `bash_helpers.c` unless the helper text itself becomes easier to
maintain as named fragments.

## H7 — Type/kind-name helpers remain duplicated

**Status:** Addressed for shared labels; keep backend-specific names local
**Kind:** duplicated helper debt
**Files:**

- `src/ds_ast.h`
- `src/ast.c`
- `src/ds_hir.h`
- `src/hir.c`
- Bash/VM/formatter consumers

**Resolution:**

The truly shared labels now have one owner:

- `ds_script_type_name()` owns source-level script type names (`string`, `int`,
  `bool`) used by AST/HIR dumps, formatting, VM help, and Bash help/type
  sidecars.
- `ds_lower_value_kind_name()` owns lowered value-kind metadata labels
  (`unknown`, `bool`, `int`, `string`, `array`, `map`, `command_result`) used by
  Bash structured metadata and HIR consumers.

Backend-specific helpers remain local when they describe backend behavior rather
than a shared contract, for example Bash static-expression type inference for
sidecar emission.

**Keep on watch:**

- Do not add another local `script_type_name()` or lowered-value-kind switch.
- Do not create a global string table for labels that are intentionally
  backend-specific.

**Do not:**

- Fold backend-specific static-expression inference into generic HIR helpers.

## H8 — `checker.c` still crosses façade boundaries and duplicates diagnostic rendering

**Status:** Medium priority
**Kind:** layering / diagnostics support debt
**File:** `src/checker.c`

**Problem:**

`checker.c` includes `backend.h` and carries local source-line diagnostic display
logic. The checker should be a presentation/warning backend, but it should not
need broad backend façade coupling or copied diagnostic rendering mechanics.

**Preferred fix:**

- Give checker a narrow declaration home if needed.
- Reuse diagnostic/source rendering support instead of keeping local copies.
- Keep checker warnings separate from hard semantic diagnostics.

## H9 — Test harnesses hardcode implementation-file lists

**Status:** Medium priority
**Kind:** test maintenance debt
**Files:** `tests/v*/run.sh`

**Problem:**

Recent file additions repeatedly broke focused unit suites because test scripts
compile explicit source lists. This makes every new helper file require manual
updates across unrelated test versions.

**Preferred fix:**

Introduce a shared test compile manifest or helper script for common source
sets. Individual suites can still add small extras, but the default lowerer,
parser, VM, and Bash source groups should not be copy-pasted in many places.

## H10 — Concept map pressure has been reduced by code, but the tree shape now needs correction

**Status:** High priority
**Kind:** process debt

**Problem:**

The recent pattern was: identify high-pressure concept, add a shared helper/home,
update docs/tests. That was useful for parity and ownership, but it also trained
the codebase toward one-file-per-concept. The next maintenance loop should pause
feature and concept extraction work and make the source tree coherent again.

**Preferred next loop:**

1. Establish header/source naming rules.
2. Decide which micro-files are real modules and which should be merged/renamed.
3. Reformat and clarify long functions in `vm_process.c`.
4. Add section boundaries and helper renames in `bash_stmt.c`.
5. Centralize test compile source lists.
6. Only then revisit remaining high-pressure concepts.

## Out of scope for this debt file

- Adding new language features.
- Changing VM/Bash semantics.
- Making long files short for its own sake.
- Marking a concept solved because it has a new file.
- Rewriting parser/lowerer/VM/Bash architecture in one pass.
