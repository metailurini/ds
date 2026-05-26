# Phase B3 — Process / pipeline / signal runtime trace

## Scope

This trace covers one remaining high-pressure concept: process execution,
pipeline execution, command-result capture, and trap/defer/signal runtime
behavior.

It is a pressure trace, not an implementation plan. It records current ownership
so a future extraction can cut along existing boundaries instead of creating a
large process/runtime rewrite.

## Current representation

- Command syntax is parsed into `DsCommand` / `DsCommandStage` / `DsRedirect` in
  `src/ds_command.h`.
- Plain command statements lower to `DS_LOWER_STMT_CMD` with a `DsCommand`.
- Captured commands lower to `DS_LOWER_EXPR_RUN` with a `DsCommand`.
- Pipelines are represented as a `DsCommand` with multiple stages; shared
  helpers in `src/ds_command_pipeline.c` own pipeline shape checks and VM pipefail-style
  status calculation.
- Command-result fields use the shared descriptor table in `src/ds_command_result.c`;
  VM field materialization is isolated in `src/vm.c`.
- `defer` and `trap` lower to `DS_LOWER_STMT_DEFER` and `DS_LOWER_STMT_TRAP`.
- Handler signal identity is stored as `DsHandlerSignal` in HIR.

## Current owners

- Parser owns command, pipeline, redirection, `defer`, and `trap` syntax shape.
- Lowerer owns command-word legality, command-result field legality, supported
  handler signals, handler capture restrictions, and rejected handler contexts.
- HIR/shared command metadata owns the accepted backend-neutral representation,
  pipeline shape checks, and the VM's pipefail-style status helper.
- VM owns accepted command execution, process spawning, waiting, captures,
  process-group mechanics, signal observation, and runtime/OS failures.
- Bash owns standalone rendering of accepted command/process behavior, helper
  selection, cleanup-aware wrappers, and generated trap/defer mechanics.
- Tests own the VM/Bash observable parity contract.

## Direct command execution path

1. Parser preserves each command word, stage, redirection operator, and source
   span.
2. Lowering validates command-word forms in `src/lower_command.c` and statement
   integration in `src/lower_stmt.c`.
3. VM compilation copies accepted command metadata into instructions in
   `src/vm_compile.c`.
4. VM execution calls `run_command()` in `src/vm_process.c`.
5. `src/vm_process.c` materializes argv, applies redirection, traces when
   enabled, forks, execs, waits, and reports OS/process failures.
6. Plain command non-zero exit is fail-fast unless classified as supported
   signal cleanup.
7. Bash statement emission renders accepted commands in `src/bash_stmt.c` and
   command words/pipelines in `src/bash_command.c`.
8. Without signal handlers, Bash relies on normal `set -euo pipefail` behavior
   plus emitted error wrappers where needed.
9. With signal handlers, Bash uses cleanup-aware wrappers for foreground direct
   commands.

## Command-result capture path

1. `run ...` expressions lower to `DS_LOWER_EXPR_RUN`.
2. Lowering records command-result value kind and validates field access against
   shared command-result metadata.
3. VM compilation emits `OP_RUN_CAPTURE`.
4. VM `run_capture()` executes either a direct command or pipeline in capture
   mode.
5. VM capture stores stdout, stderr, and normalized status in `DsCommandResult`.
   Later VM field reads are materialized by `src/vm.c`.
6. Captured non-zero exits are data, not fail-fast errors.
7. Bash direct capture uses `__ds_capture` helper emission.
8. Bash captured pipelines use emitter-managed temporary files and assign
   `stdout`, `stderr`, `code`/`status`, `ok`, and `failed` fields.
9. Both VM and Bash must preserve stdout/stderr separation and command-result
   field names.
10. Cleanup of VM buffers is `DsValue` ownership; cleanup of Bash pipeline temp
    dirs is emitted in the generated script.

## Pipeline execution path

1. Parser stores multiple stages in one `DsCommand`.
2. Lowering treats pipeline shape as accepted command metadata and keeps command
   word validation in the lowerer.
3. VM `run_command()` dispatches multi-stage commands to
   `process_execute_pipeline()`.
4. VM creates pipes, forks all stages, assigns process groups when possible,
   waits for each child, and computes pipefail-style status through
   `src/ds_command_result.c`.
5. VM plain pipelines are fail-fast on non-zero status unless signal cleanup
   owns the interruption path.
6. VM captured pipelines collect final stdout plus stage stderr into command
   result data.
7. Bash normal pipeline emission is in `src/bash_command.c`.
8. Bash plain pipeline statements add pipeline failure diagnostics in
   `src/bash_stmt.c`.
9. Bash captured pipeline assignment is emitted in `src/bash_stmt.c` with temp
   files and direct field assignments.
10. Bash signal-aware foreground pipelines use `__ds_run_pipeline` helper text
    from `src/bash_emit.c`.

## Trap / defer / signal path

1. Parser owns handler syntax only.
2. Lowering validates supported signals: `EXIT`, `INT`, and `TERM`.
3. Lowering rejects unsupported handler captures, direct returns, unsupported
   contexts, and unsupported signal names.
