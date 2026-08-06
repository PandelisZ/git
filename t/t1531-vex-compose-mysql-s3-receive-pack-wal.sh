#!/bin/sh
#
# Opt-in default-clone relay gate. It seeds a new Vex repository through the
# pack WAL, builds the narrower heads/tags artifact, then makes upload-pack's
# pack-objects fallback fail while stock protocol-v0 and protocol-v2 clients
# clone the default branch and fsck that exact partial pack.

test_description='Vex Compose MySQL + S3 default-clone relay'

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

test_expect_success VEX_COMPOSE_MYSQL_S3 'stock protocol-v0/v2 default-branch clones relay the sealed default pack without git-pack-objects' '
	HOME="$USER_HOME" "$GIT_BUILD_DIR/t/vex/mysql-s3-receive-pack-wal.sh"
'

test_done
