#!/usr/bin/env bash

# Compose L3 destructive-sweep gate.  It proves the complete conservative
# protocol: a pin rescues a disconnected graph, the first pass creates only
# durable tombstones, and an injected final-S3-delete failure leaves metadata
# deleted with retryable blob cleanup before the next pass finishes it.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

# Git's test harness exports build-local paths. Keep the client a stock binary.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-sweep.sh' >&2
	exit 2
fi

up_args=()
case "${VEX_COMPOSE_NO_BUILD:-}" in
1|true|yes) up_args+=(--no-build) ;;
esac
if [ "${#up_args[@]}" -eq 0 ]; then
	"$root/scripts/vex/up.sh" >/dev/null
else
	"$root/scripts/vex/up.sh" "${up_args[@]}" >/dev/null
fi
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"
set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git)
fi
[ -x "$stock_git" ] || {
	printf 'vex sweep E2E: stock Git executable is unavailable: %s\n' "$stock_git" >&2
	exit 127
}

compose=(docker compose -f "$docker_dir/compose.yaml" --env-file "$env_file")
ca_file="$docker_dir/certs/rootCA.pem"

wait_for_storage() {
	local attempt health

	for attempt in $(seq 1 60); do
		if health=$(curl --fail --silent --show-error --cacert "$ca_file" \
			"https://localhost:${GIT_HTTPS_PORT}/healthz" 2>/dev/null) &&
			case "$health" in
			*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) return 0 ;;
			esac
		then
			:
		fi
		sleep 1
	done
	printf '%s\n' 'vex sweep E2E: gateway did not become MySQL+S3 ready' >&2
	return 1
}

mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" \
		-p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" -e "$1"
}

minio_run() {
	"${compose[@]}" run --rm --no-deps --entrypoint /bin/sh "$@"
}

assert_minio_object_exists() {
	minio_run -e "VEX_S3_OBJECT_KEY=$1" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc stat "vex/$MINIO_BUCKET/$VEX_S3_OBJECT_KEY" >/dev/null
	'
}

assert_minio_object_missing() {
	minio_run -e "VEX_S3_OBJECT_KEY=$1" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		if mc stat "vex/$MINIO_BUCKET/$VEX_S3_OBJECT_KEY" >/dev/null 2>&1
		then
			printf "unexpected MinIO object: %s\\n" "$VEX_S3_OBJECT_KEY" >&2
			exit 1
		fi
	'
}

sweep() {
	"${compose[@]}" exec -T \
		-e GIT_VEX_TEST_ALLOW_SHORT_SWEEP_GRACE=1 \
		git-http /opt/vex-git/vex-sweep-plan \
		--git-dir="/srv/git/$repo_name" "$@"
}

sweep_with_final_delete_fault() {
	"${compose[@]}" exec -T \
		-e GIT_VEX_TEST_ALLOW_SHORT_SWEEP_GRACE=1 \
		-e GIT_VEX_TEST_S3_FAIL_FINAL_DELETE=1 \
		git-http /opt/vex-git/vex-sweep-plan \
		--git-dir="/srv/git/$repo_name" "$@"
}

sweep_field() {
	local name=$1
	local output=$2
	local value

	value=$(printf '%s\n' "$output" |
		sed -n "s/.*${name}=\\([0-9][0-9]*\\).*/\\1/p" | tail -1)
	case "$value" in
	''|*[!0-9]*)
		printf 'vex sweep E2E: invalid %s in sweep output: %s\n' \
			"$name" "$output" >&2
		return 1
		;;
	esac
	printf '%s\n' "$value"
}

pad_oid() {
	local oid=$1

	while [ "${#oid}" -lt 64 ]; do
		oid="${oid}0"
	done
	printf '%s\n' "$oid"
}

wait_for_storage

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-sweep.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

run_id="$(date +%s)-$$"
repo_name="e2e/sweep-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

client="$scratch/client"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-sweep-e2e
"$stock_git" -C "$client" config user.email vex-sweep-e2e@example.invalid
printf 'old Vex graph that becomes unreachable\n' >"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'old sweep tip'
"$stock_git" -C "$client" branch -M main
old_tip=$("$stock_git" -C "$client" rev-parse HEAD)
"$stock_git" -C "$client" remote add origin "$origin"
git_https -C "$client" push -q -u origin HEAD:refs/heads/main

"$stock_git" -C "$client" checkout -q --orphan replacement
"$stock_git" -C "$client" rm -q -rf .
printf 'current Vex graph remains readable\n' >"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'replacement sweep tip'
"$stock_git" -C "$client" branch -M main
replacement_tip=$("$stock_git" -C "$client" rev-parse HEAD)
git_https -C "$client" push -q --force origin HEAD:refs/heads/main

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????) repo_id=${repository_uri#vex://} ;;
*)
	printf 'vex sweep E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
	exit 1
	;;
esac
old_padded=$(pad_oid "$old_tip")
old_key="git/v1/$repo_id/objects/$old_tip"
old_metadata=$(mysql_query "
	SELECT COUNT(*) FROM objects
	 WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$old_padded');")
[ "$old_metadata" = 1 ] || {
	printf 'vex sweep E2E: old tip metadata is not present before sweeping: %s\n' \
		"$old_metadata" >&2
	exit 1
}
assert_minio_object_exists "$old_key"

# A durable pin must protect the old graph even after the grace timer expires.
pin_reason=retain-old-sweep-tip
pin_output=$(sweep --pin="${old_tip}:${pin_reason}")
case "$pin_output" in
*'action=added'*"oid=$old_tip"*"reason=$pin_reason"*) ;;
*)
	printf 'vex sweep E2E: unexpected pin output: %s\n' "$pin_output" >&2
	exit 1
	;;
esac
sleep 2
pinned_output=$(sweep --sweep --grace-seconds=1 --tombstone-seconds=1)
pinned_marked=$(sweep_field tombstones_marked "$pinned_output")
pinned_deleted=$(sweep_field metadata_deleted "$pinned_output")
[ "$pinned_marked" = 0 ] && [ "$pinned_deleted" = 0 ] || {
	printf 'vex sweep E2E: live pin did not protect the old graph: %s\n' \
		"$pinned_output" >&2
	exit 1
}

unpin_output=$(sweep --unpin="${old_tip}:${pin_reason}")
case "$unpin_output" in
*'action=removed'*"oid=$old_tip"*"reason=$pin_reason"*) ;;
*)
	printf 'vex sweep E2E: unexpected unpin output: %s\n' "$unpin_output" >&2
	exit 1
	;;
esac

