#!/bin/sh
#
# Opt-in destructive Vex sweep gate. It verifies a durable grace tombstone,
# metadata-before-S3 ordering, and a retry after final blob deletion fails.

test_description='Vex Compose MySQL + S3 destructive sweep'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'grace tombstone delays destructive sweep and final S3 cleanup retries' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-sweep.sh"
'

test_done
