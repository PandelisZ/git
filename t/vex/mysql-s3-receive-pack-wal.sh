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
gateway_fault_active=0
restore_gateway() {
	local actual_wal actual_fault

	if [ "$gateway_wal_active" -eq 0 ] && [ "$gateway_fault_active" -eq 0 ]; then
		return 0
	fi
	env GIT_VEX_RECEIVE_PACK_WAL= GIT_VEX_TEST_FAIL_PACK_OBJECTS= \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
	wait_for_storage
	actual_wal=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_RECEIVE_PACK_WAL:-}"')
	actual_fault=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_TEST_FAIL_PACK_OBJECTS:-}"')
	[ -z "$actual_wal" ] && [ -z "$actual_fault" ] || {
		printf '%s\n' 'vex receive-pack WAL E2E: gateway retained a test-only gate' >&2
		return 1
	}
	gateway_wal_active=0
	gateway_fault_active=0
}

activate_gateway_wal() {
	local actual_wal actual_fault

	env GIT_VEX_RECEIVE_PACK_WAL=1 GIT_VEX_TEST_FAIL_PACK_OBJECTS= \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
	wait_for_storage
	actual_wal=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_RECEIVE_PACK_WAL:-}"')
	actual_fault=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_TEST_FAIL_PACK_OBJECTS:-}"')
	[ "$actual_wal" = 1 ] && [ -z "$actual_fault" ] || {
		printf '%s\n' 'vex receive-pack WAL E2E: gateway was not recreated with the WAL gate' >&2
		return 1
	}
	gateway_wal_active=1
}

activate_gateway_relay_fault() {
	local actual_wal actual_fault

	# The raw pack has already been sealed before this point.  Disable the
	# seed-only receive path and make any upload-pack fallback fail instead, so
	# both following stock-client clones must relay the verified raw pack.
	env GIT_VEX_RECEIVE_PACK_WAL= GIT_VEX_TEST_FAIL_PACK_OBJECTS=1 \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
	wait_for_storage
	actual_wal=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_RECEIVE_PACK_WAL:-}"')
	actual_fault=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_TEST_FAIL_PACK_OBJECTS:-}"')
	[ -z "$actual_wal" ] && [ "$actual_fault" = 1 ] || {
		printf '%s\n' 'vex receive-pack WAL E2E: gateway was not recreated with the raw-pack relay fault gate' >&2
		return 1
	}
	gateway_wal_active=0
	gateway_fault_active=1
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

for table in objects pack_envelopes received_pack_objects; do
	rows=$(mysql_query "SELECT COUNT(*) FROM $table WHERE repo_id = UNHEX('$repo_id');")
	[ "$rows" = 0 ] || {
		printf 'vex receive-pack WAL E2E: fresh repository has %s unexpected %s rows\n' "$rows" "$table" >&2
		exit 1
	}
done
# `git init --initial-branch=main --ref-format=vex` persists this symbolic
# scaffold before the first object exists.  It is not a received ref and the
# raw full-seed path must preserve it while rejecting every real existing ref.
head_rows=$(mysql_query "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname = 'HEAD' AND is_symbolic = TRUE AND target_oid IS NULL AND symref_target = 'refs/heads/main';")
[ "$head_rows" = 1 ] || {
	printf 'vex receive-pack WAL E2E: fresh repository lacks its expected unborn HEAD scaffold (%s rows)\n' "$head_rows" >&2
	exit 1
}
other_ref_rows=$(mysql_query "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id') AND NOT (refname = 'HEAD' AND is_symbolic = TRUE AND target_oid IS NULL AND symref_target = 'refs/heads/main');")
[ "$other_ref_rows" = 0 ] || {
	printf 'vex receive-pack WAL E2E: fresh repository has %s non-HEAD refs\n' "$other_ref_rows" >&2
	exit 1
}

# Make a small but complete mirror with a branch, an annotated tag, a side
# branch, and one pull-only commit.  The latter is deliberately outside
# refs/heads and refs/tags, so the default-clone derived artifact must differ
# from the complete raw/full relay artifact. `--no-thin` below is an explicit
# stock-client pack contract; repacking this mirror also gives the test a
# local full-pack member count.
fixture="$scratch/fixture"
mirror="$scratch/fixture.git"
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
"$stock_git" -C "$fixture" checkout -q -b fixture-pull
printf 'Vex receive-pack WAL pull-only fixture object\n' >>"$fixture/history"
"$stock_git" -C "$fixture" add history
"$stock_git" -C "$fixture" -c commit.gpgSign=false commit -q \
	-m 'Vex receive-pack WAL pull-only fixture'
"$stock_git" -C "$fixture" update-ref refs/pull/1/head HEAD
"$stock_git" -C "$fixture" checkout -q main
"$stock_git" -C "$fixture" branch -D fixture-pull >/dev/null
"$stock_git" clone --mirror --no-local -q "$fixture" "$mirror"
# Keep the pushed raw pack intentionally non-delta so the post-seed compactor
# must produce a distinct, smaller full snapshot rather than reusing this
# fixture's source representation.  These settings belong only to the source
# mirror; the Vex repository retains its normal pack defaults.
"$stock_git" --git-dir="$mirror" config pack.window 0
"$stock_git" --git-dir="$mirror" config pack.depth 0
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
tag_oid=$("$stock_git" --git-dir="$mirror" rev-parse refs/tags/fixture-v1)
tag_target=$("$stock_git" --git-dir="$mirror" rev-parse refs/tags/fixture-v1^{})
pull_oid=$("$stock_git" --git-dir="$mirror" rev-parse refs/pull/1/head)
[ "$tag_target" = "$main_oid" ] || {
	printf 'vex receive-pack WAL E2E: annotated fixture tag target %s differs from main %s\n' "$tag_target" "$main_oid" >&2
	exit 1
}
[ "$pull_oid" != "$main_oid" ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: pull-only fixture ref unexpectedly points at main' >&2
	exit 1
}
default_expected_oids="$scratch/default-clone-expected-oids"
"$stock_git" --git-dir="$mirror" for-each-ref --format='%(objectname)' refs/heads refs/tags |
	"$stock_git" --git-dir="$mirror" rev-list --objects --no-object-names --stdin |
	LC_ALL=C sort -u >"$default_expected_oids"
default_expected_members=$(wc -l <"$default_expected_oids" | tr -d ' ')
[ "$default_expected_members" -gt 0 ] && [ "$expected_members" -gt "$default_expected_members" ] || {
	printf 'vex receive-pack WAL E2E: expected partial default closure (%s) to be nonempty and smaller than raw pack (%s)\n' \
		"$default_expected_members" "$expected_members" >&2
	exit 1
}
grep -Fx "$tag_oid" "$default_expected_oids" >/dev/null || {
	printf '%s\n' 'vex receive-pack WAL E2E: annotated tag object is absent from the expected default closure' >&2
	exit 1
}
if grep -Fx "$pull_oid" "$default_expected_oids" >/dev/null
then
	printf '%s\n' 'vex receive-pack WAL E2E: pull-only commit leaked into the expected default closure' >&2
	exit 1
fi

git_https --git-dir="$mirror" push --no-thin --mirror "$origin"

receive_rows=$(mysql_query "SELECT COUNT(*) FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id') AND source_kind = 'receive';")
[ "$receive_rows" = 1 ] || {
	printf 'vex receive-pack WAL E2E: expected one raw receive envelope, found %s\n' "$receive_rows" >&2
	exit 1
}
IFS=$'\t' read -r pack_id covering_generation pack_state pack_source_kind full_snapshot pack_key idx_key <<EOF
$(mysql_query "SELECT LOWER(LEFT(HEX(pack_id), $oid_hex_len)), covering_generation, state, source_kind, is_full_snapshot, pack_key, idx_key FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id') AND source_kind = 'receive';")
EOF
[ -n "$pack_id" ] && [ "$covering_generation" -gt 0 ] && [ "$pack_state" = sealed ] &&
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
expected_ref_rows=$((expected_refs + 1))
[ "$ref_rows" = "$expected_ref_rows" ] || {
	printf 'vex receive-pack WAL E2E: sealed ref count %s differs from mirror plus HEAD scaffold count %s\n' "$ref_rows" "$expected_ref_rows" >&2
	exit 1
}

assert_minio_object_exists "$pack_key"
assert_minio_object_exists "$idx_key"
assert_minio_object_missing "git/v1/$repo_id/objects/$main_oid"
assert_minio_staging_prefix_empty "git/v1/$repo_id/staging/"

