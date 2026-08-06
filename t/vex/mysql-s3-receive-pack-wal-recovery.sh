#!/usr/bin/env bash

# Compose E2E for recovery of the opt-in raw receive-pack WAL path.  Each
# fault point SIGKILLs receive-pack only after it has made a durable raw pack
# map and its S3 pack/idx bytes, but before the MySQL seal can expose refs.
# A fresh gateway then runs immediate, test-only recovery.  It must preserve
# the unborn HEAD scaffold while aborting the generation and compensating
# both possible S3 locations (staging and final).
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

# Keep the HTTPS client stock even when this script is run from Git's test
# harness, which exports build-local Git paths for the server side.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-receive-pack-wal-recovery.sh' >&2
	exit 2
fi

up_args=()
case "${VEX_COMPOSE_NO_BUILD:-}" in
1|true|yes) up_args+=(--no-build) ;;
esac
if [ "${#up_args[@]}" -eq 0 ]; then
	"$root/scripts/vex/up.sh" >/dev/null
else
	"$root/scripts/vex/up.sh" "${up_args[@]}" >/dev/null
fi
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
	printf '%s\n' 'vex receive-pack WAL recovery E2E: gateway did not become MySQL+S3 ready' >&2
	return 1
}

if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git)
fi
[ -x "$stock_git" ] || {
	printf 'vex receive-pack WAL recovery E2E: stock Git executable is unavailable: %s\n' "$stock_git" >&2
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

assert_equal() {
	local expected=$1
	local actual=$2
	local description=$3

	[ "$expected" = "$actual" ] || {
		printf 'vex receive-pack WAL recovery E2E: %s: expected %s, got %s\n' \
			"$description" "$expected" "$actual" >&2
		return 1
	}
}

# MySQL stores SHA-1 IDs in BINARY(32) with twelve zero bytes appended.
mysql_oid() {
	case "${#1}" in
	40) printf '%s000000000000000000000000' "$1" ;;
	64) printf '%s' "$1" ;;
	*)
		printf 'vex receive-pack WAL recovery E2E: unsupported object ID: %s\n' "$1" >&2
		exit 2
		;;
	esac
}

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

gateway_restore_wal=''
gateway_restore_kill=''
gateway_restore_recovery=''
gateway_restore_test_recovery=''
gateway_override_active=0

recreate_gateway() {
	local wal=${1:-}
	local kill_at=${2:-}
	local recovery=${3:-}
	local test_recovery=${4:-}

	env GIT_VEX_RECEIVE_PACK_WAL="$wal" \
		GIT_VEX_TEST_KILL_AT="$kill_at" \
		GIT_VEX_EPOCH_RECOVERY_SECONDS="$recovery" \
		GIT_VEX_TEST_EPOCH_RECOVERY_SECONDS="$test_recovery" \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
	gateway_override_active=1
	wait_for_storage
}

restore_gateway() {
	if [ "$gateway_override_active" -eq 0 ]; then
		return 0
	fi
	recreate_gateway "$gateway_restore_wal" "$gateway_restore_kill" \
		"$gateway_restore_recovery" "$gateway_restore_test_recovery"
	gateway_override_active=0
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-receive-pack-wal-recovery.XXXXXX")
cleanup() {
	local result=$?

	trap - EXIT HUP INT TERM
	set +e
	restore_gateway
	rm -rf -- "$scratch"
	exit "$result"
}
trap cleanup EXIT HUP INT TERM

wait_for_storage
gateway_restore_wal=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${GIT_VEX_RECEIVE_PACK_WAL:-}"')
gateway_restore_kill=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${GIT_VEX_TEST_KILL_AT:-}"')
gateway_restore_recovery=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${GIT_VEX_EPOCH_RECOVERY_SECONDS:-}"')
gateway_restore_test_recovery=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${GIT_VEX_TEST_EPOCH_RECOVERY_SECONDS:-}"')

run_id="$(date +%s)-$$"
fixture="$scratch/fixture"
mirror="$scratch/fixture.git"
"$stock_git" init -q "$fixture"
"$stock_git" -C "$fixture" config user.name vex-receive-pack-wal-recovery-e2e
"$stock_git" -C "$fixture" config user.email vex-receive-pack-wal-recovery-e2e@example.invalid
for revision in 1 2 3; do
	printf 'Vex receive-pack WAL recovery fixture revision %s\n' "$revision" >>"$fixture/history"
	"$stock_git" -C "$fixture" add history
	"$stock_git" -C "$fixture" -c commit.gpgSign=false commit -q \
		-m "Vex receive-pack WAL recovery fixture $revision"
done
"$stock_git" -C "$fixture" branch -M main
"$stock_git" -C "$fixture" branch side HEAD~1
"$stock_git" -C "$fixture" tag -a -m 'Vex receive-pack WAL recovery fixture tag' fixture-v1
"$stock_git" clone --mirror --no-local -q "$fixture" "$mirror"
"$stock_git" --git-dir="$mirror" repack -adf --write-bitmap-index

case "$("$stock_git" --git-dir="$mirror" rev-parse --show-object-format)" in
sha1) oid_hex_len=40 ;;
sha256) oid_hex_len=64 ;;
*)
	printf '%s\n' 'vex receive-pack WAL recovery E2E: unsupported fixture object format' >&2
	exit 2
	;;
