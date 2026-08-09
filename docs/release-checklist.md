# 1.0.0 Release Checklist

This checklist defines what must be signed off before tagging `1.0.0`. It does
not mark the project stable by itself.

## Required sign-offs

- [ ] **Supported language surface:** `docs/status.md`, `docs/language.ds`, and
  the examples agree on the production syntax supported through the current
  collection, environment/interpolation, glob, regex, and text-output DX
  stabilization surface.
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
  editor notes, and changelog agree on current supported, test-only, deferred,
  rejected, and out-of-scope behavior. Historical milestone docs remain records
  of the behavior and decisions at their milestone.
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
  process substitution, background jobs, richer signal APIs, first-class range
  values, typed required parameters, nested collections, advanced regex/glob
  features, and alternate backends remain either clearly rejected or
  deliberately deferred.
- [ ] **Packaging and release notes:** release notes summarize the stable surface,
  accepted limitations, host assumptions, build/test commands, and known
  non-goals without promising unsupported syntax.

## Known limitations acceptable for 1.0.0 if unchanged

- Required function parameters are still untyped unless they have literal
  defaults that give the function body a conservative static kind.
- Expression-style value functions may return scalar string/int/bool values,
  flat scalar arrays/maps, and command results through the documented structured
  return transport.
- Command-word interpolation supports accepted scalar value expressions,
  including direct scalar-returning function calls and flat index lookups.
- Formatter trivia preservation is deferred, so comment-bearing files are
  rejected by `ds fmt` instead of being rewritten unsafely.
- Signal cleanup is process-scoped and limited to `EXIT`, `INT`, and `TERM` for
  the documented foreground direct-command and simple-pipeline subset.
- Regex support is the conservative POSIX-ERE-shaped subset: `matches` accepts
  literals and runtime string patterns, while `regex.match` exposes flat capture
  maps with stable no-match entries and `regex.replace` performs global
  `$0`..`$9`/`$$` replacement. Regex split, named captures, lookaround,
  backreferences, replace-count APIs, and first-class regex values are deferred.
- Recursive `**` glob support is limited to one complete path segment per
  pattern, with sorted duplicate-free results, default hidden-path skipping, and
  no directory-symlink traversal. Multiple recursive segments, partial `**`,
  custom glob flags, shell brace/extglob expansion, and symlink-following
  traversal are deferred.
- Range syntax is a `for` loop source only, not a first-class value.
- Generated Bash targets Bash, with Bash 4+ guarded when associative arrays or
  other Bash-4-only behavior is required.
