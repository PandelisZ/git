#!/usr/bin/env bash

# Compose E2E for the standalone Vex pack worker. It exercises the normal
# per-object receive path, where the worker may construct an optional derived
# pack without becoming the authority for refs, generations, or object bytes.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

# Do not let Git's test harness make the stock client use this source tree.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-pack-worker.sh' >&2
	exit 2
fi

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

compose=(docker compose -f "$docker_dir/compose.yaml" --env-file "$env_file")
case "${VEX_COMPOSE_NO_BUILD:-}" in
1|true|yes) ;;
*) "${compose[@]}" --profile pack-worker build pack-worker >/dev/null ;;
esac

mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" \
		-p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" -e "$1"
}

ca_file="$docker_dir/certs/rootCA.pem"
wait_for_storage() {
	local attempt health

	for attempt in $(seq 1 60); do
		if health=$(curl --fail --silent --show-error --cacert "$ca_file" \
			"https://localhost:${GIT_HTTPS_PORT}/healthz" 2>/dev/null) &&
			case "$health" in
			*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*)
				return 0
				;;
			esac
		then
			:
		fi
		sleep 1
	done
	printf '%s\n' 'vex pack-worker E2E: gateway did not become MySQL+S3 ready' >&2
	return 1
}

wait_for_storage

if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git)
fi
[ -x "$stock_git" ] || {
	printf 'vex pack-worker E2E: stock Git executable is unavailable: %s\n' "$stock_git" >&2
	exit 127
}

