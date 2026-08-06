#!/usr/bin/env bash

# Compose-level dependency check. It verifies the Vex metadata schema, MinIO
# bucket, and that the source-built Vex smart-HTTP gateway can open the active
# MySQL+S3 store before stock-client E2E runs.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/integration-mysql-minio.sh' >&2
	exit 2
fi

"$root/scripts/vex/up.sh"
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"
set -a
# shellcheck disable=SC1090
. "$env_file"
set +a
compose=(docker compose -f "$docker_dir/compose.yaml" --env-file "$env_file")

expected_tables=15
table_count=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name IN ('repositories','generations','objects','generation_objects','commit_parents','refs','ref_updates','object_pins','pack_envelopes','pack_objects','pack_object_offsets','clone_leases','received_pack_objects','pack_rebuild_requests','object_sweep_tombstones');")
[ "$table_count" = "$expected_tables" ] || {
	printf 'vex integration: expected %s Vex metadata tables, found %s; use scripts/vex/reset.sh after schema changes\n' "$expected_tables" "$table_count" >&2
	exit 1
}

blob_key_type=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT DATA_TYPE FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'objects' AND column_name = 'blob_key';")
[ "$blob_key_type" = 'varbinary' ] || {
	printf 'vex integration: objects.blob_key must be metadata (VARBINARY), got %s\n' "$blob_key_type" >&2
	exit 1
}

pack_object_offset_columns=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'pack_object_offsets' AND ((column_name = 'repo_id' AND data_type = 'binary' AND character_maximum_length = 16 AND is_nullable = 'NO') OR (column_name = 'pack_id' AND data_type = 'binary' AND character_maximum_length = 32 AND is_nullable = 'NO') OR (column_name = 'oid' AND data_type = 'binary' AND character_maximum_length = 32 AND is_nullable = 'NO') OR (column_name = 'pack_offset' AND column_type = 'bigint unsigned' AND is_nullable = 'NO'));" )
[ "$pack_object_offset_columns" = 4 ] || {
	printf 'vex integration: pack object offset metadata is incomplete (%s of 4 fields)\n' "$pack_object_offset_columns" >&2
	exit 1
}

pack_object_offset_lookup_index=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) = 3 AND COALESCE(SUM(CASE WHEN seq_in_index = 1 AND column_name = 'repo_id' AND non_unique = 1 THEN 1 WHEN seq_in_index = 2 AND column_name = 'oid' AND non_unique = 1 THEN 1 WHEN seq_in_index = 3 AND column_name = 'pack_id' AND non_unique = 1 THEN 1 ELSE 0 END), 0) = 3 FROM information_schema.statistics WHERE table_schema = DATABASE() AND table_name = 'pack_object_offsets' AND index_name = 'pack_object_offsets_lookup';")
[ "$pack_object_offset_lookup_index" = 1 ] || {
	printf '%s\n' 'vex integration: pack object offset OID lookup index is incomplete' >&2
	exit 1
}

pack_object_offset_fk=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) = 2 AND COALESCE(SUM(CASE WHEN constraint_name = 'pack_object_offsets_pack_fk' AND ((ordinal_position = 1 AND column_name = 'repo_id' AND referenced_table_schema = DATABASE() AND referenced_table_name = 'pack_envelopes' AND referenced_column_name = 'repo_id') OR (ordinal_position = 2 AND column_name = 'pack_id' AND referenced_table_schema = DATABASE() AND referenced_table_name = 'pack_envelopes' AND referenced_column_name = 'pack_id')) THEN 1 ELSE 0 END), 0) = 2 FROM information_schema.key_column_usage WHERE constraint_schema = DATABASE() AND table_name = 'pack_object_offsets' AND referenced_table_name IS NOT NULL;")
[ "$pack_object_offset_fk" = 1 ] || {
	printf '%s\n' 'vex integration: pack object offsets must reference only pack_envelopes' >&2
	exit 1
}

pack_worker_columns=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'pack_rebuild_requests' AND ((column_name = 'worker_lease_id' AND data_type = 'binary' AND character_maximum_length = 16 AND is_nullable = 'YES') OR (column_name = 'claimed_target_generation' AND column_type = 'bigint unsigned' AND is_nullable = 'YES') OR (column_name = 'worker_lease_expires_at' AND data_type = 'datetime' AND datetime_precision = 6 AND is_nullable = 'YES') OR (column_name = 'worker_claimed_at' AND data_type = 'datetime' AND datetime_precision = 6 AND is_nullable = 'YES') OR (column_name = 'attempts' AND column_type = 'int unsigned' AND is_nullable = 'NO' AND column_default = '0') OR (column_name = 'last_error' AND data_type = 'varbinary' AND character_maximum_length = 1024 AND is_nullable = 'YES'));" )
[ "$pack_worker_columns" = 6 ] || {
	printf 'vex integration: pack rebuild worker lease metadata is incomplete (%s of 6 fields)\n' "$pack_worker_columns" >&2
	exit 1
}

pack_worker_claimable_index=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) = 3 AND COALESCE(SUM(CASE WHEN seq_in_index = 1 AND column_name = 'not_before' THEN 1 WHEN seq_in_index = 2 AND column_name = 'worker_lease_expires_at' THEN 1 WHEN seq_in_index = 3 AND column_name = 'repo_id' THEN 1 ELSE 0 END), 0) = 3 FROM information_schema.statistics WHERE table_schema = DATABASE() AND table_name = 'pack_rebuild_requests' AND index_name = 'pack_rebuild_requests_claimable';")
[ "$pack_worker_claimable_index" = 1 ] || {
	printf '%s\n' 'vex integration: pack rebuild worker claimable index is incomplete' >&2
	exit 1
}

# Re-running the oneshot is safe and has mc stat the pre-existing bucket.
"${compose[@]}" run --rm --no-deps minio-init >/dev/null
health=$(curl --fail --silent --show-error --cacert "$docker_dir/certs/rootCA.pem" "https://localhost:${GIT_HTTPS_PORT}/healthz")
printf 'vex integration: MySQL schema and MinIO bucket ready; git-http health: %s\n' "$health"
case "$health" in
*'"blob_backend": "s3"'*'"metadata_backend": "mysql"'*'"vex_storage_active": true'*) ;;
*)
	printf '%s\n' 'vex integration: git-http did not report active MySQL metadata and S3 object bytes' >&2
	exit 1
	;;
esac
