#!/bin/sh
#
# Opt-in direct precomputed-pack serving gate. It builds a current Vex pack,
# then makes the server's git-pack-objects fallback fail while stock clients
# clone over smart-HTTP protocol v0 and v2.

test_description='Vex Compose MySQL + S3 direct precomputed pack serving'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'stock protocol-v0/v2 clones relay the current Vex pack without git-pack-objects' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-precomputed-pack.sh"
'

test_done
