#!/usr/bin/env bash

# Compose L3 safety slice: use stock Git to make an unreachable old tip, then
# prove the operator sweep planner protects both the live tip graph and an
# explicit durable MySQL pin.  The planner is intentionally non-mutating.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CONFIG_NOSYSTEM GIT_ATTR_NOSYSTEM \
	GIT_CEILING_DIRECTORIES GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-s3-sweep-plan.sh' >&2
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
	printf 'vex sweep-plan E2E: stock Git executable is not available: %s\n' "$stock_git" >&2
	exit 127
}

compose=(docker compose -f "$docker_dir/compose.yaml" --env-file "$env_file")
ca_file="$docker_dir/certs/rootCA.pem"
health=$(curl --fail --silent --show-error --cacert "$ca_file" \
	"https://localhost:${GIT_HTTPS_PORT}/healthz")
case "$health" in
*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) ;;
*)
	printf 'vex sweep-plan E2E: endpoint did not report active MySQL+S3 storage: %s\n' "$health" >&2
	exit 1
	;;
esac

run_id="$(date +%s)-$$"
repo_name="e2e/sweep-plan-${run_id}.git"
origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
"$root/scripts/vex/seed-empty-repo.sh" "$repo_name" >/dev/null

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-mysql-s3-sweep-plan.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
git_https() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

plan_field() {
	local name=$1
	local output=$2
	local value

	value=$(printf '%s\n' "$output" | sed -n "s/.*${name}=\\([0-9][0-9]*\\).*/\\1/p")
	case "$value" in
	''|*[!0-9]*)
		printf 'vex sweep-plan E2E: invalid %s in planner output: %s\n' "$name" "$output" >&2
		return 1
		;;
	esac
	printf '%s\n' "$value"
}

planner() {
	"${compose[@]}" exec -T git-http /opt/vex-git/vex-sweep-plan \
		--git-dir="/srv/git/$repo_name" "$@"
}

client="$scratch/client"
"$stock_git" init -q "$client"
"$stock_git" -C "$client" config user.name vex-sweep-plan-e2e
"$stock_git" -C "$client" config user.email vex-sweep-plan-e2e@example.invalid
printf 'reachable initial Vex tree\n' >"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'reachable old tip'
"$stock_git" -C "$client" branch -M main
old_tip=$("$stock_git" -C "$client" rev-parse HEAD)
"$stock_git" -C "$client" remote add origin "$origin"
git_https -C "$client" push -q -u origin HEAD:refs/heads/main

# Make a disconnected replacement tip so the old commit/tree/blob graph is a
# real sealed candidate.  This mirrors the force-update retention case that
# pins must protect.
"$stock_git" -C "$client" checkout -q --orphan replacement
"$stock_git" -C "$client" rm -q -rf .
printf 'reachable replacement Vex tree\n' >"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'replacement tip'
"$stock_git" -C "$client" branch -M main
git_https -C "$client" push -q --force origin HEAD:refs/heads/main

before=$(planner)
before_candidates=$(plan_field sealed_candidates "$before")
[ "$before_candidates" -gt 0 ] || {
	printf 'vex sweep-plan E2E: disconnected old graph was not reported as a sealed candidate: %s\n' "$before" >&2
	exit 1
}

pin_reason="retain-force-old-tip"
pin_output=$(planner --pin="${old_tip}:${pin_reason}")
case "$pin_output" in
*'action=added'*"oid=$old_tip"*"reason=$pin_reason"*) ;;
*)
	printf 'vex sweep-plan E2E: unexpected pin output: %s\n' "$pin_output" >&2
	exit 1
	;;
esac
pinned=$(planner)
pinned_candidates=$(plan_field sealed_candidates "$pinned")
pin_roots=$(plan_field pin_roots "$pinned")
[ "$pin_roots" -eq 1 ] && [ "$pinned_candidates" -lt "$before_candidates" ] || {
	printf 'vex sweep-plan E2E: active pin did not protect the old graph (before=%s pinned=%s)\n' \
		"$before" "$pinned" >&2
	exit 1
}

# A stock-Git follow-up push forces the MySQL state adapter to rewrite its
# metadata.  The pin must survive that normal write boundary unchanged.
printf 'reachable post-pin Vex tree\n' >>"$client/README"
"$stock_git" -C "$client" add README
"$stock_git" -C "$client" -c commit.gpgSign=false commit -q -m 'post-pin ref update'
current_tip=$("$stock_git" -C "$client" rev-parse HEAD)
git_https -C "$client" push -q origin HEAD:refs/heads/main
after_push=$(planner)
after_push_candidates=$(plan_field sealed_candidates "$after_push")
after_push_pins=$(plan_field pin_roots "$after_push")
[ "$after_push_pins" -eq 1 ] && [ "$after_push_candidates" -eq "$pinned_candidates" ] || {
	printf 'vex sweep-plan E2E: pin was not preserved by a stock-Git push (pinned=%s after=%s)\n' \
		"$pinned" "$after_push" >&2
	exit 1
}

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????) repo_id=${repository_uri#vex://} ;;
*)
	printf 'vex sweep-plan E2E: repository lacks a Vex UUID: %s\n' "$repository_uri" >&2
	exit 1
	;;
esac
main_oid=$("${compose[@]}" exec -T mysql mysql --protocol=TCP -h 127.0.0.1 \
	-u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT LOWER(HEX(target_oid)) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname = 'refs/heads/main';")
case "$main_oid" in
"$current_tip"000000000000000000000000|"$current_tip") ;;
*)
	printf 'vex sweep-plan E2E: MySQL main tip is %s, expected %s\n' "$main_oid" "$current_tip" >&2
	exit 1
	;;
esac
pin_rows=$("${compose[@]}" exec -T mysql mysql --protocol=TCP -h 127.0.0.1 \
	-u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM object_pins WHERE repo_id = UNHEX('$repo_id') AND reason = '$pin_reason';")
[ "$pin_rows" -eq 1 ] || {
	printf 'vex sweep-plan E2E: durable MySQL pin row was lost after push (rows=%s)\n' "$pin_rows" >&2
	exit 1
}

unpin_output=$(planner --unpin="${old_tip}:${pin_reason}")
case "$unpin_output" in
*'action=removed'*"oid=$old_tip"*"reason=$pin_reason"*) ;;
*)
	printf 'vex sweep-plan E2E: unexpected unpin output: %s\n' "$unpin_output" >&2
	exit 1
	;;
esac
unpinned=$(planner)
unpinned_candidates=$(plan_field sealed_candidates "$unpinned")
[ "$unpinned_candidates" -gt "$after_push_candidates" ] || {
	printf 'vex sweep-plan E2E: unpin did not return old graph to the conservative sweep plan (after=%s unpinned=%s)\n' \
		"$after_push" "$unpinned" >&2
	exit 1
}

printf 'vex sweep-plan E2E: PASS stock Git force-update created %s sealed candidates; MySQL pin protected the old graph across a later push, then unpin restored candidates for %s\n' \
	"$before_candidates" "$origin"
