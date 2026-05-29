# Source map

This is the maintainer navigation map for the current `ds` source tree. It names
file ownership and cross-layer rules; it is not a parallel implementation guide
or roadmap. Use it with:

- `docs/architecture.md` for the end-to-end parse -> lower -> backend pipeline.
- `docs/concept-map.md` for cross-cutting concept homes.
- `docs/parity-contracts.md` for VM/Bash acceptance rules.
- `docs/diagnostics.md` for phase-owned diagnostics.
- `docs/runtime.md` for VM/runtime substrate behavior.
- `docs/technical-debt.md` for source-shape, naming, and cohesion debt.

This map describes the current owner for each file. It is not permission to
create one file per concept; use `docs/technical-debt.md` before adding new
source files for narrow helper clusters.

## Layer ownership rules

| Layer | Owns | Must not own |
| --- | --- | --- |
| Frontend | source text, tokens, parser AST, syntax recovery, syntax diagnostics | semantic acceptance, HIR, VM bytecode, Bash rendering, process behavior |
| Lowering | AST -> HIR, symbols, value/return kinds, stdlib metadata, semantic diagnostics, parity gates | VM instruction encoding, Bash quoting details, subprocess/runtime mechanics |
| HIR/shared metadata | backend-neutral accepted program contract | parser cursor state, VM registers, Bash text fragments, backend helper bodies |
| VM backend | accepted HIR execution, bytecode/private VM state, runtime values, subprocess/OS failures | source parsing, semantic validation, Bash artifact policy |
| Bash backend | accepted HIR rendering, quoting, helper selection, standalone script artifact failures | source parsing, semantic validation, canonical language semantics |
| Runtime/shared support | strings, arrays, maps, values, sources, diagnostics, allocation | grammar, backend policy, CLI command dispatch |
| CLI | source loading, import composition, frontend/lower/backend orchestration | grammar, HIR semantics, VM op behavior, Bash rendering |

## Diagnostic ownership

`docs/diagnostics.md` is canonical. In short:

- lexer/parser: lexical and syntax diagnostics;
- lowerer: semantic misuse, unsupported language forms, and VM/Bash parity gates;
- VM: runtime/OS/process failures and internal accepted-HIR invariants;
- Bash: artifact/emission failures and internal accepted-HIR invariants;
- checker/formatter: warnings and presentation diagnostics only.

If VM or Bash reports a source-language unsupported form, treat that as ownership
pressure unless it is explicitly a runtime data failure or an internal invariant.

## File ownership table

### Public façade and focused headers

| File | Owns | Notes |
| --- | --- | --- |
| `include/ds.h` | compatibility umbrella for small harnesses/external includes | do not add feature policy here |
| `src/ds_common.h` | shared source/string/span/diagnostic/allocation declarations | no token, AST, HIR, runtime value, or backend policy |
| `src/ds_command.h` | command word/stage/redirection/capture metadata and command-word shape helpers shared by AST/HIR/lowerer/backends | no parser cursor, command validation, VM execution, or Bash quoting |
| `src/ds_ast.h` | parser-facing AST node shapes and source-level script type names | syntax preservation only; no semantic value kinds or backend contracts; `ds_script_type_name()` is the shared label helper for `string`/`int`/`bool` |
| `src/frontend.h` | lexer/parser public entrypoints and token vectors | no lower/backend APIs |
| `src/ds_hir.h` | lowered program/stmt/expr/function/test/handler contract and lowered value-kind labels | backend-neutral only; `ds_lower_value_kind_name()` owns shared lowered-value metadata labels |
| `src/ds_runtime.h` | runtime value/string/array/map declarations | no grammar or backend rendering |
| `src/ds_stdlib.h` | stdlib helper metadata shared by lowerer/VM/Bash | metadata, not implementation |
| `src/ds_interpolation.h` | interpolation format-spec metadata shared by lowerer/VM/Bash | format contract only; no segment acceptance policy |
| `src/ds_signal.h` | shared supported-signal names and conventional INT/TERM runtime status metadata | consumed by VM and Bash; lowerer still owns source-level signal legality |
| `src/ds_checker.h` | checker warning entrypoint for `ds check` | narrow checker façade; no Bash/VM/backend dependency |
| `src/backend.h` | public formatter/Bash/VM/test backend entrypoints | no backend-private state; checker declarations stay in `ds_checker.h` |
| `src/parser_internal.h` | parser cursor/helpers/component prototypes | parser-private only |
| `src/lower_internal.h` | lowerer-private state and helper prototypes | keep narrow; avoid becoming a concept dump |
| `src/vm_internal.h` | VM-private bytecode/runtime/process/test declarations | no parser/lowerer policy |
| `src/bash_internal.h` | Bash-emitter-private buffers, dependency flags, render helpers | no semantic validation ownership |
| `src/bash_helpers.h` | generated Bash helper bodies catalog | helpers implement accepted HIR only |
| `src/cli_program.h` | CLI import/program composition declarations | orchestration only |

