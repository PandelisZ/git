#!/usr/bin/env bash

# Compose S4 artifact gate. It proves the derived-pack write path and normal
# write compatibility only; upload-pack still reads the Vex object source and
# this test does not claim an L4 precomputed-pack serve path.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-pack-artifact.sh' >&2
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
	printf 'vex pack artifact E2E: stock Git executable is unavailable: %s\n' "$stock_git" >&2
	exit 127
}

compose=(docker compose -f "$docker_dir/compose.yaml" --env-file "$env_file")
mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" \
		-p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" -e "$1"
}

ca_file="$docker_dir/certs/rootCA.pem"
health=$(curl --fail --silent --show-error --cacert "$ca_file" \
	"https://localhost:${GIT_HTTPS_PORT}/healthz")
case "$health" in
*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) ;;
*)
	printf 'vex pack artifact E2E: endpoint is not active MySQL+S3: %s\n' "$health" >&2
	exit 1
	;;
esac

run_id="$(date +%s)-$$"
repo_name="e2e/pack-artifact-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-pack-artifact.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

client="$scratch/client"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-pack-artifact-e2e
"$stock_git" -C "$client" config user.email vex-pack-artifact-e2e@example.invalid
for commit_number in 1 2 3; do
	printf 'Vex immutable pack fixture %s\n' "$commit_number" >>"$client/history"
	"$stock_git" -C "$client" add history
	"$stock_git" -C "$client" -c commit.gpgSign=false commit -q \
		-m "Vex immutable pack fixture $commit_number"
done
"$stock_git" -C "$client" branch -M main
"$stock_git" -C "$client" tag -a pack-artifact-base -m 'pack artifact base'
"$stock_git" -C "$client" remote add origin "$origin"
git_https -C "$client" push -q -u origin HEAD:refs/heads/main refs/tags/pack-artifact-base

builder_output=$(VEX_COMPOSE_NO_BUILD=1 "$root/scripts/vex/build-pack.sh" "$repo_name")
case "$builder_output" in
*"vex build-pack: PASS repository=$repo_name"*' pack='*' objects='*' pack_key='*' idx_key='*) ;;
*)
	printf 'vex pack artifact E2E: unexpected builder output: %s\n' "$builder_output" >&2
	exit 1
	;;
esac
object_count=$(printf '%s\n' "$builder_output" | sed -n 's/.* objects=\([0-9][0-9]*\).*/\1/p' | tail -1)
case "$object_count" in
''|*[!0-9]*|0)
	printf 'vex pack artifact E2E: invalid object count in builder output: %s\n' \
		"$builder_output" >&2
	exit 1
	;;
esac

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????) repo_id=${repository_uri#vex://} ;;
*)
	printf 'vex pack artifact E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
	exit 1
	;;
