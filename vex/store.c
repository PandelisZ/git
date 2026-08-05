#include "git-compat-util.h"
#include "vex/store-internal.h"
#include "hex.h"
#include "object-file.h"
#include "strbuf.h"
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

static int load_locked(struct vex_store *store, struct vex_meta_fs_lock *lock,
		       struct vex_meta_state *state)
{
	int ret;

	if ((ret = vex_meta_fs_lock(store->meta, lock)))
		return ret;
	if ((ret = vex_meta_fs_load(store->meta, state)))
		vex_meta_fs_unlock(lock);
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
		exists = vex_blob_fs_has_final(store->blob, &object->oid);
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
		exists = vex_blob_fs_has_final(store->blob, &entry->oid);
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

static int blob_available_for_epoch(struct vex_store *store, uint64_t epoch,
				    const struct object_id *oid)
{
	void *data;
	unsigned long size;
	int ret;

	ret = vex_blob_fs_has_final(store->blob, oid);
	if (ret < 0)
		return ret;
	if (ret)
		return VEX_STORE_OK;
	ret = vex_blob_fs_read_staging(store->blob, epoch, oid, &data, &size);
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
	struct vex_meta_fs_lock lock;
	struct vex_meta_state state;
	struct object_id *parents;
	size_t parents_nr;
	size_t i;
	int ret;

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
	ret = vex_meta_fs_save_locked(store->meta, &lock, &state);
out:
	if (lock.held)
		vex_meta_fs_unlock(&lock);
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

	if (!store_out || !opts || opts->backend != VEX_STORE_BACKEND_FS ||
	    !opts->root || !*opts->root || !opts->hash_algo ||
	    !opts->hash_algo->rawsz || !opts->hash_algo->hexsz) {
		ret = VEX_STORE_ERR_INVALID;
		goto done;
	}
	if ((ret = normalize_repo_id(opts->repo_id, &repo_id)))
		goto done;
	CALLOC_ARRAY(store, 1);
	store->backend = opts->backend;
	store->root = xstrdup(opts->root);
	store->repo_id = strbuf_detach(&repo_id, NULL);
	store->hash_algo = opts->hash_algo;
	if ((ret = vex_meta_fs_open(&store->meta, store->root, store->repo_id,
				    store->hash_algo)) ||
	    (ret = vex_blob_fs_open(&store->blob, store->root, store->repo_id,
				    store->hash_algo))) {
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
	vex_blob_fs_close(store->blob);
	free(store->root);
	free(store->repo_id);
	free(store);
}

int vex_repo_ensure(struct vex_store *store)
{
	int ret;

	if (!store)
		return VEX_STORE_ERR_INVALID;
	if ((ret = vex_meta_fs_ensure(store->meta)))
		return ret;
	return vex_blob_fs_ensure(store->blob);
}

const char *vex_store_repo_id(const struct vex_store *store)
{
	return store ? store->repo_id : NULL;
}

const struct git_hash_algo *vex_store_hash_algo(const struct vex_store *store)
{
	return store ? store->hash_algo : NULL;
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
	struct vex_meta_fs_lock lock;
	struct vex_meta_state state;
	struct vex_meta_epoch *meta_epoch;
	struct vex_epoch *epoch;
	int ret;

	if (!store || !epoch_out)
		return VEX_STORE_ERR_INVALID;
	if ((ret = vex_repo_ensure(store)) || (ret = load_locked(store, &lock, &state)))
		return ret;
	ALLOC_GROW(state.epochs, state.epochs_nr + 1, state.epochs_alloc);
	meta_epoch = &state.epochs[state.epochs_nr++];
	meta_epoch->id = state.next_epoch++;
	meta_epoch->state = VEX_EPOCH_OPEN;
	meta_epoch->push_id = push_id ? xstrdup(push_id) : NULL;
	ret = vex_meta_fs_save_locked(store->meta, &lock, &state);
	if (ret) {
		vex_meta_state_release(&state);
		return ret;
	}
	CALLOC_ARRAY(epoch, 1);
	epoch->store = store;
	epoch->id = meta_epoch->id;
	epoch->active = 1;
	*epoch_out = epoch;
	vex_meta_state_release(&state);
	return VEX_STORE_OK;
}

uint64_t vex_epoch_id(const struct vex_epoch *epoch)
{
	return epoch ? epoch->id : 0;
}

int vex_epoch_seal(struct vex_epoch *epoch)
{
	struct vex_store *store;
	struct vex_meta_fs_lock lock;
	struct vex_meta_state state;
	struct vex_meta_epoch *meta_epoch;
	size_t i;
	int ret;

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
	ret = vex_meta_fs_save_locked(store->meta, &lock, &state);
	if (!ret)
		epoch->active = 0;
out:
	if (lock.held)
		vex_meta_fs_unlock(&lock);
	vex_meta_state_release(&state);
	return ret;
}

int vex_epoch_compensate(struct vex_epoch *epoch)
{
	struct vex_store *store;
	struct vex_meta_fs_lock lock;
	struct vex_meta_state state;
	struct vex_meta_epoch *meta_epoch;
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
	abort_epoch_state(&state, epoch->id);
	ret = vex_meta_fs_save_locked(store->meta, &lock, &state);
	if (!ret)
		epoch->active = 0;
out:
	if (lock.held)
		vex_meta_fs_unlock(&lock);
	vex_meta_state_release(&state);
	if (ret)
		return ret;
	blob_ret = vex_blob_fs_compensate_epoch(store->blob, epoch->id);
	return blob_ret;
}

void vex_epoch_free(struct vex_epoch *epoch)
{
	if (!epoch)
		return;
	if (epoch->active)
		vex_epoch_compensate(epoch);
	free(epoch);
}

int vex_blob_put_staging(struct vex_store *store, struct vex_epoch *epoch,
			 const struct object_id *oid, enum object_type type,
			 const void *data, unsigned long size)
{
	struct vex_meta_fs_lock lock;
	struct vex_meta_state state;
	int ret;

	if ((ret = check_epoch(store, epoch)) || !oid)
		return ret ? ret : VEX_STORE_ERR_INVALID;
	if ((ret = verify_object(store, oid, type, data, size)))
		return ret;
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	if (!epoch_is_open(&state, epoch->id)) {
		ret = VEX_STORE_ERR_STATE;
		goto out;
	}
	vex_meta_fs_unlock(&lock);
	ret = vex_blob_fs_put_staging(store->blob, epoch->id, oid, data, size);
	vex_meta_state_release(&state);
	return ret;
out:
	vex_meta_fs_unlock(&lock);
	vex_meta_state_release(&state);
	return ret;
}

int vex_blob_promote(struct vex_store *store, struct vex_epoch *epoch)
{
	struct vex_meta_fs_lock lock;
	struct vex_meta_state state;
	int ret;

	if ((ret = check_epoch(store, epoch)))
		return ret;
	if ((ret = load_locked(store, &lock, &state)))
		return ret;
	if (!epoch_is_open(&state, epoch->id))
		ret = VEX_STORE_ERR_STATE;
	else
		ret = vex_blob_fs_promote_epoch(store->blob, &state, epoch->id);
	vex_meta_fs_unlock(&lock);
	vex_meta_state_release(&state);
	return ret;
}

int vex_blob_compensate(struct vex_store *store, struct vex_epoch *epoch)
{
	int ret = check_epoch(store, epoch);

	if (ret)
		return ret;
	return vex_blob_fs_compensate_epoch(store->blob, epoch->id);
}

int vex_object_index_put_quarantine(struct vex_store *store,
			      struct vex_epoch *epoch,
			      const struct object_id *oid,
			      enum object_type type, unsigned long size)
{
	struct vex_meta_fs_lock lock;
	struct vex_meta_state state;
	struct vex_meta_epoch *meta_epoch;
	struct vex_meta_object *object;
	int ret;

	if ((ret = check_epoch(store, epoch)) || !oid || !valid_object_type(type))
		return ret ? ret : VEX_STORE_ERR_INVALID;
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
	ret = vex_meta_fs_save_locked(store->meta, &lock, &state);
out:
	if (lock.held)
		vex_meta_fs_unlock(&lock);
	vex_meta_state_release(&state);
	return ret;
}

int vex_object_index_publish(struct vex_store *store, struct vex_epoch *epoch)
{
	struct vex_meta_fs_lock lock;
	struct vex_meta_state state;
	int ret;

	if ((ret = check_epoch(store, epoch)))
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
	ret = vex_meta_fs_save_locked(store->meta, &lock, &state);
out:
	if (lock.held)
		vex_meta_fs_unlock(&lock);
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
	if ((ret = vex_meta_fs_load(store->meta, &state)))
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
	if ((ret = vex_meta_fs_load(store->meta, &state)))
		return ret;
	object = find_object(&state, oid);
	if (!object || object->state != VEX_OBJECT_PUBLISHED) {
		ret = 0;
	} else {
		ret = vex_blob_fs_has_final(store->blob, oid);
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
	if ((ret = vex_meta_fs_load(store->meta, &state)))
		return ret;
	object = find_object(&state, oid);
	if (!object || (object->state != VEX_OBJECT_PUBLISHED &&
		(!epoch || !epoch_has_object(&state, epoch->id, oid)))) {
		ret = VEX_STORE_ERR_NOT_FOUND;
		goto out;
	}
	if (object->state == VEX_OBJECT_PUBLISHED) {
		ret = vex_blob_fs_read_final(store->blob, oid, &contents, &contents_size);
	} else {
		ret = vex_blob_fs_read_staging(store->blob, epoch->id, oid,
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
	if (check_epoch(store, epoch))
		return VEX_STORE_ERR_STATE;
	return read_object(store, epoch, oid, type, size, data);
}

int vex_ref_read(struct vex_store *store, const char *refname, struct vex_ref *ref)
{
	struct vex_meta_state state;
	struct vex_meta_ref *found;
	int ret;

	if (!store || !refname || !ref)
		return VEX_STORE_ERR_INVALID;
	memset(ref, 0, sizeof(*ref));
	if ((ret = vex_meta_fs_load(store->meta, &state)))
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
	if ((ret = vex_meta_fs_load(store->meta, &state)))
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
	struct vex_meta_fs_lock lock;
	struct vex_meta_state state;
	struct vex_meta_epoch *meta_epoch;
	size_t i;
	int ret;

	if (!transaction || transaction->committed)
		return VEX_STORE_ERR_STATE;
	store = transaction->store;
	epoch = transaction->epoch;
	if ((ret = check_epoch(store, epoch)) ||
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
	ret = vex_blob_fs_promote_epoch(store->blob, &state, epoch->id);
	if (ret) {
		int save_ret;

		abort_epoch_state(&state, epoch->id);
		save_ret = vex_meta_fs_save_locked(store->meta, &lock, &state);
		if (!save_ret)
			epoch->active = 0;
		if (lock.held)
			vex_meta_fs_unlock(&lock);
		vex_meta_state_release(&state);
		if (!save_ret)
			vex_blob_fs_compensate_epoch(store->blob, epoch->id);
		return ret;
	}
	if ((ret = publish_epoch(store, &state, epoch->id)))
		goto out;
	for (i = 0; i < transaction->updates_nr; i++) {
		if ((ret = apply_ref_update(&state, &transaction->updates[i])) ||
		    (ret = append_ref_update(&state, &transaction->updates[i],
					      meta_epoch->push_id, NULL)))
			goto out;
	}
	meta_epoch->state = VEX_EPOCH_SEALED;
	ret = vex_meta_fs_save_locked(store->meta, &lock, &state);
	if (!ret) {
		epoch->active = 0;
		transaction->committed = 1;
	}
out:
	if (lock.held)
		vex_meta_fs_unlock(&lock);
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
	struct vex_meta_fs_lock lock;
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
		ret = vex_meta_fs_save_locked(store->meta, &lock, &state);
	if (lock.held)
		vex_meta_fs_unlock(&lock);
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
	if ((ret = vex_meta_fs_load(store->meta, &state)))
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
