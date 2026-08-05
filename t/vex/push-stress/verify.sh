#!/usr/bin/env bash

# Re-run only fresh stock-Git fetch/fsck reconciliation for an existing plan.

set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
exec "$root/scripts/vex/push-stress.sh" --verify-only "$@"
