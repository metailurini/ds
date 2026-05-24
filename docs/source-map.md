# Source map

This document is the ownership map for the current `ds` source tree. It is a
refactoring aid, not a feature roadmap. When a file starts owning responsibilities
outside this map, treat that as pressure to split, move, or document a new
boundary before continuing feature work.

Use this with `docs/architecture.md`, which explains the end-to-end pipeline and
larger architectural rules, and `docs/concept-map.md`, which maps cross-cutting
language/runtime concepts to canonical homes. This file answers the narrower
maintenance questions:

- What does each file own?
- What is it allowed to know?
- What should not be here?

## Boundary rules

- Frontend files may know about source text, tokens, parser AST, and syntax
  diagnostics. They should not know VM bytecode, Bash emission, runtime process
  behavior, or semantic execution details.
- Lowering files may know AST, HIR, stdlib metadata, symbol/value-kind rules, and
  semantic diagnostics. They should not know VM instruction encoding, Bash text
  quoting rules, CLI argument parsing mechanics, or subprocess APIs.
- HIR files define the backend-facing contract. HIR should not contain
  backend-specific text fragments, VM registers, Bash helper bodies, or parser
  cursor state.
- VM files may know HIR, bytecode/private VM state, runtime values, subprocess
  behavior, and VM diagnostics. They should not parse source or revalidate
  language features already rejected by lowering except as defensive runtime
  checks.
- Bash files may know HIR, Bash syntax, helper dependency selection, quoting, and
  standalone script generation. They should not perform language validation that
  belongs in lowering or AST parsing that belongs in the frontend.
- Shared runtime files may know generic strings, arrays, maps, values, source
  loading, allocation, and diagnostics. They should not know language grammar,
  backend policy, CLI commands, or emitted Bash layout.
- CLI files may orchestrate source loading, import composition, frontend passes,
  lowering, and backend selection. They should not own grammar, HIR semantics,
  VM instruction behavior, or Bash rendering details.

## Public façade and headers

### `include/ds.h`

Owns:
- Compatibility umbrella for external/small unit harness includes.
- Re-export of focused internal headers while the project migrates to narrower
  includes.

Allowed to know:
- Names of the focused source headers that make up the current public façade.

Does not own:
- New declarations that belong in `src/*.h`.
- Implementation details.
- Feature-specific policy.

### `src/ds_common.h`

Owns:
- Shared source, string-view, source-location/span, diagnostic, and allocation
  declarations.

Allowed to know:
- C standard types needed by all phases.
- The minimal common data structures used across frontend, lowering, backends,
  and runtime.

Does not own:
- Token kinds, AST nodes, HIR nodes, runtime values, stdlib helper metadata, VM
  state, or Bash emission APIs.

### `src/ds_command.h`

Owns:
- Command word, command stage, redirection, captured/plain command metadata, and
  command-result field declarations.

Allowed to know:
- Source spans and string views.
- Command syntax data shared by AST and HIR.

Does not own:
- Parser cursor logic.
- Command semantic validation.
- VM subprocess execution.
- Bash command rendering or quoting.

### `src/ds_ast.h`

Owns:
- Parser-facing AST node shapes for expressions, statements, scripts, functions,
  tests, traps, and handlers.

Allowed to know:
- Command AST payloads and source spans.
- Syntax-preserving structures needed by formatter/debug output and lowering.

Does not own:
- Semantic value kinds.
- HIR simplification policy.
- VM bytecode or Bash output concerns.

### `src/frontend.h`

Owns:
- Token kinds/token vectors.
- Lexer and parser public entrypoints.

Allowed to know:
- AST declarations and source/diagnostic common types.

Does not own:
- Lowering APIs.
- Backend APIs.
- CLI import composition.
- Runtime execution state.

### `src/ds_hir.h`

Owns:
- Lowered program, statement, expression, script declaration, function, test, and
  handler contracts consumed by both backends.

Allowed to know:
- AST source spans and shared command payloads where HIR preserves command data.
- Backend-neutral value-kind information.

