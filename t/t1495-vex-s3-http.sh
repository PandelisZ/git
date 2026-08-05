#!/bin/sh
#
# This is an opt-in transport integration test.  Compose supplies MinIO only:
# the Compose git-http bootstrap remains classic-bare and is never used here.
# fs-http-smoke.sh starts its own loopback Vex-aware HTTP server, and the
# explicit /usr/bin/git client is intentionally unmodified stock Git.

test_description='Vex S3 stock-Git smart-HTTP'

. ./test-lib.sh

repo_root=$GIT_BUILD_DIR
env_file=$repo_root/vex/docker/.env

case "${VEX_TEST_S3:-}" in
1|true|yes) test_set_prereq VEX_S3 ;;
esac

test_expect_success VEX_S3 'stock Git clone/fetch/push uses Vex S3 blobs through loopback HTTP' '
	HOME=$USER_HOME "$repo_root/scripts/vex/up.sh" &&
	test_path_is_file "$env_file" &&
	set -a &&
	. "$env_file" &&
	set +a &&

	# Select only the blob provider.  FS remains the metadata store in this
	# slice; GIT_VEX_BACKEND=mysql+s3 is not an activation switch.
	GIT_VEX_BLOB_BACKEND=s3 &&
	GIT_VEX_FS_ROOT="$TRASH_DIRECTORY/vex-fs-metadata" &&
	GIT_VEX_S3_ENDPOINT="http://127.0.0.1:$MINIO_API_PORT" &&
	GIT_VEX_S3_BUCKET="$MINIO_BUCKET" &&
	GIT_VEX_S3_REGION="$MINIO_REGION" &&
	GIT_VEX_S3_PATH_STYLE=1 &&
	GIT_VEX_S3_ACCESS_KEY="$MINIO_ROOT_USER" &&
	GIT_VEX_S3_SECRET_KEY="$MINIO_ROOT_PASSWORD" &&
	export GIT_VEX_BLOB_BACKEND GIT_VEX_FS_ROOT GIT_VEX_S3_ENDPOINT \
		GIT_VEX_S3_BUCKET GIT_VEX_S3_REGION GIT_VEX_S3_PATH_STYLE \
		GIT_VEX_S3_ACCESS_KEY GIT_VEX_S3_SECRET_KEY &&

	"$repo_root/scripts/vex/fs-http-smoke.sh" \
		--git "$GIT_BUILD_DIR/git" --stock-git /usr/bin/git
'

test_done
