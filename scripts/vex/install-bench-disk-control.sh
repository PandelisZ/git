#!/usr/bin/env bash

# Install the one classic-bare repository permitted for the fixed same-listener
# benchmark control.  It deliberately copies through the running git-http
# container's named Compose volume rather than adding a host bind mount or a
# second HTTP server.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
docker_dir="$repo_root/vex/docker"
compose_file="$docker_dir/compose.yaml"
env_file="$docker_dir/.env"
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"

bench_volume_key=vex_bench_disk
bench_root=/srv/vex-bench-disk
bench_repository=git-git-disk.git
bench_route=/__vex_bench_disk__/git-git-disk.git

usage() {
	cat <<'EOF'
usage: scripts/vex/install-bench-disk-control.sh --source HOST_BARE_GIT_DIR
       [--origin HTTPS_URL]

Copy an existing classic-bare git-git-disk.git repository into the running
git-http service's named vex_bench_disk Compose volume, atomically replace the
fixed control path where practical, and verify the authenticated smart-HTTPS
read route.

The only accepted served path is:
  https://<same-host-and-port>/__vex_bench_disk__/git-git-disk.git

This command never starts, rebuilds, or recreates Compose services. The
git-http service must already be running.
EOF
}

die() {
	printf 'vex install-bench-disk-control: %s\n' "$*" >&2
	exit 2
}

require_uint() {
	case "$2" in
	''|*[!0-9]*) die "$1 must be a non-negative integer: $2" ;;
	esac
}

source_repository=''
origin=''

while [ "$#" -gt 0 ]; do
	case "$1" in
	--source)
		[ "$#" -ge 2 ] || die '--source requires a path'
		source_repository=$2
		shift 2
		;;
	--source=*)
		source_repository=${1#--source=}
		shift
		;;
	--origin)
		[ "$#" -ge 2 ] || die '--origin requires an HTTPS URL'
		origin=$2
		shift 2
		;;
	--origin=*)
		origin=${1#--origin=}
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

[ -n "$source_repository" ] || die '--source is required'
case "$source_repository:$origin" in
*$'\n'*|*$'\r'*) die 'paths and origins must be single-line values' ;;
esac

set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

if [ -z "$origin" ]; then
	origin="https://localhost:${GIT_HTTPS_PORT}/${bench_route#/}"
fi

validate_origin() {
	local authority_and_path authority path

	case "$origin" in
	https://*/*) ;;
	*) die "--origin must be an HTTPS URL for the fixed disk control: $origin" ;;
	esac
	case "$origin" in
	*'?'*|*'#'*|*'@'*|*' '*|*$'\t'*)
		die "--origin must not contain credentials, a query, fragment, or whitespace: $origin"
		;;
	esac
	authority_and_path=${origin#https://}
	authority=${authority_and_path%%/*}
	path=/${authority_and_path#*/}
	[ -n "$authority" ] || die "--origin has no HTTPS authority: $origin"
	[ "$path" = "$bench_route" ] ||
		die "--origin must address exactly $bench_route: $origin"
}
validate_origin

if ! source_repository=$(CDPATH= cd -- "$source_repository" && pwd -P); then
	die "--source is not an existing directory: $source_repository"
fi
[ "$(basename -- "$source_repository")" = "$bench_repository" ] ||
	die "--source must be named $bench_repository: $source_repository"

if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git || true)
fi
[ -n "$stock_git" ] && [ -x "$stock_git" ] || {
	printf 'vex install-bench-disk-control: stock Git executable is unavailable: %s\n' "$stock_git" >&2
	exit 127
}
command -v docker >/dev/null 2>&1 || {
	printf '%s\n' 'vex install-bench-disk-control: docker with the Compose plugin is required' >&2
	exit 127
}
docker compose version >/dev/null 2>&1 || {
	printf '%s\n' 'vex install-bench-disk-control: docker compose is unavailable' >&2
	exit 127
}

