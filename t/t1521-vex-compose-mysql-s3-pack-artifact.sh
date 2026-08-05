#!/bin/sh
#
# Opt-in S3 derived-pack artifact gate. It does not claim that stock clone is
# served from this pack; that remains the next L4 serving milestone.

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'custom-Git immutable pack/index goes to S3 and survives a later stock-Git receive' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-pack-artifact.sh"
'

test_done
