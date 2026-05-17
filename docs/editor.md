# Editor and LSP setup

This project is plain C built by `make`, so editors that use `clangd` need the
same compiler flags as the build.

`compile_flags.txt` is checked in for this purpose. `clangd` reads it from the
project root and applies the flags when there is no `compile_commands.json`.
This keeps editor diagnostics aligned with the current Makefile build and lets
`clangd` resolve project headers such as `include/ds.h` without falling back to
guesswork.

The expected project-side `clangd` flags are:

```txt
-std=c99
-Wall
-Wextra
-Wpedantic
-Iinclude
-Ilibs/hashmap
```

If the editor was already open before this file existed, restart the LSP client
or reopen the workspace so `clangd` reloads the root configuration.

## Notes on Neovim `lua_ls`

The repository does not use Lua. A Neovim message like this is an editor/tooling
setup problem rather than a `ds` project problem:

```txt
cannot start lua_ls ... Info: lua-language-server is not executable
```

Fix it outside this repository by either installing a working `lua-language-server`
binary or disabling `lua_ls` for this workspace in the Neovim LSP configuration.

## Notes on clangd logging

Some `clangd` messages are informational even when Neovim records them under an
`ERROR` log level. After `compile_flags.txt` is loaded, the important project-side
messages to avoid are missing build flags, fallback compile commands, and failure
to resolve `#include "ds.h"`.