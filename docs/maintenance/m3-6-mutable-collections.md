# M3.6 — Mutable Collection Boundary

This is a maintenance specification and test plan. It does not add collection
features. Feature development remains paused unless a later milestone explicitly
implements one of the deferred pieces below.

Use this document with:

- `docs/source-map.md` for file-level ownership;
- `docs/concept-map.md` for cross-cutting risk status;
- `docs/parity-contracts.md` for VM/Bash acceptance rules;
- `docs/diagnostics.md` for phase-level diagnostic ownership;
- `docs/runtime.md` for runtime value/container ownership;
- `docs/language.ds` for the user-facing collection surface.

## Goal

Make list/map literals, read-only indexing, array iteration, map iteration,
index assignment, and mutation semantics have clear owners before any collection
feature expansion. The preferred maintenance posture is:

- preserve existing list/map behavior;
- preserve VM/Bash parity;
- reject unsupported mutation forms clearly at the earliest correct phase;
- keep VM and Bash backends as executors/renderers of accepted HIR, not
  language validators;
- prefer **clear while rejected** over half-supported mutation behavior.

## Current risk

Mutable collections are dangerous because collection behavior touches many
separate contracts:

- parser expression/statement shapes for literals, indexing, loops, assignment,
  field access, and method-like calls;
- AST collection preservation for formatting, AST debug output, and lowering;
- lowerer value-kind inference, element/value restrictions, indexing rules,
  loop iterable rules, and VM/Bash parity gates;
- HIR array/map/index/loop/push nodes consumed by both backends;
- VM runtime `DsValue` arrays/maps and bounds/key failures;
- Bash array/associative-array encoding, sidecar value-type metadata, and helper
  functions;
- diagnostics that must distinguish syntax shape, semantic rejection, runtime
  missing keys, runtime out-of-range indexes, and internal backend invariants;
- tests across v0.10, v0.17, v0.24, v0.25, v0.26, and future collection
  cleanup passes.

The Hell area is not that arrays/maps exist. The Hell area is allowing mutation
or map iteration to become "kind of everywhere" before the language defines a
portable HIR and backend contract.

## Current behavior today

### Supported today

The current supported surface is intentionally narrow:

- list/array literals with scalar, Bash-emittable element expressions;
- map/object literals with non-empty string-like keys and scalar,
  Bash-emittable value expressions;
- duplicate map key diagnostics;
- read-only list indexing through a named array binding;
- read-only map indexing through a named map binding;
- map field reads such as `app.name`, where the receiver is a named map binding;
- literal indexes and named variable indexes for portable collection access;
- array `push` through `array.push(value)`;
- `for item in array` for named arrays and selected stdlib array results;
- collection-valued function returns for flat scalar arrays/maps under the
  function return-kind contract;
- VM/Bash parity tests for accepted literals, indexing, array loops, `push`, and
  flat structured returns;
- explicit runtime failures for missing map keys and out-of-range array indexes.

### Unsupported today and must remain rejected

These forms remain rejected until a later feature milestone defines canonical HIR
and backend parity behavior:

- map iteration (`for key, value in map`);
- direct iteration over map literals;
- index assignment (`xs[0] = value`, `m["key"] = value`);
- nested index assignment (`matrix[0][1] = value`, `cfg["api"].port = value`);
- map field assignment (`app.name = "api"`);
- arbitrary collection mutation beyond currently supported `array.push(value)`;
- nested arrays/maps as collection elements/values;
- empty map literals;
- empty map keys;
- computed collection indexes that are not literals or named variables;
- direct field/index access on temporary collection-returning expressions;
- collection values as direct command arguments or unsupported interpolation
  payloads;
- map iteration order semantics;
- collection destructuring;
- collection-valued parameters.

Unsupported mutation forms should be rejected as language/semantic errors, not
accidentally by Bash assignment syntax or VM runtime fallbacks.

## Current implementation trace

### Parser / AST

Relevant files:

- `src/parse_expr.c`
- `src/parse_stmt.c`
- `src/ds_ast.h`
- `src/ast.c`
- `src/format.c`

Current parser behavior:

