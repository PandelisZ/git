#!/usr/bin/env bash

# Compose crash-recovery E2E.  A stock Git client pushes over HTTPS while the
# actual receive worker is SIGKILLed at two durable transaction boundaries.
# A bounded, configured recovery pass must leave no ref/audit/object metadata
# or S3 bytes behind before the exact same commit can be retried.
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
	printf '%s\n' 'usage: t/vex/mysql-s3-receive-hard-kill.sh' >&2
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
	printf '%s\n' 'vex hard-kill E2E: gateway did not become MySQL+S3 ready' >&2
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
	printf 'vex hard-kill E2E: stock Git executable is unavailable: %s\n' "$stock_git" >&2
	exit 127
}

mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" \
		-p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" -e "$1"
}

minio_run() {
	"${compose[@]}" run --rm --no-deps --entrypoint /bin/sh "$@"
}

minio_prefix_count() {
	minio_run -e "VEX_S3_PREFIX=$1" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc stat "vex/$MINIO_BUCKET" >/dev/null
		objects=$(mc find "vex/$MINIO_BUCKET/$VEX_S3_PREFIX" 2>/dev/null || :)
		if test -z "$objects"
		then
			printf "0\n"
		else
			set -f
			set -- $objects
			printf "%s\n" "$#"
		fi
	'
}

