#!/bin/sh

test_description='Vex Compose MySQL + S3 pin-aware sweep planner'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'pins protect a sealed old graph across a later stock-Git push' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-sweep-plan.sh"
'

test_done
