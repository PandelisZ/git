#include "unit-test.h"
#include "hex.h"
#include "object-file.h"
#include "strbuf.h"
#include "tempfile.h"
#include "vex/blob-s3.h"
#include "wrapper.h"

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

/* A minimal valid, empty v2 pack and matching v2 index. */
static void write_test_receive_pack_pair(struct tempfile **pack_file,
					 struct tempfile **idx_file,
					 unsigned char *pack_id)
{
	unsigned char pack_header[] = {
		'P', 'A', 'C', 'K', 0, 0, 0, 2, 0, 0, 0, 0,
	};
	unsigned char idx_header[] = { 0xff, 't', 'O', 'c', 0, 0, 0, 2 };
	unsigned char fanout[256 * sizeof(uint32_t)] = { 0 };
	unsigned char idx_checksum[GIT_MAX_RAWSZ];
	struct git_hash_ctx hash_ctx;
	struct strbuf pack = STRBUF_INIT;
	struct strbuf idx = STRBUF_INIT;

	git_hash_init(&hash_ctx, hash_algo);
	git_hash_update(&hash_ctx, pack_header, sizeof(pack_header));
	git_hash_final(pack_id, &hash_ctx);
	strbuf_add(&pack, pack_header, sizeof(pack_header));
	strbuf_add(&pack, pack_id, hash_algo->rawsz);

	strbuf_add(&idx, idx_header, sizeof(idx_header));
	strbuf_add(&idx, fanout, sizeof(fanout));
	strbuf_add(&idx, pack_id, hash_algo->rawsz);
	git_hash_init(&hash_ctx, hash_algo);
	git_hash_update(&hash_ctx, idx.buf, idx.len);
	git_hash_final(idx_checksum, &hash_ctx);
	strbuf_add(&idx, idx_checksum, hash_algo->rawsz);

	*pack_file = mks_tempfile_t("vex-s3-receive-pack-XXXXXX");
	*idx_file = mks_tempfile_t("vex-s3-receive-idx-XXXXXX");
	cl_assert(*pack_file != NULL);
	cl_assert(*idx_file != NULL);
	cl_assert_equal_i(write_in_full((*pack_file)->fd, pack.buf, pack.len),
			  pack.len);
	cl_assert_equal_i(write_in_full((*idx_file)->fd, idx.buf, idx.len),
			  idx.len);
	cl_must_pass(close_tempfile_gently(*pack_file));
	cl_must_pass(close_tempfile_gently(*idx_file));
	strbuf_release(&idx);
	strbuf_release(&pack);
}

