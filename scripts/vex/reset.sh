#!/usr/bin/env bash

# Reset is intentionally destructive only to named Vex Compose volumes, then
# recreates the small stock-Git fixture.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)

printf '%s\n' 'vex reset: removing only the Vex Compose volumes, then recreating the small fixture.'
"$repo_root/scripts/vex/down.sh" --volumes
"$repo_root/scripts/vex/up.sh" "$@"
"$repo_root/scripts/vex/seed-small.sh"
