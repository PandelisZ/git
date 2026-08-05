#!/bin/sh
#
# Opt-in S3 derived-pack artifact/lifecycle gate. It proves a stock receive
# invalidates an old artifact, queues one debounced rebuild, and lets an
# operator-side job seal the latest generation without a receive-time repack.

test_description='Vex Compose MySQL + S3 immutable pack artifact'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'pack artifacts invalidate, debounce rebuilds, and survive stock-Git receives' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-pack-artifact.sh"
'

test_done
