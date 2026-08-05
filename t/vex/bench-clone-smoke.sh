#!/usr/bin/env bash

# Small opt-in smoke for the clone measurement harness.  It creates one Vex
# MySQL+S3 repository and a matching local bare baseline, then only checks
# that the benchmark emits sound measurements.  No latency threshold is an
# acceptance criterion here.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/bench-clone-smoke.sh' >&2
	exit 2
fi

# This script intentionally selects the host Git binary.  The git/git test
# harness exports build-local paths that must not become part of a stock-client
# HTTPS measurement.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

up_args=()
case "${VEX_COMPOSE_NO_BUILD:-}" in
1|true|yes) up_args+=(--no-build) ;;
esac
"$root/scripts/vex/up.sh" "${up_args[@]}" >/dev/null
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"
set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

ca_file="$docker_dir/certs/rootCA.pem"
health=$(curl --fail --silent --show-error --cacert "$ca_file" \
	"https://localhost:${GIT_HTTPS_PORT}/healthz")
case "$health" in
*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) ;;
*)
	printf 'vex clone benchmark smoke: endpoint is not active MySQL+S3: %s\n' "$health" >&2
	exit 1
	;;
esac

if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git)
fi
[ -x "$stock_git" ] || {
	printf 'vex clone benchmark smoke: stock Git executable is unavailable: %s\n' "$stock_git" >&2
	exit 127
}

run_id="$(date +%s)-$$"
repo_name="bench/clone-smoke-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-clone-bench-smoke.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

source_repo="$scratch/source"
baseline="$scratch/baseline.git"
git_config="$scratch/stock-gitconfig"
result="$scratch/clone-benchmark.json"
auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
"$stock_git" config --file "$git_config" http.sslCAInfo "$ca_file"
"$stock_git" config --file "$git_config" http.extraHeader "Authorization: Basic $auth"

"$stock_git" init -q "$source_repo"
"$stock_git" -C "$source_repo" config user.name vex-clone-benchmark
"$stock_git" -C "$source_repo" config user.email vex-clone-benchmark@example.invalid
for commit_number in $(seq 1 16); do
	printf 'Vex clone benchmark fixture %02d\n' "$commit_number" >>"$source_repo/history"
	"$stock_git" -C "$source_repo" add history
	"$stock_git" -C "$source_repo" -c commit.gpgSign=false commit -q \
		-m "Vex clone benchmark fixture $commit_number"
done
"$stock_git" -C "$source_repo" branch -M main
tip=$("$stock_git" -C "$source_repo" rev-parse HEAD)
"$stock_git" -C "$source_repo" remote add origin "$origin"
GIT_CONFIG_GLOBAL="$git_config" GIT_CONFIG_NOSYSTEM=1 \
	"$stock_git" -C "$source_repo" push -q -u origin HEAD:refs/heads/main
"$stock_git" clone --bare -q "$source_repo" "$baseline"
baseline_tip=$("$stock_git" -C "$baseline" rev-parse refs/heads/main)
[ "$baseline_tip" = "$tip" ] || {
	printf 'vex clone benchmark smoke: local baseline is %s, expected %s\n' \
		"$baseline_tip" "$tip" >&2
	exit 1
}

VEX_STOCK_GIT="$stock_git" "$root/scripts/vex/bench-clone.sh" \
	--origin "$origin" \
	--baseline "$baseline" \
	--branch main \
	--runs 3 \
	--output "$result" \
	--git-config "$git_config"

python3 - "$result" "$stock_git" "$tip" <<'PY'
import json
import math
import sys

result_path, stock_git, expected_tip = sys.argv[1:]
with open(result_path, encoding="utf-8") as handle:
    result = json.load(handle)

assert result["benchmark"] == "vex-stock-git-https-clone-vs-local-disk", result
assert result["measurement_only"] is True, result
assert result["l4_success_claim"] is False, result
assert result["stock_git"] == stock_git, result
assert result["runs"] == 3, result
assert result["ref"] == {
    "name": "refs/heads/main",
    "oid": expected_tip,
    "stable_across_measurement": True,
}, result
assert len(result["sample_order"]) == 3, result
for transport in ("vex_https", "local_disk"):
    values = result[transport]
    assert len(values["samples"]) == 3, values
    assert values["p50_ms"] > 0, values
    assert values["p90_ms"] > 0, values
    assert all(sample["elapsed_ms"] > 0 for sample in values["samples"]), values
for percentile in ("p50", "p90"):
    assert math.isfinite(result["ratio_vex_to_local_disk"][percentile]), result
    assert result["ratio_vex_to_local_disk"][percentile] > 0, result

print(json.dumps({
    "local_disk_p50_ms": result["local_disk"]["p50_ms"],
    "ratio_p50": result["ratio_vex_to_local_disk"]["p50"],
    "ratio_p90": result["ratio_vex_to_local_disk"]["p90"],
    "vex_https_p50_ms": result["vex_https"]["p50_ms"],
}, sort_keys=True))
PY

final_health=$(curl --fail --silent --show-error --cacert "$ca_file" \
	"https://localhost:${GIT_HTTPS_PORT}/healthz")
case "$final_health" in
*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) ;;
*)
	printf 'vex clone benchmark smoke: gateway unhealthy after measurement: %s\n' "$final_health" >&2
	exit 1
	;;
esac

printf 'vex clone benchmark smoke: result=%s (measurement only; no L4 threshold)\n' "$result"
