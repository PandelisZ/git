#include "git-compat-util.h"
#include "vex/store-internal.h"
#include "hex.h"
#include "oid-array.h"
#include "oidset.h"
#include "object-file.h"
#include "strbuf.h"
#include "trace.h"
#include "wrapper.h"

static int normalize_repo_id(const char *repo_id, struct strbuf *normalized)
{
	size_t i;
	size_t len;
	int hyphenated;

	if (!repo_id)
		return VEX_STORE_ERR_INVALID;
	len = strlen(repo_id);
	hyphenated = len == 36;
	if (len != 32 && !hyphenated)
		return VEX_STORE_ERR_INVALID;
	for (i = 0; i < len; i++) {
		unsigned char c = repo_id[i];

		if (hyphenated && (i == 8 || i == 13 || i == 18 || i == 23)) {
			if (c != '-')
				return VEX_STORE_ERR_INVALID;
			continue;
		}
		if (!isdigit(c) && (c < 'a' || c > 'f'))
			return VEX_STORE_ERR_INVALID;
		strbuf_addch(normalized, c);
	}
	return VEX_STORE_OK;
}

static int valid_object_type(enum object_type type)
{
	return type == OBJ_COMMIT || type == OBJ_TREE || type == OBJ_BLOB ||
	       type == OBJ_TAG;
}

static int check_epoch(struct vex_store *store, const struct vex_epoch *epoch)
{
	if (!store || !epoch || epoch->store != store || !epoch->active)
		return VEX_STORE_ERR_STATE;
	return VEX_STORE_OK;
}

static int check_optional_epoch(struct vex_store *store,
				const struct vex_epoch *epoch)
{
	if (!store)
		return VEX_STORE_ERR_INVALID;
	return epoch ? check_epoch(store, epoch) : VEX_STORE_OK;
}

static struct vex_meta_epoch *find_epoch(struct vex_meta_state *state,
					 uint64_t id)
{
	size_t i;

	for (i = 0; i < state->epochs_nr; i++)
		if (state->epochs[i].id == id)
			return &state->epochs[i];
	return NULL;
}

static struct vex_meta_object *find_object(struct vex_meta_state *state,
					    const struct object_id *oid)
{
	size_t i;

	for (i = 0; i < state->objects_nr; i++)
		if (oideq(&state->objects[i].oid, oid))
			return &state->objects[i];
	return NULL;
}

static struct vex_meta_ref *find_ref(struct vex_meta_state *state,
				      const char *refname)
{
	size_t i;

	for (i = 0; i < state->refs_nr; i++)
		if (!strcmp(state->refs[i].name, refname))
			return &state->refs[i];
	return NULL;
}

static int epoch_has_object(const struct vex_meta_state *state, uint64_t epoch,
			    const struct object_id *oid)
{
	size_t i;

	for (i = 0; i < state->epoch_objects_nr; i++)
		if (state->epoch_objects[i].epoch == epoch &&
		    oideq(&state->epoch_objects[i].oid, oid))
			return 1;
	return 0;
}

static int object_has_epoch_claim(const struct vex_meta_state *state,
				  const struct object_id *oid)
{
	size_t i;

	for (i = 0; i < state->epoch_objects_nr; i++)
		if (oideq(&state->epoch_objects[i].oid, oid))
			return 1;
	return 0;
}

static int object_has_other_epoch_claim(const struct vex_meta_state *state,
					uint64_t epoch,
					const struct object_id *oid)
{
	size_t i;

	for (i = 0; i < state->epoch_objects_nr; i++)
		if (state->epoch_objects[i].epoch != epoch &&
		    oideq(&state->epoch_objects[i].oid, oid))
			return 1;
	return 0;
}

static void remove_epoch_object_at(struct vex_meta_state *state, size_t at)
{
	if (at + 1 < state->epoch_objects_nr)
		memmove(&state->epoch_objects[at], &state->epoch_objects[at + 1],
			(sizeof(*state->epoch_objects) *
			 (state->epoch_objects_nr - at - 1)));
	state->epoch_objects_nr--;
}

static void remove_commit_parents_for(struct vex_meta_state *state,
				      const struct object_id *oid)
{
	size_t i;

	for (i = 0; i < state->commit_parents_nr;) {
		if (oideq(&state->commit_parents[i].commit_oid, oid)) {
			if (i + 1 < state->commit_parents_nr)
				memmove(&state->commit_parents[i],
					&state->commit_parents[i + 1],
					(sizeof(*state->commit_parents) *
					 (state->commit_parents_nr - i - 1)));
			state->commit_parents_nr--;
		} else {
			i++;
		}
	}
}

static int add_commit_parent(struct vex_meta_state *state,
			     const struct object_id *commit_oid,
			     unsigned int position,
			     const struct object_id *parent_oid)
{
	struct vex_meta_commit_parent *entry;

	ALLOC_GROW(state->commit_parents, state->commit_parents_nr + 1,
		   state->commit_parents_alloc);
	entry = &state->commit_parents[state->commit_parents_nr++];
	oidcpy(&entry->commit_oid, commit_oid);
	entry->position = position;
	oidcpy(&entry->parent_oid, parent_oid);
	return VEX_STORE_OK;
}

static void remove_object_at(struct vex_meta_state *state, size_t at)
{
	remove_commit_parents_for(state, &state->objects[at].oid);
	if (at + 1 < state->objects_nr)
		memmove(&state->objects[at], &state->objects[at + 1],
			(sizeof(*state->objects) * (state->objects_nr - at - 1)));
	state->objects_nr--;
}

static void remove_ref_at(struct vex_meta_state *state, size_t at)
{
	free(state->refs[at].name);
	vex_ref_target_release(&state->refs[at].target);
	if (at + 1 < state->refs_nr)
		memmove(&state->refs[at], &state->refs[at + 1],
			(sizeof(*state->refs) * (state->refs_nr - at - 1)));
	state->refs_nr--;
}

static int add_epoch_object(struct vex_meta_state *state, uint64_t epoch,
			    const struct object_id *oid)
{
	struct vex_meta_epoch_object *entry;

	if (epoch_has_object(state, epoch, oid))
		return VEX_STORE_OK;
	ALLOC_GROW(state->epoch_objects, state->epoch_objects_nr + 1,
		   state->epoch_objects_alloc);
	entry = &state->epoch_objects[state->epoch_objects_nr++];
	entry->epoch = epoch;
	oidcpy(&entry->oid, oid);
	return VEX_STORE_OK;
}

static int add_object(struct vex_meta_state *state, const struct object_id *oid,
		      enum object_type type, unsigned long size, uint64_t epoch)
{
	struct vex_meta_object *object;

	ALLOC_GROW(state->objects, state->objects_nr + 1, state->objects_alloc);
	object = &state->objects[state->objects_nr++];
	oidcpy(&object->oid, oid);
	object->type = type;
	object->size = size;
	object->epoch = epoch;
	object->state = VEX_OBJECT_QUARANTINE;
	return VEX_STORE_OK;
}

static int add_ref(struct vex_meta_state *state, const char *refname,
		   const struct vex_ref_target *target)
{
	struct vex_meta_ref *ref;

	ALLOC_GROW(state->refs, state->refs_nr + 1, state->refs_alloc);
	ref = &state->refs[state->refs_nr++];
	memset(ref, 0, sizeof(*ref));
	ref->name = xstrdup(refname);
	if (vex_ref_target_dup(&ref->target, target)) {
		free(ref->name);
		ref->name = NULL;
		return VEX_STORE_ERR_INVALID;
	}
	ref->version = 1;
	return VEX_STORE_OK;
}

static int append_ref_update(struct vex_meta_state *state,
			     const struct vex_ref_update *update,
			     const char *fallback_push_id, uint64_t *id)
{
	struct vex_meta_ref_update *entry;

	ALLOC_GROW(state->ref_updates, state->ref_updates_nr + 1,
		   state->ref_updates_alloc);
	entry = &state->ref_updates[state->ref_updates_nr++];
	memset(entry, 0, sizeof(*entry));
	entry->id = state->next_update++;
	if (vex_ref_update_dup(&entry->update, update))
		return VEX_STORE_ERR_INVALID;
	if (!entry->update.reason)
		entry->update.reason = xstrdup("update");
	if (!entry->update.push_id && fallback_push_id)
		entry->update.push_id = xstrdup(fallback_push_id);
	if (id)
		*id = entry->id;
	return VEX_STORE_OK;
}

static int verify_object(struct vex_store *store, const struct object_id *oid,
			 enum object_type type, const void *data, unsigned long size)
{
	struct object_id computed;

	if (!valid_object_type(type) || (!data && size))
		return VEX_STORE_ERR_INVALID;
	hash_object_file(store->hash_algo, data, size, type, &computed);
	if (!hasheq(oid->hash, computed.hash, store->hash_algo))
		return VEX_STORE_ERR_CORRUPT;
	return VEX_STORE_OK;
}

static int meta_mysql_result(int ret)
{
	switch (ret) {
	case VEX_META_MYSQL_OK:
		return VEX_STORE_OK;
	case VEX_META_MYSQL_ERR_NOT_FOUND:
		return VEX_STORE_ERR_NOT_FOUND;
	case VEX_META_MYSQL_ERR_CONFLICT:
		return VEX_STORE_ERR_CONFLICT;
	case VEX_META_MYSQL_ERR_INVALID:
		return VEX_STORE_ERR_INVALID;
	case VEX_META_MYSQL_ERR_STATE:
		return VEX_STORE_ERR_STATE;
	default:
		return VEX_STORE_ERR;
	}
}

static int meta_lock(struct vex_store *store, struct vex_meta_lock *lock)
{
	int ret;

	memset(lock, 0, sizeof(*lock));
	if (store->backend != VEX_STORE_BACKEND_MYSQL_S3) {
		ret = vex_meta_fs_lock(store->meta, &lock->fs);
		lock->held = lock->fs.held;
		return ret;
	}
	ret = meta_mysql_result(vex_meta_mysql_state_lock(store->meta_mysql,
							     store->repo_id));
	if (!ret)
		lock->held = 1;
	return ret;
}

static void meta_unlock(struct vex_store *store, struct vex_meta_lock *lock)
{
	if (!lock->held)
		return;
	if (store->backend == VEX_STORE_BACKEND_MYSQL_S3)
		vex_meta_mysql_state_unlock(store->meta_mysql);
	else
		vex_meta_fs_unlock(&lock->fs);
	lock->held = 0;
}

static int meta_load(struct vex_store *store, struct vex_meta_state *state)
{
	if (store->backend == VEX_STORE_BACKEND_MYSQL_S3)
		return meta_mysql_result(vex_meta_mysql_state_load(store->meta_mysql,
								     store->repo_id,
								     store->hash_algo, state));
	return vex_meta_fs_load(store->meta, state);
}

static int meta_save_locked(struct vex_store *store,
				    struct vex_meta_lock *lock,
				    const struct vex_meta_state *state)
{
	int ret;

	if (!lock->held)
		return VEX_STORE_ERR_STATE;
	if (store->backend == VEX_STORE_BACKEND_MYSQL_S3) {
		ret = meta_mysql_result(vex_meta_mysql_state_save_locked(
				store->meta_mysql, store->repo_id, store->hash_algo, state));
		if (!ret)
			lock->held = 0;
		else
			meta_unlock(store, lock);
		return ret;
	}
	ret = vex_meta_fs_save_locked(store->meta, &lock->fs, state);
	lock->held = lock->fs.held;
	if (ret)
		meta_unlock(store, lock);
	return ret;
}

