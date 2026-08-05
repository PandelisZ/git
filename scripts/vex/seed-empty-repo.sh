#!/usr/bin/env bash

# Ensure an empty repository exists for stock-Git push tests. In the supplied
# bootstrap this is a classic bare repo; a configured Vex server must provide
# VEX_REPO_INIT_COMMAND rather than silently using this fallback.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
docker_dir="$repo_root/vex/docker"
env_file="$docker_dir/.env"
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"

usage() {
	printf '%s\n' 'usage: scripts/vex/seed-empty-repo.sh [name-or-path]'
}

case "$#" in
0) repo_name=stress/git-replay ;;
1) repo_name=$1 ;;
*) usage >&2; exit 2 ;;
esac
case "$repo_name" in
'' | /* | *'..'* | *'//'*)
	printf 'vex seed-empty-repo: unsafe repository name: %s\n' "$repo_name" >&2
	exit 2
	;;
esac
case "$repo_name" in
*.git) ;;
*) repo_name="$repo_name.git" ;;
esac

set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

if [ ! -f "$docker_dir/certs/rootCA.pem" ]; then
	printf '%s\n' 'vex seed-empty-repo: local TLS certificate is missing; run scripts/vex/up.sh first' >&2
	exit 1
fi
compose=(docker compose -f "$docker_dir/compose.yaml" --env-file "$env_file")
health=$(curl --fail --silent --show-error --cacert "$docker_dir/certs/rootCA.pem" "https://localhost:${GIT_HTTPS_PORT}/healthz")
"${compose[@]}" exec -T git-http /usr/local/bin/vex-init-repo "$repo_name"

origin="https://localhost:${GIT_HTTPS_PORT}/${repo_name}"
printf 'vex seed-empty-repo: origin=%s\n' "$origin"
case "$health" in
*'"vex_storage_active": false'*)
	printf '%s\n' 'vex seed-empty-repo: this repository is on the classic-bare bootstrap volume, not Vex storage.' >&2
	;;
esac
