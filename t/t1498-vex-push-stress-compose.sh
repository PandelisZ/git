#!/bin/sh
#
# Opt-in because this leaves benchmark JSON under t/vex/push-stress/work/ and
# creates a unique repository in the local Vex Compose topology.

test_description='Vex Compose MySQL + S3 push-stress smoke'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'stock Git drives twenty batch-one pushes and fresh reconciliation' '
	cd "$GIT_BUILD_DIR" &&
	HOME="$USER_HOME" "$GIT_BUILD_DIR/scripts/vex/push-stress.sh" --compose-smoke
'

test_done