fault_gateway_active=0
restore_gateway() {
	if [ "$fault_gateway_active" -eq 0 ]; then
		return 0
	fi
	env GIT_VEX_TEST_FAIL_PACK_OBJECTS= \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
	wait_for_storage
	fault_gateway_active=0
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-pack-worker.XXXXXX")
cleanup() {
	local status=$?

	trap - EXIT HUP INT TERM
	set +e
	restore_gateway
	rm -rf -- "$scratch"
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

run_id="$(date +%s)-$$"
repo_name="e2e/pack-worker-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

client="$scratch/client"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-pack-worker-e2e
"$stock_git" -C "$client" config user.email vex-pack-worker-e2e@example.invalid
for commit_number in 1 2 3; do
	printf 'Vex pack worker fixture %s\n' "$commit_number" >>"$client/history"
	"$stock_git" -C "$client" add history
	"$stock_git" -C "$client" -c commit.gpgSign=false commit -q \
		-m "Vex pack worker fixture $commit_number"
done
# Force the worker through a genuine multipart S3 transfer below. Random bytes
# keep Git from compressing the fixture below the 5 MiB S3 part threshold.
multipart_part_bytes=$((5 * 1024 * 1024))
dd if=/dev/urandom of="$client/multipart.bin" bs=1m count=12 2>/dev/null
"$stock_git" -C "$client" add multipart.bin
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q \
	-m 'Vex pack worker multipart fixture'
"$stock_git" -C "$client" branch -M main
tip=$("$stock_git" -C "$client" rev-parse HEAD)
"$stock_git" -C "$client" remote add origin "$origin"
git_https -C "$client" push -q -u origin HEAD:refs/heads/main

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????) repo_id=${repository_uri#vex://} ;;
*)
	printf 'vex pack-worker E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
	exit 1
	;;
esac
persisted_name=$(mysql_query "
	SELECT name FROM repositories WHERE repo_id = UNHEX('$repo_id');")
[ "$persisted_name" = "$repo_name" ] || {
	printf 'vex pack-worker E2E: repository route was not persisted (%s)\n' \
		"$persisted_name" >&2
	exit 1
}

request_before=$(mysql_query "
	SELECT CONCAT(target_generation, '|',
		TIMESTAMPDIFF(MICROSECOND, requested_at, not_before), '|',
		IF(worker_lease_id IS NULL, 'unclaimed', 'claimed'))
	  FROM pack_rebuild_requests WHERE repo_id = UNHEX('$repo_id');")
sealed_generation=$(mysql_query "
	SELECT COALESCE(MAX(generation), 0) FROM generations
	 WHERE repo_id = UNHEX('$repo_id') AND state = 'sealed';")
IFS='|' read -r request_generation request_delay request_lease_state <<EOF
$request_before
EOF
case "$request_delay" in
''|*[!0-9]*|0)
	printf 'vex pack-worker E2E: receive did not leave one debounced unclaimed request: %s\n' \
		"$request_before" >&2
	exit 1
	;;
esac
[ "$request_generation" = "$sealed_generation" ] &&
	[ "$request_lease_state" = unclaimed ] || {
	printf 'vex pack-worker E2E: receive did not leave one debounced unclaimed request: %s\n' \
		"$request_before" >&2
	exit 1
}

# The worker does not own these C-side records. Capture them before it runs
# and compare byte-for-byte after publication.
refs_before=$(mysql_query "
	SELECT CONCAT(HEX(refname), '=', is_symbolic, '=',
		COALESCE(LOWER(HEX(target_oid)), '-'), '=',
		COALESCE(HEX(symref_target), '-'), '=', version)
	  FROM refs WHERE repo_id = UNHEX('$repo_id') ORDER BY refname;")
ref_updates_before=$(mysql_query "
	SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id');")
generations_before=$(mysql_query "
	SELECT GROUP_CONCAT(CONCAT(generation, ':', state) ORDER BY generation SEPARATOR ',')
	  FROM generations WHERE repo_id = UNHEX('$repo_id');")
objects_before=$(mysql_query "
	SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id');")
generation_objects_before=$(mysql_query "
	SELECT COUNT(*) FROM generation_objects WHERE repo_id = UNHEX('$repo_id');")

# Isolate this disposable job from any pending repositories in a shared local
# Compose volume. The real receive created the request; this only skips its
# intentional debounce window for the test.
mysql_query "
	UPDATE pack_rebuild_requests
	   SET not_before = '1000-01-01 00:00:00.000000'
	 WHERE repo_id = UNHEX('$repo_id');" >/dev/null

SECONDS=0
worker_output=$(VEX_PACK_WORKER_MULTIPART_PART_BYTES="$multipart_part_bytes" \
	VEX_PACK_WORKER_MULTIPART_CONCURRENCY=3 \
	"${compose[@]}" --profile pack-worker run --rm --no-deps pack-worker --once)
worker_elapsed=$SECONDS
case "$worker_output" in
*"vex-pack-worker: published repository=$repo_name generation="*' pack='*' objects='*) ;;
*)
	printf 'vex pack-worker E2E: unexpected worker output: %s\n' "$worker_output" >&2
	exit 1
	;;
esac
worker_objects=$(printf '%s\n' "$worker_output" | \
	sed -n 's/.* objects=\([0-9][0-9]*\).*/\1/p' | tail -1)
case "$worker_objects" in
''|*[!0-9]*|0)
	printf 'vex pack-worker E2E: invalid worker object count: %s\n' "$worker_objects" >&2
	exit 1
	;;
esac

queue_after=$(mysql_query "
	SELECT COUNT(*) FROM pack_rebuild_requests WHERE repo_id = UNHEX('$repo_id');")
[ "$queue_after" = 0 ] || {
	printf 'vex pack-worker E2E: published request remains queued: %s\n' "$queue_after" >&2
	exit 1
}
pack_row=$(mysql_query "
	SELECT CONCAT(
		LOWER(LEFT(HEX(p.pack_id), IF(r.hash_algo = 'sha1', 40, 64))), '|',
		LOWER(HEX(p.pack_id)), '|', p.pack_key, '|', p.idx_key, '|', p.covering_generation)
	  FROM pack_envelopes AS p
	  JOIN repositories AS r USING (repo_id)
	 WHERE p.repo_id = UNHEX('$repo_id') AND p.state = 'sealed'
	   AND source_kind = 'derived' AND is_full_snapshot = TRUE;")
IFS='|' read -r pack_id pack_id_binary pack_key index_key covering_generation <<EOF
$pack_row
EOF
case "${#pack_id}" in
40|64) ;;
*)
	printf 'vex pack-worker E2E: derived envelope is not current and content-addressed: %s\n' \
		"$pack_row" >&2
	exit 1
	;;
esac
[ "$pack_key" = "git/v1/$repo_id/packs/$pack_id.pack" ] &&
	[ "$index_key" = "git/v1/$repo_id/packs/$pack_id.idx" ] &&
	[ "$covering_generation" = "$sealed_generation" ] || {
	printf 'vex pack-worker E2E: derived envelope is not current and content-addressed: %s\n' \
		"$pack_row" >&2
	exit 1
}
membership_count=$(mysql_query "
	SELECT COUNT(*) FROM pack_objects
	 WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$pack_id_binary');")
offset_count=$(mysql_query "
	SELECT COUNT(*) FROM pack_object_offsets
	 WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$pack_id_binary');")
closure_count=$("$stock_git" -C "$client" rev-list --objects --no-object-names --all | sort -u | wc -l | tr -d ' ')
[ "$membership_count" = "$worker_objects" ] &&
	[ "$offset_count" = "$worker_objects" ] &&
	[ "$closure_count" = "$worker_objects" ] || {
	printf 'vex pack-worker E2E: membership=%s offsets=%s closure=%s worker=%s\n' \
		"$membership_count" "$offset_count" "$closure_count" "$worker_objects" >&2
	exit 1
}

refs_after=$(mysql_query "
	SELECT CONCAT(HEX(refname), '=', is_symbolic, '=',
		COALESCE(LOWER(HEX(target_oid)), '-'), '=',
		COALESCE(HEX(symref_target), '-'), '=', version)
	  FROM refs WHERE repo_id = UNHEX('$repo_id') ORDER BY refname;")
ref_updates_after=$(mysql_query "
	SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id');")
generations_after=$(mysql_query "
	SELECT GROUP_CONCAT(CONCAT(generation, ':', state) ORDER BY generation SEPARATOR ',')
	  FROM generations WHERE repo_id = UNHEX('$repo_id');")
objects_after=$(mysql_query "
	SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id');")
generation_objects_after=$(mysql_query "
	SELECT COUNT(*) FROM generation_objects WHERE repo_id = UNHEX('$repo_id');")
[ "$refs_after" = "$refs_before" ] &&
	[ "$ref_updates_after" = "$ref_updates_before" ] &&
	[ "$generations_after" = "$generations_before" ] &&
	[ "$objects_after" = "$objects_before" ] &&
	[ "$generation_objects_after" = "$generation_objects_before" ] || {
	printf '%s\n' 'vex pack-worker E2E: worker changed C-owned refs, generations, or canonical objects' >&2
	exit 1
}

"${compose[@]}" run --rm --no-deps --entrypoint /bin/sh \
	-e "VEX_PACK_KEY=$pack_key" -e "VEX_INDEX_KEY=$index_key" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc stat "vex/$MINIO_BUCKET/$VEX_PACK_KEY" >/dev/null
		mc stat "vex/$MINIO_BUCKET/$VEX_INDEX_KEY" >/dev/null
	'
pack_s3_bytes=$("${compose[@]}" run --rm --no-deps --entrypoint /bin/sh \
	-e "VEX_PACK_KEY=$pack_key" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc cat "vex/$MINIO_BUCKET/$VEX_PACK_KEY" | wc -c | tr -d "[:space:]"
	')
case "$pack_s3_bytes" in
''|*[!0-9]*)
	printf 'vex pack-worker E2E: invalid S3 pack byte count: %s\n' "$pack_s3_bytes" >&2
	exit 1
	;;
esac
[ "$pack_s3_bytes" -gt "$multipart_part_bytes" ] || {
	printf 'vex pack-worker E2E: pack did not cross multipart threshold (%s <= %s)\n' \
		"$pack_s3_bytes" "$multipart_part_bytes" >&2
	exit 1
}

# Force direct relay. A successful stock clone now proves the worker-created
# envelope/membership is selected instead of falling back to git-pack-objects.
env GIT_VEX_TEST_FAIL_PACK_OBJECTS=1 \
	"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
fault_gateway_active=1
wait_for_storage
relay_clone="$scratch/relay-clone"
git_https -c protocol.version=2 clone -q "$origin" "$relay_clone"
[ "$("$stock_git" -C "$relay_clone" rev-parse HEAD)" = "$tip" ] || {
	printf '%s\n' 'vex pack-worker E2E: direct pack relay did not reach the pushed tip' >&2
	exit 1
}
"$stock_git" -C "$relay_clone" fsck --no-progress >/dev/null
restore_gateway

# A later C receive rewrites state. The worker's offset map must survive it,
# even though the immutable envelope becomes obsolete and a fresh rebuild is
# queued for the newer generation.
printf 'post-worker receive\n' >>"$client/history"
"$stock_git" -C "$client" add history
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'post-worker receive'
git_https -C "$client" push -q origin HEAD:refs/heads/main
offsets_after_receive=$(mysql_query "
	SELECT COUNT(*) FROM pack_object_offsets
	 WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$pack_id_binary');")
[ "$offsets_after_receive" = "$offset_count" ] || {
	printf 'vex pack-worker E2E: normal receive lost worker offsets (%s -> %s)\n' \
		"$offset_count" "$offsets_after_receive" >&2
	exit 1
}

printf 'vex pack-worker E2E: PASS worker=%ss objects=%s multipart_pack_bytes=%s part_bytes=%s concurrency=3; immutable S3 pack/index, bulk membership/offsets, C-owned state fencing, direct stock-Git relay, and later receive preservation verified for %s\n' \
	"$worker_elapsed" "$worker_objects" "$pack_s3_bytes" "$multipart_part_bytes" "$origin"
