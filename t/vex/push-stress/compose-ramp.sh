#!/usr/bin/env bash

# Replay a 500-commit 1,2,4,...,32 ramp through the active local Vex
# MySQL+S3 smart-HTTP receiver.  Every client Git command below and in the
# Python runner resolves to /usr/bin/git, rather than the Git build under test.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
harness_dir="$root/t/vex/push-stress"
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/push-stress/compose-ramp.sh' >&2
	exit 2
fi
if ! command -v docker >/dev/null 2>&1; then
	printf '%s\n' 'vex push-stress: docker is required for Compose ramp' >&2
	exit 127
fi
if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git)
fi
[ -x "$stock_git" ] || {
	printf 'vex push-stress: stock Git executable is not available: %s\n' "$stock_git" >&2
	exit 127
}

# Do not let Git's test harness leak the source-built Vex client into this
# stock-client run.  A private global config is installed below for TLS/auth.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

set -a
# shellcheck disable=SC1090
. "$env_file"
set +a
ca_file="$docker_dir/certs/rootCA.pem"
[ -f "$ca_file" ] || {
	printf '%s\n' 'vex push-stress: local TLS certificate is missing; start Compose first' >&2
	exit 1
}
health=$(curl --fail --silent --show-error --cacert "$ca_file" \
	"https://localhost:${GIT_HTTPS_PORT}/healthz")
case "$health" in
*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) ;;
*)
	printf 'vex push-stress: endpoint is not an active Vex MySQL+S3 receiver: %s\n' "$health" >&2
	exit 1
	;;
esac

run_id="$(date +%s)-$$"
run_root="$harness_dir/work/compose-ramp-$run_id"
source_repo="$run_root/source"
out_dir="$run_root/out"
git_config="$run_root/stock-gitconfig"
repo_name="stress/push-ramp-$run_id.git"
origin="https://localhost:${GIT_HTTPS_PORT}/$repo_name"
mkdir -p "$run_root"

"$stock_git" init -q "$source_repo"
"$stock_git" -C "$source_repo" config user.name vex-push-stress
"$stock_git" -C "$source_repo" config user.email vex-push-stress@example.invalid
for step in $(seq 1 500); do
	printf 'Vex Compose ramp commit %04d\n' "$step" >>"$source_repo/history"
	"$stock_git" -C "$source_repo" add history
	"$stock_git" -C "$source_repo" -c commit.gpgSign=false commit -q \
		-m "Vex Compose ramp $step"
done
"$stock_git" -C "$source_repo" branch -M main

"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null
auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
"$stock_git" config --file "$git_config" http.sslCAInfo "$ca_file"
"$stock_git" config --file "$git_config" http.extraHeader "Authorization: Basic $auth"

export GIT_CONFIG_GLOBAL="$git_config"
export GIT_CONFIG_NOSYSTEM=1
export GIT_TERMINAL_PROMPT=0
export GIT_VEX_PUSH_STRESS_GIT="$stock_git"
export GIT_VEX_PUSH_STRESS_RECON_COMMAND="$harness_dir/compose-reconcile.sh"
set -a
# shellcheck disable=SC1091
. "$harness_dir/profiles/ramp.env"
set +a
python3 "$harness_dir/run.py" \
	--source "$source_repo" \
	--origin "$origin" \
	--branch main \
	--work-dir "$run_root" \
	--out-dir "$out_dir"

python3 - "$out_dir/summary.json" "$out_dir/run.json" "$out_dir/plan.jsonl" "$stock_git" <<'PY'
import json
import sys

summary_path, run_path, plan_path, stock_git = sys.argv[1:]
with open(summary_path, encoding="utf-8") as handle:
	summary = json.load(handle)
with open(run_path, encoding="utf-8") as handle:
	run = json.load(handle)
with open(plan_path, encoding="utf-8") as handle:
	plan = [json.loads(line) for line in handle if line.strip()]
expected = {
	"attempts": 20,
	"commits_planned": 500,
	"failures": 0,
	"recon_pass": True,
	"steps_completed": 20,
	"steps_planned": 20,
	"tip_match_final": True,
}
for key, value in expected.items():
	if summary.get(key) != value:
		raise SystemExit(
			"vex push-stress: summary {}={!r}, expected {!r}".format(
				key, summary.get(key), value
			)
		)
expected_batches = [1, 2, 4, 8, 16] + [32] * 14 + [21]
actual_batches = [item.get("batch") for item in plan]
if actual_batches != expected_batches:
	raise SystemExit(
		"vex push-stress: plan batches {!r}, expected {!r}".format(
			actual_batches, expected_batches
		)
	)
if sum(item.get("n_commits", 0) for item in plan) != 500:
	raise SystemExit("vex push-stress: plan does not contain 500 commits")
if run.get("git_executable") != stock_git:
	raise SystemExit(
		"vex push-stress: run used {!r}, expected {!r}".format(
			run.get("git_executable"), stock_git
		)
	)
PY

printf 'vex push-stress compose ramp: summary=%s\n' "$out_dir/summary.json"
