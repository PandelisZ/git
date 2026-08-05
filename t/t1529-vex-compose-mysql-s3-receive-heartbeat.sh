#!/bin/sh
#
# Opt-in integration gate for live receive heartbeats. It distinguishes a
# continuously active MySQL+S3 epoch from the existing hard-killed worker path.

test_description='Vex Compose MySQL + S3 receive heartbeat lease'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'live receive heartbeat survives grace while killed holder recovers' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-receive-heartbeat.sh"
'

test_done