Does not own:
- Parser-only syntax preservation.
- VM registers/instructions/scopes.
- Bash text fragments or helper bodies.
- Lowerer-private symbol tables.

### `src/ds_runtime.h`

Owns:
- Runtime value/string/array/map public declarations.

Allowed to know:
- Shared `DsStr`/common types.
- Generic VM/runtime value containers.

Does not own:
- Language grammar.
- Stdlib helper catalog policy.
- Process execution implementation.
- Backend-specific rendering.

### `src/ds_stdlib.h`

Owns:
- Standard-library helper metadata contract: name, Bash helper name, arity,
  return kind, validation flags, and iterable/statement-only flags.

Allowed to know:
- Common string-view types.
- Backend-neutral helper facts that lowering, VM, and Bash all share.

Does not own:
- VM implementation of helpers.
- Bash helper bodies.
- Parser recognition of calls beyond names as identifiers/fields.

### `src/backend.h`

Owns:
- Public formatter/checker, Bash emission, bytecode dump, VM run, and VM test
  entrypoints.
- Backend option structs exposed to the CLI/tests.

Allowed to know:
- AST, HIR, and runtime values needed by backend entrypoints.

Does not own:
- Backend-private data structures.
- Parser or lowerer internals.
- CLI command dispatch.

### `src/parser_internal.h`

Owns:
- Parser-private cursor, token navigation helpers, AST allocation helpers, and
  parser component prototypes.

Allowed to know:
- Token vectors, AST construction helpers, and parser diagnostics.

Does not own:
- Public frontend API declarations.
- Semantic validation.
- HIR lowering.
- Backend behavior.

### `src/lower_internal.h`

Owns:
- Lowerer-private context, symbol/value-kind structures, vectors, and component
  prototypes.

Allowed to know:
- AST, HIR, runtime containers, and stdlib metadata.
- Cross-lowering helper functions for symbols, validation, cloning, and cleanup.

Does not own:
- Public HIR declarations.
- VM bytecode structures.
- Bash emission buffers or quoting helpers.
- CLI orchestration.

### `src/vm_internal.h`

Owns:
- VM-private bytecode, instruction, scope, frame, compiler, and interpreter
  declarations shared by VM implementation files.

Allowed to know:
- Backend entrypoints, HIR, runtime values, and stdlib metadata.
- OS/process headers indirectly needed by VM components only through their `.c`
  files where possible.

Does not own:
- Public runtime value declarations.
- Parser/lowerer internals.
- Bash emission internals.
- CLI command dispatch.

### `src/bash_internal.h`

Owns:
- Bash-emitter-private buffers, emitter context, dependency flags, and component
  prototypes.

Allowed to know:
- Backend/HIR contracts and stdlib metadata.
- Internal Bash symbol naming and emitted-script helper selection state.

Does not own:
- Public backend API.
- Language validation.
- Parser or VM internals.

### `src/bash_helpers.h`

Owns:
- Public-internal declarations for emitted Bash helper body snippets.

Allowed to know:
- Names of helper body strings emitted by `src/bash_helpers.c`.

Does not own:
- Helper dependency analysis.
- Bash expression/statement rendering.
- Stdlib metadata.

### `src/cli_program.h`

Owns:
- CLI program-loading/composition API and loaded-program aggregate declaration.

Allowed to know:
- Frontend and backend/lowering entrypoints needed by CLI commands.

Does not own:
- Command-line parsing text.
- Parser grammar.
- Backend implementation details.

## Shared infrastructure implementations

### `src/source.c`

Owns:
- Reading source files into `DsSource`.
- Freeing source buffers.
- Allocation helpers and string duplication helpers.

Allowed to know:
- Filesystem read errors and `DsDiag` reporting.

Does not own:
- Import graph composition.
- Tokenization/parsing.
- Runtime process execution.

### `src/diag.c`

Owns:
- Diagnostic initialization, source location formatting, and user-facing error
  printing.

