#!/usr/bin/env bash

# Build one immutable, derived pack artifact from a live Vex MySQL+S3
# repository.  This is deliberately an operator-side builder: it records only
# pack metadata in MySQL and stores pack/index bytes in MinIO/S3.  A normal
# invocation builds the current generation immediately; --due consumes only a
# durable, debounced rebuild request created by a prior receive.  The explicit
# --from-receive mode is a narrow post-seed compactor: it keeps the sealed raw
# receive pack and its membership map canonical, then publishes an additional
# compact full-snapshot artifact for clone relay.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
docker_dir="$repo_root/vex/docker"
env_file="$docker_dir/.env"

usage() {
	printf '%s\n' 'usage: scripts/vex/build-pack.sh [--due|--from-receive] <repository.git>'
}

due_only=0
from_receive=0
case "$#" in
1) repo_name=$1 ;;
2)
	if [ "$1" = --due ]; then
		due_only=1
		repo_name=$2
	elif [ "$1" = --from-receive ]; then
		from_receive=1
		repo_name=$2
	else
		usage >&2
		exit 2
	fi
	;;
*)
	usage >&2
	exit 2
	;;
esac

case "$repo_name" in
''|/*|*..*|*'//'*|*$'\n'*|*$'\r'*)
	printf 'vex build-pack: invalid repository name: %s\n' "$repo_name" >&2
	exit 2
	;;
esac
case "$repo_name" in
*.git) ;;
*)
	printf 'vex build-pack: repository must end in .git: %s\n' "$repo_name" >&2
	exit 2
	;;
esac

up_args=()
case "${VEX_COMPOSE_NO_BUILD:-}" in
1|true|yes) up_args+=(--no-build) ;;
esac
# The focused Compose E2E gates bring the stack up themselves.  Reusing that
# already-healthy topology avoids a second full Compose --wait without making
# this operator command silently accept a missing service: the container IDs
# below remain mandatory before any pack bytes are built or registered.
case "${VEX_COMPOSE_ALREADY_UP:-}" in
1|true|yes) ;;
*) "$repo_root/scripts/vex/up.sh" "${up_args[@]}" >/dev/null ;;
esac

[ -f "$env_file" ] || env_file="$docker_dir/.env.example"
set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

compose=(docker compose -f "$docker_dir/compose.yaml" --env-file "$env_file")
mysql_query() {
	"${compose[@]}" exec -T mysql \
		mysql --local-infile=1 --protocol=TCP -h 127.0.0.1 \
		-u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" -e "$1"
}
mysql_admin_query() {
	"${compose[@]}" exec -T mysql \
		mysql --protocol=TCP -h 127.0.0.1 \
		-uroot -p"$MYSQL_ROOT_PASSWORD" -N -B "$MYSQL_DATABASE" -e "$1"
}

repository_uri=$("${compose[@]}" exec -T git-http /opt/vex-git/git \
	"--git-dir=/srv/git/$repo_name" config --get extensions.refstorage)
case "$repository_uri" in
vex://????????????????????????????????) repo_id=${repository_uri#vex://} ;;
*)
	printf 'vex build-pack: repository is not Vex-backed: %s\n' "$repo_name" >&2
	exit 1
	;;
esac

hash_algo=$(mysql_query "SELECT hash_algo FROM repositories WHERE repo_id = UNHEX('$repo_id');")
case "$hash_algo" in
sha1)
	oid_hex_length=40
	oid_padding=000000000000000000000000
	;;
sha256)
	oid_hex_length=64
	oid_padding=
	;;
*)
	printf 'vex build-pack: unsupported or missing repository hash algorithm: %s\n' \
		"$hash_algo" >&2
	exit 1
	;;
esac

covering_generation=$(mysql_query "
	SELECT COALESCE(MAX(generation), 0)
	  FROM generations
	 WHERE repo_id = UNHEX('$repo_id')
	   AND state = 'sealed';")
case "$covering_generation" in
''|*[!0-9]*|0)
	printf 'vex build-pack: repository has no sealed generation to cover: %s\n' \
		"$repo_name" >&2
	exit 1
	;;
esac

raw_pack_id=
raw_pack_id_padded=
raw_members=
if [ "$from_receive" -eq 1 ]; then
	raw_count=$(mysql_query "
		SELECT COUNT(*)
		  FROM pack_envelopes
		 WHERE repo_id = UNHEX('$repo_id')
		   AND source_kind = 'receive'
		   AND state = 'sealed'
		   AND is_full_snapshot = TRUE
		   AND covering_generation = $covering_generation;")
	[ "$raw_count" = 1 ] || {
		printf 'vex build-pack: --from-receive requires exactly one sealed raw full snapshot at generation %s for %s (found %s)\n' \
			"$covering_generation" "$repo_name" "$raw_count" >&2
		exit 1
	}
	IFS=$'\t' read -r raw_pack_id raw_generation raw_members <<EOF
$(mysql_query "
	SELECT LOWER(LEFT(HEX(p.pack_id), $oid_hex_length)), p.covering_generation,
	       COUNT(rpo.oid)
	  FROM pack_envelopes AS p
	  JOIN received_pack_objects AS rpo
	    ON rpo.repo_id = p.repo_id AND rpo.pack_id = p.pack_id
	 WHERE p.repo_id = UNHEX('$repo_id')
	   AND p.source_kind = 'receive'
	   AND p.state = 'sealed'
	   AND p.is_full_snapshot = TRUE
	   AND p.covering_generation = $covering_generation
	 GROUP BY p.pack_id, p.covering_generation;")
EOF
	case "$raw_pack_id:$raw_generation:$raw_members" in
	????????????????????????????????????????:"$covering_generation":[1-9]*|????????????????????????????????????????????????????????????????:"$covering_generation":[1-9]*) ;;
	*)
		printf 'vex build-pack: raw receive snapshot metadata is malformed for %s: %s:%s:%s\n' \
			"$repo_name" "$raw_pack_id" "$raw_generation" "$raw_members" >&2
		exit 1
		;;
	esac
	case "$raw_pack_id" in *[!0-9a-f]*)
		printf 'vex build-pack: raw receive pack id is not lowercase hexadecimal: %s\n' "$raw_pack_id" >&2
		exit 1
		;;
	esac
	case "$raw_members" in *[!0-9]*|'')
		printf 'vex build-pack: raw receive member count is invalid: %s\n' "$raw_members" >&2
		exit 1
		;;
	esac
	raw_pack_id_padded="$raw_pack_id$oid_padding"
fi

if [ "$due_only" -eq 1 ]; then
	due_generation=$(mysql_query "
		SELECT target_generation
		  FROM pack_rebuild_requests
		 WHERE repo_id = UNHEX('$repo_id')
		   AND not_before <= CURRENT_TIMESTAMP(6);")
	case "$due_generation" in
	'')
		printf 'vex build-pack: NOOP repository=%s reason=debounced-or-not-requested\n' \
			"$repo_name"
		exit 0
		;;
	*[!0-9]*)
		printf 'vex build-pack: invalid queued generation for %s: %s\n' \
			"$repo_name" "$due_generation" >&2
		exit 1
		;;
	esac
	if [ "$due_generation" -gt "$covering_generation" ]; then
		printf 'vex build-pack: queued generation %s exceeds current sealed generation %s for %s\n' \
			"$due_generation" "$covering_generation" "$repo_name" >&2
		exit 1
	fi
fi

run_id="$(date +%s)-$$"
remote_work="/var/git-scratch/vex-pack-${run_id}"
local_work=$(mktemp -d "${TMPDIR:-/tmp}/vex-pack-${run_id}.XXXXXX")
git_container=$("${compose[@]}" ps -q git-http)
mysql_container=$("${compose[@]}" ps -q mysql)
mysql_manifest="/var/lib/mysql-files/vex-pack-${run_id}.oids"
uploaded_keys=()
pack_registered=0

cleanup() {
	# A compact receive artifact is content addressed and two operators can
	# derive the same bytes concurrently.  If one publishes after both have
	# uploaded, the loser must not delete the winner's immutable S3 pair during
	# its cleanup.  Retain an unregistered raw-derived pair as an orphan for the
	# normal S3 sweep rather than risk a published envelope pointing at nothing.
	if [ "${from_receive:-0}" != 1 ] && [ "${pack_registered:-0}" != 1 ] && [ "${#uploaded_keys[@]}" -gt 0 ]; then
		"${compose[@]}" run --rm --no-deps -T --entrypoint /bin/sh \
			-e "VEX_PACK_DELETE_KEYS=${uploaded_keys[*]}" minio-init -c '
				set -eu
				mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
				for key in $VEX_PACK_DELETE_KEYS
				do
					mc rm --force "vex/$MINIO_BUCKET/$key" >/dev/null || true
				done
			' >/dev/null 2>&1 || true
	fi
	if [ -n "${git_container:-}" ] && [ -n "${remote_work:-}" ]; then
		"${compose[@]}" exec -T git-http /bin/sh -c \
			'rm -rf -- "$1"' sh "$remote_work" >/dev/null 2>&1 || true
	fi
	if [ -n "${mysql_container:-}" ] && [ -n "${mysql_manifest:-}" ]; then
		"${compose[@]}" exec -T mysql /bin/sh -c \
			'rm -f -- "$1"' sh "$mysql_manifest" >/dev/null 2>&1 || true
	fi
	rm -rf -- "$local_work"
}
trap cleanup EXIT HUP INT TERM

[ -n "$git_container" ] && [ -n "$mysql_container" ] || {
	printf '%s\n' 'vex build-pack: required Compose containers are not running' >&2
	exit 1
}

build_info=$("${compose[@]}" exec -T git-http /bin/sh -ec '
	set -eu
	repo=$1
	work=$2
	from_receive=$3
	mkdir -p "$work"
	/opt/vex-git/git --git-dir="/srv/git/$repo" rev-list --objects --no-object-names --all >"$work/oids"
	test -s "$work/oids"
	/opt/vex-git/git --git-dir="/srv/git/$repo" pack-objects --stdout --revs \
		--no-reuse-object --no-reuse-delta --threads=1 <"$work/oids" >"$work/pack"
	pack_id=$(/opt/vex-git/git index-pack -o "$work/pack.idx" "$work/pack")
	object_count=$(wc -l <"$work/oids" | tr -d "[:space:]")
	if test "$from_receive" = 1
	then
		# The index is the authoritative membership surface for the compact
		# artifact.  Do not infer its object set merely from pack-objects input.
		/opt/vex-git/git show-index <"$work/pack.idx" | awk "{ print \$2 }" >"$work/pack-oids"
		test -s "$work/pack-oids"
		object_count=$(wc -l <"$work/pack-oids" | tr -d "[:space:]")
	fi
	printf "%s\t%s\t%s\t%s\t%s\n" "$pack_id" "$object_count" \
		"$(wc -c <"$work/pack" | tr -d "[:space:]")" \
		"$(wc -c <"$work/pack.idx" | tr -d "[:space:]")" "$work"
' sh "$repo_name" "$remote_work" "$from_receive")

IFS=$'\t' read -r pack_id object_count pack_bytes idx_bytes built_remote_work <<<"$build_info"
[ "$built_remote_work" = "$remote_work" ] || {
	printf '%s\n' 'vex build-pack: unexpected server scratch path' >&2
	exit 1
}
pack_id=$(printf '%s' "$pack_id" | tr 'A-F' 'a-f')
case "$hash_algo:$pack_id" in
sha1:????????????????????????????????????????) ;;
sha256:????????????????????????????????????????????????????????????????) ;;
*)
	printf 'vex build-pack: custom Git returned an invalid pack id: %s\n' "$pack_id" >&2
	exit 1
esac
case "$object_count:$pack_bytes:$idx_bytes" in
*[!0-9:]*|0:*|*:0:*|*:*:0)
	printf '%s\n' 'vex build-pack: custom Git returned an empty or invalid pack artifact' >&2
	exit 1
	;;
esac

membership_name=oids
if [ "$from_receive" -eq 1 ]; then
	membership_name=pack-oids
fi
docker cp "$git_container:$remote_work/$membership_name" "$local_work/oids"
manifest_count=$(wc -l <"$local_work/oids" | tr -d '[:space:]')
[ "$manifest_count" = "$object_count" ] || {
	printf 'vex build-pack: manifest count mismatch (expected %s, got %s)\n' \
		"$object_count" "$manifest_count" >&2
	exit 1
}
invalid_oids=$(LC_ALL=C grep -Evc "^[0-9a-f]{$oid_hex_length}$" "$local_work/oids" || true)
[ "$invalid_oids" = 0 ] || {
	printf 'vex build-pack: manifest has %s invalid object id(s)\n' "$invalid_oids" >&2
	exit 1
}
if [ "$from_receive" -eq 1 ] && [ "$raw_members" != "$object_count" ]; then
	printf 'vex build-pack: compact index has %s objects but the raw receive map has %s for %s\n' \
		"$object_count" "$raw_members" "$repo_name" >&2
	exit 1
fi

pack_id_padded="$pack_id$oid_padding"
pack_key="git/v1/$repo_id/packs/$pack_id.pack"
idx_key="git/v1/$repo_id/packs/$pack_id.idx"
if [ "$from_receive" -eq 1 ]; then
	existing_envelopes=$(mysql_query "
		SELECT COUNT(*)
		  FROM pack_envelopes
		 WHERE repo_id = UNHEX('$repo_id')
		   AND pack_id = UNHEX('$pack_id_padded');")
	[ "$existing_envelopes" = 0 ] || {
		printf 'vex build-pack: refusing to overwrite existing pack envelope %s for %s\n' \
			"$pack_id" "$repo_name" >&2
		exit 1
	}
fi
for artifact in pack idx; do
	case "$artifact" in
	pack) artifact_key=$pack_key; artifact_file=pack ;;
	idx) artifact_key=$idx_key; artifact_file=pack.idx ;;
	esac
	"${compose[@]}" exec -T git-http /bin/sh -c \
		'dd if="$1" bs=1048576 status=none' sh "$remote_work/$artifact_file" |
		"${compose[@]}" run --rm --no-deps -T --entrypoint /bin/sh \
			-e "VEX_PACK_KEY=$artifact_key" minio-init -c '
				set -eu
				mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
				mc pipe "vex/$MINIO_BUCKET/$VEX_PACK_KEY" >/dev/null
			'
	uploaded_keys+=("$artifact_key")
done

s3_sizes=$("${compose[@]}" run --rm --no-deps -T --entrypoint /bin/sh \
	-e "VEX_PACK_KEY=$pack_key" -e "VEX_IDX_KEY=$idx_key" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc stat "vex/$MINIO_BUCKET/$VEX_PACK_KEY" >/dev/null
		mc stat "vex/$MINIO_BUCKET/$VEX_IDX_KEY" >/dev/null
		pack_bytes=$(mc cat "vex/$MINIO_BUCKET/$VEX_PACK_KEY" | wc -c | tr -d "[:space:]")
		idx_bytes=$(mc cat "vex/$MINIO_BUCKET/$VEX_IDX_KEY" | wc -c | tr -d "[:space:]")
		printf "%s\t%s\n" "$pack_bytes" "$idx_bytes"
	')
IFS=$'\t' read -r s3_pack_bytes s3_idx_bytes <<<"$s3_sizes"
[ "$s3_pack_bytes" = "$pack_bytes" ] && [ "$s3_idx_bytes" = "$idx_bytes" ] || {
	printf 'vex build-pack: S3 byte count mismatch (pack %s/%s, idx %s/%s)\n' \
		"$pack_bytes" "$s3_pack_bytes" "$idx_bytes" "$s3_idx_bytes" >&2
	exit 1
}

docker cp "$local_work/oids" "$mysql_container:$mysql_manifest"
"${compose[@]}" exec -T mysql /bin/sh -c 'chmod 0644 -- "$1"' sh "$mysql_manifest"
if [ "$hash_algo" = sha1 ]; then
	oid_sql="UNHEX(CONCAT(oid_hex, '$oid_padding'))"
	candidate_oid_sql="UNHEX(CONCAT(candidate.oid_hex, '$oid_padding'))"
else
	oid_sql='UNHEX(oid_hex)'
	candidate_oid_sql='UNHEX(candidate.oid_hex)'
fi
if [ "$from_receive" -eq 1 ]; then
	# A raw receive seed deliberately has no `objects` rows: its canonical
	# payload is the sealed raw pack and `received_pack_objects` is its complete
	# membership map.  Validate the compact pack's *index* against that map in
	# MySQL, then publish only a derived envelope.  In particular, do not insert
	# `pack_objects`: that table's FK would incorrectly claim loose S3 payloads.
	registered_count=$(mysql_admin_query "
		CREATE TEMPORARY TABLE vex_pack_input (
			oid_hex CHAR($oid_hex_length) BINARY NOT NULL PRIMARY KEY
		) ENGINE=MEMORY;
		LOAD DATA INFILE '$mysql_manifest'
			INTO TABLE vex_pack_input
			LINES TERMINATED BY '\\n' (oid_hex);
		START TRANSACTION;
		SET @vex_repository_lock = NULL;
		SELECT repo_id INTO @vex_repository_lock
		  FROM repositories
		 WHERE repo_id = UNHEX('$repo_id')
		 FOR UPDATE;
		SELECT COALESCE(MAX(generation), 0) INTO @vex_current_generation
		  FROM generations
		 WHERE repo_id = UNHEX('$repo_id')
		   AND state = 'sealed';
		SELECT COUNT(*) INTO @vex_raw_envelopes
		  FROM pack_envelopes
		 WHERE repo_id = UNHEX('$repo_id')
		   AND source_kind = 'receive'
		   AND state = 'sealed'
		   AND is_full_snapshot = TRUE
		   AND covering_generation = $covering_generation;
		SELECT COUNT(*) INTO @vex_raw_members
		  FROM received_pack_objects
		 WHERE repo_id = UNHEX('$repo_id')
		   AND pack_id = UNHEX('$raw_pack_id_padded');
		SELECT COUNT(*) INTO @vex_compact_members FROM vex_pack_input;
		SELECT COUNT(*) INTO @vex_compact_missing
		  FROM vex_pack_input AS candidate
		  LEFT JOIN received_pack_objects AS raw
		    ON raw.repo_id = UNHEX('$repo_id')
		   AND raw.pack_id = UNHEX('$raw_pack_id_padded')
		   AND raw.oid = $candidate_oid_sql
		 WHERE raw.oid IS NULL;
		SELECT COUNT(*) INTO @vex_raw_missing
		  FROM received_pack_objects AS raw
		  LEFT JOIN vex_pack_input AS candidate
		    ON candidate.oid_hex = LOWER(LEFT(HEX(raw.oid), $oid_hex_length))
		 WHERE raw.repo_id = UNHEX('$repo_id')
		   AND raw.pack_id = UNHEX('$raw_pack_id_padded')
		   AND candidate.oid_hex IS NULL;
		INSERT INTO pack_envelopes
			(repo_id, pack_id, pack_key, idx_key, bitmap_key, covering_generation,
			 state, source_kind, receive_generation, is_full_snapshot, sealed_at)
		SELECT UNHEX('$repo_id'), UNHEX('$pack_id_padded'), '$pack_key', '$idx_key',
		       NULL, $covering_generation, 'sealed', 'derived', NULL, TRUE,
		       CURRENT_TIMESTAMP(6)
		 WHERE @vex_repository_lock IS NOT NULL
		   AND @vex_current_generation = $covering_generation
		   AND @vex_raw_envelopes = 1
		   AND @vex_raw_members = $object_count
		   AND @vex_compact_members = $object_count
		   AND @vex_compact_missing = 0
		   AND @vex_raw_missing = 0;
		SELECT ROW_COUNT() INTO @vex_published_rows;
		DELETE FROM pack_rebuild_requests
		 WHERE repo_id = UNHEX('$repo_id')
		   AND target_generation <= $covering_generation
		   AND @vex_published_rows = 1;
		COMMIT;
		SELECT @vex_published_rows;")
	case "$registered_count" in
	1) ;;
	*)
		printf 'vex build-pack: raw compact validation/publication failed (rows=%s, raw_members=%s, compact_members=%s)\n' \
			"$registered_count" "$raw_members" "$object_count" >&2
		exit 1
		;;
	esac
else
	registered_count=$(mysql_admin_query "
		CREATE TEMPORARY TABLE vex_pack_input (
			oid_hex CHAR($oid_hex_length) BINARY NOT NULL PRIMARY KEY
		) ENGINE=MEMORY;
		LOAD DATA INFILE '$mysql_manifest'
			INTO TABLE vex_pack_input
			LINES TERMINATED BY '\\n' (oid_hex);
		START TRANSACTION;
		INSERT INTO pack_envelopes
			(repo_id, pack_id, pack_key, idx_key, bitmap_key, covering_generation, state, sealed_at)
		VALUES
			(UNHEX('$repo_id'), UNHEX('$pack_id_padded'), '$pack_key', '$idx_key', NULL,
			 $covering_generation, 'sealed', CURRENT_TIMESTAMP(6))
		ON DUPLICATE KEY UPDATE
			pack_key = VALUES(pack_key),
			idx_key = VALUES(idx_key),
			bitmap_key = VALUES(bitmap_key),
			covering_generation = VALUES(covering_generation),
			state = 'sealed',
			sealed_at = CURRENT_TIMESTAMP(6),
			obsolete_at = NULL;
		DELETE FROM pack_objects
		 WHERE repo_id = UNHEX('$repo_id')
		   AND pack_id = UNHEX('$pack_id_padded');
		INSERT INTO pack_objects (repo_id, pack_id, oid)
			SELECT UNHEX('$repo_id'), UNHEX('$pack_id_padded'), $oid_sql
			  FROM vex_pack_input;
		DELETE FROM pack_rebuild_requests
		 WHERE repo_id = UNHEX('$repo_id')
		   AND target_generation <= $covering_generation;
		SELECT COUNT(*)
		  FROM pack_objects
		 WHERE repo_id = UNHEX('$repo_id')
		   AND pack_id = UNHEX('$pack_id_padded');
		COMMIT;")
	case "$registered_count" in
	"$object_count") ;;
	*)
		printf 'vex build-pack: registered %s object(s), expected %s\n' \
			"$registered_count" "$object_count" >&2
		exit 1
		;;
	esac
fi
pack_registered=1

pending_rebuild_generation=$(mysql_query "
	SELECT target_generation
	  FROM pack_rebuild_requests
	 WHERE repo_id = UNHEX('$repo_id');")
case "$pending_rebuild_generation" in
'') rebuild_request=cleared ;;
*[!0-9]*)
	printf 'vex build-pack: invalid pending generation for %s: %s\n' \
		"$repo_name" "$pending_rebuild_generation" >&2
	exit 1
	;;
*) rebuild_request="pending:$pending_rebuild_generation" ;;
esac

printf 'vex build-pack: PASS repository=%s pack=%s objects=%s generation=%s pack_key=%s idx_key=%s pack_bytes=%s idx_bytes=%s rebuild_request=%s\n' \
	"$repo_name" "$pack_id" "$object_count" "$covering_generation" \
	"$pack_key" "$idx_key" "$pack_bytes" "$idx_bytes" "$rebuild_request"
