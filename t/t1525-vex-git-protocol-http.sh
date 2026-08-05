#!/bin/sh

test_description='Vex MySQL+S3 gateway Git-Protocol CGI forwarding'

TEST_NO_CREATE_REPO=1

PYTHON3_PATH=$(command -v python3 2>/dev/null || :)

. ./test-lib.sh

if test -x "$PYTHON3_PATH"
then
	test_set_prereq PYTHON3
fi

test_expect_success PYTHON3 'gateway forwards only one sanitized Git-Protocol value to CGI' '
	"$PYTHON3_PATH" "$GIT_BUILD_DIR/t/vex/test-git-protocol-http.py" \
		"$GIT_BUILD_DIR/vex/docker/git-http/vex_mysql_s3_http.py"
'

test_done