Allowed to know:
- Source spans and source text line lookup.

Does not own:
- Deciding which language rules are errors.
- Warning policy for `ds check`.
- Backend-specific recovery behavior.

### `src/runtime.c`

Owns:
- `DsString`, `DsValue`, `DsArray`, and `DsMap` implementation.
- Generic value copy/free/truthiness/string conversion/comparison.

Allowed to know:
- The embedded hashmap adapter under `src/runtime/`.
- Generic runtime value representation.

Does not own:
- VM scope or call-frame policy.
- Stdlib helper dispatch.
- Command execution.
- Bash standalone behavior.

### `src/runtime/hashmap.c`, `src/runtime/hashmap.h`, `src/runtime/hashmap.LICENSE`

Owns:
- Vendored hashmap implementation and its license.

Allowed to know:
- Its own generic hashmap API and internals.

Does not own:
- `DsMap` language-facing policy.
- Any `ds` grammar, HIR, VM, or Bash concerns.

### `src/command.c`

Owns:
- Command word/vector clone and free helpers.
- Command-result field descriptor lookup.

Allowed to know:
- `DsCommand`/`DsWord` shapes and supported command-result field names.

Does not own:
- Parsing command tokens.
- Lowering command interpolation semantics.
- Executing or emitting commands.

### `src/stdlib.c`

Owns:
- Canonical table of standard-library helper metadata.
- Helper lookup, namespace detection, and arity checks.

Allowed to know:
- Backend-neutral helper names, Bash helper names, return kinds, validation
  flags, and arity constraints.

Does not own:
- VM helper implementations.
- Bash helper bodies.
- Parser grammar for helper calls.

## Frontend implementations

### `src/lexer.c`

Owns:
- Tokenizing raw source into tokens.
- Recognizing keywords, identifiers, literals, comments, operators,
  redirections, regex tokens, command interpolation tokens, and source spans.
- Lexical diagnostics.

Allowed to know:
- Token kind names and source location advancement.

Does not own:
- Syntax validation beyond lexical errors.
- Semantic validation.
- AST construction beyond token payloads.
- HIR lowering or backend behavior.

### `src/parser.c`

Owns:
- Public `ds_parse` entrypoint.
- Parser initialization and top-level declaration/statement loop.

Allowed to know:
- Parser component dispatch and AST script root construction.

Does not own:
- Detailed expression grammar.
- Detailed command/script/function/statement grammar when delegated to split
  parser files.
- Semantic validation or backend lowering.

### `src/parse_expr.c`

Owns:
- Expression grammar and expression AST construction.
- Pratt parsing, calls, field/index access, literals, arrays, maps, ranges,
  unary/binary expressions, regex expressions, and captured `run` expressions as
  expression syntax.

Allowed to know:
- Parser cursor helpers and AST expression node allocation.
- Expression precedence.

Does not own:
- Type checking.
- Runtime semantics.
- Bash emission.
- Statement/block parsing outside expression subparsing.

### `src/parse_command.c`

Owns:
- Shell-like command statement parsing.
- Command words, pipelines, redirection suffixes, command interpolation tokens,
  captured `run` command payloads, and command operator diagnostics.

Allowed to know:
- Command-mode token joining and parser cursor helpers.

Does not own:
- Command semantic validation after parsing.
- Subprocess execution.
- Bash quoting/emission.
- General expression grammar except where captured `run` needs expression
  delimiters.

### `src/parse_script.c`

Owns:
- `script { ... }` declaration grammar.
- Script arg/option/flag type/default AST construction.

Allowed to know:
- Script declaration token forms and parser diagnostics.

Does not own:
- Runtime argv binding.
- Bash argument prelude generation.
- Semantic duplicate/default validation beyond parse-shape errors.

### `src/parse_function.c`

Owns:
- Function declaration grammar.
- Test declaration grammar.
- Function parameter/default AST construction.

Allowed to know:
- Parser block/statement helpers and AST function/test shapes.

