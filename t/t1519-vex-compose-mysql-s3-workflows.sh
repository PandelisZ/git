#!/bin/sh
#
# Opt-in P0 stock-Git workflow correctness gate for the active local Vex
# MySQL+S3 Compose topology.  It is a functional workflow test, not an L4
# throughput or concurrency claim.

test_description='Vex Compose MySQL + S3 stock-Git P0 workflows'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'stock Git HTTPS clients complete diverge/rebase/lease/merge workflow and reconcile MySQL/S3' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-stock-workflows.sh"
'

test_done