- Parses array literals as `DS_EXPR_ARRAY`.
- Parses map literals as `DS_EXPR_MAP` with source-preserved keys and values.
- Parses bracket indexing as `DS_EXPR_INDEX`.
- Parses field access as `DS_EXPR_FIELD`; lowering later decides whether it is a
  command-result field or map field.
- Parses assignment statements only for identifier-like assignment targets and
  direct `env.NAME` assignment. There is no stable AST assignment target for
  `xs[0] = value` or `map.key = value`.
- Parses `name.push(value)` as a dedicated `DS_STMT_PUSH` syntax shape.
- Parses `for key, value in expr` into the for-statement shape; lowering rejects
  map iteration while it is deferred.

Parser caveat:

- Some current collection diagnostics still come from parser shape checks, such
  as malformed map literal syntax or unsupported method names in the
  `name.push(...)` grammar. That is acceptable only for syntax-shape errors.
  Semantic collection policy belongs to lowering.

### AST

AST owns source shape and debug/format preservation only:

- array/map literal shape;
- index expression shape;
- field expression shape;
- `push` statement shape;
- `for` key/value-name shape;
- assignment statement source span/name/operator/value shape.

AST must not decide whether a collection operation is semantically valid, whether
an index is portable, or whether Bash can emit the operation.

### HIR / lowering

Relevant files:

- `src/lower_expr.c`
- `src/lower_stmt.c`
- `src/lower_functions.c`
- `src/lower_symbols.c`
- `src/lower_internal.h`
- `src/ds_hir.h`

Current lowerer behavior:

- Lowers arrays to `DS_LOWER_EXPR_ARRAY`.
- Lowers maps to `DS_LOWER_EXPR_MAP`.
- Lowers read-only index expressions to `DS_LOWER_EXPR_INDEX` with backend-neutral
  metadata: array vs map receiver, map literal key metadata, and element kind.
- Rejects direct field/index access on temporary collection-returning expressions
  for VM/Bash parity.
- Rejects computed indexes that are not literals or named variables for VM/Bash
  parity.
- Rejects map iteration with `map iteration is deferred...`.
- Rejects non-array loop iterables for array loops.
- Rejects nonportable temporary array iterables.
- Rejects empty maps, duplicate keys, empty keys, nested or nonportable element
  expressions, and type-mixed return kinds where applicable.
- Lowers `array.push(value)` to `DS_LOWER_STMT_PUSH`.

Lowering is the canonical semantic validation owner for collection behavior.

### VM execution

Relevant files:

- `src/vm.c`
- `src/vm_compile.c`
- `src/vm_scope.c`
- `src/vm_stdlib.c`
- `src/ds_runtime.h`
- runtime map/array support under `src/runtime/`

Current VM behavior:

- Executes accepted array/map literals as runtime `DsValue` containers.
- Executes accepted read-only index expressions.
- Reports runtime failures for accepted operations whose runtime data fails, such
  as missing map keys or out-of-range indexes.
- Executes accepted `array.push(value)` mutation for named arrays.
- Executes accepted array loops.

The VM must not redefine language validity for map iteration, index assignment,
nested mutation, or direct temporary access. If such forms reach VM execution,
that is either a lowerer bug or an internal invariant failure.

### Bash emission

Relevant files:

- `src/bash_expr.c`
- `src/bash_stmt.c`
- `src/bash_deps.c`
- `src/bash_helpers.c`
- `src/bash_emit.c`

Current Bash behavior:

- Emits accepted array literals using Bash indexed arrays.
- Emits accepted map literals using Bash associative arrays plus value-type
  sidecar metadata.
- Emits read-only indexes through helpers such as `__ds_array_get` and
  `__ds_map_get`.
- Emits missing-key runtime errors in generated Bash.
- Emits `array.push(value)` as Bash array append for accepted named arrays.
- Emits array loops for named arrays and supported stdlib array results.
- Includes Bash 4+ guards only when maps are used.

Bash emission must consume lowered/HIR collection metadata. It must not become
the owner of collection validity. Backend diagnostics for unsupported collection
forms should be internal invariant failures once lowering owns rejection.

### Diagnostics

Current diagnostic ownership:

