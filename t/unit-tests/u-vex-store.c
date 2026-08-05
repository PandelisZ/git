#include "unit-test.h"
#include "dir.h"
#include "hex.h"
#include "object-file.h"
#include "strbuf.h"
#include "vex/store.h"

static const struct git_hash_algo *hash_algo = &hash_algos[GIT_HASH_SHA1];

static void make_root(struct strbuf *root)
{
	char path[] = "/tmp/vex-store-XXXXXX";

	cl_assert(mkdtemp(path));
	strbuf_addstr(root, path);
}

static struct vex_store *open_store(const char *root, const char *repo_id)
{
	struct vex_store_options opts = {
		.backend = VEX_STORE_BACKEND_FS,
		.root = root,
		.repo_id = repo_id,
		.hash_algo = hash_algo,
	};
	struct vex_store *store;

	cl_must_pass(vex_store_open(&store, &opts));
	cl_must_pass(vex_repo_ensure(store));
	return store;
}

static void close_root(struct vex_store *store, struct strbuf *root)
{
	vex_store_close(store);
	cl_must_pass(remove_dir_recursively(root, 0));
	strbuf_release(root);
}

static void make_blob_oid(const char *contents, struct object_id *oid)
{
	hash_object_file(hash_algo, contents, strlen(contents), OBJ_BLOB, oid);
}

static struct vex_ref_update direct_update(const char *refname,
					    const struct object_id *old_oid,
					    const struct object_id *new_oid,
					    const char *reason)
{
	struct vex_ref_update update = {
		.refname = refname,
		.old_target = {
			.type = old_oid ? VEX_REF_TARGET_OID : VEX_REF_TARGET_NONE,
		},
		.new_target = {
			.type = VEX_REF_TARGET_OID,
		},
		.is_fast_forward = -1,
		.reason = reason,
	};

	if (old_oid)
		oidcpy(&update.old_target.oid, old_oid);
	oidcpy(&update.new_target.oid, new_oid);
	return update;
}

void test_vex_store__blob_then_metadata_and_repo_isolation(void)
{
	struct strbuf root = STRBUF_INIT;
	struct vex_store *first;
	struct vex_store *second;
	struct vex_epoch *epoch;
	struct vex_ref_transaction *transaction;
	struct vex_ref_update update;
	struct vex_ref ref;
	struct object_id oid;
	struct strbuf key = STRBUF_INIT;
	struct strbuf expected_key = STRBUF_INIT;
	enum object_type type;
	unsigned long size;
	void *contents;
	const char payload[] = "object bytes stay in the blob backend";

	make_root(&root);
	first = open_store(root.buf, "11111111111111111111111111111111");
	second = open_store(root.buf, "22222222222222222222222222222222");
	cl_must_pass(vex_epoch_allocate(first, "push-one", &epoch));
	make_blob_oid(payload, &oid);

	cl_assert_equal_i(vex_object_index_put_quarantine(first, epoch, &oid,
						      OBJ_BLOB, strlen(payload)),
			  VEX_STORE_ERR_NOT_FOUND);
	cl_must_pass(vex_store_stage_object(first, epoch, &oid, OBJ_BLOB, payload,
					  strlen(payload)));
	cl_assert_equal_i(vex_store_has_object(first, &oid), 0);
	cl_assert_equal_i(vex_store_read_object(first, &oid, &type, &size, &contents),
			  VEX_STORE_ERR_NOT_FOUND);
	cl_must_pass(vex_store_read_object_in_epoch(first, epoch, &oid, &type, &size,
						      &contents));
	cl_assert_equal_i(type, OBJ_BLOB);
	cl_assert_equal_u(size, strlen(payload));
	cl_assert_equal_i(memcmp(contents, payload, size), 0);
	free(contents);

	cl_must_pass(vex_blob_key(first, &oid, &key));
	strbuf_addf(&expected_key, "git/v1/%s/objects/%s",
		    vex_store_repo_id(first), oid_to_hex(&oid));
	cl_assert_equal_s(key.buf, expected_key.buf);
	cl_assert_equal_i(vex_store_has_object(second, &oid), 0);

	update = direct_update("refs/heads/main", NULL, &oid, "seed");
	cl_must_pass(vex_ref_transaction_begin(first, epoch, &transaction));
	cl_must_pass(vex_ref_transaction_update(transaction, &update));
	cl_must_pass(vex_ref_transaction_commit(transaction));
	vex_ref_transaction_abort(transaction);
	vex_epoch_free(epoch);

	cl_assert_equal_i(vex_store_has_object(first, &oid), 1);
	cl_must_pass(vex_store_read_object(first, &oid, &type, &size, &contents));
	cl_assert_equal_i(type, OBJ_BLOB);
	cl_assert_equal_u(size, strlen(payload));
	cl_assert_equal_i(memcmp(contents, payload, size), 0);
	free(contents);
	cl_must_pass(vex_ref_read(first, "refs/heads/main", &ref));
	cl_assert_equal_i(ref.target.type, VEX_REF_TARGET_OID);
	cl_assert(oideq(&ref.target.oid, &oid));
	vex_ref_clear(&ref);

	vex_store_close(second);
	close_root(first, &root);
	strbuf_release(&key);
	strbuf_release(&expected_key);
}

