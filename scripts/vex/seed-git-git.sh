#!/usr/bin/env bash

# Seed the real git/git fixture through the normal stock-Git HTTPS receive-pack
# path. This is an operator workflow for an already-running local Vex Compose
# topology; it never imports object bytes by writing MySQL or MinIO directly.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
docker_dir="$repo_root/vex/docker"
compose_file="$docker_dir/compose.yaml"
env_file="$docker_dir/.env"
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"

usage() {
	cat <<'EOF'
usage: scripts/vex/seed-git-git.sh [--mirror PATH] [--upstream URL] [--refresh]
                                    [--origin HTTPS_URL] [--disk-control PATH]
                                    [--replace]

Import a faithful git/git mirror into an already-active Vex MySQL+S3 Compose
repository at git/git.git, make a matching local disk-control bare repository,
seal the incoming immutable Vex pack/idx pair, and write
t/vex/out/seed-manifest.json.

The import itself is a normal stock-Git HTTPS `push --mirror`. --replace is
required before changing a non-empty target whose refs differ from the mirror.
The script never starts Compose and never deletes the host disk-control
repository; it installs a validated copy into the separate benchmark volume.

Defaults:
  --mirror       $VEX_GIT_MIRROR or t/vex/out/git-git-mirror.git
  --upstream     $VEX_GIT_GIT_UPSTREAM or https://github.com/git/git
  --origin       $GIT_VEX_ORIGIN or https://localhost:$GIT_HTTPS_PORT/git/git.git
  --disk-control t/vex/out/git-git-disk.git
EOF
}

die() {
	printf 'vex seed-git-git: %s\n' "$*" >&2
	exit 2
}

require_uint() {
	case "$2" in
	''|*[!0-9]*) die "$1 must be a non-negative integer: $2" ;;
	esac
}

require_sha256() {
	case "$1" in
	????????????????????????????????????????????????????????????????) ;;
	*) die "invalid SHA-256 digest returned by MinIO: $1" ;;
	esac
	case "$1" in
	*[!0-9a-f]*) die "invalid SHA-256 digest returned by MinIO: $1" ;;
	esac
}

absolute_path() {
	local input=$1 parent

	parent=$(dirname -- "$input")
	mkdir -p -- "$parent"
	parent=$(CDPATH= cd -- "$parent" && pwd -P)
	printf '%s/%s\n' "$parent" "$(basename -- "$input")"
}

mirror=${VEX_GIT_MIRROR:-"$repo_root/t/vex/out/git-git-mirror.git"}
upstream=${VEX_GIT_GIT_UPSTREAM:-https://github.com/git/git}
origin=''
disk_control="$repo_root/t/vex/out/git-git-disk.git"
refresh=0
replace_target=0

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
	--origin)
		[ "$#" -ge 2 ] || die '--origin requires an HTTPS URL'
		origin=$2
		shift 2
		;;
	--origin=*)
		origin=${1#--origin=}
		shift
		;;
	--disk-control)
		[ "$#" -ge 2 ] || die '--disk-control requires a path'
		disk_control=$2
		shift 2
		;;
	--disk-control=*)
		disk_control=${1#--disk-control=}
		shift
		;;
	--replace)
		replace_target=1
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

case "$mirror:$upstream:$disk_control" in
*$'\n'*|*$'\r'*) die 'paths and upstream must be single-line values' ;;
esac

set -a
# The local Compose file contains only KEY=VALUE assignments. It supplies the
# loopback endpoint and the one shared test push credential; neither is ever
# written into the manifest.
# shellcheck disable=SC1090
. "$env_file"
set +a

if [ -z "$origin" ]; then
	origin=${GIT_VEX_ORIGIN:-"https://localhost:${GIT_HTTPS_PORT}/git/git.git"}
