#!/usr/bin/env bash
set -euo pipefail

# ds: tests/v0_2/fixtures/nested_if.ds:1
__ds_ok=true

# ds: tests/v0_2/fixtures/nested_if.ds:2
__ds_ready=true

# ds: tests/v0_2/fixtures/nested_if.ds:3
if [[ "$__ds_ok" == true ]]; then
  # ds: tests/v0_2/fixtures/nested_if.ds:4
  if [[ "$__ds_ready" == true ]]; then
    # ds: tests/v0_2/fixtures/nested_if.ds:5
    echo "ready"

  fi

fi

