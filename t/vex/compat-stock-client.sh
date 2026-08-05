#!/usr/bin/env bash

# Compatibility wrapper kept separate from dependency checks: it uses only an
# unmodified host git client against the HTTPS endpoint.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
"$root/scripts/vex/smoke-stock-git.sh" "$@"
