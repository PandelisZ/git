#!/usr/bin/env bash

# Compose data-plane concurrency regression.  Two independent Vex repositories
# receive their first stock-Git HTTPS push at the same time.  In particular,
# this covers the MySQL ref_updates audit identifier allocation across repos:
# both ref transactions must publish and retain an audit row. Audit ids are
# scoped by repository, so equal numeric ids in two fresh repositories are
# valid and must not be treated as a collision.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

# Git's test harness exports build-local paths.  Keep the client half of this
# test a stock host Git client rather than the Vex-enabled server binary.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-cross-repo-concurrency.sh' >&2
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
	printf 'vex mysql-s3 cross-repo E2E: endpoint did not report active MySQL+S3 storage: %s\n' "$health" >&2
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
	printf 'vex mysql-s3 cross-repo E2E: stock Git executable is not available: %s\n' "$stock_git" >&2
	exit 127
}

mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" \
		-N -B "$MYSQL_DATABASE" -e "$1"
}

# MySQL represents SHA-1 object IDs in its BINARY(32) column by appending
# twelve zero bytes.  SHA-256 IDs already fill the column.
mysql_oid() {
	case "${#1}" in
	40) printf '%s000000000000000000000000' "$1" ;;
	64) printf '%s' "$1" ;;
	*)
		printf 'vex mysql-s3 cross-repo E2E: unsupported object ID: %s\n' "$1" >&2
		exit 2
		;;
	esac
}

git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

repository_id() {
	local repository_uri
	repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
		"--git-dir=/srv/git/$1" config --get extensions.refstorage)
	case "$repository_uri" in
	vex://????????????????????????????????) printf '%s\n' "${repository_uri#vex://}" ;;
	*)
		printf 'vex mysql-s3 cross-repo E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
		return 1
		;;
	esac
}

create_client() {
	local client=$1
	local label=$2

	"$stock_git" init -q "$client"
	"$stock_git" -C "$client" config user.name vex-mysql-s3-cross-repo-e2e
	"$stock_git" -C "$client" config user.email vex-mysql-s3-cross-repo-e2e@example.invalid
	printf 'cross repository concurrency payload: %s\n' "$label" >"$client/README"
	"$stock_git" -C "$client" add README
	"$stock_git" -C "$client" -c commit.gpgSign=false commit -q \
		-m "Vex MySQL+S3 cross-repo ${label}"
	"$stock_git" -C "$client" branch -M main
}

push_after_gate() {
	local client=$1
	local gate=$2
	local output=$3

	read -r _ <"$gate"
	git_https -C "$client" push --porcelain -u origin HEAD:refs/heads/main >"$output" 2>&1
}

run_id="$(date +%s)-$$"
repo_a="e2e/cross-repo-a-${run_id}.git"
repo_b="e2e/cross-repo-b-${run_id}.git"
origin_a="https://localhost:${GIT_HTTPS_PORT}/${repo_a}"
origin_b="https://localhost:${GIT_HTTPS_PORT}/${repo_b}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_a" >/dev/null
"$root/scripts/vex/seed-empty-repo.sh" "$repo_b" >/dev/null