# The raw pack remains canonical for recovery and membership, but an explicit
# post-seed compactor may publish a second, smaller complete S3 pack for clone
# relay.  This path must not materialize legacy loose-object metadata merely
# to satisfy `pack_objects`.
compact_output=$(VEX_COMPOSE_NO_BUILD=1 VEX_COMPOSE_ALREADY_UP=1 \
	"$root/scripts/vex/build-pack.sh" --from-receive "$repo_name")
case "$compact_output" in
*"vex build-pack: PASS repository=$repo_name"*' pack='*' objects='*' pack_key='*' idx_key='*) ;;
*)
	printf 'vex receive-pack WAL E2E: unexpected raw compact-builder output: %s\n' \
		"$compact_output" >&2
	exit 1
	;;
esac
derived_rows=$(mysql_query "SELECT COUNT(*) FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id') AND source_kind = 'derived' AND state = 'sealed' AND covering_generation = (SELECT MAX(generation) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'sealed');")
[ "$derived_rows" = 1 ] || {
	printf 'vex receive-pack WAL E2E: expected one current compact derived envelope, found %s\n' \
		"$derived_rows" >&2
	exit 1
}
IFS=$'\t' read -r compact_pack_id compact_generation compact_full compact_pack_key compact_idx_key <<EOF
$(mysql_query "SELECT LOWER(LEFT(HEX(pack_id), $oid_hex_len)), covering_generation, is_full_snapshot, pack_key, idx_key FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id') AND source_kind = 'derived' AND state = 'sealed' ORDER BY sealed_at DESC, pack_id LIMIT 1;")
EOF
[ -n "$compact_pack_id" ] && [ "$compact_generation" = "$covering_generation" ] &&
	[ "$compact_full" = 1 ] || {
	printf 'vex receive-pack WAL E2E: compact derived envelope metadata is invalid: %s:%s:%s\n' \
		"$compact_pack_id" "$compact_generation" "$compact_full" >&2
	exit 1
}
[ "$compact_pack_id" != "$pack_id" ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: compact fixture unexpectedly reused the raw pack checksum' >&2
	exit 1
}
[ "$compact_pack_key" = "git/v1/$repo_id/packs/$compact_pack_id.pack" ] &&
	[ "$compact_idx_key" = "git/v1/$repo_id/packs/$compact_pack_id.idx" ] || {
	printf 'vex receive-pack WAL E2E: compact derived keys are non-canonical: %s %s\n' \
		"$compact_pack_key" "$compact_idx_key" >&2
	exit 1
}
compact_pack_mysql_id=$(mysql_oid "$compact_pack_id")
compact_members=$(mysql_query "SELECT COUNT(*) FROM pack_objects WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$compact_pack_mysql_id');")
[ "$compact_members" = 0 ] || {
	printf 'vex receive-pack WAL E2E: compact raw-derived envelope unexpectedly has %s legacy pack-object rows\n' \
		"$compact_members" >&2
	exit 1
}
raw_members_after_compaction=$(mysql_query "SELECT COUNT(*) FROM received_pack_objects WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$pack_mysql_id');")
[ "$raw_members_after_compaction" = "$expected_members" ] || {
	printf 'vex receive-pack WAL E2E: compact publication changed raw membership (%s != %s)\n' \
		"$raw_members_after_compaction" "$expected_members" >&2
	exit 1
}
selection_kind=$(mysql_query "SELECT source_kind FROM pack_envelopes AS p WHERE p.repo_id = UNHEX('$repo_id') AND p.state = 'sealed' AND p.is_full_snapshot = TRUE AND p.idx_key IS NOT NULL AND p.covering_generation = (SELECT MAX(generation) FROM generations AS g WHERE g.repo_id = UNHEX('$repo_id') AND g.state = 'sealed') ORDER BY (p.source_kind = 'derived') DESC, p.sealed_at DESC, p.pack_id LIMIT 1;")
[ "$selection_kind" = derived ] || {
	printf 'vex receive-pack WAL E2E: compact derived envelope is not the deterministic current selection (%s)\n' \
		"$selection_kind" >&2
	exit 1
}
assert_minio_object_exists "$compact_pack_key"
assert_minio_object_exists "$compact_idx_key"

# Build the narrower default-clone relay artifact after the full compact
# snapshot.  It is deliberately not globally selectable yet: the current
# full-snapshot query must remain on the complete relay pack while a separate
# cache owner consumes this `is_full_snapshot = FALSE` shape.
default_output=$(VEX_COMPOSE_NO_BUILD=1 VEX_COMPOSE_ALREADY_UP=1 \
	"$root/scripts/vex/build-pack.sh" --default-clone-from-receive "$repo_name")
