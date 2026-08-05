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

usage() {
	cat <<'EOF'
usage: scripts/vex/push-stress.sh [--profile FILE] [runner options...]
       scripts/vex/push-stress.sh --compose-smoke

--compose-smoke uses stock /usr/bin/git to run the 20 x batch=1 smoke profile
against a unique empty Vex MySQL+S3 Compose repository. It retains JSON output
under t/vex/push-stress/work/, which is ignored by Git.
EOF
}

profile=''
compose_smoke=0
arguments=()
while [ "$#" -gt 0 ]; do
	case "$1" in
	--compose-smoke)
		compose_smoke=1
		shift
		;;
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
	-h|--help)
		usage
		exit 0
		;;
	*)
		arguments+=("$1")
		shift
		;;
	esac
done

if [ "$compose_smoke" -eq 1 ]; then
	if [ -n "$profile" ] || [ "${#arguments[@]}" -ne 0 ]; then
		printf '%s\n' 'push-stress: --compose-smoke does not accept a profile or runner options' >&2
		exit 2
	fi
	exec "$harness_dir/compose-smoke.sh"
fi

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