assert_equal() {
	local expected=$1
	local actual=$2
	local description=$3

	[ "$expected" = "$actual" ] || {
		printf 'vex hard-kill E2E: %s: expected %s, got %s\n' \
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
		printf 'vex hard-kill E2E: unsupported object ID: %s\n' "$1" >&2
		exit 2
		;;
	esac
}

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

gateway_override_active=0
recreate_gateway() {
	local kill_at=${1:-}
	local recovery_seconds=${2:-}

	env GIT_VEX_TEST_KILL_AT="$kill_at" \
		GIT_VEX_EPOCH_RECOVERY_SECONDS="$recovery_seconds" \
		GIT_VEX_TEST_EPOCH_RECOVERY_SECONDS= \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null
	gateway_override_active=1
	wait_for_storage
}

restore_gateway() {
	if [ "$gateway_override_active" -eq 0 ]; then
		return 0
	fi
	recreate_gateway '' ''
	gateway_override_active=0
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-hard-kill.XXXXXX")
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
run_id="$(date +%s)-$$"

run_fault_point() {
	local point=$1
	local slug repo_name origin repository_uri repo_id client tip tip_mysql verifier
	local baseline_published baseline_objects baseline_claims baseline_open baseline_heartbeats
	local baseline_aborted baseline_audit baseline_final baseline_staging
	local ref_rows published objects claims open heartbeat_rows aborted audit tip_rows
	local final_after_fault staging_after_fault final_after_recovery staging_after_recovery
	local remote_tip published_after_retry fault_env recovery_env

	slug=$(printf '%s' "$point" | tr '-' '_')
	repo_name="e2e/receive-hard-kill-${run_id}-${slug}.git"
	origin="https://localhost:${GIT_HTTPS_PORT}/$repo_name"
	"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null
	repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
		"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
	case "$repository_uri" in
	vex://????????????????????????????????) repo_id=${repository_uri#vex://} ;;
	*)
		printf 'vex hard-kill E2E: repository lacks a Vex UUID: %s\n' \
			"$repository_uri" >&2
		return 1
		;;
	esac

	baseline_published=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND state = 'published';")
	baseline_objects=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id');")
	baseline_claims=$(mysql_query "SELECT COUNT(*) FROM generation_objects WHERE repo_id = UNHEX('$repo_id');")
	baseline_open=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open';")
	baseline_heartbeats=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open' AND receive_heartbeat_at IS NOT NULL;")
	baseline_aborted=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'aborted';")
	baseline_audit=$(mysql_query "SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id');")
	baseline_final=$(minio_prefix_count "git/v1/$repo_id/objects/")
	baseline_staging=$(minio_prefix_count "git/v1/$repo_id/staging/")

	client="$scratch/client-$slug"
	"$stock_git" init -q "$client"
	"$stock_git" -C "$client" config user.name vex-hard-kill-e2e
	"$stock_git" -C "$client" config user.email vex-hard-kill-e2e@example.invalid
	printf 'receive-worker hard-kill at %s\n' "$point" >"$client/README"
	"$stock_git" -C "$client" add README
	"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m "Vex hard kill $point"
	"$stock_git" -C "$client" branch -M main
	tip=$("$stock_git" -C "$client" rev-parse HEAD)
	tip_mysql=$(mysql_oid "$tip")
	"$stock_git" -C "$client" remote add origin "$origin"

	# The hook lives in git-receive-pack, not the client or Compose controller.
	recreate_gateway "$point" ''
	fault_env=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_TEST_KILL_AT:-}"')
	assert_equal "$point" "$fault_env" "fault hook environment"
	if git_https -C "$client" push --porcelain -u origin HEAD:refs/heads/main \
		>"$scratch/$slug-fault.stdout" 2>"$scratch/$slug-fault.stderr"
	then
		printf 'vex hard-kill E2E: %s push unexpectedly survived SIGKILL\n' "$point" >&2
		return 1
	fi

	# The killed worker has made no ref or audit entry, but its durable epoch is
	# still open until a fresh server process performs the TTL-bounded reclaim.
	ref_rows=$(mysql_query "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname = 'refs/heads/main';")
	published=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND state = 'published';")
	objects=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id');")
	claims=$(mysql_query "SELECT COUNT(*) FROM generation_objects WHERE repo_id = UNHEX('$repo_id');")
	open=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open';")
	heartbeat_rows=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open' AND receive_heartbeat_at IS NOT NULL;")
	audit=$(mysql_query "SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id');")
	tip_rows=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$tip_mysql');")
	assert_equal 0 "$ref_rows" "$point failed receive main ref rows"
	assert_equal "$baseline_published" "$published" "$point failed receive published rows"
	assert_equal "$baseline_audit" "$audit" "$point failed receive audit rows"
	[ "$open" -gt "$baseline_open" ] || {
		printf 'vex hard-kill E2E: %s did not leave an open receive epoch\n' "$point" >&2
		return 1
	}
	[ "$heartbeat_rows" -gt "$baseline_heartbeats" ] || {
		printf 'vex hard-kill E2E: %s did not leave a durable receive heartbeat\n' "$point" >&2
		return 1
	}
	[ "$objects" -gt "$baseline_objects" ] || {
		printf 'vex hard-kill E2E: %s did not leave durable quarantine metadata\n' "$point" >&2
		return 1
	}
	[ "$claims" -gt "$baseline_claims" ] || {
		printf 'vex hard-kill E2E: %s did not leave durable generation claims\n' "$point" >&2
		return 1
	}
	[ "$tip_rows" -eq 1 ] || {
		printf 'vex hard-kill E2E: %s did not stage its commit metadata\n' "$point" >&2
		return 1
	}
	final_after_fault=$(minio_prefix_count "git/v1/$repo_id/objects/")
	staging_after_fault=$(minio_prefix_count "git/v1/$repo_id/staging/")
	case "$point" in
	before-ref-commit)
		assert_equal "$baseline_final" "$final_after_fault" "pre-commit final S3 object count"
		[ "$staging_after_fault" -gt "$baseline_staging" ] || {
			printf '%s\n' 'vex hard-kill E2E: pre-commit SIGKILL left no staging S3 bytes' >&2
			return 1
		}
		;;
	after-blob-promote)
		[ "$final_after_fault" -gt "$baseline_final" ] || {
			printf '%s\n' 'vex hard-kill E2E: post-promotion SIGKILL left no final S3 bytes to reclaim' >&2
			return 1
		}
		assert_equal "$baseline_staging" "$staging_after_fault" "post-promotion staging S3 object count"
		;;
	esac

	# Let the last durable heartbeat pass the configured one-second grace, then
	# restart the real gateway. A normal stock read activates recovery for this
	# repository; the next receive runs only after that override is removed.
	sleep 2
	recreate_gateway '' 1
	recovery_env=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_EPOCH_RECOVERY_SECONDS:-}"')
	assert_equal 1 "$recovery_env" "recovery grace environment"
	git_https ls-remote "$origin" >"$scratch/$slug-recovery-ls-remote"

	ref_rows=$(mysql_query "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname = 'refs/heads/main';")
	published=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND state = 'published';")
	objects=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id');")
	claims=$(mysql_query "SELECT COUNT(*) FROM generation_objects WHERE repo_id = UNHEX('$repo_id');")
	open=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open';")
	heartbeat_rows=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open' AND receive_heartbeat_at IS NOT NULL;")
	aborted=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'aborted';")
	audit=$(mysql_query "SELECT COUNT(*) FROM ref_updates WHERE repo_id = UNHEX('$repo_id');")
	final_after_recovery=$(minio_prefix_count "git/v1/$repo_id/objects/")
	staging_after_recovery=$(minio_prefix_count "git/v1/$repo_id/staging/")
	assert_equal 0 "$ref_rows" "$point recovered main ref rows"
	assert_equal "$baseline_published" "$published" "$point recovered published rows"
	assert_equal "$baseline_objects" "$objects" "$point recovered object rows"
	assert_equal "$baseline_claims" "$claims" "$point recovered generation claims"
	assert_equal "$baseline_open" "$open" "$point recovered open epochs"
	assert_equal "$baseline_heartbeats" "$heartbeat_rows" "$point recovered active heartbeats"
	assert_equal "$((baseline_aborted + 1))" "$aborted" "$point recovered aborted epochs"
	assert_equal "$baseline_audit" "$audit" "$point recovered audit rows"
	assert_equal "$baseline_final" "$final_after_recovery" "$point recovered final S3 object count"
	assert_equal "$baseline_staging" "$staging_after_recovery" "$point recovered staging S3 object count"

	# Do not leave a one-second grace in the next receive: a new transaction may
	# legitimately have an open epoch while its helper process attaches.
	restore_gateway
	git_https -C "$client" push -q -u origin HEAD:refs/heads/main
	remote_tip=$(git_https ls-remote "$origin" refs/heads/main | awk '{print $1}')
	assert_equal "$tip" "$remote_tip" "$point retry remote tip"
	published_after_retry=$(mysql_query "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND oid = UNHEX('$tip_mysql') AND state = 'published';")
	assert_equal 1 "$published_after_retry" "$point retry published commit rows"
	verifier="$scratch/verifier-$slug"
	git_https clone -q "$origin" "$verifier"
	assert_equal "$tip" "$("$stock_git" -C "$verifier" rev-parse refs/remotes/origin/main)" "$point retry clone tip"
	"$stock_git" -C "$verifier" fsck --no-progress >/dev/null

	printf 'vex hard-kill E2E: PASS offset=%s recovered one TTL-expired epoch; refs/audit/metadata/S3 reset and retry clone fsck passed\n' \
		"$point"
}

run_fault_point before-ref-commit
run_fault_point after-blob-promote

printf '%s\n' 'vex hard-kill E2E: PASS 2/2 SIGKILL offsets recovered through MySQL state lock and S3 compensation'
