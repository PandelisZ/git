#!/bin/sh
#
# This opt-in test exercises the same stock receive-pack epoch handoff as
# t1493, with Vex metadata on the local FS and canonical object bytes in the
# Compose MinIO bucket.  The Compose git-http bootstrap is not used.

test_description='Vex S3 receive-pack epoch handoff'

. ./test-lib.sh

repo_root=$GIT_BUILD_DIR
compose_file=$repo_root/vex/docker/compose.yaml
env_file=$repo_root/vex/docker/.env
vex_repo_id=00000000000000000000000000000036

case "${VEX_TEST_S3:-}" in
1|true|yes)
	test_set_prereq VEX_S3
	;;
esac

# The Git test harness changes HOME to its trash directory.  Docker Desktop
# needs the real HOME to locate its Compose credential helpers, and minio-init
# is the Compose-owned mc client for direct object assertions.
minio_run () {
	HOME="$USER_HOME" docker compose -f "$compose_file" --env-file "$env_file" \
		run --rm --no-deps --entrypoint /bin/sh "$@"
}

assert_minio_object_exists () {
	minio_run -e "VEX_S3_OBJECT_KEY=$1" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc stat "vex/$MINIO_BUCKET/$VEX_S3_OBJECT_KEY" >/dev/null
	'
}

assert_minio_object_missing () {
	minio_run -e "VEX_S3_OBJECT_KEY=$1" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc stat "vex/$MINIO_BUCKET" >/dev/null
		if mc stat "vex/$MINIO_BUCKET/$VEX_S3_OBJECT_KEY" >/dev/null 2>&1
		then
			printf "unexpected MinIO object: %s\\n" "$VEX_S3_OBJECT_KEY" >&2
			exit 1
		fi
	'
}

assert_minio_staging_prefix_empty () {
	minio_run -e "VEX_S3_STAGING_PREFIX=$1" minio-init -c '
		set -eu
		mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
		mc stat "vex/$MINIO_BUCKET" >/dev/null
		objects=$(mc find "vex/$MINIO_BUCKET/$VEX_S3_STAGING_PREFIX" --type f 2>/dev/null || :)
		if test -n "$objects"
		then
			printf "unexpected MinIO staging objects under: %s\\n" "$VEX_S3_STAGING_PREFIX" >&2
			exit 1
		fi
	'
}

test_expect_success VEX_S3 'stock receive-pack seals accepted S3 objects and compensates rejected objects' '
	HOME="$USER_HOME" "$repo_root/scripts/vex/up.sh" &&
	test_path_is_file "$env_file" &&
	set -a &&
	. "$env_file" &&
	set +a &&

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

	GIT_REFERENCE_BACKEND="vex://$vex_repo_id" \
		git init --bare --initial-branch=main --ref-format=vex remote.git &&
	git -C remote.git config vex.storeRoot "$GIT_VEX_FS_ROOT" &&
	git -C remote.git config receive.denyNonFastForwards true &&

	git init --initial-branch=main client &&
	git -C client config user.email test@example.com &&
	git -C client config user.name Test &&
	git -C client config commit.gpgsign false &&
	printf "accepted S3 receive\\n" >client/file &&
	git -C client add file &&
	git -C client commit -m accepted &&
	git -C client remote add origin "$TRASH_DIRECTORY/remote.git" &&
	git -C client push --receive-pack="git receive-pack" origin main &&

	git -C client rev-parse main >pushed &&
	git -C remote.git rev-parse refs/heads/main >before &&
	test_cmp pushed before &&
	git -C remote.git cat-file -e refs/heads/main^{commit} &&
	git -C remote.git cat-file -t refs/heads/main >actual &&
	echo commit >expect &&
	test_cmp expect actual &&

	accepted_key="git/v1/$vex_repo_id/objects/$(cat pushed)" &&
	staging_prefix="git/v1/$vex_repo_id/staging/" &&
	metadata_state="$GIT_VEX_FS_ROOT/meta/$vex_repo_id/state" &&
	assert_minio_object_exists "$accepted_key" &&
	assert_minio_staging_prefix_empty "$staging_prefix" &&
	test_path_is_file "$metadata_state" &&
	test_path_is_missing "$GIT_VEX_FS_ROOT/blobs" &&
	cp "$metadata_state" state &&
	awk -F "\t" -v oid="$(cat pushed)" \
		"\$1 == \"E\" { sealed[\$2] = (\$3 == 2) } \
		 \$1 == \"O\" && \$2 == oid && \$6 == 2 { found++; epoch = \$5 } \
		 END { exit !(found == 1 && sealed[epoch]) }" state &&
	awk -F "\t" \
		"\$1 == \"E\" && \$3 == 1 { found = 1 } END { exit found }" state &&

	git -C client checkout --orphan rejected &&
	git -C client rm -rf . &&
	printf "rejected S3 receive\\n" >client/file &&
	git -C client add file &&
	git -C client commit -m rejected &&
	git -C client rev-parse rejected >rejected &&
	test_must_fail git -C client push --receive-pack="git receive-pack" \
		origin rejected:refs/heads/main 2>err &&
	git -C remote.git rev-parse refs/heads/main >after &&
	test_cmp before after &&
	assert_minio_object_missing "git/v1/$vex_repo_id/objects/$(cat rejected)" &&
	assert_minio_staging_prefix_empty "$staging_prefix" &&
	test_path_is_missing "$GIT_VEX_FS_ROOT/blobs" &&
	cp "$metadata_state" state &&
	! awk -F "\t" -v oid="$(cat rejected)" \
		"\$1 == \"O\" && \$2 == oid { found = 1 } END { exit !found }" state &&
	awk -F "\t" \
		"\$1 == \"E\" && \$3 == 1 { found = 1 } END { exit found }" state
'

test_done
