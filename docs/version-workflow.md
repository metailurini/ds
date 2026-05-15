# Version Workflow

This document defines how `ds` versions are planned, implemented, tested, and closed.

The goal is to keep development moving without drifting away from the original purpose.

## Version format

`ds` uses:

```txt
major.minor.patch
```

During pre-`1.0.0` development:

- `0.x.0` is a planned milestone.
- `0.x.y` is a patch for that milestone.
- `1.0.0` is the first stable release.

Patch versions should not introduce planned new features. They are for:

- bug fixes;
- docs fixes;
- test fixes;
- small compatibility fixes.

## Version waves

Development follows four-version waves:

```txt
0.x.0     Feature foundation
0.x+1.0   Feature expansion
0.x+2.0   Feature integration / usability
0.x+3.0   Cleanup / debt repayment
```

Example:

```txt
0.1.0  lexer/parser frontend
0.2.0  Bash emitter
0.3.0  VM
0.4.0  cleanup architecture
```

Cleanup versions should be treated as real planned versions. They are not optional.

## Required docs per planned version

Every `0.x.0` planned version requires:

```txt
docs/milestones/v0.x.0-spec.md
docs/test-plans/v0.x.0-test-plan.md
```

These documents are written before implementation.

The project should usually keep docs for the current version and at least one version ahead.

At project start, that means:

```txt
docs/milestones/v0.1.0-spec.md
docs/test-plans/v0.1.0-test-plan.md
docs/milestones/v0.2.0-spec.md
docs/test-plans/v0.2.0-test-plan.md
```

## Milestone spec contents

Each milestone spec should include:

- title;
- goal;
- motivation;
- non-goals;
- supported syntax, if syntax changes;
- CLI behavior, if commands change;
- VM behavior, if runtime behavior changes;
- Bash emission behavior, if supported syntax changes;
- diagnostics and debugging behavior;
- examples;
- acceptance criteria;
- deferred items;
- completion review section.

The spec should be detailed enough to prevent scope drift, but not so large that it blocks progress.

## Test plan contents

Each test plan should include relevant sections such as:

- lexer tests;
- parser tests;
- AST tests;
- semantic tests;
- HIR tests;
- bytecode tests;
- VM tests;
- Bash emitter tests;
- parity tests;
- diagnostics tests;
- CLI smoke tests;
- edge cases;
- manual smoke tests.

Not every version needs every section. Use the sections that match the milestone.

## Normal feature version workflow

For a normal feature version:

1. Write the milestone spec.
2. Write the test plan.
3. Implement syntax and parser support if needed.
4. Implement semantic checks if needed.
5. Implement HIR lowering if needed.
6. Implement VM behavior if needed.
7. Implement Bash emission behavior if needed.
8. Add or update examples.
9. Implement tests from the test plan.
10. Fix bugs exposed by tests.
11. Update docs if implementation changed the design.
12. Complete the version review.
13. Record deferred items and technical debt.

Strict test-first development is not required. The important rule is that the test plan exists before implementation and is followed before the version is closed.

## Cleanup version workflow

For a cleanup version:

1. Read technical debt notes from the previous feature versions.
2. Choose the most important cleanup items.
3. Write a cleanup milestone spec.
4. Write a regression-focused test plan.
5. Refactor internals.
6. Avoid user-facing behavior changes unless explicitly planned.
7. Improve diagnostics or documentation where useful.
8. Run full regression tests.
9. Run VM/Bash parity tests where applicable.
10. Complete the version review.

Cleanup versions should not become dumping grounds for random new features.

## Scope rule

If it is not in the milestone spec, it does not belong in the version.

If a new idea appears during implementation, choose one:

- add it to deferred items;
- amend the spec deliberately;
- move it to a future version;
- reject it.

Do not silently expand scope.

## Completion rule

A planned version is complete only when:

- the milestone acceptance criteria are satisfied;
- the test plan is implemented or explicitly amended;
- VM behavior and Bash emission behavior match where applicable;
- docs and examples are updated;
- deferred items are recorded;
- technical debt is recorded;
- the completion review is filled in.

## Feature admission rule

A feature should not enter the language unless it has:

- user-facing syntax or API;
- semantic behavior;
- VM behavior;
- Bash emission behavior;
- diagnostics;
- tests;
- docs or examples.

If a feature is VM-only or Bash-only, it should not be considered complete.

## Commit style

Suggested commit flow for a feature version:

```txt
docs: add v0.x.0 milestone spec and test plan
feat: implement v0.x.0 parser support
feat: implement v0.x.0 vm behavior
feat: implement v0.x.0 bash emission
test: add v0.x.0 coverage
docs: update examples for v0.x.0
chore: complete v0.x.0 review
```

Suggested commit flow for cleanup versions:

```txt
refactor: simplify command lowering
refactor: unify diagnostics
test: add parity regression coverage
docs: document architecture cleanup
chore: complete v0.x.0 cleanup review
```

## Git author

Project commits should use this author identity:

```txt
Shane Nguyễn <shanenoi.org@gmail.com>
```

## Branching

Simple strategy:

```txt
main = always working
feature/v0.x.0 = current version work
```

For early solo development, working directly on `main` is acceptable if commits remain clear and the tree stays working.

## Changelog

`CHANGELOG.md` should be updated for every planned version.

Before implementation, planned entries are acceptable.

After completion, entries should move from planned language to actual language:

```md
## v0.x.0

Added:
- ...

Changed:
- ...

Fixed:
- ...

Deferred:
- ...
```

## Completion review template

Each milestone spec should end with this section:

```md
## Completion Review

Status: Not started | In progress | Complete

### Implemented

- [ ] ...

### Tests

- [ ] ...

### Docs and examples

- [ ] ...

### VM/Bash parity

- [ ] Not applicable
- [ ] Verified

### Deferred

- ...

### Technical debt

- ...
```

## Anti-drift questions

Before closing any version, answer:

- Did we implement only the milestone scope?
- Does this still serve the goal of being better than Bash for scripting?
- Does every supported feature run in VM mode?
- Does every supported feature emit to standalone Bash?
- Are errors understandable?
- Are tests covering edge cases?
- Did docs or examples become outdated?
- Did we record debt for the next cleanup version?