- Lexer/parser: malformed literal syntax, missing delimiters, malformed statement
  shape.
- Lowerer: unsupported collection features, parity gates, nonportable element
  expressions, invalid receiver/index kinds, duplicate keys, empty keys, map
  iteration rejection, temporary access rejection, and computed-index rejection.
- VM: runtime data failures for accepted programs, such as out-of-range indexes
  and missing keys.
- Bash: emitted-script runtime data failures for accepted programs and internal
  invariant failures for impossible HIR.

## Architectural failure to avoid

The failure mode is accepting a collection operation because one backend can
execute it while the other backend rejects it or gives different semantics. The
most dangerous examples are:

- accepting map iteration before defining iteration order and Bash/VM parity;
- accepting index assignment without a HIR assignment-target node and Bash sidecar
  type-update rules;
- allowing nested mutation where VM can mutate nested `DsValue` containers but
  Bash cannot update equivalent nested structures;
- allowing temporary collection access or computed indexes to pass `check` and
  fail only during Bash emission;
- allowing `push` or future collection methods to become parser-owned semantic
  policy instead of lowerer-owned validation.

## Desired ownership

### Syntax owner

Parser owns only syntax shape:

- array/map literal delimiters and entries;
- bracket-index expression shape;
- field expression shape;
- `for key[, value] in expr` shape;
- assignment statement shape;
- current `name.push(value)` method-like statement shape.

Parser should not decide collection mutability, map iteration legality, index
portability, or backend support except when the source text cannot be parsed into
any valid syntax shape.

### Semantic validation owner

Lowerer owns:

- value-kind inference for arrays/maps/index reads;
- element/value-kind restrictions for currently portable collections;
- duplicate/empty key policy where source shape is known;
- map iteration rejection while deferred;
- index assignment and nested mutation rejection once parseable assignment-target
  shapes exist;
- temporary receiver rejection;
- computed-index parity rejection;
- collection command/interpolation rejection;
- function-return collection portability gates;
- any future collection method eligibility.

### Canonical representation owner

HIR owns accepted backend-neutral collection behavior:

- `DS_LOWER_EXPR_ARRAY` for accepted array literals;
- `DS_LOWER_EXPR_MAP` for accepted map literals;
- `DS_LOWER_EXPR_INDEX` for accepted read-only index expressions;
- `DS_LOWER_STMT_FOR_ARRAY` for accepted array iteration;
- `DS_LOWER_STMT_PUSH` for accepted array append;
- future explicit HIR assignment-target/mutation nodes only after a milestone
  defines VM/Bash parity.

There is intentionally no canonical HIR for map iteration or index assignment
while they remain rejected.

### VM execution owner

VM owns execution of accepted HIR only:

- allocate/copy runtime arrays and maps;
- evaluate indexes and runtime data failures;
- mutate named arrays for accepted `push`;
- run accepted array loops;
- preserve observable status/stdout/stderr parity with Bash.

### Bash emission owner

Bash owns rendering accepted HIR only:

- encode arrays/maps and value-kind metadata;
- render read-only index helpers;
- render accepted array appends;
- render accepted array loops;
- report internal emission invariant failures if impossible HIR reaches the
  backend.

Bash does not own semantic collection acceptance.

## VM/Bash parity contract

VM and Bash must agree on:

- list literal values and element order;
- map literal keys and values;
- duplicate/empty-key rejection before backend selection;
- read-only index results for accepted indexes;
- runtime error behavior for missing keys and out-of-range indexes;
- `array.push(value)` side effects for named arrays;
- array loop iteration order;
- function-returned flat arrays/maps after binding to a name;
- rejection of map iteration, index assignment, nested mutation, temporary access,
  and unsupported collection command/interpolation forms.

VM and Bash do not need to agree on behavior for forms that are rejected before
backend selection because those forms must not reach execution/emission.

## Diagnostic contract

### Parser diagnostics

Parser should emit diagnostics for malformed syntax only, for example:

- missing `]` or `}`;
- missing `:` in a map entry;
- malformed map entry shape;
- malformed `push(...)` call shape if the parser continues to own that syntax.

### Lowerer diagnostics

Lowerer should emit diagnostics for semantic or parity rejection, including:

