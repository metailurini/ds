#!/usr/bin/env bash
set -euo pipefail

# ds: tests/v0_7/fixtures/helpers/bash_combined_redirect.ds:1
sh -c "printf out; printf err >&2" > "all.txt" 2>&1

