#!/bin/sh
#
# This intentionally opt-in test proves the failure path of the full local
# Vex Compose data plane.  It uses a stock host Git client over HTTPS while
# the server stores metadata in MySQL and canonical object bytes in MinIO/S3.

test_description='Vex Compose MySQL + S3 receive compensation'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'stock Git HTTPS rejection compensates its Vex MySQL/S3 receive epoch' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-receive-epoch.sh"
'

test_done
