#!/usr/bin/env bash

# Initialize one explicit FS-backed Vex bare repository for fs-http.py.  This
# keeps the test listener from ever silently serving a classic-bare repo.
set -euo pipefail

usage() {
	printf '%s\n' 'usage: scripts/vex/fs-http-init.sh --git PATH --root DIR --store DIR --repo NAME.git --repo-id 32-hex'
}

git_binary=''
repo_root=''
store_root=''
repo_name=''
repo_id=''
while [ "$#" -gt 0 ]; do
	case "$1" in
	--git|--root|--store|--repo|--repo-id)
		[ "$#" -ge 2 ] || { usage >&2; exit 2; }
		case "$1" in
		--git) git_binary=$2 ;;
		--root) repo_root=$2 ;;
		--store) store_root=$2 ;;
		--repo) repo_name=$2 ;;
		--repo-id) repo_id=$2 ;;
		esac
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		printf 'vex fs-http-init: unknown argument: %s\n' "$1" >&2
		usage >&2
		exit 2
		;;
	esac
done

[ -n "$git_binary" ] && [ -n "$repo_root" ] && [ -n "$store_root" ] && [ -n "$repo_name" ] && [ -n "$repo_id" ] || {
	usage >&2
	exit 2
}
[ -x "$git_binary" ] || { printf 'vex fs-http-init: Git executable not found: %s\n' "$git_binary" >&2; exit 2; }
case "$repo_name" in
*.git) ;;
*) printf 'vex fs-http-init: repository name must end in .git: %s\n' "$repo_name" >&2; exit 2 ;;
esac
case "$repo_name" in
*/*|.*|*..*|*[!A-Za-z0-9._-]*)
	printf 'vex fs-http-init: repository name is unsafe: %s\n' "$repo_name" >&2
	exit 2
	;;
esac
case "$repo_id" in
*[!0123456789abcdefABCDEF]*|'')
	printf 'vex fs-http-init: repo ID must be 32 hexadecimal characters\n' >&2
	exit 2
	;;
esac
[ "${#repo_id}" -eq 32 ] || { printf 'vex fs-http-init: repo ID must be 32 hexadecimal characters\n' >&2; exit 2; }
repo_id=$(printf '%s' "$repo_id" | tr 'A-F' 'a-f')

mkdir -p "$repo_root" "$store_root"
repo_root=$(cd "$repo_root" && pwd -P)
store_root=$(cd "$store_root" && pwd -P)
repo_path="$repo_root/$repo_name"
expected_uri="vex://$repo_id"

if [ -e "$repo_path" ]; then
	[ -f "$repo_path/HEAD" ] || { printf 'vex fs-http-init: existing path is not a bare repository: %s\n' "$repo_path" >&2; exit 1; }
	actual_uri=$("$git_binary" --git-dir="$repo_path" config --get extensions.refstorage || true)
	actual_store=$("$git_binary" --git-dir="$repo_path" config --get vex.storeRoot || true)
	[ "$actual_uri" = "$expected_uri" ] && [ "$actual_store" = "$store_root" ] || {
		printf 'vex fs-http-init: existing repository does not match requested Vex configuration: %s\n' "$repo_path" >&2
		exit 1
	}
	printf 'vex fs-http-init: Vex-FS repository already exists: %s\n' "$repo_path"
	exit 0
fi

GIT_REFERENCE_BACKEND="$expected_uri" GIT_VEX_FS_ROOT="$store_root" \
	"$git_binary" init --bare --ref-format=vex "$repo_path" >/dev/null
"$git_binary" --git-dir="$repo_path" config vex.storeRoot "$store_root"
"$git_binary" --git-dir="$repo_path" config http.receivepack true
"$git_binary" --git-dir="$repo_path" config daemon.export ok
printf 'vex fs-http-init: initialized Vex-FS repository %s (%s)\n' "$repo_path" "$expected_uri"
