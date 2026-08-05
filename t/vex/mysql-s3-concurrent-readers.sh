#!/usr/bin/env bash

# Bounded data-plane concurrency regression.  Eight stock Git HTTPS readers
# clone and repeatedly fetch while one stock Git writer advances main through
# twenty commits on a unique Vex MySQL-metadata + S3-byte repository.  This is
# deliberately a small correctness gate, not the L4 50-client acceptance run.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

# Keep the client half of this test genuinely stock Git.  Git's own test
# harness otherwise exports build-local paths that can make /usr/bin/git load
# the source-tree helpers.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-concurrent-readers.sh' >&2
	exit 2
fi

up_args=()
case "${VEX_COMPOSE_NO_BUILD:-}" in
1|true|yes) up_args+=(--no-build) ;;
esac
"$root/scripts/vex/up.sh" "${up_args[@]}"
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
	printf 'vex mysql-s3 concurrent readers: endpoint did not report active MySQL+S3 storage: %s\n' "$health" >&2
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
	printf 'vex mysql-s3 concurrent readers: stock Git executable is not available: %s\n' "$stock_git" >&2
	exit 127
}

# Fixed, intentionally modest pressure.  These are assertions about the gate,
# not environment knobs: weakening either number would change its contract.
reader_count=8
writer_push_count=20
reader_min_fetches=3
reader_max_fetches=512

run_id="$(date +%s)-$$"
repo_name="e2e/concurrent-readers-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-concurrent-readers.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

writer="$scratch/writer"
git_https init -q "$writer"
"$stock_git" -C "$writer" config user.name vex-mysql-s3-concurrent-readers
"$stock_git" -C "$writer" config user.email vex-mysql-s3-concurrent-readers@example.invalid
printf 'Vex MySQL+S3 concurrent-reader base\n' >"$writer/history"
"$stock_git" -C "$writer" add history
"$stock_git" -C "$writer" -c commit.gpgSign=false commit -q -m 'Vex MySQL+S3 concurrent-reader base'
"$stock_git" -C "$writer" branch -M main
"$stock_git" -C "$writer" remote add origin "$origin"
git_https -C "$writer" push -q -u origin HEAD:refs/heads/main

writer_commits=()
for ((step = 1; step <= writer_push_count; step++)); do
	printf 'Vex MySQL+S3 concurrent-reader writer commit %02d\n' "$step" >>"$writer/history"
	"$stock_git" -C "$writer" add history
	"$stock_git" -C "$writer" -c commit.gpgSign=false commit -q \
		-m "Vex MySQL+S3 concurrent-reader writer $step"
	writer_commits+=("$("$stock_git" -C "$writer" rev-parse HEAD)")
done
final_tip=${writer_commits[$((writer_push_count - 1))]}

start_gate="$scratch/start"
writer_started="$scratch/writer-started"
writer_result="$scratch/writer-result"

publish_writer_result() {
	local status=$1
	local pushed=$2
	printf '%s %s\n' "$status" "$pushed" >"$writer_result.tmp"
	mv "$writer_result.tmp" "$writer_result"
}

writer_worker() {
	local commit
	local pushed=0

	while [ ! -e "$start_gate" ]; do sleep 0.01; done
	: >"$writer_started"
	for commit in "${writer_commits[@]}"; do
		if ! git_https -C "$writer" push -q origin "$commit:refs/heads/main"; then
			publish_writer_result failed "$pushed"
			printf 'writer failed after %s successful pushes\n' "$pushed" >&2
			return 1
		fi
		pushed=$((pushed + 1))
		# A short yield makes the intended overlap deterministic without turning
		# this correctness test into a throughput benchmark.
		sleep 0.05
	done
	publish_writer_result success "$pushed"
	printf 'writer completed %s pushes\n' "$pushed"
}

reader_worker() {
	local reader_id=$1
	local reader="$scratch/reader-$reader_id"
	local fetches=0
	local result
	local observed_tip

	while [ ! -e "$start_gate" ]; do sleep 0.01; done
	# Start cloning only after the writer has entered its push loop, rather than
	# completing all clones before any write is possible.
	while [ ! -e "$writer_started" ]; do sleep 0.01; done
	if ! git_https clone -q "$origin" "$reader"; then
		printf 'reader %s clone failed\n' "$reader_id" >&2
		return 1
	fi

	# Continue fetching until the writer has a result, and always perform at
	# least three fetches so each reader exercises repeated reads.  The cap
	# prevents a stuck writer from leaving the opt-in test unbounded.
	while [ "$fetches" -lt "$reader_min_fetches" ] || [ ! -f "$writer_result" ]; do
		if [ "$fetches" -ge "$reader_max_fetches" ]; then
			printf 'reader %s reached fetch cap (%s) before writer completion\n' \
				"$reader_id" "$reader_max_fetches" >&2
			return 1
		fi
		if ! git_https -C "$reader" fetch -q origin \
			'+refs/heads/main:refs/remotes/origin/main'; then
			printf 'reader %s fetch %s failed\n' "$reader_id" "$((fetches + 1))" >&2
			return 1
		fi
		fetches=$((fetches + 1))
		sleep 0.05
	done

	result=$(<"$writer_result")
	if [ "$result" != "success $writer_push_count" ]; then
		printf 'reader %s observed unsuccessful writer result: %s\n' "$reader_id" "$result" >&2
		return 1
	fi
	# Re-read after the final published result.  This is the reader's final-tip
	# agreement check, independent of the writer's local repository.
	if ! git_https -C "$reader" fetch -q origin \
		'+refs/heads/main:refs/remotes/origin/main'; then
		printf 'reader %s final fetch failed\n' "$reader_id" >&2
		return 1
	fi
	fetches=$((fetches + 1))
	observed_tip=$("$stock_git" -C "$reader" rev-parse refs/remotes/origin/main)
	if [ "$observed_tip" != "$final_tip" ]; then
		printf 'reader %s final tip is %s, expected %s\n' "$reader_id" "$observed_tip" "$final_tip" >&2
		return 1
	fi
	if ! "$stock_git" -C "$reader" fsck --no-progress >/dev/null; then
		printf 'reader %s fsck failed\n' "$reader_id" >&2
		return 1
	fi
	printf '%s\n' "$fetches" >"$scratch/reader-$reader_id.fetches"
	printf 'reader %s clone=1 fetches=%s fsck=pass tip=%s\n' \
		"$reader_id" "$fetches" "$observed_tip"
}