4. VM compilation emits `OP_REGISTER_HANDLER` with signal and trap/defer kind.
5. VM runtime stores trap replacement slots and defer stacks.
6. VM cleanup dispatch runs signal trap, matching signal defers LIFO, then EXIT
   cleanup according to the current contract.
7. VM installs lightweight `INT`/`TERM` observation, consumes shared signal
   status metadata, and forwards supported foreground interruptions to child
   process groups where possible.
8. Bash emits handler functions in `src/bash_stmt.c`.
9. Bash emits cleanup stacks, trap slots, trap setup, direct-command wrappers,
   and pipeline wrappers in `src/bash_emit.c`.
10. Bash helper logic preserves the same cleanup ordering and consumes shared
    `INT`/`TERM` status metadata unless a handler overrides status.

## Runtime diagnostics ownership

Correctly VM-owned diagnostics include:

- fork, exec, pipe, temporary-file, redirection, wait, and read failures;
- plain command and pipeline non-zero exit diagnostics;
- command-result capture I/O failures;
- runtime interpolation/rendering failures caused by runtime values;
- internal field invariants for command-result/map receivers after lowering.

Correctly Bash-owned diagnostics include:

- emitted script command/pipeline failure messages;
- helper/runtime failures from accepted Bash artifacts;
- cleanup-aware wrapper failures;
- Bash ABI/internal invariant failures for accepted HIR.

Lowering must continue to own:

- unsupported command-word source forms;
- unknown command-result fields;
- unsupported handler signals or contexts;
- semantic/parity gates before either backend runs.

## Suspicious diagnostics

No current diagnostic needs moving as part of this trace.

The risk is not an individual message today. The risk is future drift: command,
pipeline, capture, and signal behavior all have backend mechanics that can sound
semantic if a new rejection is added there without a lowerer gate.

## Coupling points

- `src/vm_process.c` currently owns argv rendering, interpolation runtime,
  direct process execution, capture, pipelines, redirection, and process groups.
- `src/ds_command_pipeline.c` owns shared pipeline shape checks and VM pipefail-style status
  calculation; it does not own process execution.
- `src/vm.c` owns VM command-result/map field materialization;
  capture/storage stays in `src/vm_process.c` because it is produced by process
  execution.
- `src/bash_stmt.c` emits normal statements, direct captures, captured pipeline
  assignments, signal-aware direct commands, signal-aware pipelines, and handler
  functions.
- `src/bash_emit.c` emits global cleanup helpers plus direct-command and pipeline
  signal wrappers.
- `src/bash_deps.c` scans accepted HIR for run, pipeline, and signal-helper
  needs.
- Command-result capture and pipeline behavior share representation but have
  different failure semantics.
- Signal cleanup intersects direct commands and pipelines but should not own
  command syntax or handler legality.

## Concepts that should stay separate

- Direct process execution: spawn, redirect, wait, status, OS errors.
- Command-result capture: stdout/stderr/status as data.
- Pipeline mechanics: multi-stage pipe setup and pipefail status.
- Signal cleanup: handler registration, cleanup order, shared signal-status metadata, signal classification.
- Command-word rendering: argv materialization and interpolation runtime.
- Bash artifact assembly: helper selection and emitted script prologue.

Keeping these separate prevents signal work from becoming pipeline syntax work,
and prevents command-result capture from becoming the owner of process execution.

## Promising future extraction boundaries

- `vm_process_exec.c`: direct spawn, exec-error pipe, wait, redirection, and
  foreground process-group helpers.
- `vm.c`: already owns command-result/map field materialization;
  do not expand it into process capture or pipeline execution ownership.
- `vm_pipeline.c`: pipeline pipe setup, stage spawning, pipefail status, capture
  temp files, and pipeline signal classification.
- `vm_argv.c` or `vm_interpolation.c`: command-word materialization and runtime
  interpolation rendering for accepted HIR.
- `bash_process.c`: direct command/capture statement emission and command
  failure wrappers.
- `bash_pipeline.c`: plain/captured pipeline emission and temp-file capture
  assignment.
- `bash_handlers.c`: handler function emission, cleanup stacks, trap setup, and
  signal-aware foreground wrappers; it should consume `src/ds_signal.c` metadata
  rather than re-listing supported runtime statuses.

These are future cuts only. They should happen one boundary at a time, with
focused parity tests after each cut.

## Non-goals

- Do not redesign the process or Bash helper ABI during this trace.
- Do not expand signal support beyond `EXIT`, `INT`, and `TERM`.
- Do not add job-control APIs, async pipelines, or background process features.
- Do not move semantic command or handler validation into VM/Bash.
- Do not split broad runtime files in one pass; future cuts should choose one
  boundary at a time.
- Do not mark process, pipeline, or signal behavior `Clear`; the concept remains
  parity-sensitive and OS-sensitive.

## Current pressure summary

This concept is owned, but not low-risk. The highest-pressure coupling is the
intersection of pipeline execution, foreground process groups, signal cleanup,
and Bash helper emission. Future maintenance should preserve the current lowerer
ownership rules and cut backend mechanics along process, pipeline, argv, and
handler boundaries rather than doing one broad runtime cleanup.
