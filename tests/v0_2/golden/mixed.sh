#!/usr/bin/env bash
set -euo pipefail

# ds: tests/v0_2/fixtures/mixed.ds:1
__ds_name="Danh"

# ds: tests/v0_2/fixtures/mixed.ds:2
__ds_count=3

# ds: tests/v0_2/fixtures/mixed.ds:3
__ds_enabled=true

# ds: tests/v0_2/fixtures/mixed.ds:4
echo "Deploying ${__ds_name}"

# ds: tests/v0_2/fixtures/mixed.ds:5
if [[ "$__ds_name" == "Danh" ]]; then
  # ds: tests/v0_2/fixtures/mixed.ds:6
  echo "matched"

  # ds: tests/v0_2/fixtures/mixed.ds:7
  if ! [[ "$__ds_count" < 3 ]]; then
    # ds: tests/v0_2/fixtures/mixed.ds:8
    echo "$__ds_name"

  fi

else
  # ds: tests/v0_2/fixtures/mixed.ds:11
  echo "no"

fi

# ds: tests/v0_2/fixtures/mixed.ds:13
echo "done"

