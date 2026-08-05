#!/usr/bin/env bash

# Stop the local Vex Compose project. Volumes are preserved unless the caller
# deliberately supplies --volumes (normally through reset.sh).
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
docker_dir="$repo_root/vex/docker"
env_file="$docker_dir/.env"
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"

usage() {
	printf '%s\n' 'usage: scripts/vex/down.sh [--volumes]'
}

volumes=0
while [ "$#" -gt 0 ]; do
	case "$1" in
	--volumes)
		volumes=1
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		printf 'vex down: unknown argument: %s\n' "$1" >&2
		usage >&2
		exit 2
		;;
	esac
done

if ! command -v docker >/dev/null 2>&1 || ! docker compose version >/dev/null 2>&1; then
	printf '%s\n' 'vex down: docker with the Compose plugin is required' >&2
	exit 127
fi

# Compose only includes profiled services when the profile is enabled. Include
# the optional cache profile here so a plain `down.sh` also stops Valkey after
# an earlier `up.sh --profile cache`.
compose=(docker compose -f "$docker_dir/compose.yaml" --env-file "$env_file" --profile cache)
down_args=(down --remove-orphans)
if [ "$volumes" -eq 1 ]; then
	down_args+=(--volumes)
	printf '%s\n' 'vex down: removing Vex Compose volumes (local metadata, blobs, and thin Vex repository directories).'
fi
"${compose[@]}" "${down_args[@]}"