Does not own:
- Function return-kind inference.
- Recursion checks.
- VM call-frame behavior.
- Bash function rendering.

### `src/parse_stmt.c`

Owns:
- Statement and block grammar dispatch.
- Language statements such as `let`, assignments, conditionals, loops, cases,
  push/assert/return/defer/trap, imports, and fallback command statements.

Allowed to know:
- Expression and command parser entrypoints.
- AST statement construction.

Does not own:
- Semantic validation of names/value kinds/control flow.
- Runtime execution semantics.
- Bash statement rendering.

### `src/ast.c`

Owns:
- AST debug printing.
- AST tree cleanup.

Allowed to know:
- Complete AST shapes and command payload shapes.

Does not own:
- Parsing source into AST.
- Lowering AST into HIR.
- Formatting source text.
- Backend execution/emission.

### `src/format.c`

Owns:
- AST-to-source formatter used by `ds fmt`.
- Formatting layout decisions for currently supported syntax.

Allowed to know:
- AST syntax shapes and source text for preserved command words where needed.

Does not own:
- Parsing or semantic validation.
- HIR lowering.
- Runtime/Bash execution behavior.

## Lowering and semantic implementations

### `src/lower.c`

Owns:
- Public lowering orchestration entrypoints.
- Root lowerer setup, component sequencing, and final lowered-program assembly.

Allowed to know:
- Lowerer-private context and all lowering components.

Does not own:
- Detailed expression/statement/function/test lowering rules when delegated.
- VM bytecode details.
- Bash rendering details.

### `src/lower_expr.c`

Owns:
- Expression semantic validation.
- AST expression to HIR expression lowering.
- Command-word interpolation validation needed before both backends.
- Value-kind inference for expressions and calls where currently supported.

Allowed to know:
- Lowerer symbols, stdlib metadata, AST expressions, HIR expression shapes, and
  backend-neutral command word constraints.

Does not own:
- VM bytecode instructions.
- Bash quoting rules.
- Parser expression grammar.
- Statement block orchestration except through expression use sites.

### `src/lower_stmt.c`

Owns:
- AST statement/block to HIR statement/block lowering.
- Semantic validation for declarations, assignments, control-flow statements,
  loops, cases, traps, defers, returns, asserts, command statements, and block
  scope boundaries.

Allowed to know:
- Lowered expression results, symbol scopes, function context, and HIR statement
  shapes.

Does not own:
- Expression parser grammar.
- VM scope implementation.
- Bash statement text layout.

### `src/lower_symbols.c`

Owns:
- Lowerer symbol tables, name comparisons, namespace/member splitting, scope
  helpers, vector helpers, and name validation utilities.

Allowed to know:
- Lowerer-private symbol/value-kind structs and common naming rules.

Does not own:
- Language syntax parsing.
- Backend execution policy.
- Stdlib helper implementation.

### `src/lower_stdlib.c`

Owns:
- Literal decoding and parsing helpers used by lowering.
- Script declaration/default lowering helpers.
- Stdlib-adjacent validation that is backend-neutral.

Allowed to know:
- Stdlib metadata, script declaration AST/HIR shapes, and literal text forms.

Does not own:
- The canonical helper metadata table.
- VM helper behavior.
- Bash helper bodies.

### `src/lower_functions.c`

Owns:
- Function signature collection.
- Function default validation.
- Function body lowering coordination.
- Return-kind and recursion checks for user functions.

Allowed to know:
- Lowerer symbol/function tables, AST function declarations, HIR function shapes,
  and backend-neutral value kinds.

Does not own:
- Parser function grammar.
- VM call frame mechanics.
- Bash function text generation.

### `src/lower_tests.c`

Owns:
- Test declaration collection and lowering into HIR test entries.

Allowed to know:
- AST test declarations and lowerer/test HIR shapes.

Does not own:
- VM test execution setup.
- CLI test selection.
- Parser test grammar beyond consumed AST shape.

### `src/lower_free.c`

