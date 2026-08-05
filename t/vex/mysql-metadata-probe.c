#include "vex/meta-mysql.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_or(const char *name, const char *fallback)
{
	const char *value = getenv(name);

	return value && *value ? value : fallback;
}

static unsigned int env_port(void)
{
	const char *value = env_or("GIT_VEX_MYSQL_PORT", "3306");
	char *end;
	unsigned long port;

	errno = 0;
	port = strtoul(value, &end, 10);
	if (errno || !*value || *end || !port || port > UINT_MAX) {
		fprintf(stderr, "vex mysql probe: invalid GIT_VEX_MYSQL_PORT\n");
		exit(2);
	}
	return (unsigned int)port;
}

static void fill_oid(unsigned char oid[VEX_META_MYSQL_OID_BYTES],
		     unsigned char seed)
{
	size_t i;

	for (i = 0; i < VEX_META_MYSQL_OID_BYTES; i++)
		oid[i] = (unsigned char)(seed + i);
}

static void mix_generation(unsigned char oid[VEX_META_MYSQL_OID_BYTES],
			   uint64_t generation)
{
	size_t i;

	/* Keep repeated probe runs append-only under the same disposable repo. */
	for (i = 0; i < sizeof(generation); i++)
		oid[VEX_META_MYSQL_OID_BYTES - 1 - i] ^=
			(unsigned char)(generation >> (i * CHAR_BIT));
}

static void oid_to_hex(const unsigned char *oid, char hex[65])
{
	static const char digits[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i < VEX_META_MYSQL_OID_BYTES; i++) {
		hex[i * 2] = digits[oid[i] >> 4];
		hex[i * 2 + 1] = digits[oid[i] & 15];
	}
	hex[64] = '\0';
}

static void require_ok(struct vex_meta_mysql *db, int result,
		       const char *operation)
{
	if (result == VEX_META_MYSQL_OK)
		return;
	fprintf(stderr, "vex mysql probe: %s failed (%d): %s\n", operation,
		result, vex_meta_mysql_error(db));
	exit(1);
}

static void require_result(struct vex_meta_mysql *db, int result, int expected,
			   const char *operation)
{
	if (result == expected)
		return;
	fprintf(stderr, "vex mysql probe: %s returned %d, expected %d: %s\n",
		operation, result, expected, vex_meta_mysql_error(db));
	exit(1);
}

