#!/usr/bin/env bash

# Compose direct-pack E2E. A stock client must clone over both smart-HTTP
# protocol versions while the gateway refuses any upload-pack dispatch to
# git-pack-objects. Passing therefore proves it relayed the verified current
# Vex pack cache instead.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

# Do not let Git's test harness turn the client into the source-built server.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-precomputed-pack.sh' >&2
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
	printf '%s\n' 'vex direct-pack E2E: gateway did not become MySQL+S3 ready' >&2
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
	printf 'vex direct-pack E2E: stock Git executable is unavailable: %s\n' "$stock_git" >&2
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

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-direct-pack.XXXXXX")
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
repo_name="e2e/direct-pack-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

client="$scratch/client"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-direct-pack-e2e
"$stock_git" -C "$client" config user.email vex-direct-pack-e2e@example.invalid
for commit_number in 1 2 3; do
	printf 'Vex direct precomputed-pack fixture %s\n' "$commit_number" >>"$client/history"
	"$stock_git" -C "$client" add history
	"$stock_git" -C "$client" -c commit.gpgSign=false commit -q \
		-m "Vex direct precomputed-pack fixture $commit_number"
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
	printf 'vex direct-pack E2E: unexpected builder output: %s\n' "$builder_output" >&2
	exit 1
	;;
esac

# An eligible request must not reach create_pack_file()'s git-pack-objects
# fallback. The test-only gateway environment is inherited by each CGI
# upload-pack request, then restored by the trap above.
env GIT_VEX_TEST_FAIL_PACK_OBJECTS=1 \
	"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
fault_gateway_active=1
wait_for_storage
fault_env=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${GIT_VEX_TEST_FAIL_PACK_OBJECTS:-}"')
[ "$fault_env" = 1 ] || {
	printf '%s\n' 'vex direct-pack E2E: gateway was not recreated with the dispatch hook' >&2
	exit 1
}

clone_v0="$scratch/clone-v0"
GIT_TRACE_PACKET="$scratch/v0.trace" git_https -c protocol.version=0 \
	clone -q "$origin" "$clone_v0"
[ "$("$stock_git" -C "$clone_v0" rev-parse HEAD)" = "$tip" ] || {
	printf '%s\n' 'vex direct-pack E2E: protocol-v0 clone did not reach the packed tip' >&2
	exit 1
}
"$stock_git" -C "$clone_v0" fsck --no-progress >/dev/null

clone_v2="$scratch/clone-v2"
GIT_TRACE_PACKET="$scratch/v2.trace" git_https -c protocol.version=2 \
	clone -q "$origin" "$clone_v2"
[ "$("$stock_git" -C "$clone_v2" rev-parse HEAD)" = "$tip" ] || {
	printf '%s\n' 'vex direct-pack E2E: protocol-v2 clone did not reach the packed tip' >&2
	exit 1
}
"$stock_git" -C "$clone_v2" fsck --no-progress >/dev/null
grep -Eq 'clone< version 2$' "$scratch/v2.trace" || {
	printf '%s\n' 'vex direct-pack E2E: stock clone did not negotiate protocol v2' >&2
	exit 1
}

restore_gateway
printf 'vex direct-pack E2E: PASS stock protocol-v0/v2 clones relayed current Vex pack without git-pack-objects for %s\n' \
	"$origin"
