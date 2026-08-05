#!/usr/bin/env bash

# Compose derived-pack cache E2E. It materializes a current S3 pack into the
# gateway's named scratch volume, then makes every S3 byte read fail. A fresh
# unmodified Git clone must still work from that verified local packed source.
# This is cache-read evidence only, not direct precomputed-pack client serving.
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
	printf '%s\n' 'usage: t/vex/mysql-s3-pack-cache.sh' >&2
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
	printf '%s\n' 'vex mysql-s3 pack cache E2E: gateway did not become MySQL+S3 ready' >&2
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
	printf 'vex mysql-s3 pack cache E2E: stock Git executable is unavailable: %s\n' "$stock_git" >&2
	exit 127
}

mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" \
		-p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" -e "$1"
}

fault_gateway_active=0
restore_gateway() {
	if [ "$fault_gateway_active" -eq 0 ]; then
		return 0
	fi
	env GIT_VEX_TEST_S3_FAIL_READS= \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
	wait_for_storage
	fault_gateway_active=0
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-pack-cache.XXXXXX")
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
repo_name="e2e/pack-cache-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

client="$scratch/client"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-pack-cache-e2e
"$stock_git" -C "$client" config user.email vex-pack-cache-e2e@example.invalid
for commit_number in 1 2 3; do
	printf 'Vex derived cache fixture %s\n' "$commit_number" >>"$client/history"
	"$stock_git" -C "$client" add history
	"$stock_git" -C "$client" -c commit.gpgSign=false commit -q \
		-m "Vex derived cache fixture $commit_number"
done
"$stock_git" -C "$client" branch -M main
tip=$("$stock_git" -C "$client" rev-parse HEAD)
"$stock_git" -C "$client" remote add origin "$origin"
git_https -C "$client" push -q -u origin HEAD:refs/heads/main

builder_output=$(VEX_COMPOSE_NO_BUILD=1 VEX_COMPOSE_ALREADY_UP=1 \
	"$root/scripts/vex/build-pack.sh" "$repo_name")
case "$builder_output" in
*"vex build-pack: PASS repository=$repo_name"*' pack='*' objects='*' pack_key='*' idx_key='*) ;;
*)
	printf 'vex mysql-s3 pack cache E2E: unexpected builder output: %s\n' "$builder_output" >&2
	exit 1
	;;
esac
pack_id=$(printf '%s\n' "$builder_output" |
	sed -n 's/.* pack=\([0-9a-f][0-9a-f]*\) .*/\1/p' | tail -1)
case "${#pack_id}" in
40|64) ;;
*)
	printf 'vex mysql-s3 pack cache E2E: invalid pack ID in builder output: %s\n' \
		"$builder_output" >&2
	exit 1
	;;
esac

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????) repo_id=${repository_uri#vex://} ;;
*)
	printf 'vex mysql-s3 pack cache E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
	exit 1
	;;
esac
current_pack_rows=$(mysql_query "
	SELECT COUNT(*)
	  FROM pack_envelopes p
	 WHERE p.repo_id = UNHEX('$repo_id')
	   AND p.state = 'sealed'
	   AND p.covering_generation = (
		SELECT MAX(g.generation)
		  FROM generations g
		 WHERE g.repo_id = p.repo_id AND g.state = 'sealed'
	   );")
[ "$current_pack_rows" = 1 ] || {
	printf 'vex mysql-s3 pack cache E2E: expected one current sealed pack, found: %s\n' \
		"$current_pack_rows" >&2
	exit 1
}

# First stock clone cold-materializes the trusted pack/index pair. The cache
# resides on vex_git_scratch, which survives recreating only git-http below.
warm_clone="$scratch/warm-clone"
git_https clone -q "$origin" "$warm_clone"
[ "$("$stock_git" -C "$warm_clone" rev-parse HEAD)" = "$tip" ] || {
	printf '%s\n' 'vex mysql-s3 pack cache E2E: warm clone did not reach the pushed tip' >&2
	exit 1
}
"$stock_git" -C "$warm_clone" fsck --no-progress >/dev/null

cache_dir="/var/git-scratch/vex-pack-cache/$repo_id/$pack_id/objects/pack"
cache_pack="$cache_dir/pack-$pack_id.pack"
cache_idx="$cache_dir/pack-$pack_id.idx"
"${compose[@]}" exec -T git-http /bin/sh -c 'test -s "$1" && test -s "$2"' \
	sh "$cache_pack" "$cache_idx"

# The gateway now rejects every S3 GET, including a derived-pack download.
# This succeeds only when the new request opens the already verified local
# cache and all needed object reads resolve through its packed source.
env GIT_VEX_TEST_S3_FAIL_READS=1 \
	"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
fault_gateway_active=1
wait_for_storage
fault_env=$("${compose[@]}" exec -T git-http /bin/sh -c 'printf %s "${GIT_VEX_TEST_S3_FAIL_READS:-}"')
[ "$fault_env" = 1 ] || {
	printf '%s\n' 'vex mysql-s3 pack cache E2E: gateway was not recreated with the S3-read fault hook' >&2
	exit 1
}

fault_clone="$scratch/fault-clone"
git_https clone -q "$origin" "$fault_clone"
[ "$("$stock_git" -C "$fault_clone" rev-parse HEAD)" = "$tip" ] || {
	printf '%s\n' 'vex mysql-s3 pack cache E2E: cache-backed clone did not reach the pushed tip' >&2
	exit 1
}
"$stock_git" -C "$fault_clone" fsck --no-progress >/dev/null

restore_gateway
recovered_clone="$scratch/recovered-clone"
git_https clone -q "$origin" "$recovered_clone"
"$stock_git" -C "$recovered_clone" fsck --no-progress >/dev/null

printf 'vex mysql-s3 pack cache E2E: PASS stock Git cloned %s with all S3 byte reads disabled after verified current pack %s was materialized in vex_git_scratch\n' \
	"$origin" "$pack_id"