esac
pack_state=$(mysql_query "
	SELECT CONCAT(state, ':', covering_generation, ':', pack_key, ':', idx_key)
	  FROM pack_envelopes
	 WHERE repo_id = UNHEX('$repo_id');")
case "$pack_state" in
sealed:[1-9]*:git/v1/"$repo_id"/packs/*.pack:git/v1/"$repo_id"/packs/*.idx) ;;
*)
	printf 'vex pack artifact E2E: MySQL did not record a sealed S3 pack envelope: %s\n' \
		"$pack_state" >&2
	exit 1
	;;
esac
covering_generation=${pack_state#sealed:}
covering_generation=${covering_generation%%:*}
initial_rebuild_requests=$(mysql_query "
	SELECT COUNT(*) FROM pack_rebuild_requests
	 WHERE repo_id = UNHEX('$repo_id');")
[ "$initial_rebuild_requests" = 0 ] || {
	printf 'vex pack artifact E2E: successful initial build left %s queued rebuild request(s)\n' \
		"$initial_rebuild_requests" >&2
	exit 1
}
registered_before=$(mysql_query "
	SELECT COUNT(*) FROM pack_objects WHERE repo_id = UNHEX('$repo_id');")
[ "$registered_before" = "$object_count" ] || {
	printf 'vex pack artifact E2E: registered %s pack object(s), expected %s\n' \
		"$registered_before" "$object_count" >&2
	exit 1
}

# The pack worker persists OID-to-pack offsets independently of loose object
# metadata. Seed one real pack/member row before the next normal receive: the
# state replacement must preserve it as well as the envelope/membership, or
# its FK would make the receive fail while deleting the old derived envelope.
IFS=$'\t' read -r offset_pack_id offset_oid <<EOF
$(mysql_query "
	SELECT HEX(p.pack_id), HEX(po.oid)
	  FROM pack_envelopes AS p
	  JOIN pack_objects AS po
	    ON po.repo_id = p.repo_id AND po.pack_id = p.pack_id
	 WHERE p.repo_id = UNHEX('$repo_id')
	 ORDER BY p.pack_id, po.oid
	 LIMIT 1;")
EOF
case "$offset_pack_id:$offset_oid" in
????????????????????????????????????????????????????????????????:????????????????????????????????????????????????????????????????) ;;
*)
	printf 'vex pack artifact E2E: could not select a derived pack offset fixture: %s:%s\n' \
		"$offset_pack_id" "$offset_oid" >&2
	exit 1
	;;
esac
mysql_query "
	INSERT INTO pack_object_offsets (repo_id, pack_id, oid, pack_offset)
	VALUES (UNHEX('$repo_id'), UNHEX('$offset_pack_id'), UNHEX('$offset_oid'), 4242);" \
	>/dev/null

# This pushes through the state-replacement adapter after pack registration.
# The immutable envelope/object map must survive, but its sealed generation is
# no longer current and must be marked obsolete before any serving path can
# select it.
printf 'post-pack normal receive\n' >>"$client/history"
"$stock_git" -C "$client" add history
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'post-pack normal receive'
tip=$("$stock_git" -C "$client" rev-parse HEAD)
git_https -C "$client" push -q origin HEAD:refs/heads/main
registered_after=$(mysql_query "
	SELECT COUNT(*) FROM pack_objects WHERE repo_id = UNHEX('$repo_id');")
[ "$registered_after" = "$registered_before" ] || {
	printf 'vex pack artifact E2E: post-pack push lost object membership (%s -> %s)\n' \
		"$registered_before" "$registered_after" >&2
	exit 1
}
offsets_after=$(mysql_query "
	SELECT CONCAT(COUNT(*), ':', COALESCE(MAX(pack_offset), 0))
	  FROM pack_object_offsets
	 WHERE repo_id = UNHEX('$repo_id')
	   AND pack_id = UNHEX('$offset_pack_id')
	   AND oid = UNHEX('$offset_oid');")
[ "$offsets_after" = '1:4242' ] || {
	printf 'vex pack artifact E2E: post-pack push did not preserve pack offset metadata: %s\n' \
		"$offsets_after" >&2
	exit 1
}
envelopes_after=$(mysql_query "
	SELECT COUNT(*) FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id');")
[ "$envelopes_after" = 1 ] || {
	printf 'vex pack artifact E2E: post-pack push lost pack envelope(s): %s\n' \
		"$envelopes_after" >&2
	exit 1
}
post_push_pack_state=$(mysql_query "
	SELECT CONCAT(state, ':', covering_generation, ':',
		       DATE_FORMAT(obsolete_at, '%Y-%m-%d %H:%i:%s.%f'))
	  FROM pack_envelopes
	 WHERE repo_id = UNHEX('$repo_id');")
post_push_state=${post_push_pack_state%%:*}
post_push_remainder=${post_push_pack_state#*:}
post_push_generation=${post_push_remainder%%:*}
post_push_obsolete_at=${post_push_remainder#*:}
if [ "$post_push_state" != obsolete ] ||
	[ "$post_push_generation" != "$covering_generation" ] ||
	[ -z "$post_push_obsolete_at" ] || [ "$post_push_obsolete_at" = NULL ]; then
	printf 'vex pack artifact E2E: post-pack push did not obsolete the stale pack envelope: %s\n' \
		"$post_push_pack_state" >&2
	exit 1
fi

# A receive only queues work.  It must not synchronously replace the obsolete
# pack, and the work item must begin in a nonzero server-side debounce window.
first_rebuild_request=$(mysql_query "
	SELECT CONCAT(target_generation, '|',
			  DATE_FORMAT(not_before, '%Y%m%d%H%i%s.%f'), '|',
			  TIMESTAMPDIFF(MICROSECOND, requested_at, not_before))
	  FROM pack_rebuild_requests
	 WHERE repo_id = UNHEX('$repo_id');")
post_push_latest_generation=$(mysql_query "
	SELECT COALESCE(MAX(generation), 0) FROM generations
	 WHERE repo_id = UNHEX('$repo_id') AND state = 'sealed';")
case "$first_rebuild_request" in
"$post_push_latest_generation"'|'*'|'*) ;;
*)
	printf 'vex pack artifact E2E: stale pack invalidation did not queue the latest generation (%s, expected %s)\n' \
		"$first_rebuild_request" "$post_push_latest_generation" >&2
	exit 1
	;;
esac
first_request_not_before=${first_rebuild_request#*|}
first_request_not_before=${first_request_not_before%%|*}
first_request_delay=${first_rebuild_request##*|}
case "$first_request_delay" in
''|*[!0-9]*|0)
	printf 'vex pack artifact E2E: rebuild request has no positive debounce: %s\n' \
		"$first_rebuild_request" >&2
	exit 1
	;;
esac
deferred_output=$(VEX_COMPOSE_NO_BUILD=1 VEX_COMPOSE_ALREADY_UP=1 \
	"$root/scripts/vex/rebuild-pack.sh" "$repo_name")
case "$deferred_output" in
"vex build-pack: NOOP repository=$repo_name reason=debounced-or-not-requested") ;;
*)
	printf 'vex pack artifact E2E: due-only builder ignored debounce: %s\n' \
		"$deferred_output" >&2
	exit 1
	;;
esac

# A second push joins the existing request.  It advances the target without
# resetting not_before, so a sustained push stream batches rather than runs a
# full repack per receive or starves the eventual operator job.
printf 'post-pack coalesced receive\n' >>"$client/history"
"$stock_git" -C "$client" add history
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q \
	-m 'post-pack coalesced receive'
tip=$("$stock_git" -C "$client" rev-parse HEAD)
git_https -C "$client" push -q origin HEAD:refs/heads/main
coalesced_rebuild_request=$(mysql_query "
	SELECT CONCAT(target_generation, '|',
			  DATE_FORMAT(not_before, '%Y%m%d%H%i%s.%f'))
	  FROM pack_rebuild_requests
	 WHERE repo_id = UNHEX('$repo_id');")
coalesced_latest_generation=$(mysql_query "
	SELECT COALESCE(MAX(generation), 0) FROM generations
	 WHERE repo_id = UNHEX('$repo_id') AND state = 'sealed';")
case "$coalesced_rebuild_request" in
"$coalesced_latest_generation"'|'*) ;;
*)
	printf 'vex pack artifact E2E: coalesced rebuild target is not current: %s (expected %s)\n' \
		"$coalesced_rebuild_request" "$coalesced_latest_generation" >&2
	exit 1
	;;
esac
coalesced_not_before=${coalesced_rebuild_request#*|}
[ "$coalesced_not_before" = "$first_request_not_before" ] || {
	printf 'vex pack artifact E2E: coalesced push reset debounce (%s -> %s)\n' \
		"$first_request_not_before" "$coalesced_not_before" >&2
	exit 1
}

# This disposable test repository may advance its server-side due time without
# waiting for the production-sized debounce.  The due-only operator command
# then builds the newest generation and clears only the request it covered.
mysql_query "
	UPDATE pack_rebuild_requests
	   SET not_before = CURRENT_TIMESTAMP(6)
	 WHERE repo_id = UNHEX('$repo_id');" >/dev/null
rebuild_output=$(VEX_COMPOSE_NO_BUILD=1 VEX_COMPOSE_ALREADY_UP=1 \
	"$root/scripts/vex/rebuild-pack.sh" "$repo_name")
case "$rebuild_output" in
*"vex build-pack: PASS repository=$repo_name"*' rebuild_request=cleared') ;;
*)
	printf 'vex pack artifact E2E: due-only builder did not rebuild/clear current work: %s\n' \
		"$rebuild_output" >&2
	exit 1
	;;
esac
current_pack_count=$(mysql_query "
	SELECT COUNT(*) FROM pack_envelopes AS p
	 WHERE p.repo_id = UNHEX('$repo_id')
	   AND p.state = 'sealed'
	   AND p.covering_generation = (
		SELECT MAX(g.generation) FROM generations AS g
		 WHERE g.repo_id = UNHEX('$repo_id') AND g.state = 'sealed');")
[ "$current_pack_count" = 1 ] || {
	printf 'vex pack artifact E2E: due-only builder did not seal one current pack: %s\n' \
		"$current_pack_count" >&2
	exit 1
}
pending_rebuild_requests=$(mysql_query "
	SELECT COUNT(*) FROM pack_rebuild_requests
	 WHERE repo_id = UNHEX('$repo_id');")
[ "$pending_rebuild_requests" = 0 ] || {
	printf 'vex pack artifact E2E: current rebuild left %s queued request(s)\n' \
		"$pending_rebuild_requests" >&2
	exit 1
}

fresh="$scratch/fresh"
"$stock_git" -c http.sslCAInfo="$ca_file" clone -q "$origin" "$fresh"
fresh_tip=$("$stock_git" -C "$fresh" rev-parse refs/heads/main)
[ "$fresh_tip" = "$tip" ] || {
	printf 'vex pack artifact E2E: fresh clone tip is %s, expected %s\n' \
		"$fresh_tip" "$tip" >&2
	exit 1
}
"$stock_git" -C "$fresh" fsck --no-progress
scrub_output=$(VEX_COMPOSE_NO_BUILD=1 "$root/scripts/vex/scrub.sh" "$repo_name")
case "$scrub_output" in
*"vex scrub: PASS repository=$repo_name"*) ;;
*)
	printf 'vex pack artifact E2E: scrub failed after pack/post-pack push: %s\n' \
		"$scrub_output" >&2
	exit 1
	;;
esac

printf 'vex pack artifact E2E: PASS custom-Git pack/index lives in S3; stale envelopes become ineligible, two receives coalesce one debounced MySQL rebuild request, and the due-only operator builder seals the current generation before stock clone/fsck/scrub for %s\n' \
	"$origin"
