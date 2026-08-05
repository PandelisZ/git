#!/usr/bin/env bash

# Compose L3 retention gate. It creates a current immutable S3 pack, acquires
# a bounded MySQL lease, makes that pack obsolete with a stock-Git force push,
# and proves the read-only sweep plan retains its members until release.
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
	printf '%s\n' 'usage: t/vex/mysql-s3-pack-lease.sh' >&2
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
	printf 'vex pack lease E2E: stock Git executable is unavailable: %s\n' "$stock_git" >&2
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
	printf '%s\n' 'vex pack lease E2E: gateway did not become MySQL+S3 ready' >&2
	return 1
}

wait_for_storage

# Git CGI workers normally retain a 900-second crash-safe lease even after
# their process exits. Make only those automatic worker leases one second for
# this bounded test; the explicit planner lease below remains 900 seconds.
short_lease_gateway_active=0
restore_gateway() {
	if [ "$short_lease_gateway_active" -eq 0 ]; then
		return 0
	fi
	env GIT_VEX_TEST_PACK_LEASE_SECONDS= \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
	wait_for_storage
	short_lease_gateway_active=0
}
env GIT_VEX_TEST_PACK_LEASE_SECONDS=1 \
	"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
short_lease_gateway_active=1
wait_for_storage
short_lease_env=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${GIT_VEX_TEST_PACK_LEASE_SECONDS:-}"')
[ "$short_lease_env" = 1 ] || {
	printf '%s\n' 'vex pack lease E2E: gateway was not recreated with the short test lease' >&2
	exit 1
}

mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" \
		-p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" -e "$1"
}

wait_for_auto_leases_to_expire() {
	local excluded_lease=${1:-}
	local attempt count exclusion=''

	if [ -n "$excluded_lease" ]; then
		exclusion=" AND lease_id <> UNHEX('$excluded_lease')"
	fi
	for attempt in $(seq 1 10); do
		count=$(mysql_query "
			SELECT COUNT(*) FROM clone_leases
			 WHERE repo_id = UNHEX('$repo_id')
			   AND expires_at > CURRENT_TIMESTAMP(6)$exclusion;")
		[ "$count" = 0 ] && return 0
		sleep 1
	done
	printf 'vex pack lease E2E: automatic cache leases did not expire within 10 seconds (remaining=%s)\n' \
		"$count" >&2
	return 1
}

planner() {
	"${compose[@]}" exec -T -e GIT_VEX_TEST_DISABLE_DERIVED_PACK_CACHE=1 \
		git-http /opt/vex-git/vex-sweep-plan \
		--git-dir="/srv/git/$repo_name" "$@"
}

plan_field() {
	local name=$1
	local output=$2
	local value

	value=$(printf '%s\n' "$output" | sed -n "s/.*${name}=\\([0-9][0-9]*\\).*/\\1/p")
	case "$value" in
	''|*[!0-9]*)
		printf 'vex pack lease E2E: invalid %s in planner output: %s\n' \
			"$name" "$output" >&2
		return 1
		;;
	esac
	printf '%s\n' "$value"
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-pack-lease.XXXXXX")
cleanup() {
	local result=$?

	trap - EXIT HUP INT TERM
	set +e
	restore_gateway
	rm -rf -- "$scratch"
	exit "$result"
}
trap cleanup EXIT HUP INT TERM
auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

run_id="$(date +%s)-$$"
repo_name="e2e/pack-lease-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

client="$scratch/client"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-pack-lease-e2e
"$stock_git" -C "$client" config user.email vex-pack-lease-e2e@example.invalid
printf 'old Vex pack member graph\n' >"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'old packed tip'
"$stock_git" -C "$client" branch -M main
"$stock_git" -C "$client" remote add origin "$origin"
git_https -C "$client" push -q -u origin HEAD:refs/heads/main

builder_output=$(VEX_COMPOSE_NO_BUILD=1 VEX_COMPOSE_ALREADY_UP=1 \
	"$root/scripts/vex/build-pack.sh" "$repo_name")
case "$builder_output" in
*"vex build-pack: PASS repository=$repo_name"*' pack='*' objects='*' pack_key='*' idx_key='*) ;;
*)
	printf 'vex pack lease E2E: unexpected builder output: %s\n' "$builder_output" >&2
	exit 1
	;;
esac
pack_id=$(printf '%s\n' "$builder_output" |
	sed -n 's/.* pack=\([0-9a-f][0-9a-f]*\) .*/\1/p' | tail -1)
object_count=$(printf '%s\n' "$builder_output" |
	sed -n 's/.* objects=\([0-9][0-9]*\).*/\1/p' | tail -1)
case "${#pack_id}" in
40|64) ;;
*)
	printf 'vex pack lease E2E: invalid pack ID in builder output: %s\n' \
		"$builder_output" >&2
	exit 1
	;;
esac
case "$object_count" in
''|*[!0-9]*|0)
	printf 'vex pack lease E2E: invalid object count in builder output: %s\n' \
		"$builder_output" >&2
	exit 1
	;;
esac
pack_id_padded=$pack_id
while [ "${#pack_id_padded}" -lt 64 ]; do
	pack_id_padded="${pack_id_padded}0"
done

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????) repo_id=${repository_uri#vex://} ;;
*)
	printf 'vex pack lease E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
	exit 1
	;;
esac

# The config read above may have opened the just-built pack cache. Observe the
# one-second test TTL rather than assuming a fixed scheduler delay.
wait_for_auto_leases_to_expire