case "$default_output" in
*"vex build-pack: PASS repository=$repo_name"*' pack='*' objects='*' pack_key='*' idx_key='*) ;;
*)
	printf 'vex receive-pack WAL E2E: unexpected default-clone builder output: %s\n' \
		"$default_output" >&2
	exit 1
	;;
esac
default_rows=$(mysql_query "SELECT COUNT(*) FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id') AND source_kind = 'derived' AND state = 'sealed' AND is_full_snapshot = FALSE AND covering_generation = $covering_generation;")
[ "$default_rows" = 1 ] || {
	printf 'vex receive-pack WAL E2E: expected one current partial default-clone envelope, found %s\n' \
		"$default_rows" >&2
	exit 1
}
IFS=$'\t' read -r default_pack_id default_generation default_full default_pack_key default_idx_key <<EOF
$(mysql_query "SELECT LOWER(LEFT(HEX(pack_id), $oid_hex_len)), covering_generation, is_full_snapshot, pack_key, idx_key FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id') AND source_kind = 'derived' AND state = 'sealed' AND is_full_snapshot = FALSE ORDER BY sealed_at DESC, pack_id LIMIT 1;")
EOF
[ -n "$default_pack_id" ] && [ "$default_generation" = "$covering_generation" ] &&
	[ "$default_full" = 0 ] || {
	printf 'vex receive-pack WAL E2E: default-clone derived envelope metadata is invalid: %s:%s:%s\n' \
		"$default_pack_id" "$default_generation" "$default_full" >&2
	exit 1
}
[ "$default_pack_id" != "$pack_id" ] && [ "$default_pack_id" != "$compact_pack_id" ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: default-clone artifact unexpectedly reused a raw/full pack checksum' >&2
	exit 1
}
[ "$default_pack_key" = "git/v1/$repo_id/packs/$default_pack_id.pack" ] &&
	[ "$default_idx_key" = "git/v1/$repo_id/packs/$default_pack_id.idx" ] || {
	printf 'vex receive-pack WAL E2E: default-clone derived keys are non-canonical: %s %s\n' \
		"$default_pack_key" "$default_idx_key" >&2
	exit 1
}
default_pack_mysql_id=$(mysql_oid "$default_pack_id")
default_pack_objects=$(mysql_query "SELECT COUNT(*) FROM pack_objects WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$default_pack_mysql_id');")
[ "$default_pack_objects" = 0 ] || {
	printf 'vex receive-pack WAL E2E: default-clone envelope unexpectedly has %s legacy pack-object rows\n' \
		"$default_pack_objects" >&2
	exit 1
}
assert_minio_object_exists "$default_pack_key"
assert_minio_object_exists "$default_idx_key"
default_index="$scratch/default-clone.idx"
minio_run -e "VEX_S3_OBJECT_KEY=$default_idx_key" minio-init -c '
	set -eu
	mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
	mc cat "vex/$MINIO_BUCKET/$VEX_S3_OBJECT_KEY"
' >"$default_index"
default_index_oids="$scratch/default-clone-index-oids"
"$stock_git" show-index <"$default_index" | awk '{ print $2 }' | LC_ALL=C sort -u >"$default_index_oids"
default_index_members=$(wc -l <"$default_index_oids" | tr -d ' ')
[ "$default_index_members" = "$default_expected_members" ] || {
	printf 'vex receive-pack WAL E2E: default-clone index has %s members, expected heads/tags closure %s\n' \
		"$default_index_members" "$default_expected_members" >&2
	exit 1
}
cmp -s "$default_expected_oids" "$default_index_oids" || {
	printf '%s\n' 'vex receive-pack WAL E2E: default-clone index does not exactly match heads/tags closure' >&2
	diff -u "$default_expected_oids" "$default_index_oids" >&2 || true
	exit 1
}
raw_received_oids="$scratch/raw-received-oids"
mysql_query "SELECT LOWER(LEFT(HEX(oid), $oid_hex_len)) FROM received_pack_objects WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$pack_mysql_id');" |
	LC_ALL=C sort -u >"$raw_received_oids"