esac
main_oid=$("$stock_git" --git-dir="$mirror" rev-parse refs/heads/main)

run_fault_point() {
	local point=$1
	local slug repo_name origin repository_uri repo_id raw_row generation pack_id
	local pack_state pack_source_kind map_rows loose_rows nonhead_refs ref_updates
	local baseline_open baseline_aborted baseline_refs baseline_ref_updates
	local open_after_fault state_after_fault final_pack final_idx staging_pack staging_idx
	local clone remote_refs local_refs

	slug=$(printf '%s' "$point" | tr '-' '_')
	repo_name="e2e/receive-pack-wal-recovery-${run_id}-${slug}.git"
	origin="https://localhost:${GIT_HTTPS_PORT}/$repo_name"

	# Keep a normal receive grace during the actual push.  The fresh recovery
	# gateway below overrides it with the explicit, test-only zero-second grace.
	recreate_gateway 1 "$point" 900 ''
	"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null
	repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
		"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
	case "$repository_uri" in
	vex://????????????????????????????????) repo_id=${repository_uri#vex://} ;;
	*)
		printf 'vex receive-pack WAL recovery E2E: repository lacks a Vex UUID: %s\n' \
			"$repository_uri" >&2
		return 1
		;;
	esac

	baseline_open=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open';")
	baseline_aborted=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'aborted';")
	baseline_refs=$(mysql_query "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id');")
	baseline_ref_updates=$(mysql_query "SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id');")

	if git_https --git-dir="$mirror" push --no-thin --mirror "$origin" \
		>"$scratch/$slug-fault.stdout" 2>"$scratch/$slug-fault.stderr"
	then
		printf 'vex receive-pack WAL recovery E2E: %s push unexpectedly survived SIGKILL\n' "$point" >&2
		return 1
	fi

	# The raw map is deliberately committed before its enclosing ref epoch can
	# publish.  This is the failure record recovery uses to find the exact pack.
	raw_row=$(mysql_query "SELECT receive_generation, LOWER(LEFT(HEX(pack_id), $oid_hex_len)), state, source_kind FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id') AND source_kind = 'receive' AND state = 'building' ORDER BY receive_generation DESC LIMIT 1;")
	[ -n "$raw_row" ] || {
		printf 'vex receive-pack WAL recovery E2E: %s left no building raw receive envelope\n' "$point" >&2
		return 1
	}
	IFS=$'\t' read -r generation pack_id pack_state pack_source_kind <<EOF
