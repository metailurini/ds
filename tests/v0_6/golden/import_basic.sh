#!/usr/bin/env bash
set -euo pipefail

# ds: tests/v0_6/fixtures/imports_basic/lib.ds:1
__ds_app="api"

# ds: tests/v0_6/fixtures/imports_basic/constants.ds:1
__ds_target="production"

# ds: tests/v0_6/fixtures/imports_basic/constants.ds:2
__ds_retries=3

# ds: tests/v0_6/fixtures/imports_basic/constants.ds:3
__ds_enabled=true

# ds: tests/v0_6/fixtures/imports_basic/main.ds:3
echo "Deploying ${__ds_app} to ${__ds_target}"

# ds: tests/v0_6/fixtures/imports_basic/main.ds:4
echo "$__ds_retries"

# ds: tests/v0_6/fixtures/imports_basic/main.ds:5
if [[ "$__ds_enabled" == true ]]; then
  # ds: tests/v0_6/fixtures/imports_basic/main.ds:6
  echo "enabled"

fi