struct audit_counter {
	unsigned int count;
	int saw_seed;
};

static int count_audit(const struct vex_ref_update *update,
		       uint64_t id UNUSED, void *cb_data)
{
	struct audit_counter *counter = cb_data;

	counter->count++;
	counter->saw_seed = !strcmp(update->reason, "seed");
	return 0;
}

void test_vex_store__stale_cas_aborts_the_whole_epoch(void)
{
	struct strbuf root = STRBUF_INIT;
	struct vex_store *store;
	struct vex_epoch *first_epoch;
	struct vex_epoch *stale_epoch;
	struct vex_ref_transaction *transaction;
	struct vex_ref_update update;
	struct vex_ref ref;
	struct vex_object_info info;
	struct object_id first_oid;
	struct object_id stale_oid;
	struct audit_counter audits = { 0 };
	const char first_payload[] = "first object";
	const char stale_payload[] = "unpublished stale object";

	make_root(&root);
	store = open_store(root.buf, "33333333333333333333333333333333");
	make_blob_oid(first_payload, &first_oid);
	cl_must_pass(vex_epoch_allocate(store, "push-one", &first_epoch));
	cl_must_pass(vex_store_stage_object(store, first_epoch, &first_oid,
					  OBJ_BLOB, first_payload, strlen(first_payload)));
	update = direct_update("refs/heads/main", NULL, &first_oid, "seed");
	cl_must_pass(vex_ref_transaction_begin(store, first_epoch, &transaction));
	cl_must_pass(vex_ref_transaction_update(transaction, &update));
	cl_must_pass(vex_ref_transaction_commit(transaction));
	vex_ref_transaction_abort(transaction);
	vex_epoch_free(first_epoch);

	make_blob_oid(stale_payload, &stale_oid);
	cl_must_pass(vex_epoch_allocate(store, "push-two", &stale_epoch));
	cl_must_pass(vex_store_stage_object(store, stale_epoch, &stale_oid,
					  OBJ_BLOB, stale_payload, strlen(stale_payload)));
	update = direct_update("refs/heads/main", &stale_oid, &stale_oid, "stale");
	cl_must_pass(vex_ref_transaction_begin(store, stale_epoch, &transaction));
	cl_must_pass(vex_ref_transaction_update(transaction, &update));
	cl_assert_equal_i(vex_ref_transaction_commit(transaction), VEX_STORE_ERR_CONFLICT);
	vex_ref_transaction_abort(transaction);
	vex_epoch_free(stale_epoch);

	cl_assert_equal_i(vex_store_has_object(store, &stale_oid), 0);
	cl_assert_equal_i(vex_object_index_get(store, &stale_oid, &info),
			  VEX_STORE_ERR_NOT_FOUND);
	cl_must_pass(vex_ref_read(store, "refs/heads/main", &ref));
	cl_assert_equal_i(ref.target.type, VEX_REF_TARGET_OID);
	cl_assert(oideq(&ref.target.oid, &first_oid));
	vex_ref_clear(&ref);
	cl_must_pass(vex_ref_updates_iterate(store, count_audit, &audits));
	cl_assert_equal_i(audits.count, 1);
	cl_assert(audits.saw_seed);

	close_root(store, &root);
}

void test_vex_store__commit_parents_are_metadata(void)
{
	struct strbuf root = STRBUF_INIT;
	struct vex_store *store;
	struct vex_epoch *epoch;
	struct object_id parent_oid;
	struct object_id commit_oid;
	struct strbuf commit = STRBUF_INIT;
	struct strbuf state_path = STRBUF_INIT;
	struct strbuf state = STRBUF_INIT;
	struct strbuf expected = STRBUF_INIT;
	const char parent_payload[] = "parent object";

	make_root(&root);
	store = open_store(root.buf, "44444444444444444444444444444444");
	make_blob_oid(parent_payload, &parent_oid);
	strbuf_addf(&commit,
		    "tree %s\nparent %s\nauthor Test <test@example.com> 0 +0000\n"
		    "committer Test <test@example.com> 0 +0000\n\nmessage\n",
		    oid_to_hex(&parent_oid), oid_to_hex(&parent_oid));
	hash_object_file(hash_algo, commit.buf, commit.len, OBJ_COMMIT, &commit_oid);
	cl_must_pass(vex_epoch_allocate(store, "commit-parent", &epoch));
	cl_must_pass(vex_store_stage_object(store, epoch, &commit_oid, OBJ_COMMIT,
					  commit.buf, commit.len));

	strbuf_addf(&state_path, "%s/meta/%s/state", root.buf,
		    vex_store_repo_id(store));
	cl_must_pass(strbuf_read_file(&state, state_path.buf, 0));
	strbuf_addf(&expected, "C\t%s\t0\t%s\n", oid_to_hex(&commit_oid),
		    oid_to_hex(&parent_oid));
	cl_assert(strstr(state.buf, expected.buf));

	cl_must_pass(vex_epoch_compensate(epoch));
	vex_epoch_free(epoch);
	strbuf_release(&commit);
	strbuf_release(&state_path);
	strbuf_release(&state);
	strbuf_release(&expected);
	close_root(store, &root);
}
