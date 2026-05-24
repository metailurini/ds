# Concept home map

This document identifies important `ds` language/runtime concepts and assigns each
one a primary home. It is a refactoring aid: concepts are dangerous when their
canonical representation, validation, execution, documentation, or tests are
"kind of everywhere" instead of clearly owned.

Use this together with:

- `docs/source-map.md` for file-level ownership.
- `docs/architecture.md` for the compiler/backend pipeline.
- `docs/parity-contracts.md` for the VM/Bash acceptance contract.
- `docs/diagnostics.md` for diagnostic ownership by phase.
- `docs/language.ds` for the user-facing syntax catalog.
- `docs/runtime.md` for VM/runtime substrate rules.

## How to read this map

Each concept answers five questions:

- **Canonical representation**: the shape other phases should consume. Prefer
  AST for syntax-only facts, HIR for validated language behavior, runtime values
  for VM-only state, and Bash helpers only for emitted-script implementation
  details.
- **Validation owner**: the earliest phase that has enough information to reject
  bad programs without duplicating backend behavior.
- **Execution owner**: the component that makes the behavior happen.
- **Documentation owner**: the primary doc that should change when semantics
  change.
- **Tests**: the minimum regression surface that should exist for the concept.

Risk labels:

- **Clear**: one canonical home and predictable backend owners.
- **Watch**: mostly clear, but easy to duplicate in VM/Bash/parser code.
- **Hell candidate**: currently spans many files or has parity-sensitive rules;
  refactor before expanding.

## Concept index

