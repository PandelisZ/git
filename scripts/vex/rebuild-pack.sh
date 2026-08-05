#!/usr/bin/env bash

# Process one due, debounced Vex derived-pack rebuild for an operator or
# scheduler.  Use build-pack.sh directly when an immediate rebuild is wanted.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$script_dir/build-pack.sh" --due "$@"
