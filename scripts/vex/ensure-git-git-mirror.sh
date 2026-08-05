#!/usr/bin/env bash

# Maintain the local, reusable source mirror used by the git/git seed and
# push-stress workflows. The mirror is a host-side operator cache, never a
# Vex source of truth.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)

usage() {
	cat <<'EOF'
usage: scripts/vex/ensure-git-git-mirror.sh [--mirror PATH] [--upstream URL] [--refresh]

Ensure a local bare --mirror clone of git/git exists. A cached mirror is
reused without network access by default; --refresh runs `git remote update
--prune` against its recorded upstream before returning.

Defaults:
  --mirror   $VEX_GIT_MIRROR or t/vex/out/git-git-mirror.git
  --upstream $VEX_GIT_GIT_UPSTREAM or https://github.com/git/git
EOF
}

die() {
	printf 'vex ensure-git-git-mirror: %s\n' "$*" >&2
	exit 2
}

mirror=${VEX_GIT_MIRROR:-"$repo_root/t/vex/out/git-git-mirror.git"}
upstream=${VEX_GIT_GIT_UPSTREAM:-https://github.com/git/git}
refresh=0

while [ "$#" -gt 0 ]; do
	case "$1" in
	--mirror)
		[ "$#" -ge 2 ] || die '--mirror requires a path'
		mirror=$2
		shift 2
		;;
	--mirror=*)
		mirror=${1#--mirror=}
		shift
		;;
	--upstream)
		[ "$#" -ge 2 ] || die '--upstream requires a URL'
		upstream=$2
		shift 2
		;;
	--upstream=*)
		upstream=${1#--upstream=}
		shift
		;;
	--refresh)
		refresh=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		die "unknown option: $1"
		;;
	esac
done

case "$upstream" in
''|*$'\n'*|*$'\r'*) die 'upstream must be a single URL or transport path' ;;
esac
case "$mirror" in
''|*$'\n'*|*$'\r'*) die 'mirror must be a single filesystem path' ;;
esac

if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git || true)
fi
[ -n "$stock_git" ] && [ -x "$stock_git" ] || {
	printf 'vex ensure-git-git-mirror: stock Git executable is unavailable: %s\n' \
		"$stock_git" >&2
	exit 127
}

# Do not let an in-tree Git test invocation redirect the operator's stock
# client to source-built helpers or an unrelated object database.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CEILING_DIRECTORIES

mirror_parent=$(dirname -- "$mirror")
mkdir -p -- "$mirror_parent"
mirror_parent=$(CDPATH= cd -- "$mirror_parent" && pwd -P)
mirror="$mirror_parent/$(basename -- "$mirror")"

if [ -e "$mirror" ]; then
	[ -d "$mirror" ] || die "mirror path is not a directory: $mirror"
	is_bare=$("$stock_git" --git-dir="$mirror" rev-parse --is-bare-repository 2>/dev/null || true)
	[ "$is_bare" = true ] || die "mirror is not a bare Git repository: $mirror"
	actual_upstream=$("$stock_git" --git-dir="$mirror" remote get-url origin 2>/dev/null || true)
	[ "$actual_upstream" = "$upstream" ] || {
		printf '%s\n' 'vex ensure-git-git-mirror: existing mirror origin does not match the requested upstream.' >&2
		printf '%s\n' 'Use a distinct --mirror path or explicitly choose its matching --upstream.' >&2
		exit 2
	}
	if [ "$refresh" -eq 1 ]; then
		"$stock_git" --git-dir="$mirror" remote update --prune
	fi
else
	"$stock_git" clone --mirror "$upstream" "$mirror"
fi

head_ref=$("$stock_git" --git-dir="$mirror" symbolic-ref -q HEAD 2>/dev/null || true)
case "$head_ref" in
refs/heads/*) ;;
*) die "mirror HEAD is not a branch symbolic ref: ${head_ref:-detached}" ;;
esac
"$stock_git" --git-dir="$mirror" show-ref --verify --quiet "$head_ref" ||
	die "mirror HEAD target does not exist: $head_ref"

printf 'vex ensure-git-git-mirror: ready mirror=%s head=%s\n' "$mirror" "$head_ref"