# Keep the operator's normal global Git configuration (for example proxy
# settings), but never inherit an in-tree test repository or object database.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CEILING_DIRECTORIES

source_is_bare=$("$stock_git" --git-dir="$source_repository" rev-parse --is-bare-repository 2>/dev/null || true)
[ "$source_is_bare" = true ] || die "--source is not a bare Git repository: $source_repository"

# A control must be ordinary Git all the way through.  Do not merely rely on
# the path being outside /srv/git: a Vex ref-storage config here would make the
# result neither a disk baseline nor a valid gateway control.
source_refstorage_status=0
"$stock_git" --git-dir="$source_repository" config --local --get-all extensions.refstorage \
	>/dev/null 2>&1 || source_refstorage_status=$?
case "$source_refstorage_status" in
0) die '--source has extensions.refstorage; the disk control must be classic bare Git' ;;
1) ;;
*) die "could not inspect --source extensions.refstorage (Git exit $source_refstorage_status)" ;;
esac

source_head_ref=$("$stock_git" --git-dir="$source_repository" symbolic-ref -q HEAD) ||
	die '--source HEAD must be a symbolic branch ref'
case "$source_head_ref" in
refs/heads/*) ;;
*) die "--source HEAD is not a branch ref: $source_head_ref" ;;
esac
"$stock_git" --git-dir="$source_repository" show-ref --verify --quiet "$source_head_ref" ||
	die "--source HEAD target does not exist: $source_head_ref"
source_head_oid=$("$stock_git" --git-dir="$source_repository" rev-parse --verify 'HEAD^{commit}') ||
	die '--source HEAD does not resolve to a commit'
"$stock_git" --git-dir="$source_repository" fsck --connectivity-only --no-progress >/dev/null ||
	die '--source fails Git connectivity validation'

ca_file="$docker_dir/certs/rootCA.pem"
[ -f "$ca_file" ] || die 'local TLS CA is missing; run scripts/vex/up.sh first'
case "${VEX_HTTP_BASIC_USER:-}:${VEX_HTTP_BASIC_PASSWORD:-}" in
:) die 'Compose HTTP credentials are missing from vex/docker/.env' ;;
esac
auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
compose=(docker compose -f "$compose_file" --env-file "$env_file")

list_local_refs() {
	local git_dir=$1 output=$2
	"$stock_git" --git-dir="$git_dir" for-each-ref --format='%(objectname) %(refname)' |
		LC_ALL=C sort >"$output"
}

git_authenticated() {
	"$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" "$@"
}

list_remote_refs() {
	local output=$1
	git_authenticated ls-remote --refs "$origin" |
		awk 'NF == 2 { print $1 " " $2 }' |
		LC_ALL=C sort >"$output"
}

workdir=$(mktemp -d "${TMPDIR:-/tmp}/vex-bench-disk-install.XXXXXX")
source_refs="$workdir/source-refs"
remote_refs="$workdir/remote-refs"
verification_repository="$workdir/https-read.git"
git_container=''
remote_stage=''
remote_previous=''
remote_lock=''
remote_lock_acquired=0

cleanup() {
	local status=$?

	trap - EXIT HUP INT TERM
	set +e
	if [ "$remote_lock_acquired" -eq 1 ] && [ -n "$git_container" ]; then
		# Never delete remote_previous here.  The remote swap keeps an old
		# control there only if rollback could not complete, and preserving it is
		# safer than turning a recoverable swap failure into data loss.
		"${compose[@]}" exec -T git-http /bin/sh -c \
			'rm -rf -- "$1"; rmdir -- "$2" 2>/dev/null || true' sh \
			"$remote_stage" "$remote_lock" >/dev/null 2>&1 || true
	fi
	if [ -n "${workdir:-}" ]; then
		rm -rf -- "$workdir"
	fi
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

list_local_refs "$source_repository" "$source_refs"
[ -s "$source_refs" ] || die '--source has no refs'
source_ref_count=$(wc -l <"$source_refs" | tr -d '[:space:]')
require_uint 'source ref count' "$source_ref_count"

git_container=$("${compose[@]}" ps --status running -q git-http)
[ -n "$git_container" ] ||
	die 'git-http is not running; start the existing Compose stack with scripts/vex/up.sh first'

# Check the actual running mount rather than inferring a host volume path.  A
# successful install is meaningful only when this container owns a Compose
# named volume whose logical key is vex_bench_disk.
mount_info=$(docker inspect --format '{{range .Mounts}}{{if eq .Destination "/srv/vex-bench-disk"}}{{.Type}}|{{.Name}}{{end}}{{end}}' \
	"$git_container")
mount_type=${mount_info%%|*}
mount_name=${mount_info#*|}
[ "$mount_type" = volume ] && [ -n "$mount_name" ] ||
	die 'the running git-http container does not mount /srv/vex-bench-disk as a named volume'
mounted_volume_key=$(docker volume inspect --format '{{index .Labels "com.docker.compose.volume"}}' "$mount_name" 2>/dev/null || true)
[ "$mounted_volume_key" = "$bench_volume_key" ] ||
	die "the /srv/vex-bench-disk mount is not Compose volume $bench_volume_key: $mount_name"

run_id="$(date +%s)-$$-$RANDOM"
remote_stage="$bench_root/.git-git-disk.install-$run_id"
remote_previous="$bench_root/.git-git-disk.previous-$run_id"
remote_lock="$bench_root/.git-git-disk.install.lock"
remote_live="$bench_root/$bench_repository"

"${compose[@]}" exec -T git-http /bin/sh -ec '
	test -d "$1"
	if ! mkdir -- "$2"; then
		printf "vex install-bench-disk-control: another install appears to be active: %s\\n" "$2" >&2
		exit 1
	fi
' sh "$bench_root" "$remote_lock"
remote_lock_acquired=1

"${compose[@]}" exec -T git-http /bin/sh -ec '
	root=$1
	stage=$2
	test "$(dirname -- "$stage")" = "$root"
	mkdir -m 0700 -- "$stage"
' sh "$bench_root" "$remote_stage"

# The trailing /. copies the bare repository's contents into the pre-created
# stage directory; the named volume receives no host bind mount or Vex data.
docker cp "$source_repository/." "$git_container:$remote_stage"

"${compose[@]}" exec -T git-http /bin/sh -ec '
	root=$1
	stage=$2
	expected_head_ref=$3
	test "$(dirname -- "$stage")" = "$root"
	test -f "$stage/HEAD"
	test -d "$stage/objects"
	test "$(/opt/vex-git/git --git-dir="$stage" rev-parse --is-bare-repository)" = true
	if /opt/vex-git/git --git-dir="$stage" config --local --get-all extensions.refstorage >/dev/null 2>&1; then
		printf "%s\\n" "vex install-bench-disk-control: staged repository has extensions.refstorage" >&2
		exit 1
	fi
	head_ref=$(/opt/vex-git/git --git-dir="$stage" symbolic-ref -q HEAD)
	test "$head_ref" = "$expected_head_ref"
	/opt/vex-git/git --git-dir="$stage" show-ref --verify --quiet "$head_ref"
	/opt/vex-git/git --git-dir="$stage" fsck --connectivity-only --no-progress >/dev/null
	/opt/vex-git/git --git-dir="$stage" config --local vex.benchmarkcontrol true
	/opt/vex-git/git --git-dir="$stage" config --local http.receivepack true
	/opt/vex-git/git --git-dir="$stage" config --local http.getanyfile false
	test "$(/opt/vex-git/git --git-dir="$stage" config --local --get vex.benchmarkcontrol)" = true
	test "$(/opt/vex-git/git --git-dir="$stage" config --local --get http.receivepack)" = true
	test "$(/opt/vex-git/git --git-dir="$stage" config --local --get http.getanyfile)" = false
	if /opt/vex-git/git --git-dir="$stage" config --local --get-all extensions.refstorage >/dev/null 2>&1; then
		printf "%s\\n" "vex install-bench-disk-control: marker update introduced extensions.refstorage" >&2
		exit 1
	fi
' sh "$bench_root" "$remote_stage" "$source_head_ref"

# Both renames are within vex_bench_disk.  Requests can observe a short 404
# between them, but never a half-copied repository.  If moving the staged
# repository fails, the shell trap restores the previous live directory.
"${compose[@]}" exec -T git-http /bin/sh -ec '
	root=$1
	stage=$2
	live=$3
	previous=$4
	test "$(dirname -- "$stage")" = "$root"
	test "$(dirname -- "$live")" = "$root"
	test "$(dirname -- "$previous")" = "$root"
	test -d "$stage"
	test ! -e "$previous"
	test ! -L "$previous"
	moved_previous=0
	installed=0
	rollback() {
		status=$?
		trap - EXIT HUP INT TERM
		if [ "$installed" -ne 1 ] && [ "$moved_previous" -eq 1 ] && \
			[ ! -e "$live" ] && [ -e "$previous" ]; then
			mv -- "$previous" "$live" || \
				printf "%s\\n" "vex install-bench-disk-control: could not restore previous control" >&2
		fi
		exit "$status"
	}
	trap rollback EXIT HUP INT TERM
	if [ -e "$live" ] || [ -L "$live" ]; then
		mv -- "$live" "$previous"
		moved_previous=1
	fi
	mv -- "$stage" "$live"
	installed=1
	rm -rf -- "$previous"
	trap - EXIT HUP INT TERM
' sh "$bench_root" "$remote_stage" "$remote_live" "$remote_previous"

# Verify discovery and the upload-pack RPC through the exact authenticated
# HTTPS URL.  The shallow fetch avoids transferring the full git/git history
# a second time while still proving the same smart-HTTP read endpoint that the
# clone benchmark uses.
list_remote_refs "$remote_refs"
if ! cmp -s "$source_refs" "$remote_refs"; then
	printf '%s\n' 'vex install-bench-disk-control: served refs differ from --source (first 160 diff lines):' >&2
	diff -u "$source_refs" "$remote_refs" | sed -n '1,160p' >&2 || true
	exit 1
fi
remote_head_oid=$(git_authenticated ls-remote "$origin" HEAD |
	awk 'NF == 2 && $2 == "HEAD" { print $1; exit }')
[ "$remote_head_oid" = "$source_head_oid" ] ||
	die "served HEAD differs from --source ($remote_head_oid != $source_head_oid)"

"$stock_git" init --bare --quiet "$verification_repository"
verification_ref=refs/vex-benchmark-install/verified-head
git_authenticated -c protocol.version=2 --git-dir="$verification_repository" \
	fetch --quiet --no-tags --depth=1 "$origin" "$source_head_ref:$verification_ref"
verification_head_oid=$("$stock_git" --git-dir="$verification_repository" rev-parse --verify "$verification_ref^{commit}")
[ "$verification_head_oid" = "$source_head_oid" ] ||
	die "authenticated smart-HTTP fetch differs from --source ($verification_head_oid != $source_head_oid)"
"$stock_git" --git-dir="$verification_repository" fsck --connectivity-only --no-progress >/dev/null

# Do not leave a stale lock after a fully verified install.  Failed swaps keep
# their previous directory for manual recovery, but their lock/stage cleanup is
# handled by the EXIT trap above.
"${compose[@]}" exec -T git-http /bin/sh -ec '
	test ! -e "$1"
	test ! -L "$1"
	rmdir -- "$2"
' sh "$remote_stage" "$remote_lock"
remote_lock_acquired=0

printf 'vex install-bench-disk-control: PASS source=%s volume=%s origin=%s refs=%s head=%s https_read=shallow-fetch\n' \
	"$source_repository" "$bench_volume_key" "$origin" "$source_ref_count" "$source_head_oid"
