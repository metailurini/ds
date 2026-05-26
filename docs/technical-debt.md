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

## H3 — `vm_process.c` is long, and some functions are unclear rather than merely large

**Status:** High priority
**Kind:** readability / runtime boundary debt
**File:** `src/vm_process.c`

**Problem:**

The file owns too many runtime concerns at once: command-word materialization,
interpolation rendering, arithmetic interpolation parsing, redirection, direct
process execution, capture, pipelines, and signal interruption classification.
The biggest readability issue is not just file length; it is dense helper bodies
and mixed concerns inside the same scan path.

The arithmetic interpolation helpers are especially hard to review because the
parser/evaluator functions are compressed and visually dense.

**Preferred fix:**

First make functions readable without moving behavior:

- Reformat dense `vip_*` helpers into normal multi-line C.
- Add section comments for interpolation, argv rendering, redirection, direct
  process execution, capture, and pipeline execution.
- Rename helpers whose names only make sense to the original author.

Only after that, consider one cohesive extraction such as:

- `vm_argv.c` for command-word/redirect target materialization; or
- `vm_process_exec.c` for child spawn/wait/redirection; or
- `vm_pipeline.c` for pipeline mechanics.

**Do not:**

- Split the file by arbitrary line count.
- Move signal behavior while changing process behavior.
- Change VM/Bash parity during readability cleanup.

## H4 — `bash_stmt.c` remains a broad emitter monolith

**Status:** High priority
**Kind:** emitter cohesion debt
**File:** `src/bash_stmt.c`

**Problem:**

`bash_stmt.c` is long and still mixes statement emission, function emission,
structured return mechanics, array/map mutation emission, command/capture
statements, handler emission, and control-flow rendering. Long is acceptable,
but the helper names and section boundaries need to make the ownership obvious.

**Preferred fix:**

Do a no-behavior readability pass before any more extraction:

- Add clear section boundaries.
- Rename helpers that hide whether they emit values, type sidecars, function
  call materialization, or statements.
- Keep broad statement emission here until a cohesive cluster has enough weight
  to move.

Possible future cohesive moves:

- Bash handler emission cluster.
- Bash function-return emission cluster.
- Bash collection mutation emission cluster.

**Do not:**

- Move one or two helpers into new files.
- Split statement emission from its nearby control-flow context unless the new
  owner is obviously cohesive.

## H5 — Shared command metadata is becoming a mixed bag

**Status:** Medium priority
**Kind:** module scope debt
**Files:** `src/ds_command.h`, `src/ds_command.c`

**Problem:**

`ds_command.c` now owns command cloning/freeing, command-word shape classification,
direct-call interpolation detection, command-result fields, and pipeline
shape/status helpers. Each addition reduced a duplication point, but together
they make `ds_command.c` a generic shared-policy bucket.

**Preferred fix:**

Keep the shared descriptors, but make the module boundary explicit:

- Either rename it toward `command_model` and accept that it owns the full shared
  command model; or
- split only if there are cohesive clusters, such as command-result field catalog
  versus command-word shape helpers.

**Do not:**

- Add more unrelated VM/Bash parity helpers to `ds_command.c` without first asking
  whether they belong to command representation.

## H6 — Bash structured-value ABI is clearer but still spread across emitters

**Status:** Medium priority
**Kind:** backend ABI debt
**Files:**

- `src/bash_structured.c`
- `src/bash_stmt.c`
- `src/bash_expr.c`
- `src/bash_helpers.c`
- `src/bash_deps.c`

**Problem:**

`bash_structured.c` now owns more names/declarations/storage helpers, but the ABI
is still implemented through statement emission, expression emission, dependency
scanning, and generated helper bodies. This is expected for Bash output, but the
risk remains high whenever arrays, maps, command results, or structured function
returns change.

**Preferred fix:**

Continue moving only cohesive ABI helpers into `bash_structured.c` when there is
clear duplication. Do not move broad statement/expression rendering just to make
one file look canonical.

## H7 — Type/kind-name helpers remain duplicated

**Status:** Medium priority
**Kind:** duplicated helper debt
**Files:**

- `src/bash_expr.c`
- `src/bash_stmt.c`
- `src/bash_emit.c`
- `src/vm_args.c`
- `src/lower_stdlib.c`

**Problem:**

There are still local helpers for expression/value/script type names. Some are
backend-specific, but several exist only to print the same kind labels.

**Preferred fix:**

Create or reuse small shared name helpers only when the label is truly a shared
contract. Otherwise rename local helpers to make their backend-specific purpose
clear, for example `bash_expr_value_type_name` versus `script_decl_type_name`.

**Do not:**

- Create a global string table for labels that are intentionally backend-specific.

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
