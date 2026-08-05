#!/bin/sh
#
# Opt-in because this creates a unique local Vex Compose repository and emits a
# short clone measurement.  It validates benchmark artifact shape only; it
# intentionally carries no clone-latency acceptance threshold or L4 claim.

test_description='Vex Compose MySQL + S3 clone benchmark artifact'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'stock Git HTTPS clone benchmark records p50/p90 and local-disk ratios without an L4 threshold' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/bench-clone-smoke.sh"
'

test_done
