#!/usr/bin/env bash
set -euo pipefail

# ds: tests/v0_2/fixtures/interpolation.ds:1
__ds_first="Danh"

# ds: tests/v0_2/fixtures/interpolation.ds:2
__ds_last="Nguyen"

# ds: tests/v0_2/fixtures/interpolation.ds:3
echo "${__ds_first} ${__ds_last}!"

