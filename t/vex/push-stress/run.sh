#!/usr/bin/env bash

# Convenience entry point for direct harness development.  The repository-level
# script owns profile loading and invokes the Python runner.

set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
exec "$root/scripts/vex/push-stress.sh" "$@"
