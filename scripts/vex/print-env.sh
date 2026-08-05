#!/usr/bin/env bash

# Emit host-side Vex configuration as shell-safe exports. Use as:
#   eval "$(scripts/vex/print-env.sh)"
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
docker_dir="$repo_root/vex/docker"
env_file="$docker_dir/.env"
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: scripts/vex/print-env.sh' >&2
	exit 2
fi

set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

emit() {
	printf 'export %s=%q\n' "$1" "$2"
}

printf '%s\n' '# Host-side Vex configuration. GIT_VEX_BACKEND is the planned full topology label.'
printf '%s\n' '# GIT_VEX_BLOB_BACKEND=s3 selects the implemented S3-byte/FS-metadata slice.'
emit GIT_VEX_BACKEND "${GIT_VEX_BACKEND:-mysql+s3}"
emit GIT_VEX_BLOB_BACKEND "${GIT_VEX_BLOB_BACKEND:-s3}"
emit GIT_VEX_MYSQL_DSN "mysql://${MYSQL_USER}:${MYSQL_PASSWORD}@127.0.0.1:${MYSQL_PORT}/${MYSQL_DATABASE}"
emit GIT_VEX_S3_ENDPOINT "http://127.0.0.1:${MINIO_API_PORT}"
emit GIT_VEX_S3_BUCKET "$MINIO_BUCKET"
emit GIT_VEX_S3_REGION "$MINIO_REGION"
emit GIT_VEX_S3_PATH_STYLE 1
emit GIT_VEX_S3_ACCESS_KEY "$MINIO_ROOT_USER"
emit GIT_VEX_S3_SECRET_KEY "$MINIO_ROOT_PASSWORD"
emit GIT_VEX_SCRATCH "${TMPDIR:-/tmp}/vex-git-scratch"
emit GIT_VEX_REPO_GIT_GIT_UUID "$GIT_VEX_REPO_GIT_GIT_UUID"
emit GIT_VEX_ORIGIN "${GIT_VEX_ORIGIN:-https://localhost:${GIT_HTTPS_PORT}/git/git.git}"
emit GIT_VEX_SMOKE_ORIGIN "${GIT_VEX_SMOKE_ORIGIN:-https://localhost:${GIT_HTTPS_PORT}/smoke.git}"
emit GIT_SSL_CAINFO "$docker_dir/certs/rootCA.pem"
