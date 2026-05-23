# 1.0.0 Release Checklist

This checklist is the executable release boundary created by the `v0.24.0`
pre-1.0 hardening pass. It does not mark the project stable by itself; it lists
what must be signed off before tagging `1.0.0`.

## Required sign-offs

- [ ] **Supported language surface:** `docs/status.md`, `docs/language.ds`, and
  the examples agree on the production syntax supported through `v0.23.0` plus
  the `v0.24.0` hardening fixes.
- [ ] **VM/Bash parity:** every supported production feature runs through the VM
  and emits standalone Bash with matching stdout, stderr, exit status, and
  deterministic side effects.
- [ ] **Generated Bash standalone behavior:** emitted scripts pass `bash -n`, do
  not invoke `ds`, reserve helper names under `__ds_`, deduplicate helper
  definitions, and include Bash-version guards when Bash 4+ features are used.
- [ ] **Examples:** every non-intentionally-invalid file in `examples/` checks,
  runs, emits Bash, passes `bash -n`, and matches VM/emitted-Bash output where
  deterministic; `examples/bad.ds` remains a clear diagnostic fixture.
- [ ] **Docs:** README, status, architecture, runtime, roadmap, language catalog,
  editor notes, changelog, and milestone docs describe the same supported,
  test-only, deferred, rejected, and out-of-scope behavior.
- [ ] **Diagnostics:** malformed supported constructs and common unsupported
  constructs fail before execution or partial emission with source locations and
  messages that name the rejected feature where practical.
- [ ] **Formatter/checker:** formatter output is deterministic for the documented
  formatter surface, comment/trivia limits are explicit, and checker warning
  flags keep their documented behavior without executing user commands.
- [ ] **Sanitizer and ownership:** aggregate ASan/UBSan runs pass for the
  supported surface, or any remaining sanitizer finding is documented as outside
  supported behavior with a clear rejection path.
- [ ] **Deferred/rejected/out-of-scope features:** heredocs, here-strings,
  process substitution, background jobs, richer signal APIs, regex captures,
  runtime regex patterns, first-class ranges, map iteration, typed required
  parameters, nested collections, and alternate backends remain either clearly
  rejected or deliberately deferred.
- [ ] **Packaging and release notes:** release notes summarize the stable surface,
  accepted limitations, host assumptions, build/test commands, and known
  non-goals without promising unsupported syntax.

## Known limitations acceptable for 1.0.0 if unchanged

- Required function parameters are still untyped unless they have literal
  defaults that give the function body a conservative static kind.
- Expression-style value functions return scalar string/int/bool values only;
  collection and command-result returns remain deferred.
- Command-word interpolation does not directly evaluate function calls; bind the
  function-call result to a scalar value first.
- Formatter trivia preservation is deferred, so comment-bearing files are
  rejected by `ds fmt` instead of being rewritten unsafely.
- Signal cleanup is process-scoped and limited to `EXIT`, `INT`, and `TERM` for
  the documented foreground direct-command and simple-pipeline subset.
- Regex support is the conservative POSIX-ERE-shaped `matches` literal subset;
  captures and runtime patterns are deferred.
- Range syntax is a `for` loop source only, not a first-class value.
- Generated Bash targets Bash, with Bash 4+ guarded when associative arrays or
  other Bash-4-only behavior is required.
