#!/bin/sh
#
# Opt-in stock-Git Compose proof for a request larger than the former 64 MiB
# gateway buffer limit.  The fixture forces a Content-Length smart-HTTP push;
# the Vex gateway must relay it incrementally into git-http-backend.

test_description='Vex Compose MySQL + S3 streaming smart-HTTP relay'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'stock Git relays a >64 MiB Content-Length push and clones it back' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-streaming-http.sh"
'

test_done
