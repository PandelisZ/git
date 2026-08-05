#!/usr/bin/env bash

# Compose S3-promotion fault-injection E2E. A stock host Git client pushes
# over HTTPS while its next staged-to-final copy is forced to fail. The receive
# must compensate its epoch before a retry can publish the same commit.
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
	printf '%s\n' 'usage: t/vex/mysql-s3-s3-promotion-fault.sh' >&2
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
	printf '%s\n' 'vex mysql-s3 promotion fault E2E: gateway did not become MySQL+S3 ready' >&2
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
	printf 'vex mysql-s3 promotion fault E2E: stock Git executable is not available: %s\n' "$stock_git" >&2
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

assert_minio_prefix_empty() {
	minio_run -e "VEX_S3_PREFIX=$1" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc stat "vex/$MINIO_BUCKET" >/dev/null
		objects=$(mc find "vex/$MINIO_BUCKET/$VEX_S3_PREFIX" --type f 2>/dev/null || :)
		if test -n "$objects"
		then
			printf "unexpected MinIO objects under: %s\n" "$VEX_S3_PREFIX" >&2
			exit 1
		fi
	'
}

# MySQL represents SHA-1 IDs in BINARY(32) by appending twelve zero bytes.
mysql_oid() {
	case "${#1}" in
	40) printf '%s000000000000000000000000' "$1" ;;
	64) printf '%s' "$1" ;;
	*)
		printf 'vex mysql-s3 promotion fault E2E: unsupported object ID: %s\n' "$1" >&2
		exit 2
		;;
	esac
}

git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