reader_pids=()
for ((reader_id = 1; reader_id <= reader_count; reader_id++)); do
	reader_worker "$reader_id" >"$scratch/reader-$reader_id.log" 2>&1 &
	reader_pids+=("$!")
done
writer_worker >"$scratch/writer.log" 2>&1 &
writer_pid=$!
: >"$start_gate"

writer_failed=0
if ! wait "$writer_pid"; then
	writer_failed=1
fi
reader_failures=0
for ((reader_id = 1; reader_id <= reader_count; reader_id++)); do
	if ! wait "${reader_pids[$((reader_id - 1))]}"; then
		reader_failures=$((reader_failures + 1))
	fi
done
if [ "$writer_failed" -ne 0 ] || [ "$reader_failures" -ne 0 ]; then
	printf 'vex mysql-s3 concurrent readers: writer_failed=%s reader_failures=%s\n' \
		"$writer_failed" "$reader_failures" >&2
	for log in "$scratch/writer.log" "$scratch"/reader-*.log; do
		printf '%s\n' "--- $log" >&2
		sed 's/^/  /' "$log" >&2 || true
	done
	exit 1
fi

[ "$(<"$writer_result")" = "success $writer_push_count" ] || {
	printf 'vex mysql-s3 concurrent readers: unexpected writer result: %s\n' "$(<"$writer_result")" >&2
	exit 1
}
total_fetches=0
for ((reader_id = 1; reader_id <= reader_count; reader_id++)); do
	fetches=$(<"$scratch/reader-$reader_id.fetches")
	case "$fetches" in
	''|*[!0-9]*)
		printf 'vex mysql-s3 concurrent readers: invalid reader %s fetch count: %s\n' \
			"$reader_id" "$fetches" >&2
		exit 1
		;;
	esac
	[ "$fetches" -ge "$reader_min_fetches" ] || {
		printf 'vex mysql-s3 concurrent readers: reader %s made only %s fetches\n' \
			"$reader_id" "$fetches" >&2
		exit 1
	}
	total_fetches=$((total_fetches + fetches))
done

advertised_tip=$(git_https ls-remote "$origin" refs/heads/main | awk 'NR == 1 { print $1 }')
[ "$advertised_tip" = "$final_tip" ] || {
	printf 'vex mysql-s3 concurrent readers: advertised main is %s, expected %s\n' \
		"$advertised_tip" "$final_tip" >&2
	exit 1
}

# This shared reconciliation checks the MySQL ref/object/sealed-generation
# claim and the canonical S3 blob key for the final advertised tip.
reconcile_json="$scratch/reconcile.json"
GIT_VEX_PUSH_STRESS_ORIGIN="$origin" \
GIT_VEX_PUSH_STRESS_BRANCH=main \
GIT_VEX_PUSH_STRESS_EXPECTED_TIP="$final_tip" \
	"$root/t/vex/push-stress/compose-reconcile.sh" >"$reconcile_json"

final_health=$(curl --fail --silent --show-error --cacert "$ca_file" \
	"https://localhost:${GIT_HTTPS_PORT}/healthz")
case "$final_health" in
*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) ;;
*)
	printf 'vex mysql-s3 concurrent readers: gateway was unhealthy after the run: %s\n' "$final_health" >&2
	exit 1
	;;
esac

printf 'vex mysql-s3 concurrent readers: PASS stock-Git HTTPS writer_pushes=%s readers=%s clones=%s fetches=%s reader_fscks=%s final_tip=%s; MySQL/S3 reconciliation passed\n' \
	"$writer_push_count" "$reader_count" "$reader_count" "$total_fetches" "$reader_count" "$final_tip"
python3 - "$writer_push_count" "$reader_count" "$total_fetches" "$final_tip" "$reconcile_json" <<'PY'
import json
import sys

writer_pushes, readers, fetches, final_tip, reconcile_path = sys.argv[1:]
with open(reconcile_path, encoding="utf-8") as handle:
    reconcile = json.load(handle)
print(json.dumps({
    "clones": int(readers),
    "final_tip": final_tip,
    "mysql_s3_reconciliation": reconcile,
    "reader_fscks": int(readers),
    "readers": int(readers),
    "total_fetches": int(fetches),
    "writer_pushes": int(writer_pushes),
}, sort_keys=True))
PY
