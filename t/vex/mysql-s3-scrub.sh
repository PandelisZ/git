#!/usr/bin/env bash

# Compose L3 integrity gate: publish a stock-Git repository, then make the
# Vex scrubber read every published byte from MinIO/S3 and re-hash it against
# the MySQL object index.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-scrub.sh' >&2
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

if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git)
fi
[ -x "$stock_git" ] || {
	printf 'vex scrub E2E: stock Git executable is not available: %s\n' "$stock_git" >&2
	exit 127
}

run_id="$(date +%s)-$$"
repo_name="e2e/scrub-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-scrub.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
ca_file="$docker_dir/certs/rootCA.pem"
client="$scratch/client"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-scrub-e2e
"$stock_git" -C "$client" config user.email vex-scrub-e2e@example.invalid
printf 'first scrubbed Vex object\n' >"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'first scrubbed object'
"$stock_git" -C "$client" branch -M main
"$stock_git" -C "$client" remote add origin "$origin"
"$stock_git" -C "$client" -c http.sslCAInfo="$ca_file" \
	-c "http.extraHeader=Authorization: Basic $auth" \
	push -q -u origin HEAD:refs/heads/main

printf 'second scrubbed Vex object\n' >>"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'second scrubbed object'
"$stock_git" -C "$client" tag -a "scrub-${run_id}" -m 'scrub annotated tag'
"$stock_git" -C "$client" -c http.sslCAInfo="$ca_file" \
	-c "http.extraHeader=Authorization: Basic $auth" \
	push -q origin HEAD:refs/heads/main "refs/tags/scrub-${run_id}"

scrub_output=$(VEX_COMPOSE_NO_BUILD=1 "$root/scripts/vex/scrub.sh" "$repo_name")
case "$scrub_output" in
*"vex scrub: PASS repository=$repo_name"*"objects="*"refs="*) ;;
*)
	printf 'vex scrub E2E: unexpected scrub output: %s\n' "$scrub_output" >&2
	exit 1
	;;
esac

printf 'vex scrub E2E: PASS stock Git publish and full MySQL-indexed S3 hash scrub for %s\n' "$origin"