initial=$(planner)
initial_pack_roots=$(plan_field pack_roots "$initial")
initial_lease_roots=$(plan_field lease_roots "$initial")
[ "$initial_pack_roots" -eq "$object_count" ] && [ "$initial_lease_roots" -eq 0 ] || {
	printf 'vex pack lease E2E: current pack roots were unexpected: %s\n' "$initial" >&2
	exit 1
}

lease_output=$(planner --lease-current-pack=900)
lease_id=$(printf '%s\n' "$lease_output" |
	sed -n 's/.* lease=\([0-9a-f][0-9a-f]*\) pack=.*/\1/p' | tail -1)
case "$lease_output" in
*'action=acquired'*"lease=$lease_id"*"pack=$pack_id"*'expires_in=900'*) ;;
*)
	printf 'vex pack lease E2E: unexpected lease acquire output: %s\n' "$lease_output" >&2
	exit 1
	;;
esac
case "${#lease_id}" in
32) ;;
*)
	printf 'vex pack lease E2E: invalid lease ID in output: %s\n' "$lease_output" >&2
	exit 1
	;;
esac
lease_rows=$(mysql_query "
	SELECT COUNT(*) FROM clone_leases
	 WHERE repo_id = UNHEX('$repo_id')
	   AND lease_id = UNHEX('$lease_id')
	   AND pack_id = UNHEX('$pack_id_padded')
	   AND expires_at > CURRENT_TIMESTAMP(6);")
[ "$lease_rows" = 1 ] || {
	printf 'vex pack lease E2E: expected one live MySQL lease row, found %s\n' \
		"$lease_rows" >&2
	exit 1
}

# Replace main with a disconnected graph. This normal stock-Git receive
# rewrites the state adapter, making the old pack obsolete while the lease row
# must survive independently of the transient pack-row replacement.
"$stock_git" -C "$client" checkout -q --orphan replacement
"$stock_git" -C "$client" rm -q -rf .
printf 'replacement Vex graph\n' >"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'replacement tip'
"$stock_git" -C "$client" branch -M main
replacement_tip=$("$stock_git" -C "$client" rev-parse HEAD)
git_https -C "$client" push -q --force origin HEAD:refs/heads/main

# Let the request's one-second automatic cache lease expire; the explicit
# 900-second planner lease is the only retention root under test below.
wait_for_auto_leases_to_expire "$lease_id"

pack_state=$(mysql_query "
	SELECT state FROM pack_envelopes
	 WHERE repo_id = UNHEX('$repo_id')
	   AND pack_id = UNHEX('$pack_id_padded');")
[ "$pack_state" = obsolete ] || {
	printf 'vex pack lease E2E: old pack was not obsolete after stock-Git push: %s\n' \
		"$pack_state" >&2
	exit 1
}
lease_rows=$(mysql_query "
	SELECT COUNT(*) FROM clone_leases
	 WHERE repo_id = UNHEX('$repo_id')
	   AND lease_id = UNHEX('$lease_id')
	   AND pack_id = UNHEX('$pack_id_padded')
	   AND expires_at > CURRENT_TIMESTAMP(6);")
[ "$lease_rows" = 1 ] || {
	printf 'vex pack lease E2E: normal state rewrite lost live lease row: %s\n' \
		"$lease_rows" >&2
	exit 1
}

leased=$(planner)
leased_pack_roots=$(plan_field pack_roots "$leased")
leased_roots=$(plan_field lease_roots "$leased")
leased_candidates=$(plan_field sealed_candidates "$leased")
[ "$leased_pack_roots" -eq 0 ] && [ "$leased_roots" -eq "$object_count" ] && \
	[ "$leased_candidates" -eq 0 ] || {
	printf 'vex pack lease E2E: active lease did not retain obsolete pack members: %s\n' \
		"$leased" >&2
	exit 1
}

release_output=$(planner --release-pack-lease="$lease_id")
case "$release_output" in
*'action=released'*"lease=$lease_id"*) ;;
*)
	printf 'vex pack lease E2E: unexpected lease release output: %s\n' \
		"$release_output" >&2
	exit 1
	;;
esac
released=$(planner)
released_roots=$(plan_field lease_roots "$released")
released_candidates=$(plan_field sealed_candidates "$released")
[ "$released_roots" -eq 0 ] && [ "$released_candidates" -gt 0 ] || {
	printf 'vex pack lease E2E: released lease did not return old graph to the plan: %s\n' \
		"$released" >&2
	exit 1
}
lease_rows=$(mysql_query "
	SELECT COUNT(*) FROM clone_leases
	 WHERE repo_id = UNHEX('$repo_id') AND lease_id = UNHEX('$lease_id');")
[ "$lease_rows" = 0 ] || {
	printf 'vex pack lease E2E: released MySQL lease row remains: %s\n' "$lease_rows" >&2
	exit 1
}

fresh="$scratch/fresh"
git_https clone -q "$origin" "$fresh"
[ "$("$stock_git" -C "$fresh" rev-parse HEAD)" = "$replacement_tip" ] || {
	printf '%s\n' 'vex pack lease E2E: fresh stock clone did not reach replacement tip' >&2
	exit 1
}
"$stock_git" -C "$fresh" fsck --no-progress >/dev/null

printf 'vex pack lease E2E: PASS lease retained %s obsolete pack object(s) across a stock-Git state rewrite, then release restored %s conservative sweep candidate(s) for %s\n' \
	"$object_count" "$released_candidates" "$origin"
