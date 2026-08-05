#!/bin/sh
#
# Opt-in S3 derived-pack cache gate. It proves normal server object reads can
# use a verified local packed source after every S3 byte read is faulted.
# It does not claim direct precomputed-pack streaming to stock clients.

test_description='Vex Compose MySQL + S3 derived pack cache'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'stock clone uses the verified S3-derived packed cache when S3 reads are faulted' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-pack-cache.sh"
'

test_done
