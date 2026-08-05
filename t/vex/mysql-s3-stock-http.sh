#!/usr/bin/env bash

# Compose data-plane E2E: use an unmodified host Git client over HTTPS against
# the source-built Vex server, then prove the resulting ref metadata and one
# canonical commit object are in MySQL and MinIO/S3 respectively.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

# This script intentionally uses a system Git client. Git's own test harness
# exports its build's exec/template paths, which would otherwise leak into
# `/usr/bin/git` and make the client side non-stock (or incompatible).
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-stock-http.sh' >&2
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

compose=(docker compose -f "$docker_dir/compose.yaml" --env-file "$env_file")
ca_file="$docker_dir/certs/rootCA.pem"
health=$(curl --fail --silent --show-error --cacert "$ca_file" \
	"https://localhost:${GIT_HTTPS_PORT}/healthz")
case "$health" in
*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) ;;
*)
	printf 'vex mysql-s3 E2E: endpoint did not report active MySQL+S3 storage: %s\n' "$health" >&2
	exit 1
	;;
esac

if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	# Git's test harness prepends its build wrappers to PATH. Prefer the system
	# executable so this E2E cannot accidentally validate the Vex server with
	# the same custom Git binary it is serving.
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git)
fi
[ -x "$stock_git" ] || {
	printf 'vex mysql-s3 E2E: stock Git executable is not available: %s\n' "$stock_git" >&2
	exit 127
}

run_id="$(date +%s)-$$"
repo_name="e2e/stock-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-stock-http.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
client="$scratch/client"
verifier="$scratch/verifier"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-mysql-s3-e2e
"$stock_git" -C "$client" config user.email vex-mysql-s3-e2e@example.invalid
printf 'first Vex MySQL+S3 stock-client commit\n' >"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'first Vex MySQL+S3 E2E commit'
"$stock_git" -C "$client" branch -M main
tip_one=$("$stock_git" -C "$client" rev-parse HEAD)
"$stock_git" -C "$client" remote add origin "$origin"
"$stock_git" -C "$client" \
	-c http.sslCAInfo="$ca_file" \
	-c "http.extraHeader=Authorization: Basic $auth" \
	push -q -u origin HEAD:refs/heads/main

"$stock_git" -c http.sslCAInfo="$ca_file" clone -q "$origin" "$verifier"
actual_one=$("$stock_git" -C "$verifier" rev-parse refs/remotes/origin/main)
[ "$actual_one" = "$tip_one" ] || {
	printf '%s\n' 'vex mysql-s3 E2E: clone did not receive the pushed first commit' >&2
	exit 1
}

# Smart-HTTP protocol v2 is negotiated by forwarding the stock client's
# Git-Protocol request header to git-http-backend as HTTP_GIT_PROTOCOL.  The
# trace proves both sides of that wire negotiation instead of merely relying
# on the successful v0-compatible clone above.
protocol_v2_trace="$scratch/protocol-v2.trace"
protocol_v2_curl_trace="$scratch/protocol-v2-curl.trace"
GIT_TRACE_PACKET="$protocol_v2_trace" \
	GIT_TRACE_CURL="$protocol_v2_curl_trace" GIT_TRACE_CURL_NO_DATA=1 \
	"$stock_git" \
	-c protocol.version=2 -c http.sslCAInfo="$ca_file" \
	ls-remote "$origin" refs/heads/main >/dev/null
grep -F 'Git-Protocol: version=2' "$protocol_v2_curl_trace" >/dev/null &&
	grep -F 'ls-remote< version 2' "$protocol_v2_trace" >/dev/null || {
	printf '%s\n' 'vex mysql-s3 E2E: smart-HTTP did not negotiate protocol v2' >&2
	exit 1
}

printf 'second Vex MySQL+S3 stock-client commit\n' >>"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'second Vex MySQL+S3 E2E commit'
tip_two=$("$stock_git" -C "$client" rev-parse HEAD)
"$stock_git" -C "$client" \
	-c http.sslCAInfo="$ca_file" \
	-c "http.extraHeader=Authorization: Basic $auth" \
	push -q origin HEAD:refs/heads/main
"$stock_git" -C "$verifier" -c http.sslCAInfo="$ca_file" fetch -q origin main
actual_two=$("$stock_git" -C "$verifier" rev-parse FETCH_HEAD)
[ "$actual_two" = "$tip_two" ] || {
	printf '%s\n' 'vex mysql-s3 E2E: fetch did not receive the pushed second commit' >&2
	exit 1
}