# Pass 1 records candidate rows only; the bytes and metadata remain usable.
marked_output=$(sweep --sweep --grace-seconds=1 --tombstone-seconds=1)
grace_candidates=$(sweep_field grace_candidates "$marked_output")
tombstones_marked=$(sweep_field tombstones_marked "$marked_output")
metadata_deleted=$(sweep_field metadata_deleted "$marked_output")
[ "$grace_candidates" -gt 0 ] && [ "$tombstones_marked" -gt 0 ] && \
	[ "$metadata_deleted" = 0 ] || {
	printf 'vex sweep E2E: first sweep did not leave a durable grace tombstone: %s\n' \
		"$marked_output" >&2
	exit 1
}
old_metadata=$(mysql_query "
	SELECT COUNT(*) FROM objects
	 WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$old_padded');")
[ "$old_metadata" = 1 ] || {
	printf '%s\n' 'vex sweep E2E: first pass deleted old metadata before tombstone delay' >&2
	exit 1
}
assert_minio_object_exists "$old_key"

fresh_before_fault="$scratch/fresh-before-fault"
git_https clone -q "$origin" "$fresh_before_fault"
[ "$("$stock_git" -C "$fresh_before_fault" rev-parse HEAD)" = "$replacement_tip" ] || {
	printf '%s\n' 'vex sweep E2E: current clone did not reach replacement tip during tombstone delay' >&2
	exit 1
}
"$stock_git" -C "$fresh_before_fault" fsck --no-progress >/dev/null

# Pass 2 performs metadata first. The injected S3 failure must leave a
# retryable tombstone instead of restoring a metadata pointer to old bytes.
sleep 2
set +e
fault_output=$(sweep_with_final_delete_fault --sweep --grace-seconds=1 \
	--tombstone-seconds=1 2>&1)
fault_status=$?
set -e
[ "$fault_status" -ne 0 ] || {
	printf 'vex sweep E2E: final-S3-delete fault unexpectedly succeeded: %s\n' \
		"$fault_output" >&2
	exit 1
}
fault_deleted=$(sweep_field metadata_deleted "$fault_output")
fault_pending=$(sweep_field blobs_pending "$fault_output")
[ "$fault_deleted" -gt 0 ] && [ "$fault_pending" -gt 0 ] || {
	printf 'vex sweep E2E: failed final delete did not report metadata-first retry state: %s\n' \
		"$fault_output" >&2
	exit 1
}
old_metadata=$(mysql_query "
	SELECT COUNT(*) FROM objects
	 WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$old_padded');")
pending_tombstone=$(mysql_query "
	SELECT COUNT(*) FROM object_sweep_tombstones
	 WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$old_padded')
	   AND metadata_deleted_at IS NOT NULL AND blob_deleted_at IS NULL;")
[ "$old_metadata" = 0 ] && [ "$pending_tombstone" = 1 ] || {
	printf 'vex sweep E2E: metadata/S3 failure ordering was not durable (metadata=%s pending=%s)\n' \
		"$old_metadata" "$pending_tombstone" >&2
	exit 1
}
assert_minio_object_exists "$old_key"

fresh_after_fault="$scratch/fresh-after-fault"
git_https clone -q "$origin" "$fresh_after_fault"
[ "$("$stock_git" -C "$fresh_after_fault" rev-parse HEAD)" = "$replacement_tip" ] || {
	printf '%s\n' 'vex sweep E2E: failed old-byte cleanup broke current clone' >&2
	exit 1
}
"$stock_git" -C "$fresh_after_fault" fsck --no-progress >/dev/null

# The normal retry closes every pending final delete and records completion.
retry_output=$(sweep --sweep --grace-seconds=1 --tombstone-seconds=1)
retry_deleted=$(sweep_field blobs_deleted "$retry_output")
retry_pending=$(sweep_field blobs_pending "$retry_output")
[ "$retry_deleted" -gt 0 ] && [ "$retry_pending" = 0 ] || {
	printf 'vex sweep E2E: normal retry did not finish pending S3 deletes: %s\n' \
		"$retry_output" >&2
	exit 1
}
completed_tombstone=$(mysql_query "
	SELECT COUNT(*) FROM object_sweep_tombstones
	 WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$old_padded')
	   AND metadata_deleted_at IS NOT NULL AND blob_deleted_at IS NOT NULL;")
[ "$completed_tombstone" = 1 ] || {
	printf 'vex sweep E2E: successful retry did not durably mark old blob deletion: %s\n' \
		"$completed_tombstone" >&2
	exit 1
}
assert_minio_object_missing "$old_key"

fresh_after_retry="$scratch/fresh-after-retry"
git_https clone -q "$origin" "$fresh_after_retry"
[ "$("$stock_git" -C "$fresh_after_retry" rev-parse HEAD)" = "$replacement_tip" ] || {
	printf '%s\n' 'vex sweep E2E: completed old-byte cleanup broke current clone' >&2
	exit 1
}
"$stock_git" -C "$fresh_after_retry" fsck --no-progress >/dev/null

printf 'vex sweep E2E: PASS grace_candidates=%s tombstones=%s metadata-first=%s retry_deleted=%s for %s\n' \
	"$grace_candidates" "$tombstones_marked" "$fault_deleted" "$retry_deleted" "$origin"