- map iteration deferred;
- index assignment deferred, once parseable as a collection assignment target;
- nested mutation deferred;
- indexing non-array/non-map values;
- array index not int;
- map index not string;
- temporary collection access requires named binding;
- computed collection index must be literal or named variable;
- collection element/value expression not portable;
- collection-valued command/interpolation rejection;
- invalid `push` receiver/value-kind if parseable.

### Runtime/backend diagnostics

Runtime/backend diagnostics are allowed only for:

- runtime data failures in accepted programs, such as missing keys or out-of-range
  indexes;
- OS/artifact failures;
- internal invariant failures if lowerer validation was bypassed.

## Non-goals

This maintenance track does not add:

- map iteration;
- index assignment;
- nested mutation;
- map field assignment;
- collection destructuring;
- collection-valued parameters;
- nested array/map values;
- ordered-map semantics;
- first-class iterators;
- collection slicing;
- map deletion;
- set/dictionary APIs;
- command-argument collection splatting;
- Bash emitter or VM runtime rewrites.

## Deferred vs out of scope

Deferred items are plausible future language work but require new HIR/parity
contracts first:

- map iteration;
- index assignment;
- nested mutation;
- map field assignment;
- collection-valued parameters;
- nested collections.

Out of scope for this maintenance step:

- any new collection feature;
- any parser rewrite;
- any VM/Bash collection encoding redesign;
- cosmetic diagnostic rewrites not needed for ownership;
- broad standard-library collection API expansion.

## What must not be postponed

Before marking mutable collections as resolved beyond **Watch**, the project must
not postpone:

- a precise list of accepted vs rejected collection forms;
- lowerer-owned diagnostics for rejected semantic/parity forms;
- tests proving rejected forms fail before backend-specific behavior;
- VM/Bash parity tests for every accepted collection operation;
- backend invariant wording for impossible collection HIR;
- concept-map risk split so future work does not treat all collection behavior as
  one vague bucket.

This specification provides the list and test plan. A later implementation pass
must verify and adjust code/tests where current behavior does not match it.

## Implementation plan

### M3.6.1 — Spec and risk split

- Add this maintenance spec.
- Update `docs/concept-map.md` to split mutable collection risk into literals,
  read-only indexing, map iteration, index assignment, nested mutation,
  VM/Bash parity, and diagnostics.

### M3.6.2 — Diagnostic ownership audit

- Grep parser/lowerer/backend diagnostics for collection-specific language
  policy.
- Keep parser syntax diagnostics.
- Move obvious semantic policy from parser/backend into lowering only if the form
  already parses and the move is behavior-preserving.
- Reword backend leftovers as internal invariants when lowering should prevent
  them.

### M3.6.3 — Rejected mutation forms

- Add/verify diagnostics for parseable unsupported forms:
  - map iteration;
  - collection command/interpolation use;
  - temporary access;
  - computed indexes;
  - index assignment if parseable;
  - nested mutation if parseable.
- Do not implement mutation.

### M3.6.4 — Accepted behavior parity audit

- Verify VM/Bash parity for:
  - array literals;
  - map literals;
  - read-only array indexing;
  - read-only map indexing;
  - map field reads;
  - `array.push(value)`;
  - array loops;
  - flat array/map function returns after named binding.

### M3.6.5 — Backend invariant cleanup

- Audit Bash and VM collection diagnostics.
- Keep runtime data failures.
- Reword impossible source-language forms that should have been rejected by
  lowering as internal invariants.

### M3.6.6 — Final docs/status update

- Update `docs/concept-map.md` after code/tests prove ownership.
- Keep mutable collection expansion deferred unless a separate feature milestone
  defines HIR and parity semantics.

## Comprehensive test plan

The implementation pass should add or verify these cases using existing test
conventions.

### Documentation/source checks

- `docs/concept-map.md` links to this spec.
- `docs/language.ds` still describes map iteration, index assignment, nested
  collections, and collection-valued parameters as deferred.
- `docs/parity-contracts.md` and `docs/diagnostics.md` remain consistent with the
  lowerer-owned rejection model.

### Supported list/map literals

