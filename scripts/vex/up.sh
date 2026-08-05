#!/usr/bin/env bash

# Bring up the local Vex MySQL + MinIO topology. A successful bootstrap
# listener is intentionally reported as protocol-ready, not storage-ready.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
docker_dir="$repo_root/vex/docker"
compose_file="$docker_dir/compose.yaml"
env_file="$docker_dir/.env"

usage() {
	cat <<'EOF'
usage: scripts/vex/up.sh [--profile NAME] [--no-build]

Starts local-only MySQL, MinIO, minio-init, and git-http. The default
git-http image is an explicitly classic-bare bootstrap until a Vex-aware
server command is configured in vex/docker/.env.
EOF
}

profiles=()
build=1
while [ "$#" -gt 0 ]; do
	case "$1" in
	--profile)
		[ "$#" -ge 2 ] || { printf '%s\n' 'vex up: --profile requires a name' >&2; exit 2; }
		profiles+=("$2")
		shift 2
		;;
	--profile=*)
		profiles+=("${1#--profile=}")
		shift
		;;
	--no-build)
		build=0
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		printf 'vex up: unknown argument: %s\n' "$1" >&2
		usage >&2
		exit 2
		;;
	esac
done

if ! command -v docker >/dev/null 2>&1; then
	printf '%s\n' 'vex up: docker with the Compose plugin is required' >&2
	exit 127
fi
if ! docker compose version >/dev/null 2>&1; then
	printf '%s\n' 'vex up: docker compose is unavailable' >&2
	exit 127
fi

if [ ! -f "$env_file" ]; then
	cp "$docker_dir/.env.example" "$env_file"
	chmod 600 "$env_file"
	printf 'vex up: created local configuration %s\n' "$env_file"
fi
"$docker_dir/certs/gen.sh"

set -a
# The file is repo-controlled when absent and is otherwise the caller's local
# Compose configuration. It contains simple KEY=VALUE settings only.
# shellcheck disable=SC1090
. "$env_file"
set +a

compose=(docker compose -f "$compose_file" --env-file "$env_file")
if [ "${#profiles[@]}" -gt 0 ]; then
	for profile in "${profiles[@]}"; do
		compose+=(--profile "$profile")
	done
fi

"${compose[@]}" config --quiet
up_args=(up -d --wait --wait-timeout "${VEX_COMPOSE_WAIT_SECONDS:-120}")
if [ "$build" -eq 1 ]; then
	up_args+=(--build)
fi
"${compose[@]}" "${up_args[@]}"

minio_init_id=$("${compose[@]}" ps --all -q minio-init)
if [ -z "$minio_init_id" ]; then
	printf '%s\n' 'vex up: minio-init did not create a container' >&2
	exit 1
fi
minio_init_exit=$(docker inspect -f '{{.State.ExitCode}}' "$minio_init_id")
if [ "$minio_init_exit" != 0 ]; then
	printf 'vex up: minio-init failed with exit code %s\n' "$minio_init_exit" >&2
	"${compose[@]}" logs --no-log-prefix minio-init >&2 || true
	exit 1
fi

health_url="https://localhost:${GIT_HTTPS_PORT}/healthz"
health=$(curl --fail --silent --show-error --cacert "$docker_dir/certs/rootCA.pem" "$health_url")
printf 'vex up: git-http health: %s\n' "$health"
case "$health" in
*'"vex_storage_active": false'*)
	printf '%s\n' 'vex up: READY for MySQL/MinIO and stock-Git HTTP bootstrap; Vex storage is NOT active yet.' >&2
	printf '%s\n' 'vex up: Bootstrap successes must not be interpreted as MySQL-ref or MinIO-object verification.' >&2
	;;
*'"vex_storage_active":true'*|*'"vex_storage_active": true'*)
	printf '%s\n' 'vex up: Vex-aware git-http endpoint reports storage active.'
	;;
*)
	printf '%s\n' 'vex up: git-http health did not declare backend readiness; inspect its health payload before treating storage as active.' >&2
	;;
esac
