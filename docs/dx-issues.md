# DX backlog

This file tracks current developer-experience problems found while writing real
`ds` scripts. Resolved milestone history belongs in `docs/milestones/` and Git
history, not in this backlog.

## Current priority

### Regex capture ergonomics

`matches` handles yes/no regex checks, and `regex.match(text, pattern[, flags])`
returns the current flat map of numbered captures. That is enough for existing
runtime behavior, but extraction-heavy scripts still need a smoother shape.

An eventual design might support a match object or named captures, for example:

```ds
# Illustrative only. This syntax is not implemented.
let m = sig.match(/([A-Za-z_][A-Za-z0-9_]*)\s*\()/
let name = m.group(1)
```

Any expansion must preserve VM/Bash parity and the portable regex subset. It
should not quietly introduce backend-specific regex behavior.

## Broader future DX work

These are roadmap topics rather than active blockers:

- deeper Unicode-aware string behavior;
- richer structured values beyond the lightweight row subset;
- metadata-oriented filesystem APIs;
- streaming iterators for large data sets.

Completed DX work is preserved in the matching milestone specs, test plans, and
Git history.