$raw_row
EOF
	[ -n "$generation" ] && [ -n "$pack_id" ] && [ "$pack_state" = building ] &&
		[ "$pack_source_kind" = receive ] || {
		printf 'vex receive-pack WAL recovery E2E: %s raw envelope is malformed\n' "$point" >&2
		return 1
	}
	state_after_fault=$(mysql_query "SELECT state FROM generations WHERE repo_id = UNHEX('$repo_id') AND generation = $generation;")
	assert_equal open "$state_after_fault" "$point building generation state"
	map_rows=$(mysql_query "SELECT COUNT(*) FROM received_pack_objects WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$(mysql_oid "$pack_id")');")
	[ "$map_rows" -gt 0 ] || {
		printf 'vex receive-pack WAL recovery E2E: %s left no raw object map rows\n' "$point" >&2
		return 1
	}
	loose_rows=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id');")
	assert_equal 0 "$loose_rows" "$point raw receive loose-object rows"
	nonhead_refs=$(mysql_query "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname <> 'HEAD';")
	assert_equal 0 "$nonhead_refs" "$point failed receive non-HEAD refs"
	ref_updates=$(mysql_query "SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id');")
	assert_equal "$baseline_ref_updates" "$ref_updates" "$point failed receive ref audit"
	open_after_fault=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open';")
	assert_equal "$((baseline_open + 1))" "$open_after_fault" "$point failed receive open generation count"

	staging_pack="git/v1/$repo_id/staging/$generation/packs/$pack_id.pack"
	staging_idx="git/v1/$repo_id/staging/$generation/packs/$pack_id.idx"
	final_pack="git/v1/$repo_id/packs/$pack_id.pack"
	final_idx="git/v1/$repo_id/packs/$pack_id.idx"
	case "$point" in
	after-receive-pack-stage)
		assert_minio_object_exists "$staging_pack"
		assert_minio_object_exists "$staging_idx"
		assert_minio_object_missing "$final_pack"
		assert_minio_object_missing "$final_idx"
		;;
	after-receive-pack-promote)
		assert_minio_object_missing "$staging_pack"
		assert_minio_object_missing "$staging_idx"
		assert_minio_object_exists "$final_pack"
		assert_minio_object_exists "$final_idx"
		;;
	*)
		printf 'vex receive-pack WAL recovery E2E: unsupported fault point: %s\n' "$point" >&2
		return 2
		;;
	esac

	# This process is deliberately new: opening the Vex repository as part of a
	# stock read runs recovery with immediate test-only expiry.  No timing sleep
	# is needed or permitted in the assertion contract.
	recreate_gateway 1 '' '' 0
	git_https ls-remote "$origin" >"$scratch/$slug-recovery-ls-remote"

	assert_equal "$baseline_open" \
		"$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open';")" \
		"$point recovered open generation count"
	assert_equal "$((baseline_aborted + 1))" \
		"$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'aborted';")" \
		"$point recovered aborted generation count"
	assert_equal aborted \
		"$(mysql_query "SELECT state FROM generations WHERE repo_id = UNHEX('$repo_id') AND generation = $generation;")" \
		"$point recovered generation state"
	assert_equal 0 \
		"$(mysql_query "SELECT COUNT(*) FROM pack_envelopes WHERE repo_id = UNHEX('$repo_id') AND source_kind = 'receive' AND receive_generation = $generation;")" \
		"$point recovered raw envelope rows"
	assert_equal 0 \
		"$(mysql_query "SELECT COUNT(*) FROM received_pack_objects WHERE repo_id = UNHEX('$repo_id') AND pack_id = UNHEX('$(mysql_oid "$pack_id")');")" \
		"$point recovered raw object map rows"
	assert_equal "$baseline_refs" \
		"$(mysql_query "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id');")" \
		"$point recovered refs"
	assert_equal 0 \
		"$(mysql_query "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname <> 'HEAD';")" \
		"$point recovered dangling refs"
	assert_equal "$baseline_ref_updates" \
		"$(mysql_query "SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id');")" \
		"$point recovered ref audit"
	assert_equal 0 \
		"$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id');")" \
		"$point recovered loose-object rows"
	assert_minio_object_missing "$staging_pack"
	assert_minio_object_missing "$staging_idx"
	assert_minio_object_missing "$final_pack"
	assert_minio_object_missing "$final_idx"

	# Recovery must also leave the repository eligible for the same first raw
	# seed; this catches a dangling full-snapshot guard in addition to its data
	# cleanup assertions above.
	recreate_gateway 1 '' 900 ''
	git_https --git-dir="$mirror" push --no-thin --mirror "$origin"
	local_refs=$("$stock_git" --git-dir="$mirror" \
		for-each-ref --format='%(objectname)%09%(refname)' | LC_ALL=C sort)
	remote_refs=$(git_https ls-remote --refs "$origin" | LC_ALL=C sort)
	assert_equal "$local_refs" "$remote_refs" "$point retry refs"
	clone="$scratch/clone-$slug"
	git_https clone -q "$origin" "$clone"
	assert_equal "$main_oid" "$("$stock_git" -C "$clone" rev-parse HEAD)" \
		"$point retry clone HEAD"
	"$stock_git" -C "$clone" fsck --no-progress >/dev/null

	printf 'vex receive-pack WAL recovery E2E: PASS point=%s raw-map=%s generation=%s; recovery aborted metadata, deleted S3 pair, and retry clone+fsck passed\n' \
		"$point" "$map_rows" "$generation"
}

run_fault_point after-receive-pack-stage
run_fault_point after-receive-pack-promote

restore_gateway
printf '%s\n' 'vex receive-pack WAL recovery E2E: PASS 2/2 raw staging/final crash offsets reclaimed without dangling refs'
