#!/usr/bin/env bash

# Run a stock-Git smart-HTTP client against a Vex-FS server.  This is a local
# protocol gate, intentionally separate from the Docker classic-bare bootstrap
# and from the future MySQL/MinIO deployment.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)

usage() {
	printf '%s\n' 'usage: scripts/vex/fs-http-smoke.sh [--git VEX_GIT] [--stock-git CLIENT_GIT] [--read-only] [--keep]'
}

server_git="$repo_root/git"
stock_git_bin=''
read_only=0
keep=0
while [ "$#" -gt 0 ]; do
	case "$1" in
	--git|--stock-git)
		[ "$#" -ge 2 ] || { usage >&2; exit 2; }
		if [ "$1" = --git ]; then server_git=$2; else stock_git_bin=$2; fi
		shift 2
		;;
	--read-only)
		read_only=1
		shift
		;;
	--keep)
		keep=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		printf 'vex fs-http smoke: unknown argument: %s\n' "$1" >&2
		usage >&2
		exit 2
		;;
	esac
done

[ -x "$server_git" ] || { printf 'vex fs-http smoke: Vex Git executable not found: %s\n' "$server_git" >&2; exit 2; }
[ -n "$stock_git_bin" ] || stock_git_bin=$server_git
[ -x "$stock_git_bin" ] || { printf 'vex fs-http smoke: stock Git executable not found: %s\n' "$stock_git_bin" >&2; exit 2; }
command -v python3 >/dev/null || { printf '%s\n' 'vex fs-http smoke: python3 is required' >&2; exit 2; }
command -v curl >/dev/null || { printf '%s\n' 'vex fs-http smoke: curl is required' >&2; exit 2; }

# The in-tree test harness exports its build directory as GIT_EXEC_PATH.  A
# stock client must discover its own remote helpers rather than accidentally
# loading the in-tree helper set.
stock_git() {
	env -u GIT_EXEC_PATH "$stock_git_bin" "$@"
}

work=$(mktemp -d "${TMPDIR:-/tmp}/vex-fs-http-smoke.XXXXXX")
server_pid=''
server_log="$work/server.log"
cleanup() {
	status=$?
	if [ -n "$server_pid" ]; then
		kill "$server_pid" >/dev/null 2>&1 || true
		wait "$server_pid" >/dev/null 2>&1 || true
	fi
	if [ "$status" -ne 0 ] && [ -s "$server_log" ]; then
		printf '%s\n' 'vex fs-http smoke: server log follows:' >&2
		tail -n 80 "$server_log" >&2 || true
	fi
	if [ "$keep" -eq 0 ]; then
		rm -rf -- "$work"
	else
		printf 'vex fs-http smoke: retained artifacts at %s\n' "$work" >&2
	fi
}
trap cleanup EXIT HUP INT TERM

http_root="$work/repos"
store_root="$work/store"
repo_name='fs-smoke.git'
repo_id='00000000000000000000000000000031'
repo_path="$http_root/$repo_name"
uri="vex://$repo_id"
ready_file="$work/ready-url"

"$script_dir/fs-http-init.sh" \
	--git "$server_git" \
	--root "$http_root" \
	--store "$store_root" \
	--repo "$repo_name" \
	--repo-id "$repo_id" >/dev/null

vex_git() {
	GIT_REFERENCE_BACKEND="$uri" GIT_VEX_FS_ROOT="$store_root" \
		"$server_git" -C "$repo_path" "$@"
}

make_commit() {
	content=$1
	parent=${2:-}
	printf '%s\n' "$content" >"$work/README"
	blob=$(vex_git hash-object -w "$work/README")
	tree=$(printf '100644 blob %s\tREADME\n' "$blob" | vex_git mktree)
	args=(commit-tree "$tree")
	if [ -n "$parent" ]; then
		args+=(-p "$parent")
	fi
	printf '%s\n' "$content" | \
		GIT_AUTHOR_NAME=vex-fs-server \
		GIT_AUTHOR_EMAIL=vex-fs-server@example.invalid \
		GIT_COMMITTER_NAME=vex-fs-server \
		GIT_COMMITTER_EMAIL=vex-fs-server@example.invalid \
		GIT_REFERENCE_BACKEND="$uri" GIT_VEX_FS_ROOT="$store_root" \
		"$server_git" -C "$repo_path" "${args[@]}"
}

