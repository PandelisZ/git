#!/bin/sh
#
# This opt-in Compose test runs the MySQL-indexed S3 byte scrubber after a
# stock Git HTTPS publish. It remains out of normal developer test loops.

test_description='Vex Compose MySQL + S3 integrity scrub'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'scrub hashes each published S3 object against MySQL metadata' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-scrub.sh"
'

test_done
