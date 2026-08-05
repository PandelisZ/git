#!/usr/bin/env bash

# Seed a tiny repository through the stock Git HTTPS receive-pack path. It is
# idempotent: once refs/heads/main exists, it leaves the fixture untouched.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
docker_dir="$repo_root/vex/docker"
env_file="$docker_dir/.env"
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: scripts/vex/seed-small.sh' >&2
	exit 2
fi

set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

origin="${GIT_VEX_SMOKE_ORIGIN:-https://localhost:${GIT_HTTPS_PORT}/smoke.git}"
ca_file="$docker_dir/certs/rootCA.pem"
if [ ! -f "$ca_file" ]; then
	printf '%s\n' 'vex seed-small: local TLS certificate is missing; run scripts/vex/up.sh first' >&2
	exit 1
fi

"$repo_root/scripts/vex/seed-empty-repo.sh" smoke >/dev/null
if [ -n "$(git -c http.sslCAInfo="$ca_file" ls-remote "$origin" refs/heads/main)" ]; then
	printf 'vex seed-small: fixture already exists at %s\n' "$origin"
	exit 0
fi

scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-seed-small.XXXXXX")
cleanup() {
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

worktree="$scratch/worktree"
git init -q "$worktree"
git -C "$worktree" config user.name vex-local-seed
git -C "$worktree" config user.email vex-local-seed@example.invalid
for revision in 1 2 3; do
	printf 'Vex local fixture revision %s\n' "$revision" >"$worktree/README"
	git -C "$worktree" add README
	git -C "$worktree" -c commit.gpgSign=false commit -q -m "vex fixture $revision"
done
git -C "$worktree" branch -M main
git -C "$worktree" remote add origin "$origin"
auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
git -C "$worktree" \
	-c http.sslCAInfo="$ca_file" \
	-c "http.extraHeader=Authorization: Basic $auth" \
	push -q -u origin HEAD:refs/heads/main
printf 'vex seed-small: pushed stock-Git fixture to %s\n' "$origin"
