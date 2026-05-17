#!/usr/bin/env bash
set -euo pipefail

# ds: tests/v0_2/fixtures/if_with_else.ds:1
__ds_ok=false

# ds: tests/v0_2/fixtures/if_with_else.ds:2
if [[ "$__ds_ok" == true ]]; then
  # ds: tests/v0_2/fixtures/if_with_else.ds:3
  echo "yes"

else
  # ds: tests/v0_2/fixtures/if_with_else.ds:5
  echo "no"

fi

