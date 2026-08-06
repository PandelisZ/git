#!/usr/bin/env bash

# Compose E2E for the explicit, seed-only receive-pack WAL path.  A stock
# client pushes one complete, non-thin mirror into a freshly initialized Vex
# repository.  The assertion boundary is deliberately pack-level: Git bytes
# must be the canonical S3 pack/idx pair, while MySQL stores only the sealed
# envelope and OID-to-pack membership metadata.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

# Git's test harness exports build-local paths.  Do not let those turn the
# client half of this test into the source-built Vex server binary by accident.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-receive-pack-wal.sh' >&2
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
	printf '%s\n' 'vex receive-pack WAL E2E: gateway did not become MySQL+S3 ready' >&2
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
	printf 'vex receive-pack WAL E2E: stock Git executable is unavailable: %s\n' "$stock_git" >&2
	exit 127
}

gateway_wal_active=0
restore_gateway() {
	local actual

	if [ "$gateway_wal_active" -eq 0 ]; then
		return 0
	fi
	env GIT_VEX_RECEIVE_PACK_WAL= \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
	wait_for_storage
	actual=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_RECEIVE_PACK_WAL:-}"')
	[ -z "$actual" ] || {
		printf '%s\n' 'vex receive-pack WAL E2E: gateway retained the test-only WAL gate' >&2
		return 1
	}
	gateway_wal_active=0
}

activate_gateway_wal() {
	local actual

	env GIT_VEX_RECEIVE_PACK_WAL=1 \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
	wait_for_storage
	actual=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_RECEIVE_PACK_WAL:-}"')
	[ "$actual" = 1 ] || {
		printf '%s\n' 'vex receive-pack WAL E2E: gateway was not recreated with the WAL gate' >&2
		return 1
	}
	gateway_wal_active=1
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-receive-pack-wal.XXXXXX")
cleanup() {
	local status=$?

	trap - EXIT HUP INT TERM
	set +e
	restore_gateway
	rm -rf -- "$scratch"
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

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
		objects=$(mc find "vex/$MINIO_BUCKET/$VEX_S3_STAGING_PREFIX" 2>/dev/null || :)
		if test -n "$objects"
		then
			printf "unexpected MinIO staging objects under: %s\\n" "$VEX_S3_STAGING_PREFIX" >&2
			exit 1
		fi
	'
}

# MySQL stores SHA-1 IDs in BINARY(32) with twelve trailing zero bytes.
mysql_oid() {
	case "${#1}" in
	40) printf '%s000000000000000000000000' "$1" ;;
	64) printf '%s' "$1" ;;
	*)
		printf 'vex receive-pack WAL E2E: unsupported object ID: %s\n' "$1" >&2
		exit 2
		;;
	esac
}

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

run_id="$(date +%s)-$$"
repo_name="e2e/receive-pack-wal-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"

# The gate is only valid for the first complete seed.  Enable it before the
# repository opens, then prove this Vex repository has no pre-existing bytes
# or refs before the mirror transfer begins.
activate_gateway_wal
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????)
	repo_id=${repository_uri#vex://}
	;;
*)
	printf 'vex receive-pack WAL E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
	exit 1
	;;
esac

for table in refs objects pack_envelopes received_pack_objects; do
	rows=$(mysql_query "SELECT COUNT(*) FROM $table WHERE repo_id = UNHEX('$repo_id');")
	[ "$rows" = 0 ] || {
		printf 'vex receive-pack WAL E2E: fresh repository has %s unexpected %s rows\n' "$rows" "$table" >&2
		exit 1
	}
done

# Make a small but complete mirror with a branch, an annotated tag, and a
# side branch. `--no-thin` below is an explicit stock-client pack contract;
# repacking this mirror also gives the test a local full-pack member count.
fixture="$scratch/fixture"
mirror="$scratch/fixture.git"
clone="$scratch/clone"
"$stock_git" init -q "$fixture"
"$stock_git" -C "$fixture" config user.name vex-receive-pack-wal-e2e
"$stock_git" -C "$fixture" config user.email vex-receive-pack-wal-e2e@example.invalid
for revision in 1 2 3; do
	printf 'Vex receive-pack WAL fixture revision %s\n' "$revision" >>"$fixture/history"
	"$stock_git" -C "$fixture" add history
	"$stock_git" -C "$fixture" -c commit.gpgSign=false commit -q \
		-m "Vex receive-pack WAL fixture $revision"
done
"$stock_git" -C "$fixture" branch -M main
"$stock_git" -C "$fixture" branch side HEAD~1
"$stock_git" -C "$fixture" tag -a -m 'Vex receive-pack WAL annotated fixture tag' fixture-v1
"$stock_git" clone --mirror --no-local -q "$fixture" "$mirror"
"$stock_git" --git-dir="$mirror" repack -adf --write-bitmap-index

