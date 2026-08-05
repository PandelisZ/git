#!/usr/bin/env bash

set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/git-push-stress-plan.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

source_repo="$scratch/source.git"
seed="$scratch/seed"
git init --bare -q "$source_repo"
git init -q "$seed"
git -C "$seed" config user.name push-stress-test
git -C "$seed" config user.email push-stress-test@example.invalid
for n in 1 2 3 4 5 6 7; do
	git -C "$seed" -c commit.gpgSign=false commit --allow-empty -q -m "fixture $n"
done
git -C "$seed" branch -M main
git -C "$seed" remote add origin "$source_repo"
git -C "$seed" push -q origin main
git -C "$source_repo" symbolic-ref HEAD refs/heads/main

"$root/scripts/vex/push-stress.sh" \
	--source "$source_repo" \
	--work-dir "$scratch/work-a" \
	--out-dir "$scratch/out-a" \
	--batch-mode random \
	--batch-min 1 \
	--batch-max 3 \
	--random-seed 71 \
	--max-commits 7 \
	--plan-only >/dev/null
"$root/scripts/vex/push-stress.sh" \
	--source "$source_repo" \
	--work-dir "$scratch/work-b" \
	--out-dir "$scratch/out-b" \
	--batch-mode random \
	--batch-min 1 \
	--batch-max 3 \
	--random-seed 71 \
	--max-commits 7 \
	--plan-only >/dev/null
cmp "$scratch/out-a/plan.jsonl" "$scratch/out-b/plan.jsonl"
printf '%s\n' 'ok: deterministic plan.jsonl'