/* Commit a metadata-only MySQL mutation without rewriting the state snapshot. */
static int meta_commit_locked(struct vex_store *store, struct vex_meta_lock *lock)
{
	int ret;

	if (!lock->held || store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_STATE;
	ret = meta_mysql_result(
		vex_meta_mysql_state_commit_locked(store->meta_mysql));
	if (!ret)
		lock->held = 0;
	else
		meta_unlock(store, lock);
	return ret;
}

static int meta_ensure(struct vex_store *store)
{
	int ret;

	if (store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return vex_meta_fs_ensure(store->meta);
	ret = meta_mysql_result(vex_meta_mysql_schema_check(store->meta_mysql,
								     NULL));
	if (ret)
		return ret;
	return meta_mysql_result(vex_meta_mysql_ensure_repository(
		store->meta_mysql, store->repo_id, store->hash_algo->name, NULL));
}

static int load_locked(struct vex_store *store, struct vex_meta_lock *lock,
		       struct vex_meta_state *state)
{
	int ret;

	if ((ret = meta_lock(store, lock)))
		return ret;
	if ((ret = meta_load(store, state)))
		meta_unlock(store, lock);
	return ret;
}

static int epoch_is_open(const struct vex_meta_state *state, uint64_t id)
{
	const struct vex_meta_epoch *epoch =
		find_epoch((struct vex_meta_state *)state, id);

	return epoch && epoch->state == VEX_EPOCH_OPEN;
}

static int object_is_visible_to_epoch(const struct vex_meta_state *state,
				      const struct vex_meta_object *object,
				      uint64_t epoch)
{
	return object && (object->state == VEX_OBJECT_PUBLISHED ||
			  epoch_has_object(state, epoch, &object->oid));
}

static int object_is_visible(const struct vex_meta_state *state,
			     const struct vex_meta_object *object,
			     const struct vex_epoch *epoch)
{
	return object && (object->state == VEX_OBJECT_PUBLISHED ||
			  (epoch && epoch_has_object(state, epoch->id, &object->oid)));
}

struct vex_epoch_blob {
	struct object_id oid;
	/* Delete a final key only when this failed epoch introduced it. */
	unsigned int remove_final:1;
};

struct vex_epoch_blob_list {
	struct vex_epoch_blob *entries;
	size_t nr;
};

static int blob_s3_result(int ret)
{
	switch (ret) {
	case VEX_BLOB_S3_OK:
		return VEX_STORE_OK;
	case VEX_BLOB_S3_ERR_NOT_FOUND:
		return VEX_STORE_ERR_NOT_FOUND;
	case VEX_BLOB_S3_ERR_INVALID:
		return VEX_STORE_ERR_INVALID;
	default:
		return VEX_STORE_ERR;
	}
}

static int blob_backend_put_staging(struct vex_store *store, uint64_t epoch,
				    const struct object_id *oid,
				    const void *data, unsigned long size)
{
	switch (store->backend) {
	case VEX_STORE_BACKEND_FS:
		return vex_blob_fs_put_staging(store->blob_fs, epoch, oid, data,
					       size);
	case VEX_STORE_BACKEND_FS_S3:
	case VEX_STORE_BACKEND_MYSQL_S3:
		return blob_s3_result(vex_blob_s3_put_staging(store->blob_s3, epoch,
							 oid, data, size));
	default:
		return VEX_STORE_ERR_INVALID;
	}
}

static int blob_backend_has_final(struct vex_store *store,
				  const struct object_id *oid)
{
	int ret;

	switch (store->backend) {
	case VEX_STORE_BACKEND_FS:
		return vex_blob_fs_has_final(store->blob_fs, oid);
	case VEX_STORE_BACKEND_FS_S3:
	case VEX_STORE_BACKEND_MYSQL_S3:
		ret = vex_blob_s3_has_final(store->blob_s3, oid);
		return ret < 0 ? blob_s3_result(ret) : ret;
	default:
		return VEX_STORE_ERR_INVALID;
	}
}

static int blob_backend_read_final(struct vex_store *store,
				   const struct object_id *oid,
				   void **data, unsigned long *size)
{
	switch (store->backend) {
	case VEX_STORE_BACKEND_FS:
		return vex_blob_fs_read_final(store->blob_fs, oid, data, size);
	case VEX_STORE_BACKEND_FS_S3:
	case VEX_STORE_BACKEND_MYSQL_S3:
		return blob_s3_result(vex_blob_s3_read_final(store->blob_s3, oid,
							      data, size));
	default:
		return VEX_STORE_ERR_INVALID;
	}
}

static int blob_backend_read_staging(struct vex_store *store, uint64_t epoch,
				     const struct object_id *oid,
				     void **data, unsigned long *size)
{
	switch (store->backend) {
	case VEX_STORE_BACKEND_FS:
		return vex_blob_fs_read_staging(store->blob_fs, epoch, oid, data,
						size);
	case VEX_STORE_BACKEND_FS_S3:
	case VEX_STORE_BACKEND_MYSQL_S3:
		return blob_s3_result(vex_blob_s3_read_staging(store->blob_s3, epoch,
							       oid, data, size));
	default:
		return VEX_STORE_ERR_INVALID;
	}
}

static int blob_backend_promote_epoch(struct vex_store *store,
				      const struct vex_meta_state *state,
				      uint64_t epoch)
{
	size_t i;

	if (store->backend == VEX_STORE_BACKEND_FS)
		return vex_blob_fs_promote_epoch(store->blob_fs, state, epoch);
	if (store->backend != VEX_STORE_BACKEND_FS_S3 &&
	    store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_INVALID;
	for (i = 0; i < state->epoch_objects_nr; i++) {
		const struct vex_meta_epoch_object *entry = &state->epoch_objects[i];
		int ret;

		if (entry->epoch != epoch)
			continue;
		ret = blob_s3_result(vex_blob_s3_promote_object(store->blob_s3,
								epoch, &entry->oid));
		if (ret)
			return ret;
	}
	return VEX_STORE_OK;
}

static void snapshot_epoch_objects(const struct vex_meta_state *state,
				   uint64_t epoch,
				   struct vex_epoch_blob_list *objects)
{
	size_t i;
	size_t at = 0;

	for (i = 0; i < state->epoch_objects_nr; i++)
		if (state->epoch_objects[i].epoch == epoch)
			objects->nr++;
	if (!objects->nr)
		return;
	CALLOC_ARRAY(objects->entries, objects->nr);
	for (i = 0; i < state->epoch_objects_nr; i++) {
		const struct vex_meta_object *object;
		struct vex_epoch_blob *blob;

		if (state->epoch_objects[i].epoch != epoch)
			continue;
		object = find_object((struct vex_meta_state *)state,
				     &state->epoch_objects[i].oid);
		blob = &objects->entries[at++];
		oidcpy(&blob->oid, &state->epoch_objects[i].oid);
		/*
		 * A promoted S3 key can outlive a SIGKILL between the copy and
		 * metadata commit.  Never remove a canonical key that was already
		 * published or is still claimed by another open epoch.
		 */
		blob->remove_final = object &&
			object->state == VEX_OBJECT_QUARANTINE &&
			!object_has_other_epoch_claim(state, epoch, &blob->oid);
	}
}

static void release_epoch_objects(struct vex_epoch_blob_list *objects)
{
	free(objects->entries);
	objects->entries = NULL;
	objects->nr = 0;
}

static int blob_backend_compensate_epoch(
		struct vex_store *store, uint64_t epoch,
		const struct vex_epoch_blob_list *objects)
{
	size_t i;
	int ret = VEX_STORE_OK;

	if (store->backend == VEX_STORE_BACKEND_FS)
		return vex_blob_fs_compensate_epoch(store->blob_fs, epoch);
	if (store->backend != VEX_STORE_BACKEND_FS_S3 &&
	    store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_INVALID;
	for (i = 0; i < objects->nr; i++) {
		int cleanup = blob_s3_result(vex_blob_s3_delete_staging(
				store->blob_s3, epoch, &objects->entries[i].oid));

		if (cleanup == VEX_STORE_ERR_NOT_FOUND)
			cleanup = VEX_STORE_OK;
		if (!ret && cleanup)
			ret = cleanup;
		if (!objects->entries[i].remove_final)
			continue;
		cleanup = blob_s3_result(vex_blob_s3_delete_final(
				store->blob_s3, &objects->entries[i].oid));
		if (cleanup == VEX_STORE_ERR_NOT_FOUND)
			cleanup = VEX_STORE_OK;
		if (!ret && cleanup)
			ret = cleanup;
	}
	return ret;
}

static int ref_expected_matches(const struct vex_meta_ref *ref,
				const struct vex_ref_update *update)
{
	struct vex_ref_target current = {
		.type = VEX_REF_TARGET_NONE,
	};

	if (update->expected_version &&
	    (!ref || ref->version != update->expected_version))
		return 0;
	if (ref)
		current = ref->target;
	switch (update->old_target.type) {
	case VEX_REF_TARGET_ANY:
		return 1;
	case VEX_REF_TARGET_NONE:
		return !ref;
	case VEX_REF_TARGET_OID:
	case VEX_REF_TARGET_SYMREF:
		return ref && vex_ref_target_equal(&current, &update->old_target);
	}
	return 0;
}

static int ref_update_valid(const struct vex_ref_update *update)
{
	if (!update || !update->refname || !*update->refname ||
	    strlen(update->refname) > 1024 ||
	    update->old_target.type > VEX_REF_TARGET_SYMREF ||
	    update->new_target.type == VEX_REF_TARGET_ANY ||
	    update->new_target.type > VEX_REF_TARGET_SYMREF ||
	    (update->old_target.type == VEX_REF_TARGET_SYMREF &&
	     !update->old_target.symref) ||
	    (update->new_target.type == VEX_REF_TARGET_SYMREF &&
	     !update->new_target.symref))
		return VEX_STORE_ERR_INVALID;
	return VEX_STORE_OK;
}

static int apply_ref_update(struct vex_meta_state *state,
			    const struct vex_ref_update *update)
{
	struct vex_meta_ref *ref = find_ref(state, update->refname);
	size_t i;

	if (update->new_target.type == VEX_REF_TARGET_NONE) {
		if (!ref)
			return VEX_STORE_OK;
		for (i = 0; i < state->refs_nr; i++)
			if (&state->refs[i] == ref) {
				remove_ref_at(state, i);
				return VEX_STORE_OK;
			}
		BUG("ref disappeared while applying Vex transaction");
	}
	if (!ref)
		return add_ref(state, update->refname, &update->new_target);
	vex_ref_target_release(&ref->target);
	if (vex_ref_target_dup(&ref->target, &update->new_target))
		return VEX_STORE_ERR_INVALID;
	ref->version++;
	return VEX_STORE_OK;
}

static int verify_ref_target_object(struct vex_store *store,
				    const struct vex_meta_state *state,
				    uint64_t epoch,
				    const struct vex_ref_update *update)
{
	struct vex_meta_object *object;
	int exists;

	if (update->new_target.type != VEX_REF_TARGET_OID)
		return VEX_STORE_OK;
	object = find_object((struct vex_meta_state *)state,
			     &update->new_target.oid);
	if (!object_is_visible_to_epoch(state, object, epoch))
		return VEX_STORE_ERR_NOT_FOUND;
	if (object->state == VEX_OBJECT_PUBLISHED) {
		exists = blob_backend_has_final(store, &object->oid);
		if (exists < 0)
			return exists;
		if (!exists)
			return VEX_STORE_ERR_CORRUPT;
	}
	return VEX_STORE_OK;
}

static int publish_epoch(struct vex_store *store, struct vex_meta_state *state,
			 uint64_t epoch)
{
	size_t i;

	for (i = 0; i < state->epoch_objects_nr; i++) {
		struct vex_meta_epoch_object *entry = &state->epoch_objects[i];
		struct vex_meta_object *object;
		int exists;

		if (entry->epoch != epoch)
			continue;
		object = find_object(state, &entry->oid);
		if (!object)
			return VEX_STORE_ERR_CORRUPT;
		exists = blob_backend_has_final(store, &entry->oid);
		if (exists < 0)
			return exists;
		if (!exists)
			return VEX_STORE_ERR_NOT_FOUND;
		object->state = VEX_OBJECT_PUBLISHED;
		object->epoch = epoch;
	}
	return VEX_STORE_OK;
}

static void abort_epoch_state(struct vex_meta_state *state, uint64_t epoch_id)
{
	struct vex_meta_epoch *epoch = find_epoch(state, epoch_id);
	size_t i;

	for (i = 0; i < state->epoch_objects_nr;) {
		if (state->epoch_objects[i].epoch == epoch_id)
			remove_epoch_object_at(state, i);
		else
			i++;
	}
	for (i = 0; i < state->objects_nr;) {
		struct vex_meta_object *object = &state->objects[i];

		if (object->state == VEX_OBJECT_QUARANTINE &&
		    !object_has_epoch_claim(state, &object->oid))
			remove_object_at(state, i);
		else
			i++;
	}
	if (epoch)
		epoch->state = VEX_EPOCH_ABORTED;
}

struct vex_expired_epoch {
	uint64_t id;
	struct vex_epoch_blob_list blobs;
};

struct vex_expired_epoch_list {
	struct vex_expired_epoch *items;
	size_t nr;
	size_t alloc;
};

static int collect_expired_epoch(uint64_t id, void *cb_data)
{
	struct vex_expired_epoch_list *epochs = cb_data;
	struct vex_expired_epoch *epoch;

	ALLOC_GROW(epochs->items, epochs->nr + 1, epochs->alloc);
	epoch = &epochs->items[epochs->nr++];
	memset(epoch, 0, sizeof(*epoch));
	epoch->id = id;
	return VEX_META_MYSQL_OK;
}

static void release_expired_epochs(struct vex_expired_epoch_list *epochs)
{
	size_t i;

	for (i = 0; i < epochs->nr; i++)
		release_epoch_objects(&epochs->items[i].blobs);
	free(epochs->items);
	epochs->items = NULL;
	epochs->nr = epochs->alloc = 0;
}

int vex_store_recover_expired_epochs(struct vex_store *store,
				      unsigned int min_age_seconds,
				      unsigned int *recovered)
{
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	struct vex_expired_epoch_list epochs = { 0 };
	size_t i;
	int ret;
	int cleanup_ret = VEX_STORE_OK;

	if (!store || store->backend != VEX_STORE_BACKEND_MYSQL_S3 ||
	    min_age_seconds > 86400)
		return VEX_STORE_ERR_INVALID;
	if (recovered)
		*recovered = 0;
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	ret = meta_mysql_result(vex_meta_mysql_state_for_each_expired_open_locked(
		store->meta_mysql, store->repo_id, min_age_seconds,
		collect_expired_epoch, &epochs));
	if (ret)
		goto out;
	for (i = 0; i < epochs.nr; i++) {
		struct vex_meta_epoch *epoch = find_epoch(&state, epochs.items[i].id);

		if (!epoch || epoch->state != VEX_EPOCH_OPEN) {
			ret = VEX_STORE_ERR_STATE;
			goto out;
		}
		snapshot_epoch_objects(&state, epoch->id, &epochs.items[i].blobs);
		abort_epoch_state(&state, epoch->id);
	}
	if (epochs.nr) {
		ret = meta_save_locked(store, &lock, &state);
		if (ret)
			goto out;
		if (recovered)
			*recovered = epochs.nr;
		trace_printf("vex: recovered %zu stale receive epoch(s) repo=%s age=%us",
			     epochs.nr, store->repo_id, min_age_seconds);
		for (i = 0; i < epochs.nr; i++) {
			int one = blob_backend_compensate_epoch(store, epochs.items[i].id,
								&epochs.items[i].blobs);

			if (!cleanup_ret && one)
				cleanup_ret = one;
		}
	} else {
		meta_unlock(store, &lock);
	}
	ret = cleanup_ret;
out:
	if (lock.held)
		meta_unlock(store, &lock);
	vex_meta_state_release(&state);
	release_expired_epochs(&epochs);
	return ret;
}

#define VEX_EPOCH_RECOVERY_SECONDS 900

static int parse_epoch_recovery_seconds(const char *value,
					unsigned int *seconds, int allow_zero)
{
	char *end;
	unsigned long parsed;

	if (!value || !*value)
		return -1;
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno || *end || parsed > 86400 ||
	    (!allow_zero && !parsed))
		return -1;
	*seconds = parsed;
	return 0;
}

static unsigned int vex_epoch_recovery_seconds(void)
{
	const char *test_value = getenv("GIT_VEX_TEST_EPOCH_RECOVERY_SECONDS");
	const char *value = getenv("GIT_VEX_EPOCH_RECOVERY_SECONDS");
	unsigned int seconds;

	if (test_value && *test_value &&
	    !parse_epoch_recovery_seconds(test_value, &seconds, 1))
		return seconds;
	if (value && *value && !parse_epoch_recovery_seconds(value, &seconds, 0))
		return seconds;
	return VEX_EPOCH_RECOVERY_SECONDS;
}

/*
 * A live receiver updates MySQL at most this often.  Keep the refresh well
 * inside the configured recovery grace, while bounding metadata traffic on a
 * large pack import which stages hundreds of thousands of objects.
 */
static unsigned int vex_epoch_heartbeat_interval_seconds(void)
{
	unsigned int interval = vex_epoch_recovery_seconds() / 4;

	if (!interval)
		return 1;
	return interval > 30 ? 30 : interval;
}

static int vex_epoch_heartbeat_now(struct vex_epoch *epoch, int force)
{
	struct vex_store *store;
	struct vex_meta_lock lock;
	time_t now;
	unsigned int interval;
	int ret;

	if (!epoch || !epoch->active || !epoch->store)
		return VEX_STORE_ERR_STATE;
	store = epoch->store;
	if (store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_OK;
	now = time(NULL);
	interval = vex_epoch_heartbeat_interval_seconds();
	if (!force && epoch->last_heartbeat && now != (time_t)-1 &&
	    now >= epoch->last_heartbeat &&
	    (uintmax_t)(now - epoch->last_heartbeat) < interval)
		return VEX_STORE_OK;
	if ((ret = meta_lock(store, &lock)))
		return ret;
	ret = meta_mysql_result(
		vex_meta_mysql_state_refresh_receive_heartbeat_locked(
			store->meta_mysql, store->repo_id, epoch->id));
	if (!ret)
		ret = meta_commit_locked(store, &lock);
	if (lock.held)
		meta_unlock(store, &lock);
	if (!ret && now != (time_t)-1)
		epoch->last_heartbeat = now;
	return ret;
}

int vex_epoch_heartbeat(struct vex_epoch *epoch)
{
	return vex_epoch_heartbeat_now(epoch, 0);
}

static void vex_test_kill_at(const char *point)
{
	const char *value = getenv("GIT_VEX_TEST_KILL_AT");

	if (!value || strcmp(value, point))
		return;
	trace_printf("vex: test fault killing receive worker at %s", point);
	raise(SIGKILL);
}

static int blob_available_for_epoch(struct vex_store *store, uint64_t epoch,
				    const struct object_id *oid)
{
	void *data;
	unsigned long size;
	int ret;

	ret = blob_backend_has_final(store, oid);
	if (ret < 0)
		return ret;
	if (ret)
		return VEX_STORE_OK;
	ret = blob_backend_read_staging(store, epoch, oid, &data, &size);
	free(data);
	return ret;
}

static int parse_commit_parents(struct vex_store *store, const void *data,
				const unsigned long size,
				struct object_id **parents, size_t *parents_nr)
{
	const char *cursor;
	const char *end;
	size_t parents_alloc = 0;

	*parents = NULL;
	*parents_nr = 0;
	if (!size)
		return VEX_STORE_OK;
	if (!data)
		return VEX_STORE_ERR_INVALID;
	cursor = data;
	end = cursor + size;
	while (cursor < end) {
		const char *line = cursor;
		const char *newline = memchr(line, '\n', end - line);
		size_t line_len = newline ? (size_t)(newline - line) :
			(size_t)(end - line);
		struct object_id parent;

		cursor = newline ? newline + 1 : end;
		if (!line_len)
			break;
		if (line_len != 7 + store->hash_algo->hexsz ||
		    memcmp(line, "parent ", 7))
			continue;
		if (get_oid_hex_algop(line + 7, &parent, store->hash_algo))
			continue;
		ALLOC_GROW(*parents, *parents_nr + 1, parents_alloc);
		oidcpy(&(*parents)[(*parents_nr)++], &parent);
	}
	return VEX_STORE_OK;
}

static int index_commit_parents(struct vex_store *store, struct vex_epoch *epoch,
				const struct object_id *commit_oid,
				const void *data, unsigned long size)
{
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	struct object_id *parents;
	size_t parents_nr;
	size_t i;
	int ret;

	if ((ret = vex_epoch_heartbeat(epoch)))
		return ret;
	if ((ret = parse_commit_parents(store, data, size, &parents, &parents_nr)))
		return ret;
	if (!parents_nr) {
		free(parents);
		return VEX_STORE_OK;
	}
	if ((ret = load_locked(store, &lock, &state)))
		goto out;
	if (!epoch_is_open(&state, epoch->id) ||
	    !epoch_has_object(&state, epoch->id, commit_oid)) {
		ret = VEX_STORE_ERR_STATE;
		goto out;
	}
	remove_commit_parents_for(&state, commit_oid);
	for (i = 0; i < parents_nr; i++)
		add_commit_parent(&state, commit_oid, i, &parents[i]);
	ret = meta_save_locked(store, &lock, &state);
out:
	if (lock.held)
		meta_unlock(store, &lock);
	vex_meta_state_release(&state);
	free(parents);
	return ret;
}

int vex_store_open(struct vex_store **store_out,
		   const struct vex_store_options *opts)
{
	struct vex_store *store;
	struct strbuf repo_id = STRBUF_INIT;
	int ret;

	if (!store_out || !opts ||
	    (opts->backend != VEX_STORE_BACKEND_FS &&
	     opts->backend != VEX_STORE_BACKEND_FS_S3 &&
	     opts->backend != VEX_STORE_BACKEND_MYSQL_S3) ||
	    (opts->backend != VEX_STORE_BACKEND_MYSQL_S3 &&
	     (!opts->root || !*opts->root)) || !opts->hash_algo ||
	    !opts->hash_algo->rawsz || !opts->hash_algo->hexsz) {
		ret = VEX_STORE_ERR_INVALID;
		goto done;
	}
	if ((ret = normalize_repo_id(opts->repo_id, &repo_id)))
		goto done;
	CALLOC_ARRAY(store, 1);
	store->backend = opts->backend;
	store->root = xstrdup(opts->root ? opts->root : "");
	store->repo_id = strbuf_detach(&repo_id, NULL);
	store->hash_algo = opts->hash_algo;
	if (store->backend == VEX_STORE_BACKEND_MYSQL_S3) {
		if (!opts->meta_mysql) {
			ret = VEX_STORE_ERR_INVALID;
			vex_store_close(store);
			goto done;
		}
		ret = meta_mysql_result(vex_meta_mysql_open(&store->meta_mysql,
								    opts->meta_mysql));
	} else {
		ret = vex_meta_fs_open(&store->meta, store->root, store->repo_id,
					       store->hash_algo);
	}
	if (ret) {
		trace_printf("vex: store open backend=%d repo=%s failed (%d)%s%s",
			     store->backend, store->repo_id, ret,
			     store->meta_mysql ? ": " : "",
			     store->meta_mysql ? vex_meta_mysql_error(store->meta_mysql) : "");
		vex_store_close(store);
		goto done;
	}
	if (store->backend == VEX_STORE_BACKEND_FS) {
		ret = vex_blob_fs_open(&store->blob_fs, store->root, store->repo_id,
				       store->hash_algo);
	} else {
		struct vex_blob_s3_options blob_s3;

		if (!opts->blob_s3) {
			ret = VEX_STORE_ERR_INVALID;
		} else {
			blob_s3 = *opts->blob_s3;
			blob_s3.repo_id = store->repo_id;
			blob_s3.hash_algo = store->hash_algo;
			ret = blob_s3_result(vex_blob_s3_open(&store->blob_s3,
							      &blob_s3));
		}
	}
	if (ret) {
		trace_printf("vex: blob backend open backend=%d repo=%s failed (%d)",
			     store->backend, store->repo_id, ret);
		vex_store_close(store);
		goto done;
	}
	*store_out = store;
	ret = VEX_STORE_OK;
done:
	strbuf_release(&repo_id);
	return ret;
}

void vex_store_close(struct vex_store *store)
{
	if (!store)
		return;
	vex_meta_fs_close(store->meta);
	vex_meta_mysql_close(store->meta_mysql);
	vex_blob_fs_close(store->blob_fs);
	vex_blob_s3_close(store->blob_s3);
	free(store->root);
	free(store->repo_id);
	free(store);
}

int vex_repo_ensure(struct vex_store *store)
{
	int ret;

	if (!store)
		return VEX_STORE_ERR_INVALID;
	if ((ret = meta_ensure(store)))
		return ret;
	if (store->backend == VEX_STORE_BACKEND_FS)
		return vex_blob_fs_ensure(store->blob_fs);
	if (store->backend != VEX_STORE_BACKEND_FS_S3 &&
	    store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_INVALID;
	if (!store->blob_s3_checked) {
		ret = blob_s3_result(vex_blob_s3_check_bucket(store->blob_s3));
		if (ret)
			return ret;
		store->blob_s3_checked = 1;
	}
	if (store->backend == VEX_STORE_BACKEND_MYSQL_S3 &&
	    !store->epoch_recovery_checked) {
		ret = vex_store_recover_expired_epochs(store,
						vex_epoch_recovery_seconds(), NULL);
		if (ret)
			return ret;
		store->epoch_recovery_checked = 1;
	}
	return VEX_STORE_OK;
}

const char *vex_store_repo_id(const struct vex_store *store)
{
	return store ? store->repo_id : NULL;
}

const struct git_hash_algo *vex_store_hash_algo(const struct vex_store *store)
{
	return store ? store->hash_algo : NULL;
}

const char *vex_store_result_string(int result)
{
	switch (result) {
	case VEX_STORE_OK:
		return "success";
	case VEX_STORE_ERR:
		return "backend error";
	case VEX_STORE_ERR_NOT_FOUND:
		return "not found";
	case VEX_STORE_ERR_CONFLICT:
		return "compare-and-swap conflict";
	case VEX_STORE_ERR_INVALID:
		return "invalid request";
	case VEX_STORE_ERR_CORRUPT:
		return "corrupt object or metadata";
	case VEX_STORE_ERR_STATE:
		return "invalid epoch state";
	default:
		return "unknown Vex store error";
	}
}

const char *vex_store_error(const struct vex_store *store)
{
	if (!store)
		return "Vex store is not open";
	if (store->backend == VEX_STORE_BACKEND_MYSQL_S3)
		return vex_meta_mysql_error(store->meta_mysql);
	return "filesystem Vex store operation failed";
}

int vex_blob_key(const struct vex_store *store, const struct object_id *oid,
		   struct strbuf *key)
{
	char hex[GIT_MAX_HEXSZ + 1];

	if (!store || !oid || !key)
		return VEX_STORE_ERR_INVALID;
	hash_to_hex_algop_r(hex, oid->hash, store->hash_algo);
	strbuf_addf(key, "git/v1/%s/objects/%s", store->repo_id, hex);
	return VEX_STORE_OK;
}

int vex_epoch_allocate(struct vex_store *store, const char *push_id,
		       struct vex_epoch **epoch_out)
{
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	struct vex_meta_epoch *meta_epoch;
	struct vex_epoch *epoch;
	int ret;

	if (!store || !epoch_out)
		return VEX_STORE_ERR_INVALID;
	*epoch_out = NULL;
	if ((ret = vex_repo_ensure(store)) || (ret = load_locked(store, &lock, &state)))
		return ret;
	ALLOC_GROW(state.epochs, state.epochs_nr + 1, state.epochs_alloc);
	meta_epoch = &state.epochs[state.epochs_nr++];
	meta_epoch->id = state.next_epoch++;
	meta_epoch->state = VEX_EPOCH_OPEN;
	meta_epoch->push_id = push_id ? xstrdup(push_id) : NULL;
	meta_epoch->created_at = NULL;
	meta_epoch->receive_heartbeat_at = NULL;
	meta_epoch->sealed_at = NULL;
	ret = meta_save_locked(store, &lock, &state);
	if (ret) {
		vex_meta_state_release(&state);
		return ret;
	}
	CALLOC_ARRAY(epoch, 1);
	epoch->store = store;
	epoch->id = meta_epoch->id;
	epoch->active = 1;
	epoch->owner = 1;
	ret = vex_epoch_heartbeat_now(epoch, 1);
	if (ret) {
		/* The unleased open epoch is safe only if recovery can reclaim it. */
		vex_epoch_free(epoch);
		vex_meta_state_release(&state);
		return ret;
	}
	*epoch_out = epoch;
	vex_meta_state_release(&state);
	return VEX_STORE_OK;
}

uint64_t vex_epoch_id(const struct vex_epoch *epoch)
{
	return epoch ? epoch->id : 0;
}

int vex_epoch_attach(struct vex_store *store, uint64_t epoch_id,
		     struct vex_epoch **epoch_out)
{
	struct vex_meta_state state;
	struct vex_meta_epoch *meta_epoch;
	struct vex_epoch *epoch;
	int ret;

	if (!store || !epoch_out || !epoch_id)
		return VEX_STORE_ERR_INVALID;
	*epoch_out = NULL;
	if ((ret = meta_load(store, &state)))
		return ret;
	meta_epoch = find_epoch(&state, epoch_id);
	if (!meta_epoch || meta_epoch->state != VEX_EPOCH_OPEN) {
		ret = VEX_STORE_ERR_STATE;
		goto out;
	}
	CALLOC_ARRAY(epoch, 1);
	epoch->store = store;
	epoch->id = epoch_id;
	epoch->active = 1;
	epoch->owner = 0;
	ret = vex_epoch_heartbeat_now(epoch, 1);
	if (ret) {
		vex_epoch_detach(epoch);
		*epoch_out = NULL;
		goto out;
	}
	*epoch_out = epoch;
	ret = VEX_STORE_OK;
out:
	vex_meta_state_release(&state);
	return ret;
}

void vex_epoch_detach(struct vex_epoch *epoch)
{
	if (!epoch)
		return;
	epoch->active = 0;
	free(epoch);
}

int vex_epoch_seal(struct vex_epoch *epoch)
{
	struct vex_store *store;
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	struct vex_meta_epoch *meta_epoch;
	size_t i;
	int ret;

	if (!epoch || !epoch->active)
		return VEX_STORE_ERR_STATE;
	store = epoch->store;
	if ((ret = vex_epoch_heartbeat(epoch)))
		return ret;
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	meta_epoch = find_epoch(&state, epoch->id);
	if (!meta_epoch || meta_epoch->state != VEX_EPOCH_OPEN) {
		ret = VEX_STORE_ERR_STATE;
		goto out;
	}
	for (i = 0; i < state.epoch_objects_nr; i++) {
		struct vex_meta_epoch_object *entry = &state.epoch_objects[i];
		struct vex_meta_object *object;

		if (entry->epoch != epoch->id)
			continue;
		object = find_object(&state, &entry->oid);
		if (!object || object->state != VEX_OBJECT_PUBLISHED) {
			ret = VEX_STORE_ERR_STATE;
			goto out;
		}
	}
	meta_epoch->state = VEX_EPOCH_SEALED;
	ret = meta_save_locked(store, &lock, &state);
	if (!ret)
		epoch->active = 0;
out:
	if (lock.held)
		meta_unlock(store, &lock);
	vex_meta_state_release(&state);
	return ret;
}

int vex_epoch_compensate(struct vex_epoch *epoch)
{
	struct vex_store *store;
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	struct vex_meta_epoch *meta_epoch;
	struct vex_epoch_blob_list blobs = { 0 };
	int ret;
	int blob_ret;

	if (!epoch || !epoch->active)
		return VEX_STORE_ERR_STATE;
	store = epoch->store;
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	meta_epoch = find_epoch(&state, epoch->id);
	if (!meta_epoch || meta_epoch->state != VEX_EPOCH_OPEN) {
		ret = VEX_STORE_ERR_STATE;
		goto out;
	}
	snapshot_epoch_objects(&state, epoch->id, &blobs);
	abort_epoch_state(&state, epoch->id);
	ret = meta_save_locked(store, &lock, &state);
	if (!ret)
		epoch->active = 0;
out:
	if (lock.held)
		meta_unlock(store, &lock);
	vex_meta_state_release(&state);
	if (ret) {
		release_epoch_objects(&blobs);
		return ret;
	}
	blob_ret = blob_backend_compensate_epoch(store, epoch->id, &blobs);
	release_epoch_objects(&blobs);
	return blob_ret;
}

void vex_epoch_free(struct vex_epoch *epoch)
{
	if (!epoch)
		return;
	if (epoch->active && epoch->owner)
		vex_epoch_compensate(epoch);
	free(epoch);
}

int vex_blob_put_staging(struct vex_store *store, struct vex_epoch *epoch,
			 const struct object_id *oid, enum object_type type,
			 const void *data, unsigned long size)
{
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	int ret;

	if ((ret = check_epoch(store, epoch)) || !oid)
		return ret ? ret : VEX_STORE_ERR_INVALID;
	if ((ret = vex_epoch_heartbeat(epoch)))
		return ret;
	if ((ret = verify_object(store, oid, type, data, size)))
		return ret;
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	if (!epoch_is_open(&state, epoch->id)) {
		ret = VEX_STORE_ERR_STATE;
		goto out;
	}
	meta_unlock(store, &lock);
	ret = blob_backend_put_staging(store, epoch->id, oid, data, size);
	vex_meta_state_release(&state);
	return ret;
out:
	meta_unlock(store, &lock);
	vex_meta_state_release(&state);
	return ret;
}

int vex_blob_promote(struct vex_store *store, struct vex_epoch *epoch)
{
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	int ret;

	if ((ret = check_epoch(store, epoch)))
		return ret;
	if ((ret = vex_epoch_heartbeat(epoch)))
		return ret;
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	if (!epoch_is_open(&state, epoch->id))
		ret = VEX_STORE_ERR_STATE;
	else
		ret = blob_backend_promote_epoch(store, &state, epoch->id);
	meta_unlock(store, &lock);
	vex_meta_state_release(&state);
	return ret;
}

int vex_blob_compensate(struct vex_store *store, struct vex_epoch *epoch)
{
	struct vex_meta_state state;
	struct vex_epoch_blob_list blobs = { 0 };
	int ret = check_epoch(store, epoch);

	if (ret)
		return ret;
	if ((ret = meta_load(store, &state)))
		return ret;
	snapshot_epoch_objects(&state, epoch->id, &blobs);
	vex_meta_state_release(&state);
	ret = blob_backend_compensate_epoch(store, epoch->id, &blobs);
	release_epoch_objects(&blobs);
	return ret;
}

int vex_object_index_put_quarantine(struct vex_store *store,
			      struct vex_epoch *epoch,
			      const struct object_id *oid,
			      enum object_type type, unsigned long size)
{
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	struct vex_meta_epoch *meta_epoch;
	struct vex_meta_object *object;
	int ret;

	if ((ret = check_epoch(store, epoch)) || !oid || !valid_object_type(type))
		return ret ? ret : VEX_STORE_ERR_INVALID;
	if ((ret = vex_epoch_heartbeat(epoch)))
		return ret;
	if ((ret = blob_available_for_epoch(store, epoch->id, oid)))
		return ret;
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	meta_epoch = find_epoch(&state, epoch->id);
	if (!meta_epoch || meta_epoch->state != VEX_EPOCH_OPEN) {
		ret = VEX_STORE_ERR_STATE;
		goto out;
	}
	object = find_object(&state, oid);
	if (object && (object->type != type || object->size != size)) {
		ret = VEX_STORE_ERR_CORRUPT;
		goto out;
	}
	if (!object)
		add_object(&state, oid, type, size, epoch->id);
	ret = add_epoch_object(&state, epoch->id, oid);
	if (ret)
		goto out;
	ret = meta_save_locked(store, &lock, &state);
out:
	if (lock.held)
		meta_unlock(store, &lock);
	vex_meta_state_release(&state);
	return ret;
}

int vex_object_index_publish(struct vex_store *store, struct vex_epoch *epoch)
{
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	int ret;

	if ((ret = check_epoch(store, epoch)))
		return ret;
	if ((ret = vex_epoch_heartbeat(epoch)))
		return ret;
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	if (!epoch_is_open(&state, epoch->id)) {
		ret = VEX_STORE_ERR_STATE;
		goto out;
	}
	ret = publish_epoch(store, &state, epoch->id);
	if (ret)
		goto out;
	ret = meta_save_locked(store, &lock, &state);
out:
	if (lock.held)
		meta_unlock(store, &lock);
	vex_meta_state_release(&state);
	return ret;
}

int vex_object_index_get(struct vex_store *store, const struct object_id *oid,
			 struct vex_object_info *info)
{
	struct vex_meta_state state;
	struct vex_meta_object *object;
	int ret;

	if (!store || !oid || !info)
		return VEX_STORE_ERR_INVALID;
	if ((ret = meta_load(store, &state)))
		return ret;
	object = find_object(&state, oid);
	if (!object)
		ret = VEX_STORE_ERR_NOT_FOUND;
	else {
		info->type = object->type;
		info->size = object->size;
		info->epoch = object->epoch;
		info->state = object->state;
		ret = VEX_STORE_OK;
	}
	vex_meta_state_release(&state);
	return ret;
}

int vex_store_stage_object(struct vex_store *store, struct vex_epoch *epoch,
			   const struct object_id *oid, enum object_type type,
			   const void *data, unsigned long size)
{
	int ret;

	if ((ret = vex_blob_put_staging(store, epoch, oid, type, data, size)))
		return ret;
	if ((ret = vex_object_index_put_quarantine(store, epoch, oid, type, size)))
		return ret;
	if (type == OBJ_COMMIT)
		return index_commit_parents(store, epoch, oid, data, size);
	return VEX_STORE_OK;
}

int vex_store_has_object(struct vex_store *store, const struct object_id *oid)
{
	struct vex_meta_state state;
	struct vex_meta_object *object;
	int ret;

	if (!store || !oid)
		return VEX_STORE_ERR_INVALID;
	if ((ret = meta_load(store, &state)))
		return ret;
	object = find_object(&state, oid);
	if (!object || object->state != VEX_OBJECT_PUBLISHED) {
		ret = 0;
	} else {
		ret = blob_backend_has_final(store, oid);
		if (!ret)
			ret = VEX_STORE_ERR_CORRUPT;
	}
	vex_meta_state_release(&state);
	return ret;
}

static int read_object(struct vex_store *store, const struct vex_epoch *epoch,
		       const struct object_id *oid, enum object_type *type,
		       unsigned long *size, void **data)
{
	struct vex_meta_state state;
	struct vex_meta_object *object;
	void *contents = NULL;
	unsigned long contents_size;
	int ret;

	if (!store || !oid || !type || !size || !data)
		return VEX_STORE_ERR_INVALID;
	*data = NULL;
	if ((ret = meta_load(store, &state)))
		return ret;
	object = find_object(&state, oid);
	if (!object || (object->state != VEX_OBJECT_PUBLISHED &&
		(!epoch || !epoch_has_object(&state, epoch->id, oid)))) {
		ret = VEX_STORE_ERR_NOT_FOUND;
		goto out;
	}
	if (object->state == VEX_OBJECT_PUBLISHED) {
		ret = blob_backend_read_final(store, oid, &contents, &contents_size);
	} else {
		ret = blob_backend_read_staging(store, epoch->id, oid,
						 &contents, &contents_size);
	}
	if (ret)
		goto out;
	if (contents_size != object->size ||
	    verify_object(store, oid, object->type, contents, contents_size)) {
		ret = VEX_STORE_ERR_CORRUPT;
		goto out;
	}
	*type = object->type;
	*size = object->size;
	*data = contents;
	contents = NULL;
	ret = VEX_STORE_OK;
out:
	free(contents);
	vex_meta_state_release(&state);
	return ret;
}

int vex_store_read_object(struct vex_store *store, const struct object_id *oid,
			  enum object_type *type, unsigned long *size, void **data)
{
	return read_object(store, NULL, oid, type, size, data);
}

int vex_store_read_object_in_epoch(struct vex_store *store,
			   const struct vex_epoch *epoch, const struct object_id *oid,
			   enum object_type *type, unsigned long *size, void **data)
{
	int ret;

	if ((ret = check_epoch(store, epoch)))
		return ret;
	if ((ret = vex_epoch_heartbeat((struct vex_epoch *)epoch)))
		return ret;
	return read_object(store, epoch, oid, type, size, data);
}

static int current_pack_keys_match(const struct vex_store *store,
				   const struct vex_meta_mysql_pack *pack)
{
	char pack_hex[GIT_MAX_HEXSZ + 1];
	struct strbuf expected_pack = STRBUF_INIT;
	struct strbuf expected_idx = STRBUF_INIT;
	int matches;

	if (!store || !pack || !pack->pack_key || !pack->idx_key)
		return 0;
	hash_to_hex_algop_r(pack_hex, pack->pack_id, store->hash_algo);
	strbuf_addf(&expected_pack, "git/v1/%s/packs/%s.pack", store->repo_id,
		    pack_hex);
	strbuf_addf(&expected_idx, "git/v1/%s/packs/%s.idx", store->repo_id,
		    pack_hex);
	matches = !strcmp(pack->pack_key, expected_pack.buf) &&
		!strcmp(pack->idx_key, expected_idx.buf);
	strbuf_release(&expected_idx);
	strbuf_release(&expected_pack);
	return matches;
}

int vex_store_current_pack(struct vex_store *store,
			   struct vex_pack_artifact *artifact)
{
	struct vex_meta_mysql_pack pack = { 0 };
	int ret;

	if (!store || !artifact)
		return VEX_STORE_ERR_INVALID;
	memset(artifact, 0, sizeof(*artifact));
	if (store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_NOT_FOUND;
	ret = meta_mysql_result(vex_meta_mysql_current_sealed_pack(
		store->meta_mysql, store->repo_id, store->hash_algo, &pack));
	if (ret)
		return ret;
	if (!current_pack_keys_match(store, &pack)) {
		vex_meta_mysql_pack_release(&pack);
		return VEX_STORE_ERR_CORRUPT;
	}
	memcpy(artifact->pack_id.hash, pack.pack_id, store->hash_algo->rawsz);
	artifact->covering_generation = pack.covering_generation;
	vex_meta_mysql_pack_release(&pack);
	return VEX_STORE_OK;
}

int vex_store_download_pack(struct vex_store *store,
			    const struct vex_pack_artifact *artifact,
			    const char *pack_destination,
			    const char *idx_destination)
{
	int ret;

	if (!store || !artifact || !pack_destination || !idx_destination)
		return VEX_STORE_ERR_INVALID;
	if (store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_NOT_FOUND;
	ret = vex_blob_s3_download_derived_pack(store->blob_s3,
		artifact->pack_id.hash, store->hash_algo->rawsz, 0,
		pack_destination);
	if (ret)
		return blob_s3_result(ret);
	return blob_s3_result(vex_blob_s3_download_derived_pack(
		store->blob_s3, artifact->pack_id.hash, store->hash_algo->rawsz, 1,
		idx_destination));
}

int vex_store_pack_lease_acquire(struct vex_store *store,
				 const struct vex_pack_artifact *artifact,
				 unsigned int ttl_seconds,
				 struct vex_pack_lease *lease)
{
	int ret;

	if (!store || !artifact || !lease || !ttl_seconds ||
	    store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_INVALID;
	memset(lease, 0, sizeof(*lease));
	if (csprng_bytes(lease->id, sizeof(lease->id), 0) < 0)
		return VEX_STORE_ERR;
	ret = meta_mysql_result(vex_meta_mysql_clone_lease_acquire(
		store->meta_mysql, store->repo_id, artifact->pack_id.hash,
		store->hash_algo->rawsz, lease->id, ttl_seconds));
	if (ret) {
		memset(lease, 0, sizeof(*lease));
		return ret;
	}
	lease->active = 1;
	return VEX_STORE_OK;
}

int vex_store_pack_lease_release(struct vex_store *store,
				 struct vex_pack_lease *lease)
{
	int ret;

	if (!store || !lease || store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_INVALID;
	if (!lease->active)
		return VEX_STORE_OK;
	ret = meta_mysql_result(vex_meta_mysql_clone_lease_release(
		store->meta_mysql, store->repo_id, lease->id));
	/* A TTL can expire after a request ended; release remains idempotent. */
	if (ret == VEX_STORE_ERR_NOT_FOUND)
		ret = VEX_STORE_OK;
	if (!ret)
		memset(lease, 0, sizeof(*lease));
	return ret;
}

static int compare_object_ptrs(const void *a, const void *b)
{
	const struct vex_meta_object *const *left = a;
	const struct vex_meta_object *const *right = b;

	return oidcmp(&(*left)->oid, &(*right)->oid);
}

int vex_store_for_each_object(struct vex_store *store,
			      const struct vex_epoch *epoch,
			      const struct object_id *prefix,
			      size_t prefix_hex_len,
			      vex_object_iter_fn fn, void *cb_data)
{
	struct vex_meta_state state;
	struct vex_meta_object **objects = NULL;
	size_t objects_nr = 0;
	size_t i;
	int ret;

	if (!fn || (prefix_hex_len && !prefix) ||
	    prefix_hex_len > (store ? store->hash_algo->hexsz : 0))
		return VEX_STORE_ERR_INVALID;
	if ((ret = check_optional_epoch(store, epoch)))
		return ret;
	if ((ret = meta_load(store, &state)))
		return ret;
	if (state.objects_nr)
		ALLOC_ARRAY(objects, state.objects_nr);
	for (i = 0; i < state.objects_nr; i++) {
		if (!object_is_visible(&state, &state.objects[i], epoch))
			continue;
		if (prefix && oid_common_prefix_hexlen(&state.objects[i].oid,
							prefix) < prefix_hex_len)
			continue;
		objects[objects_nr++] = &state.objects[i];
	}
	QSORT(objects, objects_nr, compare_object_ptrs);
	for (i = 0; i < objects_nr; i++) {
		const struct vex_meta_object *object = objects[i];
		struct vex_object_info info = {
			.type = object->type,
			.size = object->size,
			.epoch = object->epoch,
			.state = object->state,
		};

		ret = fn(&object->oid, &info, cb_data);
		if (ret)
			break;
	}
	free(objects);
	vex_meta_state_release(&state);
	return ret;
}

int vex_store_count_objects(struct vex_store *store,
			    const struct vex_epoch *epoch, unsigned long *out)
{
	struct vex_meta_state state;
	unsigned long count = 0;
	size_t i;
	int ret;

	if (!out)
		return VEX_STORE_ERR_INVALID;
	*out = 0;
	if ((ret = check_optional_epoch(store, epoch)))
		return ret;
	if ((ret = meta_load(store, &state)))
		return ret;
	for (i = 0; i < state.objects_nr; i++) {
		if (!object_is_visible(&state, &state.objects[i], epoch))
			continue;
		if (count == ULONG_MAX) {
			ret = VEX_STORE_ERR_STATE;
			goto out;
		}
		count++;
	}
	*out = count;
	ret = VEX_STORE_OK;
out:
	vex_meta_state_release(&state);
	return ret;
}

int vex_store_find_abbrev_len(struct vex_store *store,
			     const struct vex_epoch *epoch,
			     const struct object_id *oid,
			     unsigned min_length, unsigned *out)
{
	struct vex_meta_state state;
	const unsigned hexsz = store ? store->hash_algo->hexsz : 0;
	size_t i;
	int ret;

	if (!oid || !out)
		return VEX_STORE_ERR_INVALID;
	if ((ret = check_optional_epoch(store, epoch)))
		return ret;
	if (min_length >= hexsz) {
		*out = hexsz;
		return VEX_STORE_OK;
	}
	*out = min_length;
	if ((ret = meta_load(store, &state)))
		return ret;
	for (i = 0; i < state.objects_nr; i++) {
		unsigned common;

		if (!object_is_visible(&state, &state.objects[i], epoch))
			continue;
		common = oid_common_prefix_hexlen(&state.objects[i].oid, oid);
		if (common == hexsz || common < *out)
			continue;
		*out = common + 1;
		if (*out >= hexsz) {
			*out = hexsz;
			break;
		}
	}
	vex_meta_state_release(&state);
	return VEX_STORE_OK;
}

/*
 * This is the mark half of Vex sweeping.  It deliberately holds the MySQL
 * repository-row transaction while it reads the canonical blob graph so the
 * resulting plan has one stable ref/pin snapshot.  The operation is
 * read-only; a later pack/lease/grace-aware delete phase can consume only the
 * sealed candidates reported here.
 */
struct vex_sweep_walk {
	struct vex_store *store;
	const struct vex_meta_state *state;
	struct oidset reachable;
	struct oidset ref_roots;
	struct oidset pin_roots;
	struct oidset pack_roots;
	struct oidset lease_roots;
	struct oidset open_epoch_roots;
	struct oid_array pending;
	int result;
};

struct vex_sweep_root_collect {
	struct vex_sweep_walk *walk;
	struct oidset *roots;
};

static int sweep_enqueue(struct vex_sweep_walk *walk,
			 const struct object_id *oid)
{
	if (!oidset_insert(&walk->reachable, oid))
		oid_array_append(&walk->pending, oid);
	return VEX_STORE_OK;
}

static int sweep_enqueue_hex_link(struct vex_sweep_walk *walk,
				  const char *hex, size_t hex_len)
{
	struct object_id oid;

	if (hex_len != walk->store->hash_algo->hexsz ||
	    get_oid_hex_algop(hex, &oid, walk->store->hash_algo))
		return VEX_STORE_ERR_CORRUPT;
	return sweep_enqueue(walk, &oid);
}

static int sweep_parse_commit(struct vex_sweep_walk *walk,
			      const void *data, unsigned long size)
{
	if (!data)
		return VEX_STORE_ERR_CORRUPT;
	const char *cursor = data;
	const char *end = cursor + size;
	int saw_tree = 0;

	while (cursor < end) {
		const char *line = cursor;
		const char *newline = memchr(line, '\n', end - line);
		size_t line_len = newline ? (size_t)(newline - line) :
			(size_t)(end - line);
		int ret;

		cursor = newline ? newline + 1 : end;
		if (!line_len)
			break;
		if (line_len >= 5 && !memcmp(line, "tree ", 5)) {
			ret = sweep_enqueue_hex_link(walk, line + 5, line_len - 5);
			if (ret)
				return ret;
			saw_tree = 1;
		} else if (line_len >= 7 && !memcmp(line, "parent ", 7)) {
			ret = sweep_enqueue_hex_link(walk, line + 7, line_len - 7);
			if (ret)
				return ret;
		}
	}
	return saw_tree ? VEX_STORE_OK : VEX_STORE_ERR_CORRUPT;
}

static int sweep_parse_tag(struct vex_sweep_walk *walk,
			   const void *data, unsigned long size)
{
	if (!data)
		return VEX_STORE_ERR_CORRUPT;
	const char *cursor = data;
	const char *end = cursor + size;
	int saw_object = 0;

	while (cursor < end) {
		const char *line = cursor;
		const char *newline = memchr(line, '\n', end - line);
		size_t line_len = newline ? (size_t)(newline - line) :
			(size_t)(end - line);
		int ret;

		cursor = newline ? newline + 1 : end;
		if (!line_len)
			break;
		if (line_len < 7 || memcmp(line, "object ", 7))
			continue;
		ret = sweep_enqueue_hex_link(walk, line + 7, line_len - 7);
		if (ret)
			return ret;
		saw_object = 1;
	}
	return saw_object ? VEX_STORE_OK : VEX_STORE_ERR_CORRUPT;
}

static int sweep_parse_tree(struct vex_sweep_walk *walk,
			    const void *data, unsigned long size)
{
	if (!size)
		return VEX_STORE_OK;
	if (!data)
		return VEX_STORE_ERR_CORRUPT;
	const unsigned char *cursor = data;
	const unsigned char *end = cursor + size;

	while (cursor < end) {
		const unsigned char *mode = cursor;
		const unsigned char *space;
		const unsigned char *nul;
		struct object_id oid;
		size_t mode_len;
		size_t i;
		int is_gitlink;

		space = memchr(cursor, ' ', end - cursor);
		if (!space || space == cursor)
			return VEX_STORE_ERR_CORRUPT;
		mode_len = space - mode;
		for (i = 0; i < mode_len; i++)
			if (mode[i] < '0' || mode[i] > '7')
				return VEX_STORE_ERR_CORRUPT;
		nul = memchr(space + 1, '\0', end - (space + 1));
		if (!nul || nul == space + 1 ||
		    (size_t)(end - (nul + 1)) < walk->store->hash_algo->rawsz)
			return VEX_STORE_ERR_CORRUPT;
		is_gitlink = mode_len == 6 && !memcmp(mode, "160000", 6);
		if (!is_gitlink) {
			oidread(&oid, nul + 1, walk->store->hash_algo);
			sweep_enqueue(walk, &oid);
		}
		cursor = nul + 1 + walk->store->hash_algo->rawsz;
	}
	return VEX_STORE_OK;
}

static int sweep_walk_one(struct vex_sweep_walk *walk,
			  const struct object_id *oid)
{
	const struct vex_meta_object *object;
	void *data = NULL;
	unsigned long size = 0;
	int ret;

	object = find_object((struct vex_meta_state *)walk->state, oid);
	if (!object || object->state != VEX_OBJECT_PUBLISHED)
		return VEX_STORE_ERR_CORRUPT;
	ret = blob_backend_read_final(walk->store, oid, &data, &size);
	if (ret)
		goto out;
	if (size != object->size ||
	    verify_object(walk->store, oid, object->type, data, size)) {
		ret = VEX_STORE_ERR_CORRUPT;
		goto out;
	}
	switch (object->type) {
	case OBJ_COMMIT:
		ret = sweep_parse_commit(walk, data, size);
		break;
	case OBJ_TREE:
		ret = sweep_parse_tree(walk, data, size);
		break;
	case OBJ_TAG:
		ret = sweep_parse_tag(walk, data, size);
		break;
	case OBJ_BLOB:
		ret = VEX_STORE_OK;
		break;
	default:
		ret = VEX_STORE_ERR_CORRUPT;
		break;
	}
out:
	free(data);
	return ret;
}

static int sweep_collect_root(
	const unsigned char raw_oid[VEX_META_MYSQL_OID_BYTES], void *cb_data)
{
	struct vex_sweep_root_collect *collect = cb_data;
	struct vex_sweep_walk *walk = collect->walk;
	struct object_id oid;
	unsigned int i;

	for (i = walk->store->hash_algo->rawsz;
	     i < VEX_META_MYSQL_OID_BYTES; i++)
		if (raw_oid[i]) {
			walk->result = VEX_STORE_ERR_CORRUPT;
			return VEX_META_MYSQL_ERR_STATE;
		}
	oidread(&oid, raw_oid, walk->store->hash_algo);
	if (!oidset_insert(collect->roots, &oid))
		sweep_enqueue(walk, &oid);
	return VEX_META_MYSQL_OK;
}

static int sweep_collect_open_epoch_roots(struct vex_sweep_walk *walk)
{
	size_t i;

	for (i = 0; i < walk->state->epoch_objects_nr; i++) {
		const struct vex_meta_epoch_object *entry =
			&walk->state->epoch_objects[i];
		const struct vex_meta_epoch *epoch =
			find_epoch((struct vex_meta_state *)walk->state, entry->epoch);
		const struct vex_meta_object *object;

		if (!epoch)
			return VEX_STORE_ERR_CORRUPT;
		if (epoch->state != VEX_EPOCH_OPEN)
			continue;
		object = find_object((struct vex_meta_state *)walk->state, &entry->oid);
		if (!object)
			return VEX_STORE_ERR_CORRUPT;
		if (!oidset_insert(&walk->open_epoch_roots, &entry->oid)) {
			/*
			 * A receive can be between staging and promotion.  Its quarantine
			 * row has no final canonical bytes to parse, but it is still a
			 * reachability root for deletion safety.  Published open-epoch
			 * objects do have canonical bytes, so walk their graph as usual.
			 */
			if (!oidset_insert(&walk->reachable, &entry->oid) &&
			    object->state == VEX_OBJECT_PUBLISHED)
				oid_array_append(&walk->pending, &entry->oid);
		}
	}
	return VEX_STORE_OK;
}

static int sweep_walk_mark_locked(struct vex_store *store,
				 const struct vex_meta_state *state,
				 struct vex_sweep_walk *walk)
{
	struct vex_sweep_root_collect pin_collect;
	struct vex_sweep_root_collect pack_collect;
	struct vex_sweep_root_collect lease_collect;
	size_t at = 0;
	size_t i;
	int ret;

	memset(walk, 0, sizeof(*walk));
	walk->store = store;
	walk->state = state;
	oidset_init(&walk->reachable, state->objects_nr);
	oidset_init(&walk->ref_roots, state->refs_nr);
	oidset_init(&walk->pin_roots, state->objects_nr);
	oidset_init(&walk->pack_roots, state->objects_nr);
	oidset_init(&walk->lease_roots, state->objects_nr);
	oidset_init(&walk->open_epoch_roots, state->epoch_objects_nr);
	pin_collect.walk = walk;
	pin_collect.roots = &walk->pin_roots;
	pack_collect.walk = walk;
	pack_collect.roots = &walk->pack_roots;
	lease_collect.walk = walk;
	lease_collect.roots = &walk->lease_roots;
	for (i = 0; i < state->refs_nr; i++) {
		const struct vex_meta_ref *ref = &state->refs[i];

		if (ref->target.type != VEX_REF_TARGET_OID)
			continue;
		if (!oidset_insert(&walk->ref_roots, &ref->target.oid))
			sweep_enqueue(walk, &ref->target.oid);
	}
	if ((ret = sweep_collect_open_epoch_roots(walk)))
		return ret;
	ret = vex_meta_mysql_for_each_active_pack_object(
		store->meta_mysql, store->repo_id, sweep_collect_root, &pack_collect);
	if (walk->result)
		return walk->result;
	if (ret)
		return meta_mysql_result(ret);
	ret = vex_meta_mysql_for_each_active_clone_lease_object(
		store->meta_mysql, store->repo_id, sweep_collect_root, &lease_collect);
	if (walk->result)
		return walk->result;
	if (ret)
		return meta_mysql_result(ret);
	ret = vex_meta_mysql_for_each_active_pin(store->meta_mysql, store->repo_id,
						   sweep_collect_root, &pin_collect);
	if (walk->result)
		return walk->result;
	if (ret)
		return meta_mysql_result(ret);
	while (at < walk->pending.nr) {
		ret = sweep_walk_one(walk, &walk->pending.oid[at++]);
		if (ret)
			return ret;
	}
	return VEX_STORE_OK;
}

static void sweep_walk_release(struct vex_sweep_walk *walk)
{
	oid_array_clear(&walk->pending);
	oidset_clear(&walk->open_epoch_roots);
	oidset_clear(&walk->lease_roots);
	oidset_clear(&walk->pack_roots);
	oidset_clear(&walk->pin_roots);
	oidset_clear(&walk->ref_roots);
	oidset_clear(&walk->reachable);
}

static void sweep_plan_from_walk(const struct vex_meta_state *state,
				 const struct vex_sweep_walk *walk,
				 struct vex_sweep_plan *plan)
{
	size_t i;

	memset(plan, 0, sizeof(*plan));
	for (i = 0; i < state->objects_nr; i++) {
		const struct vex_meta_object *object = &state->objects[i];
		const struct vex_meta_epoch *epoch;

		if (object->state != VEX_OBJECT_PUBLISHED)
			continue;
		plan->published_objects++;
		if (oidset_contains(&walk->reachable, &object->oid))
			continue;
		epoch = find_epoch((struct vex_meta_state *)state, object->epoch);
		if (epoch && epoch->state == VEX_EPOCH_SEALED)
			plan->sealed_candidates++;
	}
	plan->ref_roots = oidset_size(&walk->ref_roots);
	plan->pin_roots = oidset_size(&walk->pin_roots);
	plan->pack_roots = oidset_size(&walk->pack_roots);
	plan->lease_roots = oidset_size(&walk->lease_roots);
	plan->open_epoch_roots = oidset_size(&walk->open_epoch_roots);
	plan->reachable_objects = oidset_size(&walk->reachable);
}

int vex_store_pin_object(struct vex_store *store, const struct object_id *oid,
			 const char *reason)
{
	if (!store || !oid || !reason || !*reason ||
	    store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_INVALID;
	return meta_mysql_result(vex_meta_mysql_pin_object(
		store->meta_mysql, store->repo_id, oid->hash, store->hash_algo->rawsz,
		reason));
}

int vex_store_unpin_object(struct vex_store *store, const struct object_id *oid,
			   const char *reason)
{
	if (!store || !oid || !reason || !*reason ||
	    store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_INVALID;
	return meta_mysql_result(vex_meta_mysql_unpin_object(
		store->meta_mysql, store->repo_id, oid->hash, store->hash_algo->rawsz,
		reason));
}

int vex_store_sweep_plan(struct vex_store *store, struct vex_sweep_plan *plan)
{
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	struct vex_sweep_walk walk = { 0 };
	int ret;

	if (!store || !plan || store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_INVALID;
	memset(&state, 0, sizeof(state));
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	ret = sweep_walk_mark_locked(store, &state, &walk);
	if (!ret)
		sweep_plan_from_walk(&state, &walk, plan);
	sweep_walk_release(&walk);
	if (lock.held)
		meta_unlock(store, &lock);
	vex_meta_state_release(&state);
	return ret;
}

struct vex_sweep_generation_list {
	uint64_t *items;
	size_t nr;
	size_t alloc;
};

struct vex_sweep_tombstone {
	struct object_id oid;
	uint64_t generation;
	unsigned int due:1;
};

struct vex_sweep_tombstone_list {
	struct vex_sweep_tombstone *items;
	size_t nr;
	size_t alloc;
};

struct vex_sweep_tombstone_collect {
	struct vex_store *store;
	struct vex_sweep_tombstone_list *tombstones;
	int result;
};

struct vex_sweep_oid_collect {
	struct vex_store *store;
	struct oid_array *oids;
	int result;
};

static int sweep_oid_from_mysql(const struct vex_store *store,
				const unsigned char raw_oid[VEX_META_MYSQL_OID_BYTES],
				struct object_id *oid)
{
	unsigned int i;

	for (i = store->hash_algo->rawsz; i < VEX_META_MYSQL_OID_BYTES; i++)
		if (raw_oid[i])
			return VEX_STORE_ERR_CORRUPT;
	oidread(oid, raw_oid, store->hash_algo);
	return VEX_STORE_OK;
}

static int sweep_collect_expired_generation(uint64_t generation, void *cb_data)
{
	struct vex_sweep_generation_list *list = cb_data;

	if (!generation)
		return VEX_META_MYSQL_ERR_STATE;
	ALLOC_GROW(list->items, list->nr + 1, list->alloc);
	list->items[list->nr++] = generation;
	return VEX_META_MYSQL_OK;
}

static int sweep_generation_contains(const struct vex_sweep_generation_list *list,
				     uint64_t generation)
{
	size_t i;

	for (i = 0; i < list->nr; i++)
		if (list->items[i] == generation)
			return 1;
	return 0;
}

static int sweep_collect_tombstone(
	const struct vex_meta_mysql_sweep_tombstone *raw, void *cb_data)
{
	struct vex_sweep_tombstone_collect *collect = cb_data;
	struct vex_sweep_tombstone *item;
	int ret;

	if (!raw || !raw->generation)
		return VEX_META_MYSQL_ERR_STATE;
	ALLOC_GROW(collect->tombstones->items, collect->tombstones->nr + 1,
		   collect->tombstones->alloc);
	item = &collect->tombstones->items[collect->tombstones->nr++];
	memset(item, 0, sizeof(*item));
	ret = sweep_oid_from_mysql(collect->store, raw->oid, &item->oid);
	if (ret) {
		collect->result = ret;
		return VEX_META_MYSQL_ERR_STATE;
	}
	item->generation = raw->generation;
	item->due = raw->due;
	return VEX_META_MYSQL_OK;
}

static int sweep_collect_pending_blob(
	const unsigned char raw_oid[VEX_META_MYSQL_OID_BYTES], void *cb_data)
{
	struct vex_sweep_oid_collect *collect = cb_data;
	struct object_id oid;
	int ret;

	ret = sweep_oid_from_mysql(collect->store, raw_oid, &oid);
	if (ret) {
		collect->result = ret;
		return VEX_META_MYSQL_ERR_STATE;
	}
	oid_array_append(collect->oids, &oid);
	return VEX_META_MYSQL_OK;
}

static int sweep_epoch_has_any_object(const struct vex_meta_state *state,
				      uint64_t epoch_id)
{
	size_t i;

	for (i = 0; i < state->epoch_objects_nr; i++)
		if (state->epoch_objects[i].epoch == epoch_id)
			return 1;
	return 0;
}

static int sweep_remove_object_state(struct vex_meta_state *state,
				     const struct object_id *oid,
				     uint64_t *origin_generation)
{
	struct vex_meta_object *object;
	size_t i;

	object = find_object(state, oid);
	if (!object || object->state != VEX_OBJECT_PUBLISHED)
		return VEX_STORE_ERR_CORRUPT;
	if (origin_generation)
		*origin_generation = object->epoch;
	for (i = 0; i < state->epoch_objects_nr;) {
		if (oideq(&state->epoch_objects[i].oid, oid))
			remove_epoch_object_at(state, i);
		else
			i++;
	}
	for (i = 0; i < state->objects_nr; i++) {
		if (oideq(&state->objects[i].oid, oid)) {
			remove_object_at(state, i);
			return VEX_STORE_OK;
		}
	}
	return VEX_STORE_ERR_CORRUPT;
}

static void sweep_mark_empty_removed_epochs(struct vex_meta_state *state,
					    const struct vex_sweep_generation_list *removed)
{
	size_t i;

	for (i = 0; i < removed->nr; i++) {
		struct vex_meta_epoch *epoch = find_epoch(state, removed->items[i]);

		if (epoch && epoch->state == VEX_EPOCH_SEALED &&
		    !sweep_epoch_has_any_object(state, epoch->id))
			epoch->state = VEX_EPOCH_SWEPT;
	}
}

static int sweep_is_grace_candidate(const struct vex_meta_state *state,
				    const struct vex_sweep_walk *walk,
				    const struct vex_sweep_generation_list *expired,
				    const struct vex_meta_object *object)
{
	const struct vex_meta_epoch *epoch;

	if (!object || object->state != VEX_OBJECT_PUBLISHED ||
	    oidset_contains(&walk->reachable, &object->oid))
		return 0;
	epoch = find_epoch((struct vex_meta_state *)state, object->epoch);
	return epoch && epoch->state == VEX_EPOCH_SEALED &&
		sweep_generation_contains(expired, object->epoch);
}

int vex_store_sweep(struct vex_store *store, unsigned int grace_seconds,
		    unsigned int tombstone_seconds,
		    struct vex_sweep_result *result)
{
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	struct vex_sweep_walk walk = { 0 };
	struct vex_sweep_generation_list expired = { 0 };
	struct vex_sweep_generation_list removed = { 0 };
	struct vex_sweep_tombstone_list tombstones = { 0 };
	struct oid_array cleanup = OID_ARRAY_INIT;
	struct vex_sweep_tombstone_collect tombstone_collect;
	struct vex_sweep_oid_collect cleanup_collect;
	size_t i;
	int dirty = 0;
	int ret;
	int cleanup_ret = VEX_STORE_OK;

	if (!store || !result || !grace_seconds || !tombstone_seconds ||
	    grace_seconds > VEX_STORE_SWEEP_MAX_SECONDS ||
	    tombstone_seconds > VEX_STORE_SWEEP_MAX_SECONDS ||
	    store->backend != VEX_STORE_BACKEND_MYSQL_S3)
		return VEX_STORE_ERR_INVALID;
	memset(result, 0, sizeof(*result));
	memset(&state, 0, sizeof(state));
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	ret = sweep_walk_mark_locked(store, &state, &walk);
	if (ret)
		goto out;
	sweep_plan_from_walk(&state, &walk, &result->plan);
	ret = meta_mysql_result(vex_meta_mysql_state_for_each_expired_sealed_locked(
		store->meta_mysql, store->repo_id, grace_seconds,
		sweep_collect_expired_generation, &expired));
	if (ret)
		goto out;
	tombstone_collect.store = store;
	tombstone_collect.tombstones = &tombstones;
	tombstone_collect.result = VEX_STORE_OK;
	ret = meta_mysql_result(vex_meta_mysql_state_for_each_sweep_tombstone_locked(
		store->meta_mysql, store->repo_id, sweep_collect_tombstone,
		&tombstone_collect));
	if (tombstone_collect.result) {
		ret = tombstone_collect.result;
		goto out;
	}
	if (ret)
		goto out;
	cleanup_collect.store = store;
	cleanup_collect.oids = &cleanup;
	cleanup_collect.result = VEX_STORE_OK;
	ret = meta_mysql_result(
		vex_meta_mysql_state_for_each_pending_sweep_blob_locked(
			store->meta_mysql, store->repo_id, sweep_collect_pending_blob,
			&cleanup_collect));
	if (cleanup_collect.result) {
		ret = cleanup_collect.result;
		goto out;
	}
	if (ret)
		goto out;

	/* A new ref, pin, sealed pack, lease, or open receive rescues the row. */
	for (i = 0; i < tombstones.nr; i++) {
		const struct vex_sweep_tombstone *tombstone = &tombstones.items[i];
		struct vex_meta_object *object = find_object(&state, &tombstone->oid);
		int changed;
		uint64_t origin_generation;

		if (oidset_contains(&walk.reachable, &tombstone->oid)) {
			ret = meta_mysql_result(
				vex_meta_mysql_state_cancel_sweep_tombstone_locked(
					store->meta_mysql, store->repo_id,
					tombstone->oid.hash, store->hash_algo->rawsz,
					&changed));
			if (ret)
				goto out;
			if (changed) {
				result->tombstones_cancelled++;
				dirty = 1;
			}
			continue;
		}
		if (!object || object->state != VEX_OBJECT_PUBLISHED ||
		    object->epoch != tombstone->generation) {
			ret = VEX_STORE_ERR_CORRUPT;
			goto out;
		}
		if (!tombstone->due ||
		    !sweep_is_grace_candidate(&state, &walk, &expired, object))
			continue;
		ret = meta_mysql_result(
			vex_meta_mysql_state_mark_sweep_metadata_deleted_locked(
				store->meta_mysql, store->repo_id, tombstone->oid.hash,
				store->hash_algo->rawsz));
		if (ret)
			goto out;
		ret = sweep_remove_object_state(&state, &tombstone->oid,
						&origin_generation);
		if (ret)
			goto out;
		ALLOC_GROW(removed.items, removed.nr + 1, removed.alloc);
		removed.items[removed.nr++] = origin_generation;
		oid_array_append(&cleanup, &tombstone->oid);
		result->metadata_deleted++;
		dirty = 1;
	}
	for (i = 0; i < state.objects_nr; i++) {
		const struct vex_meta_object *object = &state.objects[i];
		int marked;

		if (!sweep_is_grace_candidate(&state, &walk, &expired, object))
			continue;
		result->grace_candidates++;
		ret = meta_mysql_result(
			vex_meta_mysql_state_mark_sweep_tombstone_locked(
				store->meta_mysql, store->repo_id, object->oid.hash,
				store->hash_algo->rawsz, object->epoch,
				tombstone_seconds, &marked));
		if (ret)
			goto out;
		if (marked) {
			result->tombstones_marked++;
			dirty = 1;
		}
	}
	if (removed.nr)
		sweep_mark_empty_removed_epochs(&state, &removed);
	if (dirty) {
		ret = meta_save_locked(store, &lock, &state);
		if (ret)
			goto out;
	} else {
		meta_unlock(store, &lock);
	}
	for (i = 0; i < cleanup.nr; i++) {
		int ready = 0;
		int one = meta_lock(store, &lock);

		if (!one)
			one = meta_mysql_result(
				vex_meta_mysql_state_prepare_sweep_blob_delete_locked(
					store->meta_mysql, store->repo_id, cleanup.oid[i].hash,
					store->hash_algo->rawsz, &ready));
		if (!one && ready) {
			/*
			 * Retain the repository-row lock across DELETE plus the durable
			 * completion marker.  A concurrent receive therefore cannot
			 * republish this immutable OID between the recheck and deletion.
			 */
			one = blob_s3_result(vex_blob_s3_delete_final(store->blob_s3,
								       &cleanup.oid[i]));
			if (one == VEX_STORE_ERR_NOT_FOUND)
				one = VEX_STORE_OK;
			if (!one)
				one = meta_mysql_result(
					vex_meta_mysql_state_mark_sweep_blob_deleted_locked(
						store->meta_mysql, store->repo_id,
						cleanup.oid[i].hash, store->hash_algo->rawsz));
		}
		if (!one)
			one = meta_commit_locked(store, &lock);
		if (lock.held)
			meta_unlock(store, &lock);
		if (one) {
			result->blobs_pending++;
			if (!cleanup_ret)
				cleanup_ret = one;
			continue;
		}
		if (ready)
			result->blobs_deleted++;
	}
	ret = cleanup_ret;

out:
	if (lock.held)
		meta_unlock(store, &lock);
	sweep_walk_release(&walk);
	vex_meta_state_release(&state);
	oid_array_clear(&cleanup);
	free(tombstones.items);
	free(removed.items);
	free(expired.items);
	return ret;
}

int vex_ref_read(struct vex_store *store, const char *refname, struct vex_ref *ref)
{
	struct vex_meta_state state;
	struct vex_meta_ref *found;
	int ret;

	if (!store || !refname || !ref)
		return VEX_STORE_ERR_INVALID;
	memset(ref, 0, sizeof(*ref));
	if ((ret = meta_load(store, &state)))
		return ret;
	found = find_ref(&state, refname);
	if (!found) {
		ret = VEX_STORE_ERR_NOT_FOUND;
		goto out;
	}
	ref->name = xstrdup(found->name);
	if ((ret = vex_ref_target_dup(&ref->target, &found->target))) {
		vex_ref_clear(ref);
		goto out;
	}
	ref->version = found->version;
	ret = VEX_STORE_OK;
out:
	vex_meta_state_release(&state);
	return ret;
}

void vex_ref_clear(struct vex_ref *ref)
{
	if (!ref)
		return;
	free((char *)ref->name);
	vex_ref_target_release(&ref->target);
	memset(ref, 0, sizeof(*ref));
}

static int compare_ref_ptrs(const void *a, const void *b)
{
	const struct vex_meta_ref *const *left = a;
	const struct vex_meta_ref *const *right = b;

	return strcmp((*left)->name, (*right)->name);
}

int vex_ref_iterate(struct vex_store *store, const char *prefix,
		    vex_ref_iter_fn fn, void *cb_data)
{
	struct vex_meta_state state;
	struct vex_meta_ref **refs;
	size_t i;
	size_t nr = 0;
	int ret;

	if (!store || !fn)
		return VEX_STORE_ERR_INVALID;
	if (!prefix)
		prefix = "";
	if ((ret = meta_load(store, &state)))
		return ret;
	ALLOC_ARRAY(refs, state.refs_nr);
	for (i = 0; i < state.refs_nr; i++)
		if (starts_with(state.refs[i].name, prefix))
			refs[nr++] = &state.refs[i];
	QSORT(refs, nr, compare_ref_ptrs);
	for (i = 0; i < nr; i++) {
		struct vex_ref ref = {
			.name = refs[i]->name,
			.target = refs[i]->target,
			.version = refs[i]->version,
		};

		ret = fn(&ref, cb_data);
		if (ret)
			break;
	}
	free(refs);
	vex_meta_state_release(&state);
	return ret;
}

int vex_ref_transaction_begin(struct vex_store *store, struct vex_epoch *epoch,
			      struct vex_ref_transaction **transaction_out)
{
	struct vex_ref_transaction *transaction;
	int ret;

	if (!transaction_out)
		return VEX_STORE_ERR_INVALID;
	if ((ret = check_epoch(store, epoch)))
		return ret;
	if ((ret = vex_epoch_heartbeat(epoch)))
		return ret;
	CALLOC_ARRAY(transaction, 1);
	transaction->store = store;
	transaction->epoch = epoch;
	*transaction_out = transaction;
	return VEX_STORE_OK;
}

int vex_ref_transaction_update(struct vex_ref_transaction *transaction,
			       const struct vex_ref_update *update)
{
	struct vex_ref_update *copy;
	size_t i;
	int ret;

	if (!transaction || transaction->committed)
		return VEX_STORE_ERR_STATE;
	if ((ret = check_epoch(transaction->store, transaction->epoch)))
		return ret;
	if ((ret = vex_epoch_heartbeat(transaction->epoch)))
		return ret;
	if ((ret = ref_update_valid(update)))
		return ret;
	for (i = 0; i < transaction->updates_nr; i++)
		if (!strcmp(transaction->updates[i].refname, update->refname))
			return VEX_STORE_ERR_INVALID;
	ALLOC_GROW(transaction->updates, transaction->updates_nr + 1,
		   transaction->updates_alloc);
	copy = &transaction->updates[transaction->updates_nr++];
	return vex_ref_update_dup(copy, update);
}

int vex_ref_transaction_commit(struct vex_ref_transaction *transaction)
{
	struct vex_store *store;
	struct vex_epoch *epoch;
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	struct vex_meta_epoch *meta_epoch;
	struct vex_epoch_blob_list blobs = { 0 };
	size_t i;
	int ret;

	if (!transaction || transaction->committed)
		return VEX_STORE_ERR_STATE;
	store = transaction->store;
	epoch = transaction->epoch;
	if ((ret = check_epoch(store, epoch)) ||
	    (ret = vex_epoch_heartbeat(epoch)) ||
	    (ret = load_locked(store, &lock, &state)))
		return ret;
	meta_epoch = find_epoch(&state, epoch->id);
	if (!meta_epoch || meta_epoch->state != VEX_EPOCH_OPEN) {
		ret = VEX_STORE_ERR_STATE;
		goto out;
	}
	for (i = 0; i < transaction->updates_nr; i++) {
		struct vex_meta_ref *ref = find_ref(&state,
						    transaction->updates[i].refname);

		if (!ref_expected_matches(ref, &transaction->updates[i])) {
			ret = VEX_STORE_ERR_CONFLICT;
			goto out;
		}
		if ((ret = verify_ref_target_object(store, &state, epoch->id,
						    &transaction->updates[i])))
			goto out;
	}
	ret = blob_backend_promote_epoch(store, &state, epoch->id);
	if (ret) {
		int save_ret;

		snapshot_epoch_objects(&state, epoch->id, &blobs);
		abort_epoch_state(&state, epoch->id);
		save_ret = meta_save_locked(store, &lock, &state);
		if (!save_ret)
			epoch->active = 0;
		if (lock.held)
			meta_unlock(store, &lock);
		vex_meta_state_release(&state);
		if (!save_ret)
			blob_backend_compensate_epoch(store, epoch->id, &blobs);
		release_epoch_objects(&blobs);
		return ret;
	}
	vex_test_kill_at("after-blob-promote");
	if ((ret = publish_epoch(store, &state, epoch->id)))
		goto out;
	for (i = 0; i < transaction->updates_nr; i++) {
		if ((ret = apply_ref_update(&state, &transaction->updates[i])) ||
		    (ret = append_ref_update(&state, &transaction->updates[i],
					      meta_epoch->push_id, NULL)))
			goto out;
	}
	meta_epoch->state = VEX_EPOCH_SEALED;
	ret = meta_save_locked(store, &lock, &state);
	if (!ret) {
		epoch->active = 0;
		transaction->committed = 1;
	}
	out:
	if (lock.held)
		meta_unlock(store, &lock);
	vex_meta_state_release(&state);
	return ret;
}

void vex_ref_transaction_abort(struct vex_ref_transaction *transaction)
{
	size_t i;

	if (!transaction)
		return;
	if (!transaction->committed && transaction->epoch->active)
		vex_epoch_compensate(transaction->epoch);
	for (i = 0; i < transaction->updates_nr; i++)
		vex_ref_update_release(&transaction->updates[i]);
	free(transaction->updates);
	free(transaction);
}

int vex_ref_updates_append(struct vex_store *store,
			   const struct vex_ref_update *update, uint64_t *id)
{
	struct vex_meta_lock lock;
	struct vex_meta_state state;
	int ret;

	if (!store)
		return VEX_STORE_ERR_INVALID;
	if ((ret = ref_update_valid(update)))
		return ret;
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	ret = append_ref_update(&state, update, NULL, id);
	if (!ret)
		ret = meta_save_locked(store, &lock, &state);
	if (lock.held)
		meta_unlock(store, &lock);
	vex_meta_state_release(&state);
	return ret;
}

int vex_ref_updates_iterate(struct vex_store *store,
			    vex_ref_update_iter_fn fn, void *cb_data)
{
	struct vex_meta_state state;
	size_t i;
	int ret;

	if (!store || !fn)
		return VEX_STORE_ERR_INVALID;
	if ((ret = meta_load(store, &state)))
		return ret;
	for (i = 0; i < state.ref_updates_nr; i++) {
		ret = fn(&state.ref_updates[i].update, state.ref_updates[i].id,
			 cb_data);
		if (ret)
			break;
	}
	vex_meta_state_release(&state);
	return ret;
}