repo_id_a=$(repository_id "$repo_a")
repo_id_b=$(repository_id "$repo_b")
[ "$repo_id_a" != "$repo_id_b" ] || {
	printf '%s\n' 'vex mysql-s3 cross-repo E2E: distinct repositories received the same Vex UUID' >&2
	exit 1
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-cross-repo.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
client_a="$scratch/client-a"
client_b="$scratch/client-b"
create_client "$client_a" a
create_client "$client_b" b
tip_a=$("$stock_git" -C "$client_a" rev-parse HEAD)
tip_b=$("$stock_git" -C "$client_b" rev-parse HEAD)
tip_a_mysql=$(mysql_oid "$tip_a")
tip_b_mysql=$(mysql_oid "$tip_b")
"$stock_git" -C "$client_a" remote add origin "$origin_a"
"$stock_git" -C "$client_b" remote add origin "$origin_b"

# Start both receives behind separate gates, then release both writers without
# serialising one push behind the other.  The fresh repositories make a failed
# push unambiguous: each is expected to append exactly one main audit row.
gate_a="$scratch/gate-a"
gate_b="$scratch/gate-b"
mkfifo "$gate_a" "$gate_b"
push_after_gate "$client_a" "$gate_a" "$scratch/push-a.log" &
push_a_pid=$!
push_after_gate "$client_b" "$gate_b" "$scratch/push-b.log" &
push_b_pid=$!
printf '%s\n' go >"$gate_a" &
release_a_pid=$!
printf '%s\n' go >"$gate_b" &
release_b_pid=$!
wait "$release_a_pid"
wait "$release_b_pid"

failed=0
if ! wait "$push_a_pid"; then
	failed=1
	printf '%s\n' 'vex mysql-s3 cross-repo E2E: repository A push failed:' >&2
	sed 's/^/  /' "$scratch/push-a.log" >&2
fi
if ! wait "$push_b_pid"; then
	failed=1
	printf '%s\n' 'vex mysql-s3 cross-repo E2E: repository B push failed:' >&2
	sed 's/^/  /' "$scratch/push-b.log" >&2
fi
if [ "$failed" -ne 0 ]; then
	exit 1
fi
if grep -F 'Vex ref transaction failed' "$scratch/push-a.log" "$scratch/push-b.log" >/dev/null
then
	printf '%s\n' 'vex mysql-s3 cross-repo E2E: a concurrent receive reported a Vex transaction failure' >&2
	exit 1
fi

remote_a=$(git_https ls-remote "$origin_a" refs/heads/main | awk '{print $1}')
remote_b=$(git_https ls-remote "$origin_b" refs/heads/main | awk '{print $1}')
[ "$remote_a" = "$tip_a" ] || {
	printf 'vex mysql-s3 cross-repo E2E: repository A main is %s, expected %s\n' "$remote_a" "$tip_a" >&2
	exit 1
}
[ "$remote_b" = "$tip_b" ] || {
	printf 'vex mysql-s3 cross-repo E2E: repository B main is %s, expected %s\n' "$remote_b" "$tip_b" >&2
	exit 1
}

main_a=$(mysql_query "SELECT HEX(target_oid) FROM refs WHERE repo_id = UNHEX('$repo_id_a') AND refname = 'refs/heads/main';")
main_b=$(mysql_query "SELECT HEX(target_oid) FROM refs WHERE repo_id = UNHEX('$repo_id_b') AND refname = 'refs/heads/main';")
[ "$main_a" = "$(printf '%s' "$tip_a_mysql" | tr '[:lower:]' '[:upper:]')" ] || {
	printf 'vex mysql-s3 cross-repo E2E: MySQL repository A main is %s, expected %s\n' "$main_a" "$tip_a_mysql" >&2
	exit 1
}
[ "$main_b" = "$(printf '%s' "$tip_b_mysql" | tr '[:lower:]' '[:upper:]')" ] || {
	printf 'vex mysql-s3 cross-repo E2E: MySQL repository B main is %s, expected %s\n' "$main_b" "$tip_b_mysql" >&2
	exit 1
}

audit_a=$(mysql_query "SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id_a') AND refname = 'refs/heads/main' AND new_oid = UNHEX('$tip_a_mysql');")
audit_b=$(mysql_query "SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id_b') AND refname = 'refs/heads/main' AND new_oid = UNHEX('$tip_b_mysql');")
[ "$audit_a" = 1 ] && [ "$audit_b" = 1 ] || {
	printf 'vex mysql-s3 cross-repo E2E: expected one main audit row per repo, got A=%s B=%s\n' "$audit_a" "$audit_b" >&2
	exit 1
}
audit_id_a=$(mysql_query "SELECT id FROM ref_updates WHERE repo_id = UNHEX('$repo_id_a') AND refname = 'refs/heads/main' AND new_oid = UNHEX('$tip_a_mysql');")
audit_id_b=$(mysql_query "SELECT id FROM ref_updates WHERE repo_id = UNHEX('$repo_id_b') AND refname = 'refs/heads/main' AND new_oid = UNHEX('$tip_b_mysql');")
case "$audit_id_a:$audit_id_b" in
*[!0-9:]*|:*|*:)
	printf 'vex mysql-s3 cross-repo E2E: invalid repository-scoped ref audit ids (A=%s B=%s)\n' "$audit_id_a" "$audit_id_b" >&2
	exit 1
	;;
esac
[ "$audit_id_a" -gt 0 ] && [ "$audit_id_b" -gt 0 ] || {
	printf 'vex mysql-s3 cross-repo E2E: invalid repository-scoped ref audit ids (A=%s B=%s)\n' "$audit_id_a" "$audit_id_b" >&2
	exit 1
}

printf 'vex mysql-s3 cross-repo E2E: PASS simultaneous stock-Git HTTPS pushes published main in both repositories; MySQL retained one repository-scoped ref_updates audit row per push (ids A=%s B=%s)\n' \
	"$audit_id_a" "$audit_id_b"