### Shared infrastructure

| File | Owns | Notes |
| --- | --- | --- |
| `src/source.c` | source manager and span/source lookup | diagnostics support only |
| `src/diag.c` | diagnostic collection/rendering for errors and warnings | displays phase decisions; does not decide them |
| `src/runtime.c` | `DsValue`/runtime containers/helpers | VM runtime substrate, not syntax policy |
| `src/runtime/hashmap.*` | generic hashmap implementation | no language semantics |
| `src/ds_command.c` | command payload lifecycle helpers: clone/free/init for words, stages, redirects, and command payloads | data ownership only; no command-word policy, command-result fields, or pipeline semantics |
| `src/ds_command_facts.c` | compact policy-neutral command facts: word shape, command-result field descriptors, and pipeline shape/status helpers | shared facts for lowerer, VM, and Bash; no source-language diagnostics or process execution |
| `src/ds_interpolation.c` | interpolation format-spec parser and kind support table | shared contract for format specs only; lowerer still owns acceptance |
| `src/ds_stdlib.c` | stdlib helper metadata table | canonical helper facts for lowerer/VM/Bash |
| `src/ds_signal.c` | shared supported-signal names and conventional INT/TERM runtime status metadata | consumed by VM and Bash; lowerer still owns source-level signal legality |

### Frontend implementations

| File | Owns | Notes |
| --- | --- | --- |
| `src/lexer.c` | tokens, lexical diagnostics, conservative literal tokenization | regex literal syntax errors are lexical |
| `src/parser.c` | parser entrypoint and top-level coordination | no semantic validation |
| `src/parse_expr.c` | expression syntax and AST shape | may preserve unsupported parseable forms; lowerer decides semantics |
| `src/parse_command.c` | command/pipeline/redirection syntax and recovery | no command-word semantic validation |
| `src/parse_script.c` | script declaration/import/test syntax | frontend shape only |
| `src/parse_function.c` | function declaration syntax | no return-kind semantics |
| `src/parse_stmt.c` | statement syntax and assignment-shape preservation | preserves parseable bracket index assignment for lowerer validation; still rejects statement syntax errors |
| `src/ast.c` | AST allocation/free/debug printing | mirrors AST only |
| `src/format.c` | AST-preserving formatting | formatting policy, not language acceptance |

### Lowering and semantic implementations

