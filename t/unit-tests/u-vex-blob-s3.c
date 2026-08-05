#include "unit-test.h"
#include "hex.h"
#include "object-file.h"
#include "strbuf.h"
#include "vex/blob-s3.h"

static const struct git_hash_algo *hash_algo = &hash_algos[GIT_HASH_SHA1];

static struct vex_blob_s3 *open_test_blob(void)
{
	struct vex_blob_s3_options opts = {
		.endpoint = "http://127.0.0.1:9000/",
		.bucket = "git-vex-objects",
		.region = "us-east-1",
		.access_key = "vexminio",
		.secret_key = "vex-local-minio",
		.repo_id = "00000000-0000-4000-8000-000000000001",
		.hash_algo = hash_algo,
	};
	struct vex_blob_s3 *blob;

	cl_must_pass(vex_blob_s3_open(&blob, &opts));
	return blob;
}

void test_vex_blob_s3__keys_use_the_vex_per_repository_layout(void)
{
	struct vex_blob_s3 *blob = open_test_blob();
	struct object_id oid;
	struct strbuf final = STRBUF_INIT;
	struct strbuf staged = STRBUF_INIT;
	struct strbuf expected = STRBUF_INIT;
	char hex[GIT_MAX_HEXSZ + 1];

	hash_object_file(hash_algo, "vex-s3-key", strlen("vex-s3-key"),
			 OBJ_BLOB, &oid);
	hash_to_hex_algop_r(hex, oid.hash, hash_algo);
	cl_must_pass(vex_blob_s3_final_key(blob, &oid, &final));
	cl_must_pass(vex_blob_s3_staging_key(blob, 47, &oid, &staged));
	strbuf_addf(&expected,
		    "git/v1/00000000000040008000000000000001/objects/%s", hex);
	cl_assert_equal_s(final.buf, expected.buf);
	strbuf_reset(&expected);
	strbuf_addf(&expected,
		    "git/v1/00000000000040008000000000000001/staging/47/objects/%s",
		    hex);
	cl_assert_equal_s(staged.buf, expected.buf);
	strbuf_release(&final);
	strbuf_release(&staged);
	strbuf_release(&expected);
	vex_blob_s3_close(blob);
}

void test_vex_blob_s3__minio_put_read_promote_when_requested(void)
{
	const char *run = getenv("VEX_S3_INTEGRATION");
	struct vex_blob_s3 *blob;
	struct object_id oid;
	struct object_id read_oid;
	char payload[128];
	void *read_back;
	unsigned long size;

	if (!run || strcmp(run, "1")) {
		cl_skip();
		return;
	}
	blob = open_test_blob();
	xsnprintf(payload, sizeof(payload),
		  "Vex S3 driver integration payload pid=%ld\n", (long)getpid());
	hash_object_file(hash_algo, payload, strlen(payload), OBJ_BLOB, &oid);
	cl_must_pass(vex_blob_s3_check_bucket(blob));
	cl_must_pass(vex_blob_s3_put_staging(blob, 47001, &oid, payload,
					 strlen(payload)));
	cl_assert_equal_i(vex_blob_s3_has_final(blob, &oid), 0);
	cl_must_pass(vex_blob_s3_read_staging(blob, 47001, &oid, &read_back,
					  &size));
	cl_assert_equal_i(size, strlen(payload));
	cl_assert_equal_s(read_back, payload);
	free(read_back);
	cl_must_pass(vex_blob_s3_promote_object(blob, 47001, &oid));
	cl_assert_equal_i(vex_blob_s3_has_final(blob, &oid), 1);
	cl_must_pass(vex_blob_s3_read_final(blob, &oid, &read_back, &size));
	cl_assert_equal_i(size, strlen(payload));
	cl_assert_equal_s(read_back, payload);
	hash_object_file(hash_algo, read_back, size, OBJ_BLOB, &read_oid);
	cl_assert(oideq(&read_oid, &oid));
	free(read_back);
	cl_assert_equal_i(vex_blob_s3_read_staging(blob, 47001, &oid,
						    &read_back, &size),
			  VEX_BLOB_S3_ERR_NOT_FOUND);
	cl_must_pass(vex_blob_s3_delete_staging(blob, 47001, &oid));
	vex_blob_s3_close(blob);
}

void test_vex_blob_s3__minio_parallel_receive_batch_is_all_or_nothing(void)
{
	const char *run = getenv("VEX_S3_INTEGRATION");
	struct vex_blob_s3 *blob;
	struct object_id oids[2];
	struct vex_blob_s3_staged_object objects[2];
	char payloads[2][128];
	void *read_back;
	unsigned long size;
	size_t i;

	if (!run || strcmp(run, "1")) {
		cl_skip();
		return;
	}
	for (i = 0; i < ARRAY_SIZE(objects); i++) {
		xsnprintf(payloads[i], sizeof(payloads[i]),
			  "Vex S3 receive batch payload=%zu pid=%ld\n", i,
			  (long)getpid());
		hash_object_file(hash_algo, payloads[i], strlen(payloads[i]),
				 OBJ_BLOB, &oids[i]);
		objects[i].oid = &oids[i];
		objects[i].data = payloads[i];
		objects[i].size = strlen(payloads[i]);
	}
	blob = open_test_blob();
	cl_must_pass(vex_blob_s3_put_staging_batch(blob, 47002, objects,
						     ARRAY_SIZE(objects)));
	for (i = 0; i < ARRAY_SIZE(objects); i++) {
		cl_must_pass(vex_blob_s3_read_staging(blob, 47002, &oids[i],
						      &read_back, &size));
		cl_assert_equal_i(size, objects[i].size);
		cl_assert_equal_s(read_back, payloads[i]);
		free(read_back);
	}
	cl_must_pass(vex_blob_s3_promote_batch(blob, 47002, oids,
						ARRAY_SIZE(oids)));
	for (i = 0; i < ARRAY_SIZE(objects); i++) {
		cl_assert_equal_i(vex_blob_s3_has_final(blob, &oids[i]), 1);
		cl_assert_equal_i(vex_blob_s3_read_staging(blob, 47002, &oids[i],
							    &read_back, &size),
				  VEX_BLOB_S3_ERR_NOT_FOUND);
	}
	vex_blob_s3_close(blob);

	/* One real upload then a synthetic batch failure must leave no staging key. */
	cl_must_pass(setenv("GIT_VEX_TEST_S3_FAIL_STAGE_BATCH_ONCE", "1", 1));
	blob = open_test_blob();
	cl_must_pass(unsetenv("GIT_VEX_TEST_S3_FAIL_STAGE_BATCH_ONCE"));
	cl_assert_equal_i(vex_blob_s3_put_staging_batch(blob, 47003, objects,
							   ARRAY_SIZE(objects)),
			  VEX_BLOB_S3_ERR);
	for (i = 0; i < ARRAY_SIZE(objects); i++)
		cl_assert_equal_i(vex_blob_s3_read_staging(blob, 47003, &oids[i],
							    &read_back, &size),
				  VEX_BLOB_S3_ERR_NOT_FOUND);
	vex_blob_s3_close(blob);
}
