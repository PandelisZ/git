#!/usr/bin/env bash

# Operator-owned post-run check for the local Vex MySQL+S3 Compose receiver.
# The Python harness supplies the advertised target tip; this script confirms
# that its ref/object metadata and canonical blob key agree with that tip.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"

origin=${GIT_VEX_PUSH_STRESS_ORIGIN:?missing GIT_VEX_PUSH_STRESS_ORIGIN}
branch=${GIT_VEX_PUSH_STRESS_BRANCH:?missing GIT_VEX_PUSH_STRESS_BRANCH}
expected_tip=${GIT_VEX_PUSH_STRESS_EXPECTED_TIP:?missing GIT_VEX_PUSH_STRESS_EXPECTED_TIP}

case "$expected_tip" in
????????????????????????????????????????|????????????????????????????????????????????????????????????????) ;;
*) printf '%s\n' 'vex push-stress recon: expected tip must be a Git object ID' >&2; exit 2 ;;
esac
case "$expected_tip" in *[!0123456789abcdef]*)
	printf '%s\n' 'vex push-stress recon: expected tip must use lower-case hexadecimal' >&2
	exit 2
	esac
case "$branch" in ''|/*|*'..'*|*'//'*|*[!A-Za-z0-9._/-]*)
	printf '%s\n' 'vex push-stress recon: unsafe target branch' >&2
	exit 2
	esac

set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

origin_prefix="https://localhost:${GIT_HTTPS_PORT}/"
case "$origin" in
"$origin_prefix"*.git) repo_name=${origin#"$origin_prefix"} ;;
*) printf 'vex push-stress recon: expected local HTTPS origin, got %s\n' "$origin" >&2; exit 2 ;;
esac
case "$repo_name" in ''|/*|*'..'*|*'//'*|*[!A-Za-z0-9._/-]*)
	printf '%s\n' 'vex push-stress recon: unsafe repository name' >&2
	exit 2
	esac

compose=(docker compose -f "$docker_dir/compose.yaml" --env-file "$env_file")
uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$uri" in
vex://????????????????????????????????) repo_id=${uri#vex://} ;;
*) printf 'vex push-stress recon: repository lacks a Vex UUID: %s\n' "$uri" >&2; exit 1 ;;
esac
case "$repo_id" in *[!0123456789abcdef]*)
	printf '%s\n' 'vex push-stress recon: repository UUID is not canonical lower-case hex' >&2
	exit 1
	esac

refname="refs/heads/$branch"
object_key="git/v1/$repo_id/objects/$expected_tip"
ref_rows=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM refs WHERE repo_id = UNHEX('$repo_id') AND refname = '$refname' AND LOWER(HEX(target_oid)) LIKE '${expected_tip}%';")
[ "$ref_rows" -ge 1 ] || {
	printf '%s\n' 'vex push-stress recon: MySQL ref tip does not match the final advertised tip' >&2
	exit 1
}
object_rows=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM objects WHERE repo_id = UNHEX('$repo_id') AND state = 'published' AND LOWER(HEX(oid)) LIKE '${expected_tip}%' AND blob_key = '$object_key';")
[ "$object_rows" -ge 1 ] || {
	printf '%s\n' 'vex push-stress recon: final commit is absent from published MySQL object metadata' >&2
	exit 1
}
sealed_rows=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM objects AS o JOIN generations AS g ON g.repo_id = o.repo_id AND g.generation = o.generation WHERE o.repo_id = UNHEX('$repo_id') AND LOWER(HEX(o.oid)) LIKE '${expected_tip}%' AND o.state = 'published' AND g.state = 'sealed';")
[ "$sealed_rows" -ge 1 ] || {
	printf '%s\n' 'vex push-stress recon: final object lacks a sealed Vex generation' >&2
	exit 1
}
"${compose[@]}" run --rm --no-deps --entrypoint /bin/sh \
	-e "VEX_S3_OBJECT_KEY=$object_key" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc stat "vex/$MINIO_BUCKET/$VEX_S3_OBJECT_KEY" >/dev/null
	'

printf '{"canonical_blob_recon":"pass","object_key":"%s","repo_id":"%s","tip":"%s"}\n' \
	"$object_key" "$repo_id" "$expected_tip"
