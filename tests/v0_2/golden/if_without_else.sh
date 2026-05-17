#!/usr/bin/env bash
set -euo pipefail

# ds: tests/v0_2/fixtures/if_without_else.ds:1
__ds_ok=true

# ds: tests/v0_2/fixtures/if_without_else.ds:2
if [[ "$__ds_ok" == true ]]; then
  # ds: tests/v0_2/fixtures/if_without_else.ds:3
  echo "ok"

fi