Owns:
- HIR/lowered-program cleanup.

Allowed to know:
- Complete HIR allocation ownership shape.

Does not own:
- AST cleanup.
- VM runtime value cleanup.
- Bash buffer cleanup.

### `src/hir.c`

Owns:
- HIR debug printing.

Allowed to know:
- Complete HIR shapes and source spans for debug output.

Does not own:
- Creating or validating HIR.
- Bytecode generation.
- Bash emission.

### `src/checker.c`

Owns:
- AST-level static warnings used by `ds check`, such as unused declarations and
  shadowing warnings.

Allowed to know:
- AST shapes and source locations for warning output.

Does not own:
- Hard semantic errors that should be produced by lowering.
- HIR transformation.
- Backend execution/emission.

## Bash backend implementations

### `src/bash_emit.c`

Owns:
- Public Bash emission entrypoint implementation.
- Emitted script header/prelude, script-argument prelude, helper inclusion, and
  artifact writing.

Allowed to know:
- Lowered program structure, Bash emitter context, helper dependency flags, and
  output-file errors.

Does not own:
- Expression/statement rendering details delegated to split files.
- Language validation.
- VM behavior.

### `src/bash_deps.c`

Owns:
- Analysis of lowered program/expression/statement usage to determine which Bash
  helpers must be emitted.

Allowed to know:
- HIR shapes and helper dependency flags.

Does not own:
- Helper body text.
- Rendering expressions/statements.
- Stdlib metadata table contents except through helper usage facts.

### `src/bash_expr.c`

Owns:
- HIR expression and condition rendering to Bash text.
- Bash representation of expression values, command-result fields, collection
  operations, stdlib expression calls, and type metadata expressions.

Allowed to know:
- HIR expression value-kind metadata and Bash emitter buffer utilities.

Does not own:
- Language validation.
- AST parsing.
- VM expression evaluation.
- Statement rendering outside expression/condition contexts.

### `src/bash_command.c`

Owns:
- HIR command word, redirection, pipeline, and captured `run` argument rendering
  to Bash text.

Allowed to know:
- Lowered command payloads and Bash quoting/interpolation utilities.

Does not own:
- Parsing command syntax.
- Command semantic validation.
- VM subprocess execution.

### `src/bash_stmt.c`

Owns:
- HIR statement, block, function, loop, case, trap, defer, assert, and test body
  rendering to Bash text.

Allowed to know:
- HIR statement shapes, Bash expression/command rendering helpers, and emitter
  indentation/buffer conventions.

Does not own:
- Expression semantic validation.
- VM control-flow execution.
- Public Bash emission orchestration.

### `src/bash_quote.c`

Owns:
- Bash output buffer utilities.
- Shared Bash quoting, interpolation, identifier/symbol naming, and literal
  escaping helpers.

Allowed to know:
- Bash syntax rules for safe emitted text and internal symbol naming.

Does not own:
- Which statements/expressions get emitted.
- Language semantic validation.
- Helper dependency selection.

### `src/bash_helpers.c`

Owns:
- Text bodies of emitted standalone Bash helper functions.

Allowed to know:
- Bash helper implementation text for command-result, collection, debug, and
  stdlib operations.

Does not own:
- Deciding whether a helper is needed.
- HIR expression/statement rendering.
- VM stdlib behavior except for parity intent.

## VM backend implementations

### `src/vm_compile.c`

Owns:
- HIR to VM bytecode construction.
- Constants/instruction emission and source-location preservation for bytecode.

Allowed to know:
- HIR shapes and VM-private instruction/register/frame structures.

Does not own:
- HIR semantic validation.
- Main interpreter loop.
- Subprocess execution.
- Bash emission.

### `src/vm.c`

Owns:
- Public VM run entrypoints.
- Main interpreter loop.
- Signal handler installation/pending-signal bridge.
- Top-level VM lifecycle coordination.

Allowed to know:
- VM-private bytecode/state structures, runtime values, and runtime diagnostics.

