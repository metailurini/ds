# Concept home map

This map names the canonical home for cross-cutting `ds` concepts. It should
match the current source tree, not future design wishes. Use it with:

- `docs/source-map.md` for file-level ownership.
- `docs/diagnostics.md` for phase-owned diagnostics.
- `docs/parity-contracts.md` for VM/Bash acceptance rules.
- `docs/architecture.md` and `docs/runtime.md` for pipeline/runtime details.
- `docs/maintenance/` for compact boundary records.

## Status labels

- **Clear**: stable ownership, supported behavior, low architectural risk.
- **Watch**: supported behavior with mostly clear ownership, but backend/parity or
  maintenance drift remains plausible.
- **High-pressure Watch**: ownership exists, but the concept is central,
  cross-cutting, or fragile enough that related feature work must start with a
  focused design/maintenance pass.
- **Clear while rejected**: unsupported/deferred behavior has an explicit owner,
  documented rejection, and diagnostics/tests protecting that rejection.
- **Hell candidate**: no stable home, unclear ownership, duplicated semantics
  across layers, or unsupported behavior leaks inconsistently into parser/VM/Bash.

## Concept truth table

| Concept | Status | Canonical representation | Owners | Supported behavior | Rejected/deferred behavior | Known traps / next maintenance trigger |
| --- | --- | --- | --- | --- | --- | --- |
| Source spans and diagnostics substrate | **Clear** | `DsSource`, `DsSpan`, `DsLoc`, `DsDiag` | Every phase attaches spans; `diag.c` renders | Stable location reporting | None concept-specific | Preserve spans when moving validation. |
| Diagnostics ownership | **Watch** | Phase ownership contract in `docs/diagnostics.md` | Parser syntax; lowerer semantics/parity; checker warnings; VM runtime/OS; Bash artifact/internal invariants | Phase-owned user diagnostics; backend wording limited to runtime/data/artifact/internal invariants | Backend semantic ownership | Any new backend `unsupported`/`cannot emit` message needs classification before it lands. |
| VM/Bash parity contract | **High-pressure Watch** | HIR plus shared metadata catalogs | Lowerer gates; VM executes; Bash renders | Accepted features require VM and Bash behavior unless scoped otherwise | Backend-only acceptance without contract | Start every feature with HIR/shared-metadata and parity-test plan. |
| General interpolation segment model | **High-pressure Watch** | Parser string text; HIR `DS_LOWER_EXPR_INTERP` parts; shared format-spec contract in `src/interpolation.c`; command-word text validated by `lower_command.c` | Parser preserves text; `lower_interpolation.c` handles normal strings; `lower_command.c` handles command words; VM/Bash render accepted segments using shared format metadata | `{name}`, `{name.field}`, supported expression/function-call forms, format specs where documented | Unsupported segment shapes and unsupported format specs | String and command-word paths are related but not identical; keep segment acceptance in lowering and format syntax/kind metadata shared. |
| String interpolation | **Watch** | HIR interpolation expression with scalar parts | Parser string syntax; `lower_interpolation.c` expression/scalar validation; VM/Bash render | Scalar variables, fields, supported calls/expressions, formatting helpers | Non-scalar payloads and unsupported shapes | Keep command-specific rules in `lower_command.c`; backend shape errors should stay internal invariants. |
| Command words | **High-pressure Watch** | AST/HIR `DsCommand` payloads | `parse_command.c` syntax; `lower_command.c` word legality; VM/Bash command backends | Plain commands, quoted/unquoted words, redirection, pipelines, capture metadata | Unsupported interpolation/word forms | Any new word behavior must update parser, lowerer, HIR command payload, VM argv, Bash quoting, and parity tests together. |
| Command-word interpolation | **High-pressure Watch** | `DsCommandWord` text plus lowerer validation/materialization | `lower_command.c` owns validation, format checks, command-result fields, scalar value-call materialization | Existing variable/field/arithmetic/format interpolation and supported scalar quoted value-call materialization | Structured return interpolation, unsupported command-result/function forms | Do not move acceptance into `vm_process.c`, `bash_command.c`, or `bash_quote.c`. |
| Direct function-call interpolation in command words | **Watch** | Private lowerer string temp plus rewritten command word | Lowerer uses return-kind metadata; VM/Bash consume rewritten payload | Currently supported scalar quoted-word value calls | Structured returns and unsupported positions | Expand only after function return and command-word contracts are updated. |
| Captured command results | **Watch** | HIR run/call result with shared command-result field catalog | Lowerer validates fields; VM captures process output/status; `vm_command_result.c` materializes VM fields; Bash helpers encode fields | `stdout`, `stderr`, `status`/`code`, `ok`, `failed` | Unknown fields and unsupported temporary structured access | Field catalog must remain shared; VM/Bash fallback diagnostics are internal invariants. |
| Command-result functions | **Watch** | `DS_LOWER_VALUE_COMMAND_RESULT` call return kind | Lowerer/stdlib/function metadata; VM/Bash call implementations | Supported helpers/user calls returning command results | Direct temporary field/index access where rejected | New producers must update metadata, lowerer value kinds, Bash ABI, VM/Bash parity tests. |
| Function return kinds | **Watch** | Function metadata plus HIR `return_kind`/value kind | `lower_functions.c` and return lowering; VM/Bash call/return backends | Scalar and currently portable structured returns | Nonportable structured returns and invalid mixed returns | New call positions or structured returns must start with return-kind contract review. |
| Bash structured value ABI | **High-pressure Watch** | Bash raw variables plus array/map/command-result type sidecars/helpers; ABI names/declarations in `src/bash_structured.c` | Lowerer defines accepted value kinds; Bash emitter/helpers encode them | Accepted arrays, maps, command results, structured returns after binding | Shapes without Bash encoding or direct temporary access | Treat Bash ABI as implementation detail, never canonical language semantics; feature changes still need a focused ABI/parity pass. |
| Process execution runtime contract | **Watch** | HIR command payloads lowered to VM subprocess calls or Bash command emission | Parser/lowerer own command shape; VM/Bash own accepted process execution, redirection, capture, and OS failures | Plain commands, redirection, captured process results, command-result status fields | Backend-only source acceptance or process semantics that bypass HIR | Keep OS/process failures backend-owned while accepted observable results stay covered by VM/Bash parity tests. |
| Pipeline behavior | **High-pressure Watch** | HIR command payload with stages/redirection/capture metadata | Parser syntax; lowerer restrictions; VM process pipeline; Bash pipeline emission | Plain and captured pipelines with documented stdout/stderr/status behavior | Unsupported stage/redirect/capture forms | Process status and signal cleanup are fragile; future changes need focused runtime/parity pass. |
| Signal/job-control runtime contract | **High-pressure Watch** | HIR handlers plus VM/Bash cleanup, signal, and foreground-process runtime state | Lowerer validates handlers/signals; VM owns signal/process-group runtime mechanics; Bash owns trap/helper emission | Deterministic supported `INT`/`TERM` cleanup, defer/trap ordering, foreground command/pipeline interruption classification | Rich job-control APIs and unsupported handler contexts | OS-sensitive; keep signal policy in lowering and backend mechanics behind parity tests. |
| Trap/defer/signal declarations | **Watch** | AST handler nodes lowered to HIR handler declarations | Parser syntax; lowerer signal/context legality; VM/Bash runtime execution | Supported handler declarations and cleanup behavior | Unsupported signals, handler captures, direct returns where rejected | Keep signal policy in lowering, not parser/backends. |
| Handler context APIs | **Clear while rejected** | No accepted HIR context value beyond current handler contract | Lowerer rejects unsupported context/capture/use | Current cleanup handlers without rich context API | Rich signal/job-control/context variables | Add only with explicit HIR context model and VM/Bash parity plan. |
| Direct `env.NAME` access/assignment | **Watch** | AST field/assignment lowered to `env.get`/`env.set` behavior; no general env object | Lowerer validates env names/scalar assignment; VM stdlib and Bash/native rendering execute | Direct reads and scalar assignments | Compound assignment, non-scalar assignment, env object semantics | Future richer env access needs explicit HIR/env contract first. |
| Environment helper calls | **Watch** | Stdlib metadata and HIR calls/statements | Lowerer arity/name validation; VM stdlib; Bash helpers/native ops | `env.get`, `env.set`, `env.unset` behavior as documented | Invalid names/arity/value kinds | Keep helper metadata canonical; helpers do not define language semantics. |
| Regex policy | **Watch** | HIR `matches` expression with conservative regex literal metadata | Lexer/parser literal syntax; lowerer supported surface/parity gates; VM/Bash regex execution | Conservative regex literals as right operand of `matches` | Captures, replacement, split, runtime regex strings | Any expansion needs regex value/capture model and VM/Bash parity tests. |
| Regex captures/replacement/runtime regex strings | **Clear while rejected** | No accepted HIR/value model | Lowerer rejects parseable unsupported forms; lexer/parser reject malformed literals | None until designed | Captures, replacement, runtime regex values/strings | Do not leak partial support through VM/Bash helpers. |
| Glob expansion | **Watch** | Stdlib helper metadata and HIR iterable call | Lowerer for static pattern restrictions/iterable eligibility; VM/Bash helpers execute | `glob`, `glob!`, sorted observable behavior | Recursive `**` where known; no-match behavior per helper | Dynamic pattern restrictions are runtime data failures. |
| Recursive `**` glob patterns | **Clear while rejected** | No accepted recursive glob contract | Lowerer rejects statically known patterns; VM/Bash helpers reject dynamic patterns at runtime | None | Recursive `**` patterns | Add only after cross-backend ordering and shell-glob semantics are specified. |
| Mutable collections | **High-pressure Watch** | AST literals/indexing lowered to HIR arrays/maps/index expressions; no mutation HIR node | Parser syntax and unsupported assignment rejection; lowerer parity gates; VM/Bash accepted containers | List/map literals, named read-only indexing, array loops, `array.push`, flat structured returns | Map iteration, index assignment, nested mutation, unsupported temporaries/computed indexes | Collection work must start from M3.6; Bash encoding and value/reference semantics are pressure points. |
| Collection portability policy | **High-pressure Watch** | HIR value kinds plus Bash sidecar metadata and lowerer parity gates | Lowerer defines portable shapes; VM/Bash execute/render accepted shapes | Flat portable collection values where documented | Nested collections, temporary structured access, nonportable loop/index forms | Do not let VM-capable collection behavior bypass Bash parity gates. |
| Map iteration | **Clear while rejected** | No accepted HIR iteration-order contract | Lowerer rejects | None | Map iteration/order semantics | Requires explicit order policy and VM/Bash parity tests. |
| Index assignment and nested mutation | **Clear while rejected** | No AST/HIR mutation representation | Parser rejects unsupported collection assignment syntax | None beyond current `array.push` | `xs[0] = v`, `map.key = v`, nested mutation | Add only with HIR assignment-target/mutation semantics. |
| List/map literal representation | **Watch** | HIR array/map expressions and value kinds | Parser literal shape/empty-map syntax rejection; lowerer key/value portability | Non-empty maps with valid keys, lists with portable elements | Empty map literal, duplicate/empty keys, nested unsupported values | Empty map remains parser-owned while `{}` has no supported meaning. |
| Read-only collection indexing | **Watch** | HIR index expression with collection/index metadata | Lowerer target/key/index legality; VM/Bash runtime access | Named list/map indexing with literal or named-variable indexes | Temporary collection access and unsupported computed indexes | Runtime missing-key/out-of-range remains VM/Bash runtime data failure. |
| Scalar variables and assignment | **Clear** | HIR let/assign statements and lowerer symbol table | Parser statement syntax; lowerer symbols/value kinds; VM/Bash assignment backends | Scalar declarations/assignments | Unknown/invalid assignments | Keep collection/env special cases out of generic scalar ownership. |
| Conditionals and boolean logic | **Clear** | HIR expressions/statements | Lowerer expression validity; VM/Bash condition evaluation | Supported comparisons, boolean logic, conditions | Unsupported operators/operand kinds | Backend fallback diagnostics should remain internal invariants. |
| Loops over iterables | **Watch** | HIR for-loop with iterable expression and element kind | Lowerer iterable eligibility; VM/Bash loop execution | Array/range/supported iterable loops | Map iteration and nonportable temporary array iterables | New iterable kinds need VM/Bash loop parity tests. |
| Imports/program composition | **Watch** | CLI-loaded composed program then AST/HIR | CLI source/import loading; parser import syntax; lowerer/backends consume composed HIR | Current import composition | Import I/O/load failures | Keep composition diagnostics in CLI/frontend orchestration. |
| Stdlib helper metadata | **Watch** | `ds_stdlib.h` metadata plus `stdlib.c` table | Lowerer consumes metadata; VM/Bash implement helpers | Shared arity/kind/return metadata for helpers | Helper behavior not represented in metadata where needed | New helpers must update metadata before backend implementation. |
| Stdlib dynamic runtime restrictions | **Watch** | Runtime/helper checks for data-dependent arguments | Lowerer handles static restrictions; VM/Bash helpers handle dynamic data failures | Runtime failures for dynamic empty separators, required glob no-match, dynamic recursive glob, etc. | Treating data-dependent failures as static semantics | Keep wording/runtime classification clear; add tests when dynamic cases change. |
| Runtime values | **Watch** | `DsValue` containers and VM runtime state; Bash encoded equivalents | Lowerer controls value kinds reaching runtime; VM runtime executes; Bash helpers encode | Scalars, arrays/maps, command-result objects where accepted | Runtime values defining syntax/semantics | Runtime may fail on data/OS errors, not accept new language forms. |
| Bash helper selection | **Watch** | Helper dependency flags derived from accepted HIR | Bash deps/emitter select helpers; lowerer owns legality | Emit only required helpers for accepted HIR | Helper selection as semantic validation | Helper presence/absence tests catch accidental coupling. |
| Formatter behavior | **Watch** | AST-preserving formatted output | Parser builds AST; formatter owns presentation | Supported syntax formatting | Semantic acceptance/rejection | Formatter should not need unsupported-only semantic nodes. |

## Current Hell candidates

None in the current maintenance baseline. The high-pressure Watch entries above
are the places most likely to become Hell candidates if a future feature bypasses
the documented owner.

## Maintenance triggers

- Promote a **Watch** or **High-pressure Watch** concept to **Hell candidate** if
  semantic checks appear in both lowerer and a backend, or if VM/Bash accept
  different source behavior.
- Keep unsupported features **Clear while rejected** only while diagnostics are
  documented and tested before backend execution, except data-dependent runtime
  restrictions that genuinely cannot be known statically.
- Do not mark a concept **Clear** until adding nearby behavior no longer requires
  touching parser, lowerer, VM, Bash, docs, and tests together.
- When adding feature scope, update this file only after source inspection proves
  the owner/representation is real.
