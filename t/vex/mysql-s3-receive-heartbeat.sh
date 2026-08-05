#!/usr/bin/env bash

# Compose gate for the durable receive heartbeat.  It holds a real MySQL+S3
# epoch past a short recovery grace while refreshing it, proves recovery leaves
# that live epoch alone, then SIGKILLs the holder and proves the same recovery
# path reclaims the stale epoch.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-receive-heartbeat.sh' >&2
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
	printf '%s\n' 'vex receive heartbeat E2E: gateway did not become MySQL+S3 ready' >&2
	return 1
}

mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" \
		-p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" -e "$1"
}

assert_equal() {
	local expected=$1
	local actual=$2
	local description=$3

	[ "$expected" = "$actual" ] || {
		printf 'vex receive heartbeat E2E: %s: expected %s, got %s\n' \
			"$description" "$expected" "$actual" >&2
		return 1
	}
}

wait_for_storage
run_id="$(date +%s)-$$"
repo_id=$(printf 'vex-receive-heartbeat-%s' "$run_id" | sha256sum | cut -c1-32)
holder_pid=
holder_log="/tmp/vex-epoch-lease-$repo_id.log"

cleanup() {
	local result=$?

	trap - EXIT HUP INT TERM
	set +e
	if [ -n "$holder_pid" ]; then
		"${compose[@]}" exec -T git-http /bin/sh -c 'kill -KILL "$1"' sh "$holder_pid"
	fi
	"${compose[@]}" exec -T git-http /bin/sh -c 'rm -f -- "$1"' sh "$holder_log"
	exit "$result"
}
trap cleanup EXIT HUP INT TERM

holder_pid=$("${compose[@]}" exec -T \
	-e GIT_VEX_EPOCH_RECOVERY_SECONDS=2 \
	git-http /bin/sh -c '
		nohup /opt/vex-git/vex-epoch-lease --repo-id="$1" --hold-seconds=30 \
			>"$2" 2>&1 < /dev/null &
		printf "%s\n" "$!"
	' sh "$repo_id" "$holder_log")
case "$holder_pid" in
''|*[!0-9]*)
	printf 'vex receive heartbeat E2E: could not start holder: %s\n' "$holder_pid" >&2
	exit 1
	;;
esac

for attempt in $(seq 1 20); do
	open=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open' AND receive_heartbeat_at IS NOT NULL;")
	[ "$open" = 1 ] && break
	sleep 1
done
assert_equal 1 "$open" 'holder created an active receive heartbeat'

first_heartbeat=$(mysql_query "SELECT DATE_FORMAT(receive_heartbeat_at, '%Y-%m-%d %H:%i:%s.%f') FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open';")
sleep 3
second_heartbeat=$(mysql_query "SELECT DATE_FORMAT(receive_heartbeat_at, '%Y-%m-%d %H:%i:%s.%f') FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open';")
[ -n "$first_heartbeat" ] && [ "$first_heartbeat" != "$second_heartbeat" ] || {
	printf '%s\n' 'vex receive heartbeat E2E: active holder did not advance its durable heartbeat' >&2
	exit 1
}

recovery=$("${compose[@]}" exec -T -e GIT_VEX_EPOCH_RECOVERY_SECONDS=2 \
	git-http /opt/vex-git/vex-epoch-lease --repo-id="$repo_id" --recover-seconds=2)
case "$recovery" in
*'state=recovered recovered=0 age_seconds=2'*) ;;
*)
	printf 'vex receive heartbeat E2E: recovery reclaimed a live epoch: %s\n' "$recovery" >&2
	exit 1
	;;
esac

"${compose[@]}" exec -T git-http /bin/sh -c 'kill -KILL "$1"' sh "$holder_pid"
holder_pid=
sleep 3
recovery=$("${compose[@]}" exec -T -e GIT_VEX_EPOCH_RECOVERY_SECONDS=2 \
	git-http /opt/vex-git/vex-epoch-lease --repo-id="$repo_id" --recover-seconds=2)
case "$recovery" in
*'state=recovered recovered=1 age_seconds=2'*) ;;
*)
	printf 'vex receive heartbeat E2E: recovery did not reclaim killed holder: %s\n' "$recovery" >&2
	exit 1
	;;
esac

open=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'open';")
aborted=$(mysql_query "SELECT COUNT(*) FROM generations WHERE repo_id = UNHEX('$repo_id') AND state = 'aborted';")
assert_equal 0 "$open" 'killed holder has no remaining open epoch'
assert_equal 1 "$aborted" 'killed holder becomes one aborted epoch'

printf '%s\n' 'vex receive heartbeat E2E: PASS live epoch refreshed past grace, recovery skipped it, and a killed holder was reclaimed'
