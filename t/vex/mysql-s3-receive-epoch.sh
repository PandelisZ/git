#!/usr/bin/env bash

# Compose data-plane failure-path E2E.  A stock host Git client pushes over
# HTTPS to the source-built Vex server.  The rejected update must leave no
# published MySQL object row, no canonical S3 object, and no staging objects.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

# Git's test harness exports build-local paths.  Do not let those turn the
# client half of this test into the Vex-enabled server binary by accident.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-receive-epoch.sh' >&2
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
	printf 'vex mysql-s3 receive E2E: endpoint did not report active MySQL+S3 storage: %s\n' "$health" >&2
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
	printf 'vex mysql-s3 receive E2E: stock Git executable is not available: %s\n' "$stock_git" >&2
	exit 127
}

mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" \
		-N -B "$MYSQL_DATABASE" -e "$1"
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
		mc stat "vex/$MINIO_BUCKET" >/dev/null
		if mc stat "vex/$MINIO_BUCKET/$VEX_S3_OBJECT_KEY" >/dev/null 2>&1
		then
			printf "unexpected MinIO object: %s\\n" "$VEX_S3_OBJECT_KEY" >&2
			exit 1
		fi
	'
}

assert_minio_staging_prefix_empty() {
	minio_run -e "VEX_S3_STAGING_PREFIX=$1" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc stat "vex/$MINIO_BUCKET" >/dev/null
		objects=$(mc find "vex/$MINIO_BUCKET/$VEX_S3_STAGING_PREFIX" --type f 2>/dev/null || :)
		if test -n "$objects"
		then
			printf "unexpected MinIO staging objects under: %s\\n" "$VEX_S3_STAGING_PREFIX" >&2
			exit 1
		fi
	'
}

# MySQL represents SHA-1 object IDs in the fixed-width BINARY(32) column by
# appending twelve zero bytes.  SHA-256 IDs already fill the column.
mysql_oid() {
	case "${#1}" in
	40) printf '%s000000000000000000000000' "$1" ;;
	64) printf '%s' "$1" ;;
	*)
		printf 'vex mysql-s3 receive E2E: unsupported object ID: %s\n' "$1" >&2
		exit 2
		;;
	esac
}

git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

run_id="$(date +%s)-$$"
repo_name="e2e/receive-epoch-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

# This is only thin-repository server policy.  Vex's ref/object truth remains
# in MySQL/S3; the setting gives the stock HTTP receive-pack a safe rejection.
"${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config receive.denyNonFastForwards true

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????)
	repo_id=${repository_uri#vex://}
	;;
*)
	printf 'vex mysql-s3 receive E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
	exit 1
	;;
esac

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-receive-epoch.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
client="$scratch/client"
verifier="$scratch/verifier"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-mysql-s3-receive-e2e
"$stock_git" -C "$client" config user.email vex-mysql-s3-receive-e2e@example.invalid
printf 'accepted Vex MySQL+S3 receive\n' >"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'accepted Vex MySQL+S3 receive'
"$stock_git" -C "$client" branch -M main
accepted_oid=$("$stock_git" -C "$client" rev-parse HEAD)
accepted_mysql_oid=$(mysql_oid "$accepted_oid")
"$stock_git" -C "$client" remote add origin "$origin"
git_https -C "$client" push -q -u origin HEAD:refs/heads/main

main_before=$(mysql_query "SELECT HEX(target_oid) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname = 'refs/heads/main';")
[ "$main_before" = "$(printf '%s' "$accepted_mysql_oid" | tr '[:lower:]' '[:upper:]')" ] || {
	printf 'vex mysql-s3 receive E2E: MySQL main does not name accepted commit: %s\n' "$main_before" >&2
	exit 1
}
accepted_rows=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$accepted_mysql_oid') AND state = 'published';")
[ "$accepted_rows" = 1 ] || {
	printf '%s\n' 'vex mysql-s3 receive E2E: accepted commit is not published in MySQL' >&2
	exit 1
}
assert_minio_object_exists "git/v1/$repo_id/objects/$accepted_oid"
assert_minio_staging_prefix_empty "git/v1/$repo_id/staging/"

git_https clone -q "$origin" "$verifier"
[ "$("$stock_git" -C "$verifier" rev-parse refs/remotes/origin/main)" = "$accepted_oid" ] || {
	printf '%s\n' 'vex mysql-s3 receive E2E: accepted push is not readable before rejection' >&2
	exit 1
}

"$stock_git" -C "$client" checkout -q --orphan rejected
"$stock_git" -C "$client" rm -qr --cached .
printf 'rejected Vex MySQL+S3 receive\n' >"$client/REJECTED"
"$stock_git" -C "$client" add REJECTED
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'rejected Vex MySQL+S3 receive'
rejected_oid=$("$stock_git" -C "$client" rev-parse HEAD)
rejected_mysql_oid=$(mysql_oid "$rejected_oid")
if git_https -C "$client" push origin rejected:refs/heads/main \
	>"$scratch/rejected.stdout" 2>"$scratch/rejected.stderr"
then
	printf '%s\n' 'vex mysql-s3 receive E2E: non-fast-forward push unexpectedly succeeded' >&2
	exit 1
fi

main_after=$(mysql_query "SELECT HEX(target_oid) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname = 'refs/heads/main';")
[ "$main_after" = "$main_before" ] || {
	printf 'vex mysql-s3 receive E2E: rejected receive changed MySQL main from %s to %s\n' "$main_before" "$main_after" >&2
	exit 1
}
published_rejected_rows=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$rejected_mysql_oid') AND state = 'published';")
[ "$published_rejected_rows" = 0 ] || {
	printf '%s\n' 'vex mysql-s3 receive E2E: rejected commit remains published in MySQL' >&2
	exit 1
}
any_rejected_rows=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$rejected_mysql_oid');")
[ "$any_rejected_rows" = 0 ] || {
	printf '%s\n' 'vex mysql-s3 receive E2E: rejected commit still has a MySQL object row' >&2
	exit 1
}
rejected_claim_rows=$(mysql_query "SELECT COUNT(*) FROM generation_objects WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$rejected_mysql_oid');")
[ "$rejected_claim_rows" = 0 ] || {
	printf '%s\n' 'vex mysql-s3 receive E2E: rejected commit still has a MySQL generation claim' >&2
	exit 1
}
open_epochs=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open';")
[ "$open_epochs" = 0 ] || {
	printf '%s\n' 'vex mysql-s3 receive E2E: rejected receive left an open MySQL generation' >&2
	exit 1
}
assert_minio_object_missing "git/v1/$repo_id/objects/$rejected_oid"
assert_minio_staging_prefix_empty "git/v1/$repo_id/staging/"

git_https -C "$verifier" fetch -q origin main
[ "$("$stock_git" -C "$verifier" rev-parse FETCH_HEAD)" = "$accepted_oid" ] || {
	printf '%s\n' 'vex mysql-s3 receive E2E: accepted main is unreadable after rejection' >&2
	exit 1
}
"$stock_git" -C "$verifier" fsck --no-progress >/dev/null

printf 'vex mysql-s3 receive E2E: PASS rejected stock-Git HTTPS update kept main=%s; MySQL and canonical S3 exclude rejected=%s; staging is empty; accepted main remains readable at %s\n' \
	"$accepted_oid" "$rejected_oid" "$origin"