| Concept | Canonical representation | Validation owner | Execution owner | Documentation owner | Tests | Risk |
| --- | --- | --- | --- | --- | --- | --- |
| Command words | AST command payload, then lowerer-validated HIR command payload using `DsCommand*`; see `docs/maintenance/m3-4-command-words-interpolation-boundary.md` | Parser for syntax shape; lowerer for interpolation/word legality and normalization | VM command process path; Bash command emitter | `docs/maintenance/m3-4-command-words-interpolation-boundary.md`, `docs/language.ds`, `docs/architecture.md`, `docs/source-map.md` | VM command tests, Bash parity tests, diagnostics for invalid words | **Watch** |
| Captured command results | HIR expression with shared command-result metadata and runtime `DsValue` object | Lowerer | VM process execution; Bash command-result helpers | Language docs, architecture backend boundary docs | VM tests, Bash parity tests, field diagnostics | **Watch** |
| Command-result fields | Shared descriptor table in command metadata, consumed through HIR | Lowerer for field validity | VM field reads/interpolation; Bash condition/expression helpers | Language docs and architecture command-result notes | Diagnostic tests plus VM/Bash parity field tests | **Watch** |
| Command-result functions | Stdlib/helper metadata lowered into `DS_LOWER_EXPR_CALL.return_kind` plus `DS_LOWER_VALUE_COMMAND_RESULT`; see `docs/maintenance/m3-3-command-result-functions.md` | Lowerer via stdlib/function metadata and command-result field catalog | VM stdlib/user-call execution; Bash stdlib/helper emission consuming lowered call metadata | `docs/maintenance/m3-3-command-result-functions.md`, language stdlib docs, runtime docs | VM tests, Bash parity tests, wrong-arity/field/temporary-access diagnostics | **Watch** |
| Function return kinds | Function declaration metadata and HIR return kind/value kind; see `docs/maintenance/m3-2-function-return-kinds.md` | Function collection/lowering | VM scope/function call path; Bash function emission | `docs/maintenance/m3-2-function-return-kinds.md`, language docs, and milestone specs | VM tests, Bash parity tests, invalid return diagnostics | **Watch** |
| Direct function-call interpolation in command words | Pre-materialized lowerer private string binding plus rewritten command word for the currently supported scalar quoted-word form; see `docs/maintenance/m3-4-command-words-interpolation-boundary.md` | Lowerer using function return-kind metadata | VM command argv construction; Bash command rendering/quoting consuming the rewritten command payload | `docs/maintenance/m3-4-command-words-interpolation-boundary.md`, language docs, and architecture command word rules | VM/Bash parity tests; diagnostics for unsupported return kinds and invalid interpolation forms | **Watch** |
| Pipeline behavior | HIR command/pipeline command payload | Parser for syntax; lowerer for semantic restrictions | VM process pipeline execution; Bash command emitter | Language docs and runtime/process docs | VM/Bash parity tests including stdout/stderr/status | **Hell candidate** |
| Plain command execution | HIR command statement | Parser for command syntax; lowerer for command word validation | VM process execution; Bash command statement emission | Language docs, runtime docs | VM/Bash parity tests; failure/redirect diagnostics | **Watch** |
| Redirection | AST/HIR command redirection metadata | Parser for syntax; lowerer for conservative target validation | VM process file setup; Bash command emitter | Language docs and runtime process docs | VM/Bash parity tests with file side effects; diagnostics | **Watch** |
| Trap/defer behavior | AST handler nodes lowered to HIR handler declarations; see `docs/maintenance/m3-x-trap-defer-signal-ownership.md` | Parser for handler syntax; lowerer for supported signal/context restrictions | VM signal/defer runtime; Bash trap/defer emission | `docs/maintenance/m3-x-trap-defer-signal-ownership.md`, runtime docs, language docs | VM tests, Bash parity tests, deterministic signal tests, diagnostics | **Watch** |
| Signal handling | HIR handler declarations plus backend-specific runtime state; see `docs/maintenance/m3-x-trap-defer-signal-ownership.md` | Lowerer for supported signal literals and handler legality | VM foreground process/signal runtime; Bash `trap` emission | `docs/maintenance/m3-x-trap-defer-signal-ownership.md`, runtime docs, v0.22 milestone docs | Deterministic signal harness, VM/Bash parity tests, diagnostics | **Watch** |
| Handler context | HIR handler scope metadata once supported; currently deferred by `docs/maintenance/m3-x-trap-defer-signal-ownership.md` | Lowerer | VM handler frame/scope; Bash handler variables | `docs/maintenance/m3-x-trap-defer-signal-ownership.md`, language docs, runtime docs | VM/Bash parity tests and diagnostics | **Watch** |
| Environment variables via helpers | Stdlib metadata and HIR call expression/statement | Lowerer via stdlib metadata and env-name validation | VM stdlib env helpers; Bash helper/native env operations | Language stdlib docs, runtime docs | VM/Bash parity tests; process-local mutation tests; diagnostics | **Watch** |
| Direct `env.NAME` access/assignment | Not yet a stable canonical representation; should become explicit HIR env access/assign nodes or remain rejected | Lowerer if syntax parses; parser only for shape | VM stdlib/env runtime and Bash env emission only after HIR exists | Language docs and roadmap/milestones | Diagnostics until supported; then VM/Bash parity tests | **Hell candidate** |
| String interpolation | AST string segments, then HIR interpolation/expression segments | Parser for token/string segmentation; lowerer for expression and format validity | VM string rendering; Bash quoting/interpolation helpers | Language docs and architecture interpolation notes | VM/Bash parity tests; format and unsupported-expression diagnostics | **Hell candidate** |
| Interpolation format specifiers | HIR interpolation segment metadata | Lowerer | VM string formatting; Bash helper/printf formatting | Language docs and milestone specs | VM/Bash parity tests; invalid specifier diagnostics | **Watch** |
| Glob expansion | Stdlib helper metadata and HIR iterable call | Lowerer via stdlib metadata and pattern restrictions | VM stdlib glob implementation; Bash helper/emission | Language stdlib docs, runtime docs | VM/Bash parity tests with sorted output; diagnostics | **Watch** |
| Recursive `**` glob patterns | Explicitly rejected/deferred pattern form until parity strategy exists | Lowerer/stdlib validation | None until supported | Language docs, roadmap/milestones | Diagnostic tests; later VM/Bash parity tests | **Clear while rejected** |
| Regex match | HIR call/operator with conservative regex literal metadata | Parser for literal syntax; lowerer for supported regex surface | VM regex runtime; Bash regex emission | Language docs, runtime regex notes, milestone specs | VM/Bash parity tests; unsupported-regex diagnostics | **Hell candidate** |
| Regex captures/replacement/runtime regex strings | No stable home yet; should not leak into backend-specific ad hoc behavior | Lowerer should reject until canonical HIR/value model exists | None until supported | Language docs and runtime regex plan | Diagnostic tests now; future VM/Bash parity tests | **Hell candidate** |
| Lists/arrays | AST collection literal, HIR collection expression, runtime array value | Parser for literal shape; lowerer for value-kind/element restrictions | VM runtime values; Bash collection helpers/tags | Language docs and runtime docs | VM/Bash parity tests; type/shape diagnostics | **Watch** |
| Maps/objects | AST map literal, HIR map expression, runtime map value | Parser for literal shape; lowerer for key/value restrictions | VM runtime map values; Bash collection helpers/tags | Language docs and runtime docs | VM/Bash parity tests; duplicate/invalid key diagnostics | **Watch** |
| Map iteration | No stable execution home if deferred; eventually HIR iterable semantics | Lowerer should reject until defined | VM iterator runtime; Bash collection iteration helpers | Language docs and milestones | Diagnostic tests now; future VM/Bash parity tests | **Hell candidate** |
| Index reads | HIR index expression over named lists/maps where supported | Lowerer for target/key/index legality, named-binding parity restrictions, and portable literal-or-variable index expressions | VM runtime indexing; Bash helper emission | Language docs and runtime docs | VM/Bash parity tests; bounds/type diagnostics; diagnostics for temporary collection/computed-index access | **Watch** |
| Index assignment | No stable home if deferred; should become explicit HIR assignment target | Lowerer should reject until assignment semantics are defined | VM mutation runtime; Bash collection update helpers | Language docs and roadmap/milestones | Diagnostic tests now; future VM/Bash parity tests | **Hell candidate** |
| Scalar variables and assignment | HIR variable declarations/assignments and lowerer symbol table facts | Parser for statement syntax; lowerer for symbol/value-kind rules | VM scope runtime; Bash statement emission | Language docs, architecture lowering docs | VM/Bash parity tests; duplicate/unknown/invalid assignment diagnostics | **Clear** |
| Conditionals and boolean logic | HIR expression/statement forms | Lowerer for expression validity and type-like constraints | VM interpreter; Bash condition emitter | Language docs | VM/Bash parity tests; diagnostics | **Clear** |
| Loops over iterables | HIR loop statement with iterable expression | Lowerer for iterable eligibility and portable iterable representation | VM interpreter/scope; Bash loop emission | Language docs | VM/Bash parity tests; diagnostics for non-iterables and nonportable temporary array iterables | **Watch** |
| Imports/program composition | Loaded-program aggregate plus AST/HIR composed program | CLI program loader for source/import composition; parser for import syntax | All backends consume composed HIR | Architecture docs and language docs | CLI/check/run/emit tests; diagnostics for import failures | **Watch** |
| Bash parity | HIR as contract plus shared metadata/helper catalogs; see `docs/parity-contracts.md` | Lowerer rejects unsupported parity risks unless explicitly VM-only, Bash-only, diagnostic-only, or currently rejected | VM backend and Bash backend independently execute the same accepted HIR | `docs/parity-contracts.md`, architecture docs, release checklist, milestone specs | Every accepted feature needs VM tests, Bash parity tests, and diagnostics for unsupported forms unless explicitly scoped otherwise | **Watch** |
| VM process execution | VM-private process spec/result around HIR commands | Lowerer validates language rules; VM validates OS/runtime failures | `vm_process.c` and VM interpreter | Runtime docs and architecture backend docs | VM tests plus Bash parity tests for observable behavior | **Watch** |
| Bash helper selection | Bash dependency flags derived from HIR and stdlib metadata | Lowerer for language legality; Bash deps for helper need | Bash emitter/helpers | Architecture docs and source map | Bash emission tests, `bash -n`, parity tests | **Watch** |
| Diagnostics | `DsDiag`/source spans plus the phase ownership contract in `docs/diagnostics.md` | Lexer/parser for syntax; lowerer for semantic/unsupported/parity-gate diagnostics; checker for warnings; VM/Bash only for backend-appropriate failures | Diagnostic renderer/source manager display messages; each owning phase decides its own errors | `docs/diagnostics.md`, architecture docs, parity contract | Diagnostic tests per phase; parity-gate rejection tests before backend selection; runtime/backend tests only for backend-specific failures | **Watch** |
| Formatter behavior | AST-preserving formatter output | Parser must build enough AST; formatter owns formatting policy | Formatter only, no language execution | Language syntax docs and formatter docs/milestones | Formatter snapshot tests; parse-after-format tests | **Watch** |
| Standard-library helper catalog | `ds_stdlib.h` metadata and `stdlib.c` table | Lowerer consumes metadata for arity/kind/flags | VM stdlib and Bash helpers consume same metadata | Language stdlib docs and runtime docs | VM/Bash parity tests plus wrong-arity/name diagnostics | **Watch** |
| Runtime values | `DsValue`/runtime containers | Lowerer controls which language values can reach runtime | VM runtime; Bash has standalone encoded equivalents/helpers | Runtime docs and architecture docs | Runtime/unit tests where available; VM/Bash parity tests | **Watch** |
| Source spans/source manager | `DsSource`, `DsSpan`, `DsLoc` | Each phase attaches/propagates spans | Diagnostics and CLI display | Architecture diagnostics/source manager docs | Diagnostic location tests | **Clear** |

