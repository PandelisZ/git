#!/bin/sh
#
# This intentionally opt-in regression exercises two independent repositories
# on the full local Vex Compose MySQL+S3 data plane at the same time.

test_description='Vex Compose MySQL + S3 cross-repository concurrency'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'simultaneous stock Git pushes retain one MySQL ref audit row per repository' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-cross-repo-concurrency.sh"
'

test_done
