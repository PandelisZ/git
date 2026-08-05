#!/bin/sh
#
# This is deliberately an opt-in Docker/MinIO integration test.  It proves
# that the Vex ODB uses MinIO for raw bytes while its metadata remains in the
# configured FS store; the Compose git-http bootstrap is not involved.

test_description='Vex S3 blob store with FS metadata'

. ./test-lib.sh

repo_root=$GIT_BUILD_DIR
compose_file=$repo_root/vex/docker/compose.yaml
env_file=$repo_root/vex/docker/.env
vex_repo_id=00000000000000000000000000000034

case "${VEX_TEST_S3:-}" in
1|true|yes)
	test_set_prereq VEX_S3
	;;
esac

# Keep credentials in the Compose container environment rather than putting
# them in a host command line.  minio-init contains the mc client used to
# create the bucket, so it can also attest to the exact final object key.
assert_minio_final_object () {
	# Docker Desktop discovers the Compose plugin from the real user home.  The
	# Git test harness intentionally substitutes HOME with its trash directory.
	HOME="$USER_HOME" docker compose -f "$compose_file" --env-file "$env_file" \
		run --rm --no-deps --entrypoint /bin/sh \
		-e "VEX_S3_OBJECT_KEY=$1" minio-init -c '
			mc alias set vex "$MINIO_ENDPOINT" "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null &&
			mc stat "vex/$MINIO_BUCKET/$VEX_S3_OBJECT_KEY" >/dev/null
		'
}

test_expect_success VEX_S3 'Vex central store keeps bytes in S3 and metadata in FS' '
	HOME="$USER_HOME" "$repo_root/scripts/vex/up.sh" &&
	test_path_is_file "$env_file" &&
	set -a &&
	. "$env_file" &&
	set +a &&

	# GIT_VEX_BACKEND=mysql+s3 names the eventual full deployment and is
	# intentionally not the activation switch for this FS-metadata slice.
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
		git init --bare --initial-branch=main --ref-format=vex vex.git &&
	git -C vex.git config vex.storeRoot "$GIT_VEX_FS_ROOT" &&
	printf "Vex S3 central-store object from %s\\n" "$TRASH_DIRECTORY" >input &&
	oid=$(git -C vex.git hash-object -w "$TRASH_DIRECTORY/input") &&
	git -C vex.git update-ref refs/tags/s3-store "$oid" &&

	metadata_state="$GIT_VEX_FS_ROOT/meta/$vex_repo_id/state" &&
	s3_key="git/v1/$vex_repo_id/objects/$oid" &&
	ref_name_hex=726566732f746167732f73332d73746f7265 &&
	loose_dir=$(printf "%s" "$oid" | cut -c1-2) &&
	loose_name=$(printf "%s" "$oid" | cut -c3-) &&
	test_path_is_file "$metadata_state" &&
	test_grep "$oid" "$metadata_state" &&
	test_grep "$ref_name_hex" "$metadata_state" &&
	test_path_is_missing "$GIT_VEX_FS_ROOT/blobs" &&
	test_path_is_missing "vex.git/objects/$loose_dir/$loose_name" &&
	assert_minio_final_object "$s3_key" &&

	# A fresh Git process must read through the Vex ODB after the write.
	git -C vex.git cat-file blob "$oid" >actual &&
	test_cmp input actual
'

test_done