comm -23 "$default_index_oids" "$raw_received_oids" >"$scratch/default-not-in-raw"
[ ! -s "$scratch/default-not-in-raw" ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: default-clone index contains an OID absent from the raw receive map' >&2
	exit 1
}
raw_members_after_default=$(mysql_query "SELECT COUNT(*) FROM received_pack_objects WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$pack_mysql_id');")
[ "$raw_members_after_default" = "$expected_members" ] || {
	printf 'vex receive-pack WAL E2E: default-clone publication changed raw membership (%s != %s)\n' \
		"$raw_members_after_default" "$expected_members" >&2
	exit 1
}

# Same content, generation, and S3 keys must be idempotent: a second builder
# may win or lose a concurrent metadata race, but it must leave one sealed
# partial envelope rather than attempting to overwrite a raw/full envelope.
default_repeat_output=$(VEX_COMPOSE_NO_BUILD=1 VEX_COMPOSE_ALREADY_UP=1 \
	"$root/scripts/vex/build-pack.sh" --default-clone-from-receive "$repo_name")
case "$default_repeat_output" in
*"vex build-pack: PASS repository=$repo_name"*' pack='*' objects='*' pack_key='*' idx_key='*) ;;
*)
	printf 'vex receive-pack WAL E2E: default-clone builder was not idempotent: %s\n' \
		"$default_repeat_output" >&2
	exit 1
	;;
esac
default_rows_after_repeat=$(mysql_query "SELECT COUNT(*) FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id') AND source_kind = 'derived' AND state = 'sealed' AND is_full_snapshot = FALSE AND covering_generation = $covering_generation;")
[ "$default_rows_after_repeat" = 1 ] || {
	printf 'vex receive-pack WAL E2E: idempotent default-clone build left %s partial envelopes\n' \
		"$default_rows_after_repeat" >&2
	exit 1
}
selection_after_default=$(mysql_query "SELECT LOWER(LEFT(HEX(pack_id), $oid_hex_len)) FROM pack_envelopes AS p WHERE p.repo_id = UNHEX('$repo_id') AND p.state = 'sealed' AND p.is_full_snapshot = TRUE AND p.idx_key IS NOT NULL AND p.covering_generation = (SELECT MAX(generation) FROM generations AS g WHERE g.repo_id = UNHEX('$repo_id') AND g.state = 'sealed') ORDER BY (p.source_kind = 'derived') DESC, p.sealed_at DESC, p.pack_id LIMIT 1;")
[ "$selection_after_default" = "$compact_pack_id" ] || {
	printf 'vex receive-pack WAL E2E: partial default-clone envelope changed global full relay selection (%s != %s)\n' \
		"$selection_after_default" "$compact_pack_id" >&2
	exit 1
}

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

clone_and_verify_relay() {
	local protocol clone_path trace expected_pack
	local -a cloned_packs

	protocol=$1
	clone_path=$2
	trace=$3
	expected_pack=$4
	# Small fixtures are normally unpacked by the stock client (the default
	# fetch.unpackLimit is 100), which would erase the received pack checksum
	# before this test can assert the relay artifact.  Keep any multi-object
	# response as a pack solely for this wire-level assertion.
	GIT_TRACE_PACKET="$trace" git_https -c fetch.unpackLimit=1 -c "protocol.version=$protocol" \
		clone -q --branch main "$origin" "$clone_path"
	shopt -s nullglob
	cloned_packs=("$clone_path"/.git/objects/pack/pack-*.pack)
	shopt -u nullglob
	[ "${#cloned_packs[@]}" -eq 1 ] || {
		printf 'vex receive-pack WAL E2E: protocol-v%s clone has %s packed object files, expected one selected relay pack\n' \
			"$protocol" "${#cloned_packs[@]}" >&2
		return 1
	}
	[ "${cloned_packs[0]##*/}" = "pack-$expected_pack.pack" ] || {
		printf 'vex receive-pack WAL E2E: protocol-v%s clone received %s instead of selected pack-%s.pack\n' \
			"$protocol" "${cloned_packs[0]##*/}" "$expected_pack" >&2
		return 1
	}
	[ "$("$stock_git" -C "$clone_path" rev-parse HEAD)" = "$main_oid" ] || {
		printf 'vex receive-pack WAL E2E: protocol-v%s clone HEAD differs from sealed main\n' "$protocol" >&2
		return 1
	}
	[ "$("$stock_git" -C "$clone_path" rev-parse refs/tags/fixture-v1)" = "$tag_oid" ] || {
		printf 'vex receive-pack WAL E2E: protocol-v%s clone lost the annotated tag object\n' "$protocol" >&2
		return 1
	}
	[ "$("$stock_git" -C "$clone_path" rev-parse refs/tags/fixture-v1^{})" = "$main_oid" ] || {
		printf 'vex receive-pack WAL E2E: protocol-v%s clone annotated tag does not peel to main\n' "$protocol" >&2
		return 1
	}
	"$stock_git" -C "$clone_path" fsck --no-progress >/dev/null
}

