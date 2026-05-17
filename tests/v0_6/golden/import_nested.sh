#!/usr/bin/env bash
set -euo pipefail

# ds: tests/v0_6/fixtures/imports_nested/shared/common.ds:1
__ds_common="COMMON"

# ds: tests/v0_6/fixtures/imports_nested/shared/b.ds:2
__ds_b="B"

# ds: tests/v0_6/fixtures/imports_nested/shared/b.ds:3
echo "b sees ${__ds_common}"

# ds: tests/v0_6/fixtures/imports_nested/shared/a.ds:2
__ds_a="A"

# ds: tests/v0_6/fixtures/imports_nested/shared/a.ds:3
echo "a sees ${__ds_b} and ${__ds_common}"

# ds: tests/v0_6/fixtures/imports_nested/main.ds:2
echo "root sees ${__ds_a} ${__ds_b} ${__ds_common}"

