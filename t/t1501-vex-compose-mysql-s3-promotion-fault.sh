#!/bin/sh
#
# This intentionally opt-in regression forces one S3 staged-to-final promotion
# failure in the active Compose Vex data plane, then verifies epoch
# compensation with a stock Git HTTPS client.

test_description='Vex Compose MySQL + S3 promotion-failure compensation'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'stock Git HTTPS promotion failure compensates MySQL/S3 and retry succeeds' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-s3-promotion-fault.sh"
'

test_done