fi
case "$origin" in
https://*/git/git.git) ;;
*) die "--origin must be the HTTPS git/git endpoint: $origin" ;;
esac
origin_authority=${origin#https://}
origin_authority=${origin_authority%%/*}
[ -n "$origin_authority" ] || die "--origin has no HTTPS authority: $origin"
# Keep the matched disk control on the same listener as the Vex candidate.
# The installer independently rejects any URL that is not this exact fixed
# namespace, then proves authenticated discovery and upload-pack reads.
disk_origin="https://${origin_authority}/__vex_bench_disk__/git-git-disk.git"

if [ -n "${VEX_STOCK_GIT:-}" ]; then
	stock_git=$VEX_STOCK_GIT
elif [ -x /usr/bin/git ]; then
	stock_git=/usr/bin/git
else
	stock_git=$(command -v git || true)
fi
[ -n "$stock_git" ] && [ -x "$stock_git" ] || {
	printf 'vex seed-git-git: stock Git executable is unavailable: %s\n' "$stock_git" >&2
	exit 127
}
command -v docker >/dev/null 2>&1 || {
	printf '%s\n' 'vex seed-git-git: docker with the Compose plugin is required' >&2
	exit 127
}
docker compose version >/dev/null 2>&1 || {
	printf '%s\n' 'vex seed-git-git: docker compose is unavailable' >&2
	exit 127
}
command -v curl >/dev/null 2>&1 || {
	printf '%s\n' 'vex seed-git-git: curl is required for the active-stack check' >&2
	exit 127
}
command -v python3 >/dev/null 2>&1 || {
	printf '%s\n' 'vex seed-git-git: python3 is required for the redacted manifest' >&2
	exit 127
}

# Do not let an in-tree Git test invocation redirect the operator's stock
# client to source-built helpers or an unrelated object database. Deliberately
# retain normal user Git configuration for upstream credentials.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
	GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_EXEC_PATH \
	GIT_TEMPLATE_DIR GIT_CEILING_DIRECTORIES

ca_file="$docker_dir/certs/rootCA.pem"
[ -f "$ca_file" ] || die 'local TLS CA is missing; run scripts/vex/up.sh first'
compose=(docker compose -f "$compose_file" --env-file "$env_file")

storage_is_ready() {
	local health

	health=$(curl --connect-timeout 2 --max-time 5 --fail --silent --show-error --cacert "$ca_file" \
		"https://localhost:${GIT_HTTPS_PORT}/healthz" 2>/dev/null) || return 1
	case "$health" in
	*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) return 0 ;;
	*) return 1 ;;
	esac
}

wait_for_storage() {
	local attempt

	for attempt in $(seq 1 60); do
		if storage_is_ready; then
			return 0
		fi
		sleep 1
	done
	return 1
}

wait_for_storage ||
	die 'Compose is not actively serving the Vex MySQL+S3 data plane; run scripts/vex/up.sh first'

# A git/git-scale stock-Git push has a much larger Content-Length, can run
# longer than the normal small-fixture CGI deadline, and must not be mistaken
# for a hard-killed epoch while its pack WAL is still active. Keep these
# configuration changes entirely in the running gateway: recreate once with
# the seed bounds and receive-pack WAL gate, then recreate again with the
# exact previous values before reporting success or returning an error.
seed_request_limit=8589934592
seed_timeout_seconds=3600
seed_epoch_recovery_seconds=3600
gateway_restore_limit=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${VEX_HTTP_MAX_REQUEST_BYTES:-}"')
# Older already-running gateway containers predate explicit timeout variables.
# Read their effective Python defaults so a seed can upgrade/recreate them
# safely rather than rejecting an otherwise healthy active stack.
gateway_restore_cgi_timeout=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${VEX_HTTP_CGI_TIMEOUT_SECONDS:-300}"')
gateway_restore_io_timeout=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${VEX_HTTP_IO_TIMEOUT_SECONDS:-300}"')
gateway_restore_epoch_recovery=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${GIT_VEX_EPOCH_RECOVERY_SECONDS-}"')
gateway_restore_receive_pack_wal=$("${compose[@]}" exec -T git-http /bin/sh -c \
	'printf %s "${GIT_VEX_RECEIVE_PACK_WAL-}"')
gateway_effective_epoch_recovery=${gateway_restore_epoch_recovery:-900}
require_uint 'active git-http VEX_HTTP_MAX_REQUEST_BYTES' "$gateway_restore_limit"
require_uint 'active git-http VEX_HTTP_CGI_TIMEOUT_SECONDS' "$gateway_restore_cgi_timeout"
require_uint 'active git-http VEX_HTTP_IO_TIMEOUT_SECONDS' "$gateway_restore_io_timeout"
require_uint 'active git-http GIT_VEX_EPOCH_RECOVERY_SECONDS' "$gateway_effective_epoch_recovery"
gateway_seed_limit=$gateway_restore_limit
gateway_seed_cgi_timeout=$gateway_restore_cgi_timeout
gateway_seed_io_timeout=$gateway_restore_io_timeout
gateway_seed_epoch_recovery=$gateway_effective_epoch_recovery
gateway_seed_receive_pack_wal=1
if [ "$gateway_seed_limit" -lt "$seed_request_limit" ]; then
	gateway_seed_limit=$seed_request_limit
fi
if [ "$gateway_seed_cgi_timeout" -lt "$seed_timeout_seconds" ]; then
	gateway_seed_cgi_timeout=$seed_timeout_seconds
fi
if [ "$gateway_seed_io_timeout" -lt "$seed_timeout_seconds" ]; then
	gateway_seed_io_timeout=$seed_timeout_seconds
fi
if [ "$gateway_seed_epoch_recovery" -lt "$seed_epoch_recovery_seconds" ]; then
	gateway_seed_epoch_recovery=$seed_epoch_recovery_seconds
fi
gateway_override_active=0
scratch=''

restore_gateway_limits() {
	[ "$gateway_override_active" -eq 1 ] || return 0
	if ! env VEX_HTTP_MAX_REQUEST_BYTES="$gateway_restore_limit" \
		VEX_HTTP_CGI_TIMEOUT_SECONDS="$gateway_restore_cgi_timeout" \
		VEX_HTTP_IO_TIMEOUT_SECONDS="$gateway_restore_io_timeout" \
		GIT_VEX_EPOCH_RECOVERY_SECONDS="$gateway_restore_epoch_recovery" \
		GIT_VEX_RECEIVE_PACK_WAL="$gateway_restore_receive_pack_wal" \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null; then
		printf '%s\n' 'vex seed-git-git: failed to restore the git-http gateway limits' >&2
		return 1
	fi
	if ! wait_for_storage; then
		printf '%s\n' 'vex seed-git-git: git-http did not become healthy after restoring its limits' >&2
		return 1
	fi
	gateway_override_active=0
}

raise_gateway_limits_for_seed() {
	local observed_limit observed_cgi_timeout observed_io_timeout observed_epoch_recovery observed_receive_pack_wal

	if [ "$gateway_restore_limit" -ge "$seed_request_limit" ] && \
		[ "$gateway_restore_cgi_timeout" -ge "$seed_timeout_seconds" ] && \
		[ "$gateway_restore_io_timeout" -ge "$seed_timeout_seconds" ] && \
		[ "$gateway_effective_epoch_recovery" -ge "$seed_epoch_recovery_seconds" ] && \
		[ "$gateway_restore_receive_pack_wal" = "$gateway_seed_receive_pack_wal" ]; then
		return 0
	fi
	gateway_override_active=1
	if ! env VEX_HTTP_MAX_REQUEST_BYTES="$gateway_seed_limit" \
		VEX_HTTP_CGI_TIMEOUT_SECONDS="$gateway_seed_cgi_timeout" \
		VEX_HTTP_IO_TIMEOUT_SECONDS="$gateway_seed_io_timeout" \
		GIT_VEX_EPOCH_RECOVERY_SECONDS="$gateway_seed_epoch_recovery" \
		GIT_VEX_RECEIVE_PACK_WAL="$gateway_seed_receive_pack_wal" \
		"${compose[@]}" up -d --no-build --no-deps --force-recreate git-http >/dev/null; then
		return 1
	fi
	if ! wait_for_storage; then
		return 1
	fi
	observed_limit=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${VEX_HTTP_MAX_REQUEST_BYTES:-}"')
	observed_cgi_timeout=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${VEX_HTTP_CGI_TIMEOUT_SECONDS:-}"')
	observed_io_timeout=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${VEX_HTTP_IO_TIMEOUT_SECONDS:-}"')
	observed_epoch_recovery=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_EPOCH_RECOVERY_SECONDS:-900}"')
	observed_receive_pack_wal=$("${compose[@]}" exec -T git-http /bin/sh -c \
		'printf %s "${GIT_VEX_RECEIVE_PACK_WAL:-}"')
	[ "$observed_limit" = "$gateway_seed_limit" ] && \
		[ "$observed_cgi_timeout" = "$gateway_seed_cgi_timeout" ] && \
		[ "$observed_io_timeout" = "$gateway_seed_io_timeout" ] && \
		[ "$observed_epoch_recovery" = "$gateway_seed_epoch_recovery" ] && \
		[ "$observed_receive_pack_wal" = "$gateway_seed_receive_pack_wal" ]
}

cleanup() {
	local status=$? restore_status=0

	trap - EXIT HUP INT TERM
	set +e
	restore_gateway_limits || restore_status=$?
	if [ -n "${scratch:-}" ]; then
		rm -rf -- "$scratch"
	fi
	if [ "$status" -eq 0 ] && [ "$restore_status" -ne 0 ]; then
		exit "$restore_status"
	fi
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

if ! raise_gateway_limits_for_seed; then
	die 'could not temporarily raise git-http to the required git/git pack-WAL seed limits'
fi

case "${VEX_HTTP_BASIC_USER:-}:${VEX_HTTP_BASIC_PASSWORD:-}" in
:) die 'Compose push credentials are missing from vex/docker/.env' ;;
esac
auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')

git_vex_read() {
	"$stock_git" -c http.sslCAInfo="$ca_file" "$@"
}

list_local_refs() {
	local git_dir=$1 output=$2
	"$stock_git" --git-dir="$git_dir" for-each-ref --format='%(objectname) %(refname)' |
		LC_ALL=C sort >"$output"
}

list_remote_refs() {
	local output=$1
	git_vex_read ls-remote --refs "$origin" |
		awk 'NF == 2 { print $1 " " $2 }' |
		LC_ALL=C sort >"$output"
}

count_reachable_objects() {
	local git_dir=$1 count
	count=$("$stock_git" --git-dir="$git_dir" rev-list --objects --no-object-names --all |
		wc -l | tr -d '[:space:]')
	require_uint 'reachable object count' "$count"
	printf '%s\n' "$count"
}

count_commits() {
	local git_dir=$1 count
	count=$("$stock_git" --git-dir="$git_dir" rev-list --count --all)
	require_uint 'commit count' "$count"
	printf '%s\n' "$count"
}

assert_same_refs() {
	local expected=$1 actual=$2 label=$3
	if ! cmp -s "$expected" "$actual"; then
		printf 'vex seed-git-git: %s refs differ from the source mirror:\n' "$label" >&2
		diff -u "$expected" "$actual" >&2 || true
		exit 1
	fi
}

s3_object_sha256() {
	local object_key=$1

	"${compose[@]}" run --rm --no-deps -T --entrypoint /bin/sh \
		-e "VEX_MANIFEST_KEY=$object_key" minio-init -c '
			set -eu
			mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
			mc cat "vex/$MINIO_BUCKET/$VEX_MANIFEST_KEY"
		' |
		python3 -c '
import hashlib
import sys

digest = hashlib.sha256()
while True:
    chunk = sys.stdin.buffer.read(1024 * 1024)
    if not chunk:
        break
    digest.update(chunk)
print(digest.hexdigest())
'
}

s3_object_bytes() {
	local object_key=$1

	"${compose[@]}" run --rm --no-deps -T --entrypoint /bin/sh \
		-e "VEX_MANIFEST_KEY=$object_key" minio-init -c '
			set -eu
			mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
			mc cat "vex/$MINIO_BUCKET/$VEX_MANIFEST_KEY" | wc -c | tr -d "[:space:]"
		'
}

mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --local-infile=1 --protocol=TCP -h 127.0.0.1 \
		-u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" -e "$1"
}

collect_image_fact() {
	local service=$1 output=$2 container_id image_id repo_digest

	container_id=$("${compose[@]}" ps --all -q "$service")
	[ -n "$container_id" ] || die "Compose service has no container: $service"
	image_id=$(docker inspect --format '{{.Image}}' "$container_id")
	repo_digest=$(docker image inspect --format '{{index .RepoDigests 0}}' "$image_id" 2>/dev/null || true)
	printf '%s\t%s\t%s\n' "$service" "$image_id" "$repo_digest" >>"$output"
}

mirror=$(absolute_path "$mirror")
disk_control=$(absolute_path "$disk_control")
[ "$mirror" != "$disk_control" ] || die 'mirror and disk-control paths must differ'

ensure_args=(--mirror "$mirror" --upstream "$upstream")
if [ "$refresh" -eq 1 ]; then
	ensure_args+=(--refresh)
fi
"$repo_root/scripts/vex/ensure-git-git-mirror.sh" "${ensure_args[@]}"

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-seed-git-git.XXXXXX")

source_refs="$scratch/source-refs"
target_before_refs="$scratch/target-before-refs"
target_refs="$scratch/target-refs"
disk_refs="$scratch/disk-refs"
images_tsv="$scratch/images.tsv"

source_head_ref=$("$stock_git" --git-dir="$mirror" symbolic-ref -q HEAD)
case "$source_head_ref" in
refs/heads/*) ;;
*) die "source mirror HEAD is not a branch symbolic ref: $source_head_ref" ;;
esac
source_head_oid=$("$stock_git" --git-dir="$mirror" rev-parse --verify 'HEAD^{commit}')
source_hash_algo=$("$stock_git" --git-dir="$mirror" rev-parse --show-object-format)
case "$source_hash_algo" in
sha1) source_oid_hex_length=40 ;;
sha256) source_oid_hex_length=64 ;;
*) die "source mirror uses an unsupported object format: $source_hash_algo" ;;
esac
"$stock_git" --git-dir="$mirror" show-ref --verify --quiet "$source_head_ref" ||
	die "source mirror HEAD target does not exist: $source_head_ref"
"$stock_git" --git-dir="$mirror" fsck --connectivity-only --no-progress >/dev/null
list_local_refs "$mirror" "$source_refs"
[ -s "$source_refs" ] || die 'source mirror has no refs'
source_ref_count=$(wc -l <"$source_refs" | tr -d '[:space:]')
require_uint 'source ref count' "$source_ref_count"
source_object_count=$(count_reachable_objects "$mirror")
[ "$source_object_count" -gt 0 ] || die 'source mirror has no reachable objects'
source_commit_count=$(count_commits "$mirror")
[ "$source_commit_count" -gt 0 ] || die 'source mirror has no reachable commits'

# A disk control is intentionally a separate bare repository: --no-local
# prevents hardlink/reference shortcuts, and the documented full repack makes
# the baseline's pack layout explicit and reproducible.
if [ -e "$disk_control" ]; then
	[ -d "$disk_control" ] || die "disk-control path is not a directory: $disk_control"
	disk_is_bare=$("$stock_git" --git-dir="$disk_control" rev-parse --is-bare-repository 2>/dev/null || true)
	[ "$disk_is_bare" = true ] || die "disk-control path is not a bare Git repository: $disk_control"
	list_local_refs "$disk_control" "$disk_refs"
	assert_same_refs "$source_refs" "$disk_refs" 'existing disk-control'
else
	"$stock_git" clone --quiet --mirror --no-local "$mirror" "$disk_control"
fi
"$stock_git" --git-dir="$disk_control" symbolic-ref HEAD "$source_head_ref"
"$stock_git" --git-dir="$disk_control" repack -adf --write-bitmap-index
"$stock_git" --git-dir="$disk_control" fsck --connectivity-only --no-progress >/dev/null
list_local_refs "$disk_control" "$disk_refs"
assert_same_refs "$source_refs" "$disk_refs" 'disk-control'
disk_head_oid=$("$stock_git" --git-dir="$disk_control" rev-parse --verify 'HEAD^{commit}')
[ "$disk_head_oid" = "$source_head_oid" ] || die 'disk-control HEAD does not match the source mirror'
disk_object_count=$(count_reachable_objects "$disk_control")
[ "$disk_object_count" = "$source_object_count" ] ||
	die "disk-control object count differs from source ($disk_object_count != $source_object_count)"

# The normal initializer creates a thin Vex repository only; the following
# stock client push is what publishes the actual refs and canonical S3 bytes.
"$repo_root/scripts/vex/seed-empty-repo.sh" git/git.git >/dev/null
repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	'--git-dir=/srv/git/git/git.git' config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????) repo_id=${repository_uri#vex://} ;;
*) die "git/git.git is not Vex-backed: $repository_uri" ;;
esac

list_remote_refs "$target_before_refs"
if cmp -s "$source_refs" "$target_before_refs"; then
	printf '%s\n' 'vex seed-git-git: Vex target refs already match the source mirror'
else
	if [ -s "$target_before_refs" ]; then
		printf '%s\n' 'vex seed-git-git: pack-WAL seed requires a target with no published refs.' >&2
		if [ "$replace_target" -eq 1 ]; then
			printf '%s\n' 'vex seed-git-git: --replace is not available for the first-pack seed path.' >&2
		fi
		exit 1
	fi
	# The first raw pack admits precisely one unborn symbolic HEAD scaffold.
	# Configure that scaffold before the pack is retained, so git/git's master
	# default is preserved without attempting an unsafe later metadata rewrite.
	"${compose[@]}" exec -T git-http /opt/vex-git/git \
		"--git-dir=/srv/git/git/git.git" symbolic-ref HEAD "$source_head_ref" >/dev/null
	target_preseed_head_ref=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
		"--git-dir=/srv/git/git/git.git" symbolic-ref -q HEAD)
	[ "$target_preseed_head_ref" = "$source_head_ref" ] ||
		die 'Vex target HEAD did not retain the source default before pack seed'
	GIT_DIR="$mirror" "$stock_git" -c http.sslCAInfo="$ca_file" \
		-c "http.extraHeader=Authorization: Basic $auth" \
		push --mirror "$origin"
fi

# The source default must already be the permitted unborn scaffold. Once the
# raw pack is sealed, generic ref rewrites are intentionally rejected until a
# future complete pack-set implementation exists.
target_head_ref=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	'--git-dir=/srv/git/git/git.git' symbolic-ref -q HEAD)
[ "$target_head_ref" = "$source_head_ref" ] ||
	die 'Vex target HEAD does not match the source default branch'

list_remote_refs "$target_refs"
assert_same_refs "$source_refs" "$target_refs" 'Vex target'
target_head_oid=$(git_vex_read ls-remote "$origin" HEAD |
	awk 'NF == 2 && $2 == "HEAD" { print $1; exit }')
[ "$target_head_oid" = "$source_head_oid" ] ||
	die "Vex target HEAD does not match the source mirror ($target_head_oid != $source_head_oid)"

raw_pack_rows=$(mysql_query "
	SELECT COUNT(*)
	  FROM pack_envelopes AS p
	 WHERE p.repo_id = UNHEX('$repo_id')
	   AND p.source_kind = 'receive'
	   AND p.state = 'sealed'
	   AND p.is_full_snapshot = TRUE
	   AND p.covering_generation = (
	       SELECT MAX(generation) FROM generations
	        WHERE repo_id = UNHEX('$repo_id') AND state = 'sealed'
	   );")
[ "$raw_pack_rows" = 1 ] ||
	die "expected exactly one current sealed raw receive pack, found $raw_pack_rows"

IFS=$'\t' read -r pack_id pack_generation pack_key idx_key pack_object_count <<EOF
$(mysql_query "
	SELECT LOWER(LEFT(HEX(p.pack_id), $source_oid_hex_length)),
	       p.covering_generation, p.pack_key, p.idx_key, COUNT(rpo.oid)
	  FROM pack_envelopes AS p
	  JOIN received_pack_objects AS rpo
	    ON rpo.repo_id = p.repo_id AND rpo.pack_id = p.pack_id
	 WHERE p.repo_id = UNHEX('$repo_id')
	   AND p.source_kind = 'receive'
	   AND p.state = 'sealed'
	   AND p.is_full_snapshot = TRUE
	   AND p.covering_generation = (
	       SELECT MAX(generation) FROM generations
	        WHERE repo_id = UNHEX('$repo_id') AND state = 'sealed'
	   )
	 GROUP BY p.pack_id, p.covering_generation, p.pack_key, p.idx_key;")
EOF
case "$pack_id" in
????????????????????????????????????????|????????????????????????????????????????????????????????????????) ;;
*) die "raw receive metadata returned an invalid pack id: $pack_id" ;;
esac
case "$pack_id" in *[!0-9a-f]*) die "raw receive metadata returned an invalid pack id: $pack_id" ;; esac
require_uint 'pack object count' "$pack_object_count"
require_uint 'pack generation' "$pack_generation"
[ "$pack_object_count" = "$source_object_count" ] ||
	die "raw pack object count differs from source ($pack_object_count != $source_object_count)"
case "$pack_key" in "git/v1/$repo_id/packs/$pack_id.pack") ;; *) die "unexpected Vex pack key: $pack_key" ;; esac
case "$idx_key" in "git/v1/$repo_id/packs/$pack_id.idx") ;; *) die "unexpected Vex index key: $idx_key" ;; esac

# The raw pair is the S3 batch boundary: pack payload bytes were transferred
# once, immutable keys were promoted together, and MySQL holds only the OID
# map. Recheck the sealed S3 bytes for the reproducible seed manifest without
# materializing them on the host.
pack_sha256=$(s3_object_sha256 "$pack_key")
require_sha256 "$pack_sha256"
pack_bytes=$(s3_object_bytes "$pack_key")
idx_bytes=$(s3_object_bytes "$idx_key")
require_uint 'pack byte count' "$pack_bytes"
require_uint 'pack index byte count' "$idx_bytes"

"$repo_root/scripts/vex/install-bench-disk-control.sh" \
	--source "$disk_control" --origin "$disk_origin"

: >"$images_tsv"
collect_image_fact mysql "$images_tsv"
collect_image_fact minio "$images_tsv"
collect_image_fact minio-init "$images_tsv"
collect_image_fact git-http "$images_tsv"

manifest="$repo_root/t/vex/out/seed-manifest.json"
mkdir -p -- "$(dirname -- "$manifest")"
stock_git_version=$("$stock_git" --version)
python3 - "$manifest" "$source_refs" "$target_refs" \
	"$upstream" "$source_head_ref" "$source_head_oid" "$source_commit_count" "$source_object_count" \
	"$origin" "$repo_id" "$target_head_ref" "$target_head_oid" \
	"$pack_id" "$pack_key" "$idx_key" "$pack_object_count" "$pack_generation" \
	"$pack_bytes" "$idx_bytes" "$pack_sha256" "$disk_control" "$disk_origin" "$images_tsv" \
	"$compose_file" "${COMPOSE_PROJECT_NAME:-vex}" "$stock_git_version" <<'PY'
import hashlib
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlsplit, urlunsplit


(
    manifest_name,
    source_refs_name,
    target_refs_name,
    upstream,
    source_head_ref,
    source_head_oid,
    source_commit_count,
    source_object_count,
    origin,
    repository_id,
    target_head_ref,
    target_head_oid,
    pack_id,
    pack_key,
    idx_key,
    pack_object_count,
    pack_generation,
    pack_bytes,
    idx_bytes,
    pack_sha256,
    disk_control_name,
    disk_origin,
    images_name,
    compose_name,
    compose_project,
    stock_git_version,
) = sys.argv[1:]


def digest_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def ref_facts(path_name: str) -> dict[str, object]:
    data = Path(path_name).read_bytes()
    lines = [line for line in data.splitlines() if line]
    return {"count": len(lines), "sha256": digest_bytes(data)}


def redact_url(value: str) -> str:
    parsed = urlsplit(value)
    if parsed.scheme in {"http", "https"}:
        host = parsed.hostname or ""
        if ":" in host and not host.startswith("["):
            host = "[{}]".format(host)
        if parsed.port is not None:
            host = "{}:{}".format(host, parsed.port)
        return urlunsplit((parsed.scheme, host, parsed.path, "", ""))
    if parsed.scheme == "file":
        return "file://<local-path-redacted>"
    return "<opaque-transport-redacted>"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


disk_control = Path(disk_control_name)
disk_pack_dir = disk_control / "objects" / "pack"
disk_packs = []
for pack_file in sorted(disk_pack_dir.glob("*.pack")):
    disk_packs.append(
        {
            "bytes": pack_file.stat().st_size,
            "name": pack_file.name,
            "sha256": sha256_file(pack_file),
        }
    )
if not disk_packs:
    raise SystemExit("vex seed-git-git: disk-control has no .pack after repack")

images = []
for raw_line in Path(images_name).read_text(encoding="utf-8").splitlines():
    service, image_id, repo_digest = raw_line.split("\t", 2)
    images.append(
        {
            "image_id": image_id or None,
            "repo_digest": repo_digest or None,
            "service": service,
        }
    )

created_at = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
default_branch = source_head_ref[len("refs/heads/"):]

payload = {
    "commit_count": int(source_commit_count),
    "compose": {
        "compose_project": compose_project,
        "compose_sha256": sha256_file(Path(compose_name)),
        "images": images,
    },
    "disk_control": {
        "head": {"oid": source_head_oid, "ref": source_head_ref},
        "origin": redact_url(disk_origin),
        "objects": {"reachable_count": int(source_object_count)},
        "pack_files": disk_packs,
        "repack": "git repack -adf --write-bitmap-index",
        "repository": disk_control.name,
        "refs": ref_facts(source_refs_name),
        "storage": {"compose_volume": "vex_bench_disk", "kind": "classic-bare"},
        "transport": "authenticated-stock-git-smart-https",
    },
    "created_at": created_at,
    "default_branch": default_branch,
    "generated_at_utc": created_at,
    "head": source_head_oid,
    "import": {
        "target_ref_snapshot_matches_source": True,
        "transport": "stock-git-smart-https-push-mirror",
    },
    "schema_version": 1,
    "source": {
        "commit_count": int(source_commit_count),
        "head": {"oid": source_head_oid, "ref": source_head_ref},
        "objects": {"reachable_count": int(source_object_count)},
        "refs": ref_facts(source_refs_name),
        "upstream": redact_url(upstream),
    },
    "stock_git": {"version": stock_git_version},
    "object_count": int(source_object_count),
    "pack_bytes": int(pack_bytes),
    "upstream": redact_url(upstream),
    "vex": {
        "commit_count": int(source_commit_count),
        "head": {"oid": target_head_oid, "ref": target_head_ref},
        "objects": {"reachable_count": int(pack_object_count)},
        "origin": redact_url(origin),
        "precomputed_pack": {
            "bytes": int(pack_bytes),
            "covering_generation": int(pack_generation),
            "id": pack_id,
            "idx_bytes": int(idx_bytes),
            "idx_key": idx_key,
            "key": pack_key,
            "object_count": int(pack_object_count),
            "sha256": pack_sha256,
        },
        "repository": "git/git.git",
        "repository_id": repository_id,
        "refs": ref_facts(target_refs_name),
    },
}

manifest = Path(manifest_name)
manifest.parent.mkdir(parents=True, exist_ok=True)
temporary = manifest.with_name(manifest.name + ".tmp-{}".format(os.getpid()))
try:
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.chmod(temporary, 0o600)
    os.replace(temporary, manifest)
finally:
    temporary.unlink(missing_ok=True)
PY

restore_gateway_limits || die 'git-http gateway limits could not be restored after the seed'

printf 'vex seed-git-git: PASS repository=git/git.git refs=%s objects=%s pack=%s disk_control=%s disk_origin=%s manifest=%s\n' \
	"$source_ref_count" "$source_object_count" "$pack_id" "$disk_control" "$disk_origin" "$manifest"
