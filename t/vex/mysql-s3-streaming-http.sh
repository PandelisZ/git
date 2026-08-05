#!/usr/bin/env bash

# Compose streaming gateway E2E.  A non-compressible 72 MiB fixture forces a
# smart-HTTP Content-Length request above the gateway's former 64 MiB whole-
# body buffer.  The stock client then clones and fscks the result.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-streaming-http.sh' >&2
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

# Existing developer .env files may retain the former 64 MiB setting.  This
# explicit test fixture exercises the raised production default without
# rewriting that user-owned local configuration.
VEX_HTTP_MAX_REQUEST_BYTES=8589934592 \
	"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null

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
	printf '%s\n' 'vex streaming E2E: gateway did not become MySQL+S3 ready' >&2
	return 1
}

wait_for_storage

gateway_limit=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${VEX_HTTP_MAX_REQUEST_BYTES:-}"')
[ "$gateway_limit" = 8589934592 ] || {
	printf 'vex streaming E2E: gateway did not use the required request limit: %s\n' "$gateway_limit" >&2
	exit 1
}

if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git)
fi
[ -x "$stock_git" ] || {
	printf 'vex streaming E2E: stock Git executable is unavailable: %s\n' "$stock_git" >&2
	exit 127
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-streaming.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
run_id="$(date +%s)-$$"
repo_name="e2e/streaming-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

client="$scratch/client"
clone="$scratch/clone"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-streaming-e2e
"$stock_git" -C "$client" config user.email vex-streaming-e2e@example.invalid
# ``urandom`` prevents pack compression from hiding the actual HTTP request
# size.  ``http.postBuffer`` forces Git to make this smart-HTTP POST with a
# Content-Length instead of the chunked fallback it normally chooses for a
# large pack.
dd if=/dev/urandom of="$client/payload.bin" bs=1048576 count=72 >/dev/null 2>&1
"$stock_git" -C "$client" add payload.bin
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'streaming gateway fixture'
"$stock_git" -C "$client" branch -M main
tip=$("$stock_git" -C "$client" rev-parse HEAD)
"$stock_git" -C "$client" remote add origin "$origin"

trace="$scratch/push-curl.trace"
GIT_TRACE_CURL="$trace" GIT_TRACE_CURL_NO_DATA=1 \
	"$stock_git" -C "$client" \
		-c http.sslCAInfo="$ca_file" \
		-c http.postBuffer=134217728 \
		-c "http.extraHeader=Authorization: Basic $auth" \
		push -q -u origin HEAD:refs/heads/main

awk '
	/Content-Length:/ {
		for (field = 1; field <= NF; field++)
			if ($field ~ /^[0-9]+$/ && $field > 67108864)
				found = 1
	}
	END { exit !found }
' "$trace" || {
	printf '%s\n' 'vex streaming E2E: stock client did not send a >64 MiB Content-Length request' >&2
	exit 1
}

"$stock_git" -c http.sslCAInfo="$ca_file" clone -q "$origin" "$clone"
[ "$("$stock_git" -C "$clone" rev-parse HEAD)" = "$tip" ] || {
	printf '%s\n' 'vex streaming E2E: clone did not receive the large pushed tip' >&2
	exit 1
}
"$stock_git" -C "$clone" fsck --no-progress >/dev/null

# Metadata/byte placement remains the same as the normal stock HTTP test.
repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????) ;;
*)
	printf 'vex streaming E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
	exit 1
	;;
esac

printf 'vex streaming E2E: PASS stock Git relayed and cloned a >64 MiB Content-Length MySQL+S3 push for %s\n' \
	"$origin"