# The raw WAL seed, complete derived snapshot, and narrower default-clone
# artifact are now MySQL/S3-verified. Turn the seed gate off, then use the
# same fault seam as the direct-pack test: passing the following default-branch
# clones proves upload-pack did not invoke git-pack-objects and the received
# client pack checksum proves it selected the partial artifact rather than the
# raw or complete pack. The annotated tag makes protocol-v0's include-tag
# negotiation part of this relay contract too.
restore_gateway
clone_v0="$scratch/clone-v0"
clone_v2="$scratch/clone-v2"
activate_gateway_relay_fault
clone_and_verify_relay 0 "$clone_v0" "$scratch/v0.trace" "$default_pack_id"
clone_and_verify_relay 2 "$clone_v2" "$scratch/v2.trace" "$default_pack_id"
grep -Eq 'clone< version 2$' "$scratch/v2.trace" || {
	printf '%s\n' 'vex receive-pack WAL E2E: stock clone did not negotiate protocol v2' >&2
	exit 1
}

restore_gateway

# A raw WAL seed is a canonical base, not a write-once repository.  Advance
# the same branch with a normal stock-Git receive, then fetch into a reader
# that predates the push.  This forces a fresh server process to resolve the
# raw base plus newly published per-object overlay; the old relay artifacts
# may no longer be advertised as current, but their shape and the raw map must
# survive the metadata state replacement intact.
post_seed_tip_before=$main_oid
printf 'Vex raw-base incremental overlay\n' >>"$clone_v2/history"
"$stock_git" -C "$clone_v2" add history
"$stock_git" -C "$clone_v2" -c commit.gpgSign=false commit -q \
	-m 'Vex raw base incremental overlay'