static void assert_same_file(const char *expected_path, const char *actual_path)
{
	struct strbuf expected = STRBUF_INIT;
	struct strbuf actual = STRBUF_INIT;

	cl_assert(strbuf_read_file(&expected, expected_path, 0) >= 0);
	cl_assert(strbuf_read_file(&actual, actual_path, 0) >= 0);
	cl_assert_equal_i(actual.len, expected.len);
	cl_assert(!memcmp(actual.buf, expected.buf, expected.len));
	strbuf_release(&actual);
	strbuf_release(&expected);
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

void test_vex_blob_s3__receive_pack_keys_are_fenced_to_one_epoch_and_pair(void)
{
	struct vex_blob_s3 *blob = open_test_blob();
	struct object_id oid;
	struct strbuf staged = STRBUF_INIT;
	struct strbuf final = STRBUF_INIT;
	struct strbuf expected = STRBUF_INIT;
	struct tempfile *pack_file;
	struct tempfile *idx_file;
	unsigned char raw_pack_id[GIT_MAX_RAWSZ];
	unsigned char wrong_pack_id[GIT_MAX_RAWSZ];
	char hex[GIT_MAX_HEXSZ + 1];

	hash_object_file(hash_algo, "vex-receive-pack-key",
			 strlen("vex-receive-pack-key"), OBJ_BLOB, &oid);
	hash_to_hex_algop_r(hex, oid.hash, hash_algo);
	cl_must_pass(vex_blob_s3_receive_pack_staging_key(
		blob, 48, oid.hash, hash_algo->rawsz,
		VEX_BLOB_S3_PACK_COMPONENT_PACK, &staged));
	cl_must_pass(vex_blob_s3_receive_pack_final_key(
		blob, oid.hash, hash_algo->rawsz, VEX_BLOB_S3_PACK_COMPONENT_IDX,
		&final));
	strbuf_addf(&expected,
		"git/v1/00000000000040008000000000000001/staging/48/packs/%s.pack",
		hex);
	cl_assert_equal_s(staged.buf, expected.buf);
	strbuf_reset(&expected);
	strbuf_addf(&expected, "git/v1/00000000000040008000000000000001/packs/%s.idx",
		hex);
	cl_assert_equal_s(final.buf, expected.buf);
	cl_assert_equal_i(vex_blob_s3_receive_pack_staging_key(
		blob, 0, oid.hash, hash_algo->rawsz,
		VEX_BLOB_S3_PACK_COMPONENT_PACK, &staged),
		VEX_BLOB_S3_ERR_INVALID);
	cl_assert_equal_i(vex_blob_s3_receive_pack_final_key(
		blob, oid.hash, hash_algo->rawsz - 1,
		VEX_BLOB_S3_PACK_COMPONENT_PACK, &final),
		VEX_BLOB_S3_ERR_INVALID);
	cl_assert_equal_i(vex_blob_s3_receive_pack_final_key(
		blob, oid.hash, hash_algo->rawsz,
		(enum vex_blob_s3_pack_component)42, &final),
		VEX_BLOB_S3_ERR_INVALID);
	cl_assert_equal_i(vex_blob_s3_stage_receive_pack(
		blob, 48, oid.hash, hash_algo->rawsz, "pack", "pack.idx"),
		VEX_BLOB_S3_ERR_INVALID);
	cl_assert_equal_i(vex_blob_s3_delete_final_batch(
		blob, &oid, VEX_BLOB_S3_DELETE_OBJECTS_MAX + 1),
		VEX_BLOB_S3_ERR_INVALID);
	cl_assert_equal_i(vex_blob_s3_delete_staging_batch(blob, 0, &oid, 1),
		VEX_BLOB_S3_ERR_INVALID);
	write_test_receive_pack_pair(&pack_file, &idx_file, raw_pack_id);
	memcpy(wrong_pack_id, raw_pack_id, hash_algo->rawsz);
	wrong_pack_id[0] ^= 1;
	/* The mismatched WAL footer is rejected before any S3 request is made. */
	cl_assert_equal_i(vex_blob_s3_stage_receive_pack(
		blob, 48, wrong_pack_id, hash_algo->rawsz, pack_file->filename.buf,
		idx_file->filename.buf), VEX_BLOB_S3_ERR_INVALID);
	delete_tempfile(&idx_file);
	delete_tempfile(&pack_file);
	strbuf_release(&expected);
	strbuf_release(&final);
	strbuf_release(&staged);
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

void test_vex_blob_s3__minio_delete_objects_batches_loose_keys(void)
{
	const char *run = getenv("VEX_S3_INTEGRATION");
	struct vex_blob_s3 *blob;
	struct object_id oids[3];
	struct vex_blob_s3_staged_object objects[3];
	struct object_id at_limit[VEX_BLOB_S3_DELETE_OBJECTS_MAX];
	char payloads[3][128];
	char at_limit_payload[128];
	void *read_back;
	unsigned long size;
	size_t i;

	if (!run || strcmp(run, "1")) {
		cl_skip();
		return;
	}
	for (i = 0; i < ARRAY_SIZE(objects); i++) {
		xsnprintf(payloads[i], sizeof(payloads[i]),
			  "Vex S3 DeleteObjects payload=%zu pid=%ld\n", i,
			  (long)getpid());
		hash_object_file(hash_algo, payloads[i], strlen(payloads[i]),
				 OBJ_BLOB, &oids[i]);
		objects[i].oid = &oids[i];
		objects[i].data = payloads[i];
		objects[i].size = strlen(payloads[i]);
	}
	blob = open_test_blob();
	cl_must_pass(vex_blob_s3_delete_final_batch(blob, oids,
		ARRAY_SIZE(oids)));
	cl_must_pass(vex_blob_s3_put_staging_batch(blob, 47005, objects,
		ARRAY_SIZE(objects)));
	cl_must_pass(vex_blob_s3_delete_staging_batch(blob, 47005, oids,
		ARRAY_SIZE(oids)));
	for (i = 0; i < ARRAY_SIZE(oids); i++)
		cl_assert_equal_i(vex_blob_s3_read_staging(blob, 47005, &oids[i],
							    &read_back, &size),
				  VEX_BLOB_S3_ERR_NOT_FOUND);
	cl_must_pass(vex_blob_s3_delete_staging_batch(blob, 47005, oids,
		ARRAY_SIZE(oids)));
	cl_must_pass(vex_blob_s3_put_staging_batch(blob, 47005, objects,
		ARRAY_SIZE(objects)));
	cl_must_pass(vex_blob_s3_promote_batch(blob, 47005, oids,
		ARRAY_SIZE(oids)));
	cl_must_pass(vex_blob_s3_delete_final_batch(blob, oids,
		ARRAY_SIZE(oids)));
	for (i = 0; i < ARRAY_SIZE(oids); i++)
		cl_assert_equal_i(vex_blob_s3_has_final(blob, &oids[i]), 0);
	cl_must_pass(vex_blob_s3_delete_final_batch(blob, oids,
		ARRAY_SIZE(oids)));
	/* The protocol maximum is accepted as one 1,000-key DeleteObjects body. */
	for (i = 0; i < ARRAY_SIZE(at_limit); i++) {
		xsnprintf(at_limit_payload, sizeof(at_limit_payload),
			  "Vex S3 DeleteObjects maximum=%zu pid=%ld\n", i,
			  (long)getpid());
		hash_object_file(hash_algo, at_limit_payload,
				 strlen(at_limit_payload), OBJ_BLOB, &at_limit[i]);
	}
	cl_must_pass(vex_blob_s3_delete_final_batch(blob, at_limit,
		ARRAY_SIZE(at_limit)));
	vex_blob_s3_close(blob);
}

void test_vex_blob_s3__minio_stages_and_promotes_raw_receive_pack_pair(void)
{
	const char *run = getenv("VEX_S3_INTEGRATION");
	struct vex_blob_s3 *blob;
	struct tempfile *pack_file;
	struct tempfile *idx_file;
	struct tempfile *downloaded_pack;
	struct tempfile *downloaded_idx;
	unsigned char pack_id[GIT_MAX_RAWSZ];

	if (!run || strcmp(run, "1")) {
		cl_skip();
		return;
	}
	write_test_receive_pack_pair(&pack_file, &idx_file, pack_id);
	downloaded_pack = mks_tempfile_t("vex-s3-downloaded-pack-XXXXXX");
	downloaded_idx = mks_tempfile_t("vex-s3-downloaded-idx-XXXXXX");
	cl_assert(downloaded_pack != NULL);
	cl_assert(downloaded_idx != NULL);
	cl_must_pass(close_tempfile_gently(downloaded_pack));
	cl_must_pass(close_tempfile_gently(downloaded_idx));

	blob = open_test_blob();
	/* The test's empty pack checksum is deterministic, so clean prior debris. */
	cl_must_pass(vex_blob_s3_delete_receive_pack_final(
		blob, pack_id, hash_algo->rawsz));
	cl_must_pass(vex_blob_s3_stage_receive_pack(
		blob, 47004, pack_id, hash_algo->rawsz, pack_file->filename.buf,
		idx_file->filename.buf));
	cl_must_pass(vex_blob_s3_promote_receive_pack(blob, 47004, pack_id,
		hash_algo->rawsz));
	cl_must_pass(vex_blob_s3_download_derived_pack(
		blob, pack_id, hash_algo->rawsz, 0, downloaded_pack->filename.buf));
	cl_must_pass(vex_blob_s3_download_derived_pack(
		blob, pack_id, hash_algo->rawsz, 1, downloaded_idx->filename.buf));
	assert_same_file(pack_file->filename.buf, downloaded_pack->filename.buf);
	assert_same_file(idx_file->filename.buf, downloaded_idx->filename.buf);
	/* Both cleanup calls are deliberately repeatable after promotion. */
	cl_must_pass(vex_blob_s3_delete_receive_pack_staging(
		blob, 47004, pack_id, hash_algo->rawsz));
	cl_must_pass(vex_blob_s3_delete_receive_pack_final(
		blob, pack_id, hash_algo->rawsz));
	cl_must_pass(vex_blob_s3_delete_receive_pack_final(
		blob, pack_id, hash_algo->rawsz));
	vex_blob_s3_close(blob);
	delete_tempfile(&downloaded_idx);
	delete_tempfile(&downloaded_pack);
	delete_tempfile(&idx_file);
	delete_tempfile(&pack_file);
}
