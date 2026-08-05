#!/usr/bin/env bash

# Read-only integrity scrub for one active Vex MySQL + S3 repository.
#
# The metadata query is deliberately an index walk.  It never treats MySQL as
# an object-byte source: each published object is streamed from its recorded
# S3 key and re-hashed with its Git object header.  Ref targets are also
# checked against published metadata before the S3 pass starts.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
docker_dir="$repo_root/vex/docker"
env_file="$docker_dir/.env"

usage() {
	printf '%s\n' 'usage: scripts/vex/scrub.sh <repository.git>'
}

if [ "$#" -ne 1 ]; then
	usage >&2
	exit 2
fi

repo_name=$1
case "$repo_name" in
''|/*|*..*|*$'\n'*|*$'\r'*)
	printf 'vex scrub: invalid repository name: %s\n' "$repo_name" >&2
	exit 2
	;;
esac

up_args=()
case "${VEX_COMPOSE_NO_BUILD:-}" in
1|true|yes) up_args+=(--no-build) ;;
esac
"$repo_root/scripts/vex/up.sh" "${up_args[@]}" >/dev/null

[ -f "$env_file" ] || env_file="$docker_dir/.env.example"
set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

compose=(docker compose -f "$docker_dir/compose.yaml" --env-file "$env_file")
mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" \
		-N -B "$MYSQL_DATABASE" -e "$1"
}

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????)
	repo_id=${repository_uri#vex://}
	;;
*)
	printf 'vex scrub: repository is not Vex-backed: %s\n' "$repo_name" >&2
	exit 1
	;;
esac

hash_algo=$(mysql_query "SELECT hash_algo FROM repositories WHERE repo_id = UNHEX('$repo_id');")
case "$hash_algo" in
sha1)
	hash_command=sha1sum
	oid_hex_length=40
	;;
sha256)
	hash_command=sha256sum
	oid_hex_length=64
	;;
*)
	printf 'vex scrub: unsupported or missing repository hash algorithm: %s\n' "$hash_algo" >&2
	exit 1
	;;
esac

missing_ref_targets=$(mysql_query "
	SELECT COUNT(*)
	  FROM refs AS r
	  LEFT JOIN objects AS o
	    ON o.repo_id = r.repo_id
	   AND o.oid = r.target_oid
	   AND o.state = 'published'
	 WHERE r.repo_id = UNHEX('$repo_id')
	   AND r.target_oid IS NOT NULL
	   AND o.oid IS NULL;")
if [ "$missing_ref_targets" != 0 ]; then
	printf 'vex scrub: %s ref target(s) lack published object metadata for %s\n' \
		"$missing_ref_targets" "$repo_name" >&2
	exit 1
fi

bad_generations=$(mysql_query "
	SELECT COUNT(*)
	  FROM objects AS o
	  LEFT JOIN generations AS g
	    ON g.repo_id = o.repo_id
	   AND g.generation = o.generation
	 WHERE o.repo_id = UNHEX('$repo_id')
	   AND o.state = 'published'
	   AND (g.generation IS NULL OR g.state NOT IN ('sealed', 'sweepable', 'swept'));")
if [ "$bad_generations" != 0 ]; then
	printf 'vex scrub: %s published object(s) have a non-durable generation for %s\n' \
		"$bad_generations" "$repo_name" >&2
	exit 1
fi

manifest=$(mktemp "${TMPDIR:-/tmp}/vex-scrub.XXXXXX")
cleanup() {
	rm -f -- "$manifest"
}
trap cleanup EXIT HUP INT TERM

mysql_query "
	SELECT LOWER(LEFT(HEX(oid), $oid_hex_length)), obj_type, size, blob_key
	  FROM objects
	 WHERE repo_id = UNHEX('$repo_id')
	   AND state = 'published'
	 ORDER BY oid;" >"$manifest"
object_count=$(wc -l <"$manifest" | tr -d '[:space:]')
ref_count=$(mysql_query "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id');")

"${compose[@]}" run --rm --no-deps -T --entrypoint /bin/sh \
	-e "VEX_SCRUB_HASH_COMMAND=$hash_command" \
	-e "VEX_SCRUB_EXPECTED_OBJECTS=$object_count" \
	minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		checked=0
		failed=0
		while IFS="$(printf "\\t")" read -r oid type_number size key
		do
			test -n "$oid" || continue
			case "$type_number" in
			1) type=commit ;;
			2) type=tree ;;
			3) type=blob ;;
			4) type=tag ;;
			*)
				printf "vex scrub: unsupported object type %s for %s\\n" "$type_number" "$oid" >&2
				failed=$((failed + 1))
				continue
				;;
			esac
			actual=$({
				printf "%s %s\\0" "$type" "$size"
				mc cat "vex/$MINIO_BUCKET/$key"
			} | "$VEX_SCRUB_HASH_COMMAND")
			actual=${actual%% *}
			if test "$actual" != "$oid"
			then
				printf "vex scrub: S3 hash mismatch for %s (got %s)\\n" "$oid" "$actual" >&2
				failed=$((failed + 1))
			fi
			checked=$((checked + 1))
		done
		if test "$checked" != "$VEX_SCRUB_EXPECTED_OBJECTS"
		then
			printf "vex scrub: expected %s object(s), checked %s\\n" "$VEX_SCRUB_EXPECTED_OBJECTS" "$checked" >&2
			exit 1
		fi
		test "$failed" = 0
	' <"$manifest"

printf 'vex scrub: PASS repository=%s objects=%s refs=%s hash=%s\n' \
	"$repo_name" "$object_count" "$ref_count" "$hash_algo"
