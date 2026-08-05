#!/bin/sh

# Idempotent MinIO bootstrap.  This creates the namespace in which the Vex
# blob backend stores canonical object bytes; it does not write Git objects.
set -eu

alias_name=vex-local
attempt=0
max_attempts=${MINIO_INIT_MAX_ATTEMPTS:-60}

while ! mc alias set "$alias_name" "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null 2>&1; do
	attempt=$((attempt + 1))
	if [ "$attempt" -ge "$max_attempts" ]; then
		printf '%s\n' 'minio-init: unable to configure the MinIO alias' >&2
		exit 1
	fi
	sleep 1
done

mc mb --ignore-existing "$alias_name/$MINIO_BUCKET" >/dev/null
mc stat "$alias_name/$MINIO_BUCKET" >/dev/null
printf 'minio-init: bucket ready: %s\n' "$MINIO_BUCKET"