post_seed_tip=$("$stock_git" -C "$clone_v2" rev-parse HEAD)
git_https -C "$clone_v2" push -q origin HEAD:refs/heads/main
[ "$post_seed_tip" != "$post_seed_tip_before" ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: incremental overlay did not advance main' >&2
	exit 1
}
post_seed_generation=$(mysql_query "
	SELECT COALESCE(MAX(generation), 0) FROM generations
	 WHERE repo_id = UNHEX('$repo_id') AND state = 'sealed';")
[ "$post_seed_generation" -gt "$covering_generation" ] || {
	printf 'vex receive-pack WAL E2E: incremental overlay did not seal a newer generation (%s <= %s)\n' \
		"$post_seed_generation" "$covering_generation" >&2
	exit 1
}
raw_after_overlay=$(mysql_query "
	SELECT CONCAT(state, ':', covering_generation, ':', COUNT(rpo.oid))
	  FROM pack_envelopes AS p
	  JOIN received_pack_objects AS rpo
	    ON rpo.repo_id = p.repo_id AND rpo.pack_id = p.pack_id
	 WHERE p.repo_id = UNHEX('$repo_id')
	   AND p.pack_id = UNHEX('$pack_mysql_id')
	 GROUP BY p.state, p.covering_generation;")
[ "$raw_after_overlay" = "sealed:$covering_generation:$expected_members" ] || {
	printf 'vex receive-pack WAL E2E: normal overlay changed the raw canonical map: %s\n' \
		"$raw_after_overlay" >&2
	exit 1
}
default_after_overlay=$(mysql_query "
	SELECT CONCAT(state, ':', is_full_snapshot)
	  FROM pack_envelopes
	 WHERE repo_id = UNHEX('$repo_id')
	   AND pack_id = UNHEX('$default_pack_mysql_id');")
[ "$default_after_overlay" = 'obsolete:0' ] || {
	printf 'vex receive-pack WAL E2E: normal overlay did not preserve/obsolete the partial relay shape: %s\n' \
		"$default_after_overlay" >&2
	exit 1
}
overlay_objects=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id');")
[ "$overlay_objects" -gt 0 ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: normal overlay did not publish any per-object metadata' >&2
	exit 1
}
overlay_rebuild=$(mysql_query "
	SELECT target_generation FROM pack_rebuild_requests
	 WHERE repo_id = UNHEX('$repo_id');")
[ "$overlay_rebuild" = "$post_seed_generation" ] || {
	printf 'vex receive-pack WAL E2E: normal overlay did not queue the current pack rebuild (%s != %s)\n' \
		"$overlay_rebuild" "$post_seed_generation" >&2
	exit 1
}

# `pull_oid` is deliberately absent from the original heads/tags relay.  Fetch
# it after the overlay, then publish a new branch pointing at that raw-only
# object.  The branch update carries no new object bytes, so acceptance proves
# the ref transaction consults the retained received-pack map rather than
# requiring a legacy per-object metadata row.
raw_branch=raw-base-pull
git_https -C "$clone_v2" fetch -q origin \
	"refs/pull/1/head:refs/remotes/origin/$raw_branch"
[ "$("$stock_git" -C "$clone_v2" rev-parse "refs/remotes/origin/$raw_branch")" = "$pull_oid" ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: raw-only pull ref could not be fetched after overlay' >&2
	exit 1
}
"$stock_git" -C "$clone_v2" branch -f "$raw_branch" "$pull_oid"
git_https -C "$clone_v2" push -q origin \
	"refs/heads/$raw_branch:refs/heads/$raw_branch"
remote_raw_branch=$(git_https ls-remote --refs "$origin" "refs/heads/$raw_branch" |
	awk 'NF == 2 { print $1; exit }')
[ "$remote_raw_branch" = "$pull_oid" ] || {
	printf 'vex receive-pack WAL E2E: raw-only branch target %s differs from %s\n' \
		"$remote_raw_branch" "$pull_oid" >&2
	exit 1
}

# Exercise server-side all-object enumeration in a fresh process.  This is the
# union boundary: raw members have no `objects` rows, while the overlay only
# exists in those rows, so both IDs must be present together.
server_all_objects="$scratch/server-all-objects"
"${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" cat-file --batch-all-objects --batch-check \
	>"$server_all_objects"
grep -q "^$pull_oid " "$server_all_objects" &&
	grep -q "^$post_seed_tip " "$server_all_objects" || {
	printf '%s\n' 'vex receive-pack WAL E2E: all-object enumeration omitted raw or overlay content' >&2
	exit 1
}

# A cold reader must resolve both layers without relying on one of the earlier
# relay clones retaining the old default pack locally.
clone_after_overlay="$scratch/clone-after-overlay"
git_https clone -q --branch main "$origin" "$clone_after_overlay"
[ "$("$stock_git" -C "$clone_after_overlay" rev-parse HEAD)" = "$post_seed_tip" ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: cold clone did not reach the raw-base overlay tip' >&2
	exit 1
}
git_https -C "$clone_after_overlay" fetch -q origin \
	"refs/heads/$raw_branch:refs/remotes/origin/$raw_branch"
[ "$("$stock_git" -C "$clone_after_overlay" rev-parse "refs/remotes/origin/$raw_branch")" = "$pull_oid" ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: cold clone did not fetch the raw-only branch' >&2
	exit 1
}
"$stock_git" -C "$clone_after_overlay" fsck --no-progress >/dev/null

git_https -C "$clone_v0" fetch -q origin refs/heads/main:refs/remotes/origin/main
[ "$("$stock_git" -C "$clone_v0" rev-parse refs/remotes/origin/main)" = "$post_seed_tip" ] || {
	printf '%s\n' 'vex receive-pack WAL E2E: reader fetch did not reach the raw-base overlay tip' >&2
	exit 1
}
"$stock_git" -C "$clone_v0" fsck --no-progress >/dev/null

printf 'vex receive-pack WAL E2E: PASS stock non-thin initial mirror refs=%s raw_objects=%s default_objects=%s raw_pack=%s compact_pack=%s default_pack=%s; raw MySQL map retained, the default heads/tags index is exact and partial, compact S3 pack/idx remains globally selected, protocol-v0/v2 annotated-tag fsck clones received the default pack without git-pack-objects, and a normal overlay supported raw-only ref publication plus cold clone/fsck for %s\n' \
	"$expected_refs" "$expected_members" "$default_expected_members" "$pack_id" \
	"$compact_pack_id" "$default_pack_id" "$origin"