thin_manifest() {
	(
		cd "$repo_path"
		# receive-pack may leave its zero-byte auto-maintenance lock behind.
		# It is neither a Git object nor a ref; any nonempty lock or any other
		# file below the thin object/ref stubs remains a hard failure.
		find objects refs -type f \
			! \( -path objects/maintenance.lock -size 0 \) \
			-exec cksum {} \; | LC_ALL=C sort
	)
}

assert_no_classic_bytes() {
	current_manifest=$(thin_manifest)
	if [ "$current_manifest" != "$thin_baseline" ]; then
		printf '%s\n' 'vex fs-http smoke: server repository gained classic object/ref files; refusing to call this Vex evidence' >&2
		printf '%s\n' "baseline: $thin_baseline" >&2
		printf '%s\n' "actual: $current_manifest" >&2
		exit 1
	fi
}

thin_baseline=$(thin_manifest)
tip_one=$(make_commit 'Vex FS smart-HTTP seed')
vex_git update-ref refs/heads/main "$tip_one"
vex_git symbolic-ref HEAD refs/heads/main
assert_no_classic_bytes

python3 "$script_dir/fs-http.py" \
	--git "$server_git" \
	--root "$http_root" \
	--store "$store_root" \
	--ready-file "$ready_file" >"$server_log" 2>&1 &
server_pid=$!

for _ in $(seq 1 100); do
	[ -s "$ready_file" ] && break
	sleep 0.1
done
[ -s "$ready_file" ] || { printf '%s\n' 'vex fs-http smoke: server did not become ready' >&2; exit 1; }
origin=$(tr -d '\r\n' <"$ready_file")/$repo_name
health=$(curl --fail --silent --show-error "${origin%/$repo_name}/healthz")
case "$health" in
*'"vex_storage_active": true'*) ;;
*)
	printf 'vex fs-http smoke: endpoint did not attest to Vex FS storage: %s\n' "$health" >&2
	exit 1
	;;
esac
case "$health" in
*'"storage_backend": "fs"'*) ;;
*)
	printf 'vex fs-http smoke: endpoint did not report the FS storage backend: %s\n' "$health" >&2
	exit 1
	;;
esac

stock_git clone -q "$origin" "$work/client"
actual_one=$(stock_git -C "$work/client" rev-parse HEAD)
[ "$actual_one" = "$tip_one" ] || { printf '%s\n' 'vex fs-http smoke: clone did not receive Vex seed tip' >&2; exit 1; }

tip_two=$(make_commit 'Vex FS smart-HTTP fetch' "$tip_one")
vex_git update-ref refs/heads/main "$tip_two" "$tip_one"
stock_git -C "$work/client" fetch -q origin main
actual_two=$(stock_git -C "$work/client" rev-parse FETCH_HEAD)
[ "$actual_two" = "$tip_two" ] || { printf '%s\n' 'vex fs-http smoke: fetch did not receive the Vex tip' >&2; exit 1; }
stock_git -C "$work/client" fsck --no-progress >/dev/null
assert_no_classic_bytes

if [ "$read_only" -eq 1 ]; then
	printf 'vex fs-http smoke: PASS Vex-FS stock-Git clone/fetch (%s)\n' "$origin"
	exit 0
fi

stock_git -C "$work/client" config user.name vex-fs-stock-client
stock_git -C "$work/client" config user.email vex-fs-stock-client@example.invalid
printf 'stock Git receiver check\n' >"$work/client/STOCK-PUSH"
stock_git -C "$work/client" add STOCK-PUSH
stock_git -C "$work/client" -c commit.gpgSign=false commit -q -m 'stock Git Vex FS push'
expected_push=$(stock_git -C "$work/client" rev-parse HEAD)
stock_git -C "$work/client" push -q origin HEAD:refs/heads/stock-client
actual_push=$(vex_git rev-parse refs/heads/stock-client)
[ "$actual_push" = "$expected_push" ] || { printf '%s\n' 'vex fs-http smoke: Vex ref did not match stock-client push' >&2; exit 1; }
# `git fsck` currently reaches file-ODB-only maintenance code.  Prove the
# received graph through the Vex reader instead, while the generic fsck path
# is completed separately.
vex_git cat-file -e "$actual_push^{commit}"
vex_git cat-file -e "$actual_push^{tree}"
assert_no_classic_bytes
printf 'vex fs-http smoke: PASS Vex-FS stock-Git clone/fetch/push (%s)\n' "$origin"
