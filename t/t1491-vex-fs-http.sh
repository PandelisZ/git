#!/bin/sh

test_description='Vex FS smart-HTTP read path'

. ./test-lib.sh

stock_git=${GIT_VEX_STOCK_GIT:-/usr/bin/git}
if test -x "$stock_git"
then
	test_set_prereq VEX_STOCK_GIT
fi
if test -x "$GIT_BUILD_DIR/git-http-backend"
then
	test_set_prereq VEX_HTTP_BACKEND
fi

test_expect_success VEX_STOCK_GIT,VEX_HTTP_BACKEND 'stock Git clone and fetch use the Vex FS smart-HTTP server' '
	TMPDIR="$TRASH_DIRECTORY" \
	"$GIT_BUILD_DIR/scripts/vex/fs-http-smoke.sh" \
		--git "$GIT_BINARY" \
		--stock-git "$stock_git" \
		--read-only >out 2>err &&
	test_grep "PASS Vex-FS stock-Git clone/fetch" out
'

test_done
