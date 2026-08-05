#!/usr/bin/env bash
#
# Stock-Git incremental push stress entry point.
#
# Profiles are deliberately plain environment files.  CLI flags still win,
# because the Python runner reads them after this wrapper has sourced a profile.

set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
harness_dir="$repo_root/t/vex/push-stress"

if ! command -v python3 >/dev/null 2>&1; then
	printf '%s\n' 'push-stress: python3 is required' >&2
	exit 127
fi

profile=''
arguments=()
while [ "$#" -gt 0 ]; do
	case "$1" in
	--profile)
		if [ "$#" -lt 2 ]; then
			printf '%s\n' 'push-stress: --profile requires a path' >&2
			exit 2
		fi
		profile=$2
		shift 2
		;;
	--profile=*)
		profile=${1#--profile=}
		shift
		;;
	*)
		arguments+=("$1")
		shift
		;;
	esac
done

if [ -n "$profile" ]; then
	case "$profile" in
	/*) profile_path=$profile ;;
	*) profile_path="$PWD/$profile" ;;
	esac
	if [ ! -f "$profile_path" ]; then
		printf 'push-stress: profile not found: %s\n' "$profile" >&2
		exit 2
	fi
	set -a
	# Profiles are repository-controlled .env files, not user shell fragments.
	# shellcheck disable=SC1090
	. "$profile_path"
	set +a
fi

exec python3 "$harness_dir/run.py" "${arguments[@]}"