int main(void)
{
	static const char default_repo[] = "00000000-0000-4000-8000-000000000004";
	static const char push_id[] = "00000000-0000-4000-8000-000000000005";
	struct vex_meta_mysql_options options = {
		.host = env_or("GIT_VEX_MYSQL_HOST", "127.0.0.1"),
		.port = 0,
		.user = env_or("GIT_VEX_MYSQL_USER", "vex"),
		.password = env_or("GIT_VEX_MYSQL_PASSWORD", "vex-local-mysql"),
		.database = env_or("GIT_VEX_MYSQL_DATABASE", "vex"),
		.unix_socket = NULL,
	};
	struct vex_meta_mysql *db = NULL;
	const char *repo_id = env_or("GIT_VEX_MYSQL_PROBE_REPO_ID", default_repo);
	unsigned char parent_oid[VEX_META_MYSQL_OID_BYTES];
	unsigned char commit_oid[VEX_META_MYSQL_OID_BYTES];
	unsigned char abort_parent_oid[VEX_META_MYSQL_OID_BYTES];
	unsigned char abort_commit_oid[VEX_META_MYSQL_OID_BYTES];
	char parent_hex[65];
	char commit_hex[65];
	char abort_parent_hex[65];
	char abort_commit_hex[65];
	char parent_key[256];
	char commit_key[256];
	char abort_parent_key[256];
	char abort_commit_key[256];
	char refname[128];
	uint64_t generation;
	uint64_t abort_generation;
	uint64_t version;
	unsigned int table_count;
	enum vex_meta_mysql_epoch_state epoch_state;
	struct vex_meta_mysql_object object = { 0 };
	int result;

	options.port = env_port();
	result = vex_meta_mysql_open(&db, &options);
	if (result != VEX_META_MYSQL_OK) {
		fprintf(stderr, "vex mysql probe: connect failed (%d)\n", result);
		return 1;
	}
	require_ok(db, vex_meta_mysql_ping(db), "ping");
	require_ok(db, vex_meta_mysql_schema_check(db, &table_count), "schema check");
	require_ok(db, vex_meta_mysql_ensure_repository(db, repo_id, "sha256",
						       "vex mysql metadata probe"),
		   "ensure repository");
	require_ok(db, vex_meta_mysql_epoch_begin(db, repo_id, push_id, &generation),
		   "allocate generation");

	fill_oid(parent_oid, 0x10);
	fill_oid(commit_oid, 0x80);
	mix_generation(parent_oid, generation);
	mix_generation(commit_oid, generation);
	oid_to_hex(parent_oid, parent_hex);
	oid_to_hex(commit_oid, commit_hex);
	snprintf(parent_key, sizeof(parent_key), "git/v1/%s/objects/%s", repo_id,
		 parent_hex);
	snprintf(commit_key, sizeof(commit_key), "git/v1/%s/objects/%s", repo_id,
		 commit_hex);
	require_ok(db, vex_meta_mysql_stage_object(db, repo_id, generation,
						 parent_oid, sizeof(parent_oid), 1, 17,
						 parent_key), "stage parent metadata");
	require_ok(db, vex_meta_mysql_stage_object(db, repo_id, generation,
						 commit_oid, sizeof(commit_oid), 1, 23,
						 commit_key), "stage commit metadata");
	require_ok(db, vex_meta_mysql_add_commit_parent(db, repo_id,
							 commit_oid, sizeof(commit_oid), 0,
							 parent_oid, sizeof(parent_oid)),
		   "record commit parent metadata");
	require_ok(db, vex_meta_mysql_epoch_get_state(db, repo_id, generation,
						       &epoch_state),
		   "read open generation state");
	if (epoch_state != VEX_META_MYSQL_EPOCH_OPEN) {
		fprintf(stderr, "vex mysql probe: generation was not open before seal\n");
		vex_meta_mysql_close(db);
		return 1;
	}
	require_result(db, vex_meta_mysql_object_get_visible(db, repo_id,
							     commit_oid, sizeof(commit_oid), 0,
							     &object),
		       VEX_META_MYSQL_ERR_NOT_FOUND,
		       "hide quarantine object from public read");
	require_ok(db, vex_meta_mysql_object_get_visible(db, repo_id, commit_oid,
							      sizeof(commit_oid), generation, &object),
		   "read quarantine object in owner generation");
	if (object.object_type != 1 || object.size != 23 ||
	    object.state != VEX_META_MYSQL_OBJECT_QUARANTINE ||
	    object.generation != generation || !object.blob_key ||
	    strcmp(object.blob_key, commit_key)) {
		fprintf(stderr, "vex mysql probe: quarantine object metadata mismatch\n");
		vex_meta_mysql_object_release(&object);
		vex_meta_mysql_close(db);
		return 1;
	}
	vex_meta_mysql_object_release(&object);
	require_result(db, vex_meta_mysql_object_get_visible(db, repo_id,
							     commit_oid, sizeof(commit_oid), generation + 1,
							     &object),
		       VEX_META_MYSQL_ERR_NOT_FOUND,
		       "hide quarantine object from a different generation");
	require_ok(db, vex_meta_mysql_epoch_seal(db, repo_id, generation),
		   "seal generation");
	require_ok(db, vex_meta_mysql_object_get_visible(db, repo_id, commit_oid,
							      sizeof(commit_oid), 0, &object),
		   "read published object metadata");
	if (object.state != VEX_META_MYSQL_OBJECT_PUBLISHED ||
	    object.generation != generation || !object.blob_key ||
	    strcmp(object.blob_key, commit_key)) {
		fprintf(stderr, "vex mysql probe: published object metadata mismatch\n");
		vex_meta_mysql_object_release(&object);
		vex_meta_mysql_close(db);
		return 1;
	}
	vex_meta_mysql_object_release(&object);

	snprintf(refname, sizeof(refname), "refs/vex-probe/%" PRIu64, generation);
	require_ok(db, vex_meta_mysql_ref_cas_direct(db, repo_id, refname,
						       VEX_META_MYSQL_VERSION_ABSENT,
						       commit_oid, sizeof(commit_oid),
						       "probe", "create direct ref", &version),
		   "create direct ref CAS");
	if (version != 1) {
		fprintf(stderr, "vex mysql probe: first ref version was %" PRIu64 "\n",
			version);
		vex_meta_mysql_close(db);
		return 1;
	}
	require_ok(db, vex_meta_mysql_ref_cas_direct(db, repo_id, refname, version,
						       parent_oid, sizeof(parent_oid),
						       "probe", "update direct ref", &version),
		   "update direct ref CAS");
	if (version != 2) {
		fprintf(stderr, "vex mysql probe: second ref version was %" PRIu64 "\n",
			version);
		vex_meta_mysql_close(db);
		return 1;
	}
	result = vex_meta_mysql_ref_cas_direct(db, repo_id, refname, 1,
						  commit_oid, sizeof(commit_oid), "probe",
						  "stale CAS must fail", NULL);
	if (result != VEX_META_MYSQL_ERR_CONFLICT) {
		fprintf(stderr, "vex mysql probe: stale ref CAS unexpectedly returned %d\n",
			result);
		vex_meta_mysql_close(db);
		return 1;
	}

	require_ok(db, vex_meta_mysql_epoch_begin(db, repo_id, push_id,
						   &abort_generation),
		   "allocate abort generation");
	require_ok(db, vex_meta_mysql_epoch_get_state(db, repo_id, abort_generation,
						       &epoch_state),
		   "read abort generation state");
	if (epoch_state != VEX_META_MYSQL_EPOCH_OPEN) {
		fprintf(stderr, "vex mysql probe: abort generation was not open\n");
		vex_meta_mysql_close(db);
		return 1;
	}
	/* A compensation must not remove a published object claimed by this epoch. */
	require_ok(db, vex_meta_mysql_stage_object(db, repo_id, abort_generation,
							 parent_oid, sizeof(parent_oid), 1, 17,
							 parent_key),
		   "claim published object in abort generation");
	fill_oid(abort_parent_oid, 0x20);
	fill_oid(abort_commit_oid, 0x40);
	mix_generation(abort_parent_oid, abort_generation);
	mix_generation(abort_commit_oid, abort_generation);
	oid_to_hex(abort_parent_oid, abort_parent_hex);
	oid_to_hex(abort_commit_oid, abort_commit_hex);
	snprintf(abort_parent_key, sizeof(abort_parent_key),
		 "git/v1/%s/objects/%s", repo_id, abort_parent_hex);
	snprintf(abort_commit_key, sizeof(abort_commit_key),
		 "git/v1/%s/objects/%s", repo_id, abort_commit_hex);
	require_ok(db, vex_meta_mysql_stage_object(db, repo_id, abort_generation,
							 abort_parent_oid, sizeof(abort_parent_oid), 1, 29,
							 abort_parent_key),
		   "stage abort parent metadata");
	require_ok(db, vex_meta_mysql_stage_object(db, repo_id, abort_generation,
							 abort_commit_oid, sizeof(abort_commit_oid), 1, 31,
							 abort_commit_key),
		   "stage abort commit metadata");
	require_ok(db, vex_meta_mysql_add_commit_parent(db, repo_id,
							 abort_commit_oid, sizeof(abort_commit_oid), 0,
							 abort_parent_oid, sizeof(abort_parent_oid)),
		   "record abort commit parent metadata");
	require_ok(db, vex_meta_mysql_object_get_visible(db, repo_id,
							      abort_commit_oid, sizeof(abort_commit_oid),
							      abort_generation, &object),
		   "read abort quarantine object in owner generation");
	if (object.state != VEX_META_MYSQL_OBJECT_QUARANTINE ||
	    !object.blob_key || strcmp(object.blob_key, abort_commit_key)) {
		fprintf(stderr, "vex mysql probe: abort object metadata mismatch\n");
		vex_meta_mysql_object_release(&object);
		vex_meta_mysql_close(db);
		return 1;
	}
	vex_meta_mysql_object_release(&object);
	snprintf(refname, sizeof(refname), "refs/vex-probe/abort/%" PRIu64,
		 abort_generation);
	require_result(db, vex_meta_mysql_ref_cas_direct(db, repo_id, refname,
							       VEX_META_MYSQL_VERSION_ABSENT,
							       abort_commit_oid, sizeof(abort_commit_oid),
							       "probe", "unpublished target must fail", NULL),
		       VEX_META_MYSQL_ERR_STATE,
		       "reject direct ref target from an open generation");
	require_ok(db, vex_meta_mysql_epoch_abort(db, repo_id, abort_generation),
		   "abort generation and clean metadata");
	require_ok(db, vex_meta_mysql_epoch_get_state(db, repo_id, abort_generation,
						       &epoch_state),
		   "read aborted generation state");
	if (epoch_state != VEX_META_MYSQL_EPOCH_ABORTED) {
		fprintf(stderr, "vex mysql probe: abort generation did not become aborted\n");
		vex_meta_mysql_close(db);
		return 1;
	}
	require_result(db, vex_meta_mysql_object_get_visible(db, repo_id,
							     abort_commit_oid, sizeof(abort_commit_oid),
							     abort_generation, &object),
		       VEX_META_MYSQL_ERR_NOT_FOUND,
		       "hide object from aborted generation");
	require_ok(db, vex_meta_mysql_object_get_visible(db, repo_id, parent_oid,
							      sizeof(parent_oid), 0, &object),
		   "preserve published object after abort");
	if (object.state != VEX_META_MYSQL_OBJECT_PUBLISHED ||
	    !object.blob_key || strcmp(object.blob_key, parent_key)) {
		fprintf(stderr, "vex mysql probe: published object lost during abort\n");
		vex_meta_mysql_object_release(&object);
		vex_meta_mysql_close(db);
		return 1;
	}
	vex_meta_mysql_object_release(&object);
	require_result(db, vex_meta_mysql_epoch_abort(db, repo_id, abort_generation),
		       VEX_META_MYSQL_ERR_STATE,
		       "reject a second abort for the same generation");

	printf("vex mysql probe: PASS tables=%u generation=%" PRIu64
	       " abort_generation=%" PRIu64 " ref_version=%" PRIu64
	       " metadata_only=1\n",
	       table_count, generation, abort_generation, version);
	vex_meta_mysql_close(db);
	return 0;
}
