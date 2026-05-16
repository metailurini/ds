# Product Principles

`ds` exists to make shell scripting easier, safer, and more readable than Bash while keeping the useful shell-native parts of Bash.

This document is the project constitution. When a proposed feature conflicts with this document, the feature should be delayed, redesigned, or rejected.

## Core purpose

`ds` is a scripting language for command-line automation.

It should be comfortable for people who already write shell scripts, but it should remove the parts of Bash that are hard to read, hard to debug, or easy to misuse.

## Principles

### 1. Commands should feel shell-native

Running commands should remain direct and familiar:

```ds
git status
npm install
cat access.log | grep "ERROR" | sort
```

`ds` should not force every command into function-call syntax.

Good:

```ds
npm run build
```

Bad:

```ds
run("npm", ["run", "build"])
```

The second form may exist internally, but normal scripts should stay shell-like.

### 2. Logic should feel like a normal programming language

Bash conditionals, loops, arrays, and functions are often difficult to read. `ds` should use clear language syntax:

```ds
if target == "production" {
  npm test
} else {
  echo "Skipping tests"
}
```

Avoid copying Bash forms like:

```bash
if [ "$target" = "production" ]; then
  npm test
fi
```

### 3. Every feature must run in the VM

The direct execution path is core:

```sh
ds ./script.ds
```

A feature is not complete unless the VM can execute it.

### 4. Every feature must emit to standalone Bash

The Bash emission path is also core:

```sh
ds emit bash ./script.ds -o ./script.sh
```

A feature is not complete unless it can be emitted to Bash.

Generated Bash must be standalone. A user should be able to copy the emitted `.sh` file to another machine and run it with Bash without installing `ds`.

### 5. Bash emission shapes language design

Because every feature must emit to Bash, the language should avoid features that are awkward or impossible to express in Bash.

Prefer:

- strings;
- integers;
- booleans;
- arrays;
- simple maps;
- functions;
- imports bundled into output;
- command execution;
- pipes;
- redirections;
- first-class CLI args;
- simple standard-library helpers.

Avoid early:

- classes;
- inheritance;
- async/await;
- threads;
- macros;
- advanced type systems;
- operator overloading;
- complicated pattern matching;
- features that require a large runtime.

### 6. CLI argument parsing is a first-class feature

One of the biggest reasons to create `ds` is to make script arguments pleasant.

Target syntax:

```ds
script {
  arg app: string
  option target: string = "staging"
  option retries: int = 3
  flag force: bool = false
}
```

This should work in both VM mode and emitted Bash mode.

The language should eventually generate useful `--help` output automatically.

### 7. Debuggability is a core feature

`ds` should be easy to debug while developing the language and while using the language.

The tool should eventually expose internal stages:

```sh
ds tokens file.ds
ds ast file.ds
ds hir file.ds
ds bytecode file.ds
```

Runtime debugging should include:

```sh
ds run --trace-cmd file.ds
ds run --trace-vm file.ds
```

Errors should include:

- file name;
- line number;
- column number;
- source span where possible;
- clear explanation;
- helpful next step when obvious.

### 8. Prefer boring syntax over clever syntax

`ds` should copy simple, proven ideas from other languages, but not their complicated features.

Good ideas to copy:

- `let name = "Danh"`
- `if condition { ... }`
- `for item in items { ... }`
- arrays like `["api", "web"]`
- object/map literals like `{ api: 3000 }`
- `defer { ... }` for cleanup

Avoid copying complex ideas early:

- generics;
- traits;
- lifetimes;
- prototypes;
- class hierarchies;
- metaprogramming.

### 9. Avoid Bash weirdness

`ds` should not require users to understand these Bash details:

- `2>&1`;
- `$?`;
- `IFS`;
- `[[ ... ]]` vs `[ ... ]`;
- word splitting;
- `${array[@]}`;
- `trap '...' EXIT`;
- `set -euo pipefail` edge cases.

When a Bash concept is useful, `ds` should expose a readable version.

Example:

```ds
npm run build &> "build.log"
```

instead of:

```bash
npm run build > build.log 2>&1
```

### 10. Tests are part of the feature

A feature is not complete until it has tests for:

- parser behavior;
- semantic behavior;
- VM behavior;
- Bash emission behavior;
- VM/Bash parity where applicable;
- edge cases;
- diagnostics.

Strict test-first development is not required, but each version must have a test plan before implementation.

### 11. Keep extension points explicit

The implementation should be easy to extend, but not magical.

New standard-library builtins should define:

- VM behavior;
- Bash emission behavior;
- diagnostics;
- tests;
- docs or examples.

If a builtin cannot be emitted to Bash cleanly, it should not enter the stable language surface.

### 12. Keep the C runtime boring and explicit

Because `ds` is implemented in C, the project needs internal runtime primitives such as strings, arrays, maps, values, diagnostics, process helpers, and eventually regex support.

These primitives should be:

- small;
- explicit;
- heavily tested;
- easy to debug in C;
- wrapped behind `ds` APIs;
- designed with clear ownership rules.

Supporting C projects may be included when useful, but their APIs should not leak through the whole codebase. The hashmap project is owned alongside `ds`; keeping it under `libs/hashmap/` is acceptable as a temporary staging step, but the long-term goal is to absorb the useful implementation into `src/core` behind `DsMap` so it becomes a normal part of the `ds` runtime.

Runtime-backed features must not become VM-only shortcuts. If a feature uses the C runtime in VM mode, it still needs standalone Bash emission behavior before it becomes part of the supported language.

## Non-goals

`ds` is not trying to be:

- a general-purpose systems language;
- a replacement for C, Go, Python, or JavaScript;
- a POSIX shell clone;
- a native compiler project;
- a package manager project;
- a complex application framework.

`ds` is a practical scripting language for command-line automation.

## Feature admission checklist

Before adding any feature, answer:

- Does it make scripting easier than Bash?
- Can the VM execute it simply?
- Can the Bash emitter emit it cleanly?
- Can generated Bash remain standalone?
- Does it avoid creating VM-only runtime behavior?
- Can users understand it without reading a long manual?
- Can it be tested thoroughly?
- Does it fit the current milestone spec?

If the answer is no, delay the feature.