# P0/P1 ref coverage: an annotated tag and a second branch must publish in one
# atomic receive-pack transaction. The transient branch is then deleted, which
# exercises a non-fast-forward ref mutation without changing main.
tag_name="vex-e2e-${run_id}"
atomic_branch="atomic-${run_id}"
"$stock_git" -C "$client" tag -a "$tag_name" -m 'Vex MySQL+S3 annotated E2E tag' "$tip_two"
tag_oid=$("$stock_git" -C "$client" rev-parse "refs/tags/$tag_name")
"$stock_git" -C "$client" branch "$atomic_branch" "$tip_two"
"$stock_git" -C "$client" \
	-c http.sslCAInfo="$ca_file" \
	-c "http.extraHeader=Authorization: Basic $auth" \
	push -q --atomic origin \
	"refs/heads/$atomic_branch:refs/heads/$atomic_branch" \
	"refs/tags/$tag_name:refs/tags/$tag_name"
"$stock_git" -C "$verifier" -c http.sslCAInfo="$ca_file" fetch -q --tags origin
tag_target=$("$stock_git" -C "$verifier" rev-parse "refs/tags/$tag_name^{}")
[ "$tag_target" = "$tip_two" ] || {
	printf '%s\n' 'vex mysql-s3 E2E: annotated tag did not dereference to the atomic-push commit' >&2
	exit 1
}
"$stock_git" -C "$client" \
	-c http.sslCAInfo="$ca_file" \
	-c "http.extraHeader=Authorization: Basic $auth" \
	push -q origin ":refs/heads/$atomic_branch"
if [ -n "$("$stock_git" -c http.sslCAInfo="$ca_file" ls-remote "$origin" "refs/heads/$atomic_branch")" ]; then
	printf '%s\n' 'vex mysql-s3 E2E: deleted branch remains advertised by the server' >&2
	exit 1
fi

shallow="$scratch/shallow"
"$stock_git" -c http.sslCAInfo="$ca_file" clone -q --depth=1 --branch main "$origin" "$shallow"
shallow_tip=$("$stock_git" -C "$shallow" rev-parse HEAD)
[ "$shallow_tip" = "$tip_two" ] || {
	printf '%s\n' 'vex mysql-s3 E2E: depth-one clone did not receive main tip' >&2
	exit 1
}
[ "$("$stock_git" -C "$shallow" rev-list --count HEAD)" = 1 ] || {
	printf '%s\n' 'vex mysql-s3 E2E: depth-one clone retained more than one main commit' >&2
	exit 1
}
"$stock_git" -C "$verifier" fsck --no-progress >/dev/null

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????)
	repo_id=${repository_uri#vex://}
	;;
*)
	printf 'vex mysql-s3 E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
	exit 1
	;;
esac

ref_rows=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname = 'refs/heads/main';")
[ "$ref_rows" -ge 1 ] || {
	printf '%s\n' 'vex mysql-s3 E2E: pushed main ref is absent from MySQL metadata' >&2
	exit 1
}
tag_ref_rows=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname = 'refs/tags/$tag_name';")
[ "$tag_ref_rows" -eq 1 ] || {
	printf '%s\n' 'vex mysql-s3 E2E: annotated tag is absent from MySQL metadata' >&2
	exit 1
}
deleted_ref_rows=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname = 'refs/heads/$atomic_branch';")
[ "$deleted_ref_rows" -eq 0 ] || {
	printf '%s\n' 'vex mysql-s3 E2E: deleted branch remains in MySQL metadata' >&2
	exit 1
}
object_rows=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND state = 'published';")
[ "$object_rows" -ge 1 ] || {
	printf '%s\n' 'vex mysql-s3 E2E: pushed objects are absent from MySQL metadata' >&2
	exit 1
}

object_key="git/v1/$repo_id/objects/$tip_two"
tag_key="git/v1/$repo_id/objects/$tag_oid"
"${compose[@]}" run --rm --no-deps --entrypoint /bin/sh \
	-e "VEX_S3_OBJECT_KEY=$object_key" -e "VEX_S3_TAG_KEY=$tag_key" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc stat "vex/$MINIO_BUCKET/$VEX_S3_OBJECT_KEY" >/dev/null
		mc stat "vex/$MINIO_BUCKET/$VEX_S3_TAG_KEY" >/dev/null
	'

printf 'vex mysql-s3 E2E: PASS stock Git HTTPS push/clone/fetch, atomic annotated tag, delete, and depth-one clone; MySQL metadata and S3 bytes verified for %s\n' \
	"$origin"