## Hell areas to prioritize

These concepts have the highest risk of becoming "kind of everywhere":

1. **Command words and direct interpolation**: M3.4 now specifies and implements
   the boundary: parser owns command syntax shape; lowerer owns command-word
   semantics, unsupported forms, and pre-materialization of supported scalar
   function-call interpolation; VM/Bash consume accepted command payloads. It is
   tracked under Watch because future command-word expansion can still drift.
2. **Trap/defer/signal behavior**: M3.x now specifies and implements the first
   ownership cleanup in `docs/maintenance/m3-x-trap-defer-signal-ownership.md`:
   parser preserves handler syntax shape, lowerer owns supported-signal and
   handler-context diagnostics, HIR is the accepted handler contract, VM owns
   cleanup/signal execution and foreground process classification, and Bash owns
   standalone trap/helper emission. It remains Watch because signal behavior is
   OS-sensitive and future changes can still drift if they bypass the accepted
   HIR handler contract.
3. **Regex expansion beyond conservative literals**: capture arrays, replacement,
   split, and runtime regex strings need a parity strategy before implementation.
   Otherwise VM regex libraries and Bash regex rules will diverge.
4. **Mutable collections (`map` iteration, index assignment)**: collection value
   encoding already affects VM values and Bash helper sidecars. Mutation and map
   iteration need explicit HIR nodes and tests before adding syntax sugar.
