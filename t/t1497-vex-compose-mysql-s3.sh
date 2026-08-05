#!/bin/sh
#
# This intentionally opt-in test exercises the full local Compose data plane:
# a source-built Vex Git server with MySQL metadata and S3-compatible bytes,
# reached only by an unmodified host Git client over HTTPS.

test_description='Vex Compose MySQL + S3 smart-HTTP E2E'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'stock Git HTTPS P0/P1 ref and shallow-clone basics use Vex MySQL metadata plus S3 bytes' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-stock-http.sh"
'

test_done
