#!/bin/sh
#
# Opt-in standalone Vex pack-worker gate. It proves a durable derived-pack
# job executes out of process without taking authority over Git refs/epochs.

test_description='Vex Compose MySQL + S3 standalone pack worker'

. ./test-lib.sh

case "${VEX_TEST_COMPOSE_MYSQL_S3:-}" in
1|true|yes)
	if command -v docker >/dev/null 2>&1 && \
		HOME="$USER_HOME" docker compose version >/dev/null 2>&1
	then
		test_set_prereq VEX_COMPOSE_MYSQL_S3
	fi
	;;
esac

test_expect_success VEX_COMPOSE_MYSQL_S3 'standalone worker publishes a current S3 pack without owning refs or epochs' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-pack-worker.sh"
'

test_done
