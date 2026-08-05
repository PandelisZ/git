#!/usr/bin/env bash

# End-to-end proof that the harness talks to a classic bare Git target only
# through stock Git transport commands.  The fixture is deliberately small so
# it can run without a git/git mirror or network access.

set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/git-push-stress-classic.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

source_repo="$scratch/source.git"
target_repo="$scratch/target.git"
seed="$scratch/seed"
disk_clone="$scratch/disk-clone"
work_dir="$scratch/work"
out_dir="$scratch/out"

git init --bare -q "$source_repo"
git init --bare -q "$target_repo"
git init -q "$seed"
git -C "$seed" config user.name push-stress-test
git -C "$seed" config user.email push-stress-test@example.invalid
for n in $(seq 1 24); do
	git -C "$seed" -c commit.gpgSign=false commit --allow-empty -q -m "fixture commit $n"
done
git -C "$seed" branch -M main
git -C "$seed" remote add origin "$source_repo"
git -C "$seed" push -q origin main
git -C "$source_repo" symbolic-ref HEAD refs/heads/main

baseline_start=$(python3 -c 'import time; print(time.time_ns())')
git clone --no-checkout -q "$source_repo" "$disk_clone"
baseline_end=$(python3 -c 'import time; print(time.time_ns())')
baseline_ms=$(python3 -c 'import sys; print(round((int(sys.argv[2])-int(sys.argv[1]))/1000000, 3))' "$baseline_start" "$baseline_end")

"$root/scripts/vex/push-stress.sh" \
	--profile "$root/t/vex/push-stress/profiles/smoke.env" \
	--source "$source_repo" \
	--origin "$target_repo" \
	--work-dir "$work_dir" \
	--out-dir "$out_dir"

python3 -c '
import json
import pathlib
import sys
out = pathlib.Path(sys.argv[1])
summary = json.loads((out / "summary.json").read_text())
recon = json.loads((out / "recon.json").read_text())
assert summary["steps_planned"] == 20, summary
assert summary["steps_completed"] == 20, summary
assert summary["metrics_lines_with_push_exit"] >= 20, summary
assert summary["tip_match_final"], summary
assert recon["pass"], recon
print(json.dumps({"baseline_disk_clone_ms": float(sys.argv[2]), "recon_pass": recon["pass"], "steps": summary["steps_completed"]}, sort_keys=True))
' "$out_dir" "$baseline_ms"