- Array literal with strings, ints, bools where currently supported.
- Map literal with identifier keys and quoted string keys.
- Duplicate map key rejected before backend selection.
- Empty map literal rejected.
- Empty map key rejected.
- Nested arrays/maps rejected.
- Complex element/value expressions rejected unless bound first, according to the
  current Bash-emittable rule.

### Supported read-only indexing

- `let xs = ["api", "web"]; let first = xs[0]` VM/Bash parity.
- `let i = 0; let first = xs[i]` VM/Bash parity if named-variable indexes are
  supported.
- `let m = { api: 3000 }; let p = m["api"]` VM/Bash parity.
- `let key = "api"; let p = m[key]` VM/Bash parity if named-variable indexes are
  supported.
- `let p = m.api` VM/Bash parity.
- Array index out of range reports runtime failure consistently enough for
  accepted programs.
- Missing map key reports runtime failure consistently enough for accepted
  programs.

### Rejected indexing forms

- `let first = ["api"][0]` rejected by lowerer for temporary access.
- `let name = app().name` rejected by lowerer for temporary access.
- `let first = xs[0 + 0]` rejected by lowerer for computed index.
- `let value = m["api".trim()]` rejected by lowerer for computed index.
- Array index with non-int rejected by lowerer.
- Map index with non-string rejected by lowerer.
- Indexing a scalar rejected by lowerer.

### Supported iteration forms

- `for item in xs` over a named array has VM/Bash parity.
- `for item in string.split(...)`, `glob(...)`, and `lines(...)` remain accepted
  only where currently supported.
- Function-returned arrays require named binding before looping if current parity
  rules require it.

### Rejected map iteration forms

- `for key, value in m` rejected by lowerer with the map-iteration diagnostic.
- `for key, value in { api: 3000 }` rejected.
- `for key in m` should not silently become map key iteration unless explicitly
  supported by a future milestone; current behavior should either reject as
  non-array iterable or the map-iteration diagnostic, depending on the parsed
  form.
- Map iteration rejection must be identical for `check`, `run`, and `emit bash`.

### Rejected index assignment and nested mutation

- `xs[0] = "api"` rejected; if parser treats this as malformed syntax today,
  document that syntax-shape rejection. If a later parser accepts assignment
  targets, lowerer must own the semantic rejection.
- `m["api"] = 3000` rejected.
- `m.api = 3000` rejected.
- `matrix[0][1] = "x"` rejected.
- `cfg["api"].port = 8080` rejected.
- Rejections must not come from Bash assignment syntax or VM runtime fallback.

### Supported mutation form: array push

- `xs.push("cron")` VM/Bash parity.
- `xs.push(value)` with scalar named value VM/Bash parity.
- `push` on non-array rejected at the semantic layer if parseable.
- wrong arity/malformed `push` remains syntax/shape diagnostic where appropriate.

### Command-result and function-returned collection edges

- Array/map returned from a function can be bound and then read through accepted
  index/field access.
- Direct temporary access remains rejected.
- Command-result values are not maps; command-result fields follow the command
  result contract, not collection mutation rules.
- Collection values are rejected as direct command arguments/interpolation unless
  already supported by the command-word contract.

### VM/Bash parity tests

For every accepted behavior above, include:

- direct VM run assertion;
- emitted Bash run assertion;
- stdout/stderr/status comparison when relevant.

### Diagnostics tests

For rejected behavior above, include:

- `ds check` rejection;
- `ds emit bash` rejection with the same frontend/lowering diagnostic;
- `ds run` rejection when useful to prove VM did not become the semantic owner;
- diagnostic text that names the current limitation and suggests binding first
  where that is the accepted workaround.

## Exit criteria

M3.6 implementation cleanup can move collection sub-risks out of Hell only when:

- accepted collection behavior is fully listed and covered by VM/Bash parity
  tests;
- rejected mutation/map-iteration forms fail before backend selection;
- backend diagnostics for impossible collection HIR are internal invariants;
- map iteration and index assignment are either **Clear while rejected** or have a
  new feature milestone with canonical HIR/parity contracts;
- `docs/concept-map.md` reflects the narrowed sub-risks rather than one vague
  mutable-collections Hell candidate.
