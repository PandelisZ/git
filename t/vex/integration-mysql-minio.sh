#!/usr/bin/env bash

# Compose-level dependency check. It verifies that the Vex metadata schema is
# present in MySQL and that the MinIO bucket exists, without pretending that
# the bootstrap Git endpoint stores Git data in either service.
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

expected_tables=10
table_count=$("${compose[@]}" exec -T mysql \
	mysql --protocol=TCP -h 127.0.0.1 -u "$MYSQL_USER" -p"$MYSQL_PASSWORD" -N -B "$MYSQL_DATABASE" \
	-e "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name IN ('repositories','generations','objects','generation_objects','commit_parents','refs','ref_updates','object_pins','pack_envelopes','pack_objects');")
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

# Re-running the oneshot is safe and has mc stat the pre-existing bucket.
"${compose[@]}" run --rm --no-deps minio-init >/dev/null
health=$(curl --fail --silent --show-error --cacert "$docker_dir/certs/rootCA.pem" "https://localhost:${GIT_HTTPS_PORT}/healthz")
printf 'vex integration: MySQL schema and MinIO bucket ready; git-http health: %s\n' "$health"
case "$health" in
*'"vex_storage_active": false'*)
	printf '%s\n' 'vex integration: dependency readiness passed; no Vex Git object/ref data-plane claim in bootstrap mode.' >&2
	;;
esac
