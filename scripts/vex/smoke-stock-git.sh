#!/usr/bin/env bash

# Exercise clone, fetch, and push only through an unmodified host git. In
# bootstrap mode this proves smart-HTTP/TLS wiring only, never Vex storage.
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
docker_dir="$repo_root/vex/docker"
env_file="$docker_dir/.env"
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"

usage() {
	printf '%s\n' 'usage: scripts/vex/smoke-stock-git.sh [--origin URL] [--no-seed]'
}

origin=''
seed=1
while [ "$#" -gt 0 ]; do
	case "$1" in
	--origin)
		[ "$#" -ge 2 ] || { printf '%s\n' 'vex smoke: --origin requires a URL' >&2; exit 2; }
		origin=$2
		shift 2
		;;
	--origin=*)
		origin=${1#--origin=}
		shift
		;;
	--no-seed)
		seed=0
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		printf 'vex smoke: unknown argument: %s\n' "$1" >&2
		usage >&2
		exit 2
		;;
	esac
done

set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

default_origin="${GIT_VEX_SMOKE_ORIGIN:-https://localhost:${GIT_HTTPS_PORT}/smoke.git}"
[ -n "$origin" ] || origin=$default_origin
ca_file="$docker_dir/certs/rootCA.pem"
if [ ! -f "$ca_file" ]; then
	printf '%s\n' 'vex smoke: local TLS certificate is missing; run scripts/vex/up.sh first' >&2
	exit 1
fi

if [ "$seed" -eq 1 ] && [ "$origin" = "$default_origin" ]; then
	"$repo_root/scripts/vex/seed-small.sh"
fi

health=$(curl --fail --silent --show-error --cacert "$ca_file" "https://localhost:${GIT_HTTPS_PORT}/healthz")
scratch=$(mktemp -d "${TMPDIR:-/tmp}/vex-stock-git-smoke.XXXXXX")
branch="vex-smoke-$$"
auth=$(printf '%s' "${VEX_HTTP_BASIC_USER}:${VEX_HTTP_BASIC_PASSWORD}" | base64 | tr -d '\n')
cleanup() {
	if [ -d "$scratch/client/.git" ]; then
		git -C "$scratch/client" \
			-c http.sslCAInfo="$ca_file" \
			-c "http.extraHeader=Authorization: Basic $auth" \
			push -q origin ":refs/heads/$branch" >/dev/null 2>&1 || true
	fi
	rm -rf -- "$scratch"
}
trap cleanup EXIT HUP INT TERM

git -c http.sslCAInfo="$ca_file" clone -q "$origin" "$scratch/client"
git -C "$scratch/client" config user.name vex-stock-git-smoke
git -C "$scratch/client" config user.email vex-stock-git-smoke@example.invalid
git -C "$scratch/client" checkout -q -b "$branch"
printf 'stock Git smoke branch %s\n' "$branch" >"$scratch/client/SMOKE"
git -C "$scratch/client" add SMOKE
git -C "$scratch/client" -c commit.gpgSign=false commit -q -m 'stock Git smart HTTP smoke'
expected_oid=$(git -C "$scratch/client" rev-parse HEAD)
git -C "$scratch/client" \
	-c http.sslCAInfo="$ca_file" \
	-c "http.extraHeader=Authorization: Basic $auth" \
	push -q origin "HEAD:refs/heads/$branch"
git -c http.sslCAInfo="$ca_file" clone -q --no-checkout "$origin" "$scratch/verifier"
actual_oid=$(git -C "$scratch/verifier" rev-parse "refs/remotes/origin/$branch")
[ "$actual_oid" = "$expected_oid" ] || {
	printf '%s\n' 'vex smoke: fetched branch tip did not match the pushed commit' >&2
	exit 1
}
git -C "$scratch/verifier" fsck --no-progress >/dev/null
printf 'vex smoke: stock git clone/fetch/push passed for %s\n' "$origin"
case "$health" in
*'"vex_storage_active": false'*)
	printf '%s\n' 'vex smoke: PASS is bootstrap classic-bare protocol evidence only; Vex MySQL/MinIO storage remains unverified.' >&2
	;;
*'"vex_storage_active":true'*|*'"vex_storage_active": true'*)
	printf '%s\n' 'vex smoke: endpoint reports Vex storage active; retain integration evidence for metadata/blob correctness.'
	;;
*)
	printf '%s\n' 'vex smoke: endpoint did not make a backend-readiness claim.' >&2
	;;
esac