fault_gateway_active=0
restore_gateway() {
	if [ "$fault_gateway_active" -eq 0 ]; then
		return 0
	fi
	env GIT_VEX_TEST_S3_FAIL_PROMOTE_ONCE= \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
	wait_for_storage
	fault_gateway_active=0
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-promotion-fault.XXXXXX")
cleanup() {
	local status=$?

	trap - EXIT HUP INT TERM
	set +e
	restore_gateway
	rm -rf -- "$scratch"
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

run_id="$(date +%s)-$$"
repo_name="e2e/s3-promotion-fault-$run_id.git"
origin="https://localhost:${GIT_HTTPS_PORT}/$repo_name"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????)
	repo_id=${repository_uri#vex://}
	;;
*)
	printf 'vex mysql-s3 promotion fault E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
	exit 1
	;;
esac

baseline_published_rows=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND state = 'published';")
baseline_object_rows=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id');")
baseline_claim_rows=$(mysql_query "SELECT COUNT(*) FROM generation_objects WHERE repo_id = UNHEX('$repo_id');")
baseline_sealed_epochs=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'sealed';")
baseline_aborted_epochs=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'aborted';")
baseline_audit_rows=$(mysql_query "SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id');")

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
client="$scratch/client"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-mysql-s3-promotion-fault-e2e
"$stock_git" -C "$client" config user.email vex-mysql-s3-promotion-fault-e2e@example.invalid
printf 'forced staged-to-final S3 promotion failure\n' >"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'Vex MySQL+S3 promotion fault'
"$stock_git" -C "$client" branch -M main
tip=$("$stock_git" -C "$client" rev-parse HEAD)
tip_mysql=$(mysql_oid "$tip")
"$stock_git" -C "$client" remote add origin "$origin"

# Recreate only the long-lived gateway with the test hook. MySQL, MinIO, and
# their volumes stay up; the EXIT trap recreates the same service without it.
env GIT_VEX_TEST_S3_FAIL_PROMOTE_ONCE=1 \
	"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
fault_gateway_active=1
wait_for_storage
fault_env=$("${compose[@]}" exec -T git-http /bin/sh -c 'printf %s "${GIT_VEX_TEST_S3_FAIL_PROMOTE_ONCE:-}"')
[ "$fault_env" = 1 ] || {
	printf '%s\n' 'vex mysql-s3 promotion fault E2E: gateway was not recreated with the fault hook' >&2
	exit 1
}

if git_https -C "$client" push --porcelain -u origin HEAD:refs/heads/main \
	>"$scratch/fault.stdout" 2>"$scratch/fault.stderr"
then
	printf '%s\n' 'vex mysql-s3 promotion fault E2E: push unexpectedly survived the forced promotion failure' >&2
	exit 1
fi

ref_rows=$(mysql_query "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname = 'refs/heads/main';")
[ "$ref_rows" = 0 ] || {
	printf 'vex mysql-s3 promotion fault E2E: failed receive left %s main ref rows\n' "$ref_rows" >&2
	exit 1
}
published_rows=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND state = 'published';")
[ "$published_rows" = "$baseline_published_rows" ] || {
	printf 'vex mysql-s3 promotion fault E2E: failed receive changed published object rows from %s to %s\n' \
		"$baseline_published_rows" "$published_rows" >&2
	exit 1
}
object_rows=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id');")
[ "$object_rows" = "$baseline_object_rows" ] || {
	printf 'vex mysql-s3 promotion fault E2E: failed receive changed object rows from %s to %s\n' \
		"$baseline_object_rows" "$object_rows" >&2
	exit 1
}
tip_rows=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$tip_mysql');")
[ "$tip_rows" = 0 ] || {
	printf 'vex mysql-s3 promotion fault E2E: failed receive retained %s rows for its commit\n' "$tip_rows" >&2
	exit 1
}
claim_rows=$(mysql_query "SELECT COUNT(*) FROM generation_objects WHERE repo_id = UNHEX('$repo_id');")
[ "$claim_rows" = "$baseline_claim_rows" ] || {
	printf 'vex mysql-s3 promotion fault E2E: failed receive changed generation claims from %s to %s\n' \
		"$baseline_claim_rows" "$claim_rows" >&2
	exit 1
}
sealed_epochs=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'sealed';")
[ "$sealed_epochs" = "$baseline_sealed_epochs" ] || {
	printf 'vex mysql-s3 promotion fault E2E: failed receive changed sealed epochs from %s to %s\n' \
		"$baseline_sealed_epochs" "$sealed_epochs" >&2
	exit 1
}
open_epochs=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open';")
[ "$open_epochs" = 0 ] || {
	printf 'vex mysql-s3 promotion fault E2E: failed receive left %s open epochs\n' "$open_epochs" >&2
	exit 1
}
aborted_epochs=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'aborted';")
[ "$aborted_epochs" -gt "$baseline_aborted_epochs" ] || {
	printf '%s\n' 'vex mysql-s3 promotion fault E2E: failed receive did not record an aborted epoch' >&2
	exit 1
}
audit_rows=$(mysql_query "SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id');")
[ "$audit_rows" = "$baseline_audit_rows" ] || {
	printf 'vex mysql-s3 promotion fault E2E: failed receive changed ref audit rows from %s to %s\n' \
		"$baseline_audit_rows" "$audit_rows" >&2
	exit 1
}
assert_minio_prefix_empty "git/v1/$repo_id/objects/"
assert_minio_prefix_empty "git/v1/$repo_id/staging/"

# Restore normal gateway configuration before retrying the same client commit.
restore_gateway
git_https -C "$client" push -q -u origin HEAD:refs/heads/main
remote_tip=$(git_https ls-remote "$origin" refs/heads/main | awk '{print $1}')
[ "$remote_tip" = "$tip" ] || {
	printf 'vex mysql-s3 promotion fault E2E: retry published %s, expected %s\n' "$remote_tip" "$tip" >&2
	exit 1
}
published_after_retry=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$tip_mysql') AND state = 'published';")
[ "$published_after_retry" = 1 ] || {
	printf 'vex mysql-s3 promotion fault E2E: retry did not publish its commit (rows=%s)\n' "$published_after_retry" >&2
	exit 1
}
verifier="$scratch/verifier"
git_https clone -q "$origin" "$verifier"
[ "$("$stock_git" -C "$verifier" rev-parse refs/remotes/origin/main)" = "$tip" ] || {
	printf '%s\n' 'vex mysql-s3 promotion fault E2E: retry commit is not cloneable' >&2
	exit 1
}
"$stock_git" -C "$verifier" fsck --no-progress >/dev/null

printf 'vex mysql-s3 promotion fault E2E: PASS one forced S3 promotion failure left no ref, published object, final S3 bytes, generation claims, or audit rows; aborted epochs=%s; retry published %s\n' \
	"$aborted_epochs" "$tip"
