#!/bin/sh
#
# Opt-in S3 derived-pack lease gate. It proves an obsolete immutable pack
# remains a conservative sweep root while its bounded lease is live. This is
# retention evidence only, not direct precomputed-pack client serving.

test_description='Vex Compose MySQL + S3 immutable pack lease'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'lease root protects obsolete pack members in read-only sweep plan' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-pack-lease.sh"
'

test_done
