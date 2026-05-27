# `src/` ownership guide

The source tree is split by compiler/runtime phase. The detailed per-file map is
in [`../docs/source-map.md`](../docs/source-map.md), and cross-cutting concepts
are mapped in [`../docs/concept-map.md`](../docs/concept-map.md). Keep both
documents updated whenever a file gains or loses a responsibility or a language
concept gets a clearer canonical home.

## Phase boundaries

```txt
source files
  -> source/diagnostics/runtime substrate
  -> lexer
  -> parser / AST
  -> lowering / semantic validation / HIR
  -> VM bytecode + runtime execution
  -> Bash standalone emission
```

The main rule: supported language behavior must pass through HIR before reaching
either backend. The VM and Bash backend should not implement separate languages.

## Directory map

- `lexer.c`, `parser.c`, `parse_*.c`, `parser_internal.h`, `frontend.h`: frontend
  syntax ownership. These files produce tokens/AST and should not know backend
  runtime behavior.
- `ds_ast.h`, `ast.c`, `format.c`: AST shape, AST debug output, AST cleanup,
  source formatting, and shared source-level script type labels.
- `lower*.c`, `lower_internal.h`, `ds_hir.h`, `hir.c`: semantic validation,
  AST-to-HIR lowering, lowered-program ownership, HIR debug output, and shared
  lowered value-kind labels.
- `bash_*.c`, `bash_internal.h`, `bash_helpers.h`: HIR-to-standalone-Bash
  emission. Keep concerns cohesive: command rendering in `bash_command.c`,
  structured-value ABI helpers in `bash_structured.c`, function wrappers and
  user-call materialization in `bash_function.c`, return control flow in
  `bash_return.c`, and statement dispatch in `bash_stmt.c`.
- `vm*.c`, `vm_internal.h`: HIR-to-bytecode construction, bytecode dumping, VM
  execution, scopes, script argv binding, subprocess/runtime behavior, stdlib
  execution, and VM test setup.
- `runtime.c`, `ds_runtime.h`, `runtime/`: generic runtime containers and the
  vendored hashmap implementation.
- `source.c`, `diag.c`, `ds_common.h`: source loading, diagnostics, allocation,
  source spans, and shared primitive declarations.
- `ds_command.c`, `ds_command.h`: shared command payload containers and
  lifecycle helpers.
- `ds_command_word.c`, `ds_command_word.h`: raw command-word shape helpers used
  by lowering, VM argv materialization, and Bash command emission.
- `ds_command_result.c`, `ds_command_result.h`: command-result field catalog,
  field kinds, and backend storage aliases.
- `ds_command_pipeline.c`, `ds_command_pipeline.h`: pipeline shape/status helpers
  shared by VM and Bash-oriented code.
- `ds_stdlib.c`, `ds_stdlib.h`: canonical stdlib helper metadata shared by lowering,
  VM, and Bash.
- `cli_program.c`, `cli_program.h`, `main.c`: CLI orchestration, import-aware
  program loading, command dispatch, and top-level user-facing command behavior.
- `backend.h`: public backend/checker/formatter entrypoints used by CLI/tests.

## Refactoring checklist

Before putting code in a file, ask:

1. Does this file already own this phase of the pipeline?
2. Is the new code backend-neutral, VM-specific, Bash-specific, parser-specific,
   or CLI-specific?
3. Would this make the file validate a rule that another phase already owns?
4. Would this make the VM and Bash backend diverge instead of consuming the same
   HIR fact?
5. Should this be a new helper in the same phase rather than expanding a dense
   file?

If the answer is unclear, update `docs/source-map.md` or `docs/concept-map.md`
first. Ambiguous ownership is refactoring work, not just documentation work.