Moved to **Watch** after maintenance cleanup:

- **Command-result functions**: M3.3 names the lowerer/HIR command-result
  contract. Stdlib return metadata now maps once into lowered call value kinds,
  command-result producers/portable returns are classified by lowerer helpers,
  and Bash expression/statement typing consumes lowered call metadata instead of
  re-deriving stdlib call result types from helper names. Remaining risk is drift
  when future command-result-producing helpers or direct temporary support are
  added.

- **Function return kinds**: M3.2 centralizes return-statement validation in the
  lowerer, removes the parser-side semantic `return`-outside-function diagnostic,
  names provisional return-contract discovery for forward value calls, funnels
  expression-position call validation through a lowerer helper, and documents VM
  and Bash return fallbacks as internal invariants. Remaining risk is drift when
  future command interpolation or structured-return features add new call/return
  positions.
- **Bash parity itself**: narrowed by `docs/parity-contracts.md`. A feature is
   accepted only when it has backend-neutral HIR/shared metadata and defined VM
   and Bash behavior, unless documented as VM-only, Bash-only, diagnostic-only,
   or currently rejected. Lowering now rejects temporary collection and
   command-result field/index access, computed collection index expressions,
   nonportable temporary array loop iterables, and nonportable structured
   function-return payloads that previously reached VM but failed Bash emission.
   Remaining risk is
   enforcement drift during feature work, so keep it under **Watch** rather than
   treating it as vague ownership.
- **Diagnostics**: narrowed by `docs/diagnostics.md`. Every phase can emit
   diagnostics, but ownership must follow the rule being checked: parser reports
   syntax, lowerer reports semantic language misuse and unsupported parity forms,
   checker reports warnings, VM reports runtime/OS failures, and Bash emitter
   reports only artifact or accepted-HIR emission errors. Remaining risk is
   remaining backend `unsupported` diagnostics that need classification as
   runtime failures, artifact failures, or defensive accepted-HIR invariants.

## Refactoring rules from this map

- Do not add a backend-only feature path unless `docs/parity-contracts.md`
  explicitly documents it as VM-only or Bash-only. Otherwise add or update the
  HIR representation first, then wire VM and Bash from that representation.
- Do not let Bash helpers become canonical semantics. Helpers implement already
  validated HIR behavior for standalone scripts.
- Do not let VM runtime values define source syntax. Runtime values execute HIR;
  parser/lowerer own user-facing language acceptance.
- Do not spread string/command interpolation rules across VM and Bash. Preserve a
  shared command/string segment model and make backend code render it.
- Do not introduce regex, glob, signal, or collection behavior without deciding
  whether unsupported forms are rejected in lowering or represented in HIR.
- Every concept marked **Hell candidate** needs at least one explicit diagnostic
  test for unsupported forms and at least one VM/Bash parity test for supported
  observable behavior.