shopt -s nullglob
pack_indexes=("$mirror"/objects/pack/*.idx)
shopt -u nullglob
[ "${#pack_indexes[@]}" -eq 1 ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: fixture did not produce exactly one source pack' >&2
	exit 1
}

case "$("$stock_git" --git-dir="$mirror" rev-parse --show-object-format)" in
sha1) oid_hex_len=40 ;;
sha256) oid_hex_len=64 ;;
*)
	printf '%s\n' 'vex receive-pack WAL E2E: unsupported fixture object format' >&2
	exit 2
	;;
esac
expected_members=$("$stock_git" verify-pack -v "${pack_indexes[0]}" |
	awk -v oid_hex_len="$oid_hex_len" '
		length($1) == oid_hex_len &&
		($2 == "commit" || $2 == "tree" || $2 == "blob" || $2 == "tag") {
			members++
		}
		END { print members + 0 }
	')
[ "$expected_members" -gt 0 ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: fixture pack has no object members' >&2
	exit 1
}
expected_refs=$("$stock_git" --git-dir="$mirror" for-each-ref --format='%(refname)' | wc -l | tr -d ' ')
main_oid=$("$stock_git" --git-dir="$mirror" rev-parse refs/heads/main)

git_https --git-dir="$mirror" push --no-thin --mirror "$origin"

receive_rows=$(mysql_query "SELECT COUNT(*) FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id') AND source_kind = 'receive';")
[ "$receive_rows" = 1 ] || {
	printf 'vex receive-pack WAL E2E: expected one raw receive envelope, found %s\n' "$receive_rows" >&2
	exit 1
}
IFS=$'\t' read -r pack_id pack_state pack_source_kind full_snapshot pack_key idx_key <<EOF
$(mysql_query "SELECT LOWER(LEFT(HEX(pack_id), $oid_hex_len)), state, source_kind, is_full_snapshot, pack_key, idx_key FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id') AND source_kind = 'receive';")
EOF
[ -n "$pack_id" ] && [ "$pack_state" = sealed ] &&
	[ "$pack_source_kind" = receive ] && [ "$full_snapshot" = 1 ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: raw receive envelope is not sealed full-snapshot metadata' >&2
	exit 1
}
[ "$pack_key" = "git/v1/$repo_id/packs/$pack_id.pack" ] || {
	printf 'vex receive-pack WAL E2E: unexpected canonical pack key: %s\n' "$pack_key" >&2
	exit 1
}
[ "$idx_key" = "git/v1/$repo_id/packs/$pack_id.idx" ] || {
	printf 'vex receive-pack WAL E2E: unexpected canonical idx key: %s\n' "$idx_key" >&2
	exit 1
}

pack_mysql_id=$(mysql_oid "$pack_id")
main_mysql_oid=$(mysql_oid "$main_oid")
mapped_members=$(mysql_query "SELECT COUNT(*) FROM received_pack_objects WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$pack_mysql_id');")
[ "$mapped_members" = "$expected_members" ] || {
	printf 'vex receive-pack WAL E2E: raw pack map has %s members, expected %s\n' "$mapped_members" "$expected_members" >&2
	exit 1
}
mapped_main=$(mysql_query "SELECT COUNT(*) FROM received_pack_objects WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$pack_mysql_id') AND oid = UNHEX('$main_mysql_oid');")
[ "$mapped_main" = 1 ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: main tip is not mapped into the sealed raw pack' >&2
	exit 1
}
loose_rows=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id');")
[ "$loose_rows" = 0 ] || {
	printf 'vex receive-pack WAL E2E: raw-pack seed unexpectedly wrote %s loose-object rows\n' "$loose_rows" >&2
	exit 1
}
ref_rows=$(mysql_query "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id');")
[ "$ref_rows" = "$expected_refs" ] || {
	printf 'vex receive-pack WAL E2E: sealed ref count %s differs from mirror count %s\n' "$ref_rows" "$expected_refs" >&2
	exit 1
}

assert_minio_object_exists "$pack_key"
assert_minio_object_exists "$idx_key"
assert_minio_object_missing "git/v1/$repo_id/objects/$main_oid"
assert_minio_staging_prefix_empty "git/v1/$repo_id/staging/"

local_refs=$("$stock_git" --git-dir="$mirror" \
	for-each-ref --format='%(objectname)%09%(refname)' | LC_ALL=C sort)
remote_refs=$(git_https ls-remote --refs "$origin" | LC_ALL=C sort)
[ "$remote_refs" = "$local_refs" ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: server refs differ from the initial mirror' >&2
	printf '%s\n' "$remote_refs" >"$scratch/remote-refs"
	printf '%s\n' "$local_refs" >"$scratch/local-refs"
	diff -u "$scratch/local-refs" "$scratch/remote-refs" >&2 || true
	exit 1
}
remote_head=$(git_https ls-remote "$origin" HEAD |
	awk 'NF == 2 && $2 == "HEAD" { print $1; exit }')
[ "$remote_head" = "$main_oid" ] || {
	printf 'vex receive-pack WAL E2E: remote HEAD %s differs from main %s\n' "$remote_head" "$main_oid" >&2
	exit 1
}

git_https clone -q "$origin" "$clone"
[ "$("$stock_git" -C "$clone" rev-parse HEAD)" = "$main_oid" ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: clone HEAD differs from sealed main' >&2
	exit 1
}
"$stock_git" -C "$clone" fsck --no-progress >/dev/null

restore_gateway
printf 'vex receive-pack WAL E2E: PASS stock non-thin initial mirror refs=%s objects=%s pack=%s; sealed MySQL map and canonical S3 pack/idx verified; clone+fsck passed for %s\n' \
	"$expected_refs" "$expected_members" "$pack_id" "$origin"
