#!/usr/bin/env bash

# Compile the standalone Vex metadata driver with a Linux MySQL-compatible C
# client and run it on the Compose network.  This avoids accidentally testing
# another MySQL instance that may already own host port 3306 on a developer
# workstation.  The build container is removed after each run.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
docker_dir="$root/vex/docker"
env_file="$docker_dir/.env"
probe_image=${VEX_MYSQL_PROBE_IMAGE:-debian:bookworm-slim}

if [ "$#" -gt 0 ]; then
	printf '%s\n' 'usage: t/vex/mysql-metadata-probe.sh' >&2
	exit 2
fi

"$root/scripts/vex/up.sh"
[ -f "$env_file" ] || env_file="$docker_dir/.env.example"
set -a
# shellcheck disable=SC1090
. "$env_file"
set +a

export GIT_VEX_MYSQL_HOST=mysql
export GIT_VEX_MYSQL_PORT=3306
export GIT_VEX_MYSQL_USER="$MYSQL_USER"
export GIT_VEX_MYSQL_PASSWORD="$MYSQL_PASSWORD"
export GIT_VEX_MYSQL_DATABASE="$MYSQL_DATABASE"

docker run --rm --network vex_net \
	--env GIT_VEX_MYSQL_HOST \
	--env GIT_VEX_MYSQL_PORT \
	--env GIT_VEX_MYSQL_USER \
	--env GIT_VEX_MYSQL_PASSWORD \
	--env GIT_VEX_MYSQL_DATABASE \
	-v "$root:/src:ro" -w /tmp "$probe_image" sh -c '
		set -eu
		apt-get update >/dev/null
		DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
			build-essential default-libmysqlclient-dev pkg-config >/dev/null
		cc -std=gnu99 -Wall -Wextra -Werror -I/src \
			$(pkg-config --cflags libmariadb) -DVEX_HAVE_MYSQL_CLIENT \
			/src/t/vex/mysql-metadata-probe.c /src/vex/meta-mysql.c \
			$(pkg-config --libs libmariadb) -o vex-mysql-metadata-probe
		exec ./vex-mysql-metadata-probe
	'

printf '%s\n' 'vex mysql integration: native metadata driver passed against Compose MySQL'
