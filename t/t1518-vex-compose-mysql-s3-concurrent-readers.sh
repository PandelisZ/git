#!/bin/sh
#
# Opt-in bounded reader/writer correctness gate for the active local Vex
# MySQL+S3 Compose topology.  It is intentionally not an L4 50-client claim.

test_description='Vex Compose MySQL + S3 bounded concurrent readers'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'eight stock-Git readers survive a 20-push writer and reconcile MySQL/S3' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-concurrent-readers.sh"
'

test_done