| File | Owns | Notes |
| --- | --- | --- |
| `src/lower.c` | lowerer orchestration, program-level symbol setup, and test-block collection | coordinates lowering passes; test collection stays here because it is one small pass, not a standalone module |
| `src/lower_expr.c` | expression lowering and expression value-kind checks | delegates generic string interpolation to `lower_interpolation.c`; keep command-specific interpolation in `lower_command.c` |
| `src/lower_interpolation.c` | generic string interpolation parsing/lowering for normal string expressions | owns normal-string interpolation segment validation; not command-word policy |
| `src/lower_collection.c` | collection portability policy gates for named storage, literal/variable indexes, portable elements, and portable array iterables | lowerer-owned VM/Bash parity rules; VM/Bash must not rediscover these acceptance rules |
| `src/lower_command.c` | command-word/interpolation validation, command-result field legality in words, direct scalar value-call interpolation materialization | consumes shared command-word shape and format-spec metadata; stable owner for command-word lowering |
| `src/lower_stmt.c` | statement lowering, statement-level semantic checks, command statement integration | owns flat index-assignment target/RHS validation, collection loop legality, and delegates command-word details to `lower_command.c` |
| `src/lower_symbols.c` | lowerer scopes/symbol facts | no syntax parsing or backend rendering |
| `src/lower_stdlib.c` | stdlib declaration/use validation | consumes `ds_stdlib.h` metadata |
| `src/lower_functions.c` | function collection, return-kind discovery/validation, call-return contracts | owns function return contract pressure |
| `src/lower_free.c` | lowered tree cleanup | no policy |
| `src/hir.c` | HIR allocation/free/debug helpers | mirrors HIR only |
| `src/ds_checker.c` | warnings/static checks over AST/HIR where applicable | uses shared diagnostic rendering; no hard acceptance except checker-owned warnings |

### Bash backend

| File | Owns | Notes |
| --- | --- | --- |
| `src/bash_emit.c` | standalone script wrapper, prologue, helper emission | consumes shared signal status metadata for cleanup-aware wrappers; artifact assembly only |
| `src/bash_deps.c` | helper dependency detection from accepted HIR, including expression payloads nested in statement-style user function call arguments | no semantic validation |
| `src/bash_structured.c` | Bash structured-value ABI names, type-sidecar writes, structured declarations, command-result storage, and structured return payload helpers | no language validity or semantic value-kind ownership |
| `src/bash_expr.c` | Bash rendering for accepted HIR expressions/conditions | internal invariant diagnostics only for rejected-by-lowering shapes |
| `src/bash_command.c` | shell-safe command argv/pipeline/redirection rendering and captured pipeline assignment mechanics | consumes validated `DsCommand`; no command semantics ownership |
| `src/bash_function.c` | Bash function definition emission, parameter/default binding, and user-function call materialization | owns function wrapper shape and nested user-call argument plumbing; body emission delegates back to statement emitter |
| `src/bash_stmt.c` | Bash rendering for accepted HIR statements, return control flow, handlers, assignment/mutation, and control flow | statement dispatcher; delegates structured payload ABI to `src/bash_structured.c` |
| `src/bash_quote.c` | Bash quoting and accepted string interpolation rendering | interpolation shape invariants are defensive |
| `src/bash_helpers.c` | emitted Bash helper implementations | runtime/artifact behavior for accepted HIR |

### VM backend

| File | Owns | Notes |
| --- | --- | --- |
| `src/vm_compile.c` | accepted HIR -> VM instructions | no semantic language validation |
| `src/vm.c` | VM instruction interpreter, cleanup dispatch, scalar/runtime behavior, command-result/map field materialization, and VM-backed test setup | consumes shared signal status metadata; lowerer owns field legality; unknown command-result fields here are invariants |
| `src/vm_dump.c` | bytecode/debug dump | presentation only |
| `src/vm_args.c` | VM argument handling | runtime call boundary only |
| `src/vm_scope.c` | VM scope stack/storage | runtime state only |
| `src/vm_process.c` | command argv materialization, processes, pipelines, redirection, command-result capture, accepted interpolation rendering | intentionally long but sectioned by concern; uses VM field materialization from `src/vm.c`; consumes shared signal status metadata |
| `src/vm_stdlib.c` | VM stdlib helper implementations | runtime data/OS failures; lowerer owns helper legality where statically known |

### CLI

| File | Owns | Notes |
| --- | --- | --- |
| `src/main.c` | command-line parsing and top-level dispatch | no grammar or feature policy |
| `src/cli_program.c` | source loading/import composition and backend orchestration | no semantics beyond orchestration failures |