Does not own:
- Bytecode construction details.
- Scope helper internals when delegated.
- Subprocess execution details when delegated.
- Bash behavior.

### `src/vm_dump.c`

Owns:
- Bytecode/debug output formatting.

Allowed to know:
- VM-private instruction/constant shapes.

Does not own:
- Bytecode generation.
- VM execution.
- HIR debug printing.

### `src/vm_args.c`

Owns:
- Runtime binding and validation of `script { ... }` arguments for VM execution.

Allowed to know:
- Lowered script declarations, runtime values, and VM root scope binding helpers.

Does not own:
- Parser script declaration grammar.
- Lowering script declaration semantics.
- Bash argument prelude generation.

### `src/vm_scope.c`

Owns:
- VM scope chain, variable lookup/binding/assignment, function-call setup, and
  frame/scope cleanup helpers.

Allowed to know:
- VM-private scope/frame structs and runtime values.

Does not own:
- Parser/lowerer symbol tables.
- Bytecode instruction selection.
- Bash variable naming.

### `src/vm_process.c`

Owns:
- VM command interpolation, redirection setup, foreground/background process
  execution, pipeline execution, subprocess result capture, and job/signal
  runtime behavior.

Allowed to know:
- VM runtime values, lowered command payloads, OS process APIs, and signal/job
  control details.

Does not own:
- Parsing command syntax.
- Lowering command validation.
- Bash command rendering.

### `src/vm_stdlib.c`

Owns:
- VM execution of standard-library helpers such as `file.*`, `dir.*`, `path.*`,
  `cmd.*`, `env.*`, `glob`, `glob!`, and `lines`.

Allowed to know:
- VM runtime values, stdlib metadata names, filesystem/environment/glob APIs, and
  helper-specific runtime diagnostics.

Does not own:
- Canonical helper metadata.
- Bash helper bodies.
- Parser/lowering validation except defensive runtime checks.

### `src/vm_test.c`

Owns:
- VM-backed execution setup for a single lowered test.

Allowed to know:
- Lowered tests and VM run options/entrypoints.

Does not own:
- Test parsing or test collection.
- CLI test discovery/output policy.
- Bash test execution behavior.

## CLI implementation

### `src/main.c`

Owns:
- Command-line interface parsing and command dispatch.
- User-facing command usage and top-level exit-code policy.

Allowed to know:
- CLI program-loading helpers and public frontend/backend entrypoints.

Does not own:
- Source import composition internals.
- Parser grammar.
- Lowering semantics.
- Backend implementation details.

### `src/cli_program.c`

Owns:
- CLI source loading, lexing/parsing/lowering pipelines, import resolution,
  import graph composition, duplicate/cycle handling, and loaded-program cleanup.

Allowed to know:
- Source manager, lexer/parser/lowerer entrypoints, AST composition details needed
  to build a combined program, and CLI import path mechanics.

Does not own:
- Command-line option parsing.
- Language grammar beyond consuming import AST nodes.
- VM bytecode or Bash rendering behavior.

## Known ownership pressure points

These are not mandatory refactors for this documentation change, but they are the
places to watch during the maintenance phase:

- `src/checker.c` is an AST warning pass while hard semantic errors live in
  lowering. Keep that distinction strict; do not let checker become a second
  semantic validator.
- `src/cli_program.c` composes imports by manipulating AST-level containers.
  That is acceptable for current CLI orchestration, but it should not become a
  general AST transformation engine.
- `src/lower_expr.c` owns both expression lowering and command-word validation.
  If command interpolation grows substantially, consider splitting command-word
  semantic lowering into its own lowerer component.
- `src/vm_process.c` owns many OS/process/job-control details. If richer
  job-control APIs grow, consider splitting command interpolation, process
  spawning, and signal/job-control responsibilities.
- `src/bash_expr.c` is naturally dense because Bash expression rendering has many
  cases. If collection/map/function-value features expand, split by rendered
  expression family rather than adding more unrelated helpers.
