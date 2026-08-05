#!/bin/sh
#
# Opt-in integration gate for a hard-killed Vex receive worker. It requires
# Docker Compose and validates bounded epoch recovery against MySQL plus S3.

test_description='Vex Compose MySQL + S3 receive-worker hard-kill recovery'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'stock Git HTTPS recovers two SIGKILL receive offsets' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-receive-hard-kill.sh"
'

test_done