### Test harness infrastructure

| File | Owns | Notes |
| --- | --- | --- |
| `tests/lib/testlib.sh` | shared shell assertions and VM/Bash parity helpers | no implementation source lists |
| `tests/lib/build_sources.sh` | focused C unit-test compile groups | reads canonical project sources from `Makefile`; versioned `run.sh` files should choose named groups, not copy large `src/*.c` lists |

## Cross-layer contracts

- Accepted user-facing behavior must lower into backend-neutral HIR/shared metadata
  before VM or Bash can execute/render it.
- Unsupported semantic/parity forms should fail in lowering, except syntax-only
  unsupported forms may fail in the parser when no AST/HIR representation exists.
- VM/Bash may keep defensive internal invariant checks for accepted-HIR shapes that
  should be impossible after lowering.
- Bash helper bodies and VM runtime code may report runtime data failures, such as
  OS/process errors, missing collection keys, out-of-range indexes, or dynamic
  helper arguments that cannot be known statically.
- Formatter/debug printers mirror AST/HIR shapes; they do not define language
  support.
- New concepts should update `docs/concept-map.md` before spreading across parser,
  lowerer, VM, and Bash.

## Known hotspots

| Hotspot | Current status | Maintenance rule |
| --- | --- | --- |
| Command words/interpolation | `src/lower_command.c` is the focused lowerer owner; `src/ds_command_facts.c` owns raw command-word shape helpers; `src/ds_interpolation.c` owns format-spec syntax/kind metadata; VM/Bash consume accepted command payloads | keep semantic validation out of `src/vm_process.c` and `src/bash_command.c` |
| Function returns/command-result functions | lowerer/HIR return-kind metadata is the contract | do not re-derive return kinds in Bash/VM from helper names |
| Bash structured value ABI | `src/bash_structured.c` owns Bash sidecar names, static type-name mapping, type-sidecar writes, structured target declarations, command-result storage names, and structured return payload helpers; statement/expression/function/return emitters consume those helpers | keep the ABI as Bash implementation detail, not language semantics |
| Mutable collections | accepted literals/indexing/push/array loops/map loops are HIR-backed; `src/lower_collection.c` owns portability gates; Bash push/map loops and collection value copies update or preserve type sidecars; unsupported assignment syntax is parser-rejected | do not add mutation AST/HIR without a parity contract |
| Regex | conservative literals are accepted; captures/replacement/runtime regex strings remain rejected | lowerer owns parity gates after lexer syntax |
| Trap/defer/signal | HIR handler contract plus shared signal status metadata in `src/ds_signal.c`; VM/Bash runtime implementations consume it | keep OS/job-control behavior scoped to documented foreground forms |
| Pipeline behavior | accepted command pipeline payload plus shared shape/status helpers in `src/ds_command_facts.c` and VM/Bash process implementations | keep process semantics backend-owned, but language restrictions in parser/lowerer |
| Direct `env.NAME` | AST field/assignment syntax lowered to env helper/set-env behavior | keep env-name validation in lowering where statically known |
| String interpolation | `src/lower_interpolation.c` owns normal-string interpolation lowering; `src/ds_interpolation.c` owns shared format-spec parsing; VM/Bash render accepted interpolation | backend messages for bad shapes should be internal invariants |

## Maintenance checklist

Before adding or moving behavior:

1. Identify the concept home in `docs/concept-map.md`.
2. Decide whether the parser, lowerer, HIR, VM, Bash, or runtime owns the rule.
3. Add/adjust the HIR/shared metadata contract before backend implementation for
   accepted cross-backend behavior.
4. Put user-facing unsupported/semantic diagnostics in the owning frontend/lowerer
   phase.
5. Keep VM/Bash diagnostics for runtime/artifact failures or internal accepted-HIR
   invariants.
6. Add VM/Bash parity tests for accepted behavior and diagnostic tests for rejected
   behavior before expanding the feature surface.