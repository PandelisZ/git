#include "git-compat-util.h"
#include "vex/store-internal.h"
#include "hex.h"
#include "path.h"
#include "strbuf.h"
#include "wrapper.h"

/*
 * The fs metadata backend keeps one atomically-replaced state file per repo.
 * It is intentionally a metadata-only mirror of the SQL schema: object bytes
 * are never serialized here.  A lock file protects writers while rename makes
 * readers observe either the old complete state or the new complete state.
 */

#define VEX_META_FS_VERSION "vex-meta-fs-v1"

struct vex_meta_fs {
	char *root;
	char *repo_id;
	const struct git_hash_algo *hash_algo;
};

static void meta_state_path(const struct vex_meta_fs *meta, struct strbuf *path)
{
	strbuf_addf(path, "%s/meta/%s/state", meta->root, meta->repo_id);
}

static int meta_ensure_parent(const struct vex_meta_fs *meta)
{
	struct strbuf path = STRBUF_INIT;
	int ret;

	meta_state_path(meta, &path);
	ret = safe_create_leading_directories_no_share(path.buf) == SCLD_OK ?
		VEX_STORE_OK : VEX_STORE_ERR;
	strbuf_release(&path);
	return ret;
}

static void encode_string(struct strbuf *out, const char *value)
{
	const unsigned char *p = (const unsigned char *)value;

	if (!value) {
		strbuf_addch(out, '-');
		return;
	}

	for (; *p; p++)
		strbuf_addf(out, "%02x", *p);
}

static int decode_string(const char *encoded, char **value)
{
	size_t len;
	char *result;

	*value = NULL;
	if (!strcmp(encoded, "-"))
		return VEX_STORE_OK;
	len = strlen(encoded);
	if (len & 1)
		return VEX_STORE_ERR_CORRUPT;
	result = xmallocz(len / 2);
	if (hex_to_bytes((unsigned char *)result, encoded, len / 2)) {
		free(result);
		return VEX_STORE_ERR_CORRUPT;
	}
	*value = result;
	return VEX_STORE_OK;
}

static void encode_oid(struct strbuf *out, const struct object_id *oid,
		       const struct git_hash_algo *hash_algo)
{
	char hex[GIT_MAX_HEXSZ + 1];

	if (!oid) {
		strbuf_addch(out, '-');
		return;
	}
	hash_to_hex_algop_r(hex, oid->hash, hash_algo);
	strbuf_addstr(out, hex);
}

static int decode_oid(const char *encoded, const struct git_hash_algo *hash_algo,
		      struct object_id *oid)
{
	if (!strcmp(encoded, "-")) {
		oidclr(oid, hash_algo);
		return VEX_STORE_OK;
	}
	if (strlen(encoded) != hash_algo->hexsz ||
	    get_oid_hex_algop(encoded, oid, hash_algo))
		return VEX_STORE_ERR_CORRUPT;
	return VEX_STORE_OK;
}

static int parse_u64(const char *value, uint64_t *result)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno || !*value || *end)
		return VEX_STORE_ERR_CORRUPT;
	*result = parsed;
	return VEX_STORE_OK;
}

static int parse_ulong(const char *value, unsigned long *result)
{
	uint64_t parsed;

	if (parse_u64(value, &parsed) || parsed > ULONG_MAX)
		return VEX_STORE_ERR_CORRUPT;
	*result = parsed;
	return VEX_STORE_OK;
}

static int parse_uint(const char *value, unsigned int *result)
{
	uint64_t parsed;

	if (parse_u64(value, &parsed) || parsed > UINT_MAX)
		return VEX_STORE_ERR_CORRUPT;
	*result = parsed;
	return VEX_STORE_OK;
}

static int parse_int(const char *value, int *result)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(value, &end, 10);
	if (errno || !*value || *end || parsed < INT_MIN || parsed > INT_MAX)
		return VEX_STORE_ERR_CORRUPT;
	*result = parsed;
	return VEX_STORE_OK;
}

static int split_fields(char *line, char **fields, size_t fields_alloc)
{
	char *next = line;
	int fields_nr = 0;

	while (next && fields_nr < fields_alloc) {
		char *tab = strchr(next, '\t');

		fields[fields_nr++] = next;
		if (!tab)
			return fields_nr;
		*tab = '\0';
		next = tab + 1;
	}
	return next ? -1 : fields_nr;
}

void vex_ref_target_release(struct vex_ref_target *target)
{
	free((char *)target->symref);
	target->symref = NULL;
	target->type = VEX_REF_TARGET_NONE;
	memset(&target->oid, 0, sizeof(target->oid));
}

int vex_ref_target_dup(struct vex_ref_target *dst,
			 const struct vex_ref_target *src)
{
	memset(dst, 0, sizeof(*dst));
	if (src->type < VEX_REF_TARGET_ANY ||
	    src->type > VEX_REF_TARGET_SYMREF)
		return VEX_STORE_ERR_INVALID;
	if (src->type == VEX_REF_TARGET_SYMREF && !src->symref)
		return VEX_STORE_ERR_INVALID;

	dst->type = src->type;
	oidcpy(&dst->oid, &src->oid);
	if (src->symref)
		dst->symref = xstrdup(src->symref);
	return VEX_STORE_OK;
}

int vex_ref_target_equal(const struct vex_ref_target *a,
			   const struct vex_ref_target *b)
{
	if (a->type != b->type)
		return 0;
	switch (a->type) {
	case VEX_REF_TARGET_ANY:
	case VEX_REF_TARGET_NONE:
		return 1;
	case VEX_REF_TARGET_OID:
		return oideq(&a->oid, &b->oid);
	case VEX_REF_TARGET_SYMREF:
		return a->symref && b->symref && !strcmp(a->symref, b->symref);
	}
	return 0;
}

void vex_ref_update_release(struct vex_ref_update *update)
{
	if (!update)
		return;
	free((char *)update->refname);
	vex_ref_target_release(&update->old_target);
	vex_ref_target_release(&update->new_target);
	free((char *)update->actor);
	free((char *)update->push_id);
	free((char *)update->reason);
	free((char *)update->message);
	memset(update, 0, sizeof(*update));
}

int vex_ref_update_dup(struct vex_ref_update *dst,
			 const struct vex_ref_update *src)
{
	memset(dst, 0, sizeof(*dst));
	if (!src->refname || vex_ref_target_dup(&dst->old_target, &src->old_target) ||
	    vex_ref_target_dup(&dst->new_target, &src->new_target))
		goto invalid;

	dst->refname = xstrdup(src->refname);
	dst->expected_version = src->expected_version;
	dst->is_fast_forward = src->is_fast_forward;
	if (src->actor)
		dst->actor = xstrdup(src->actor);
	if (src->push_id)
		dst->push_id = xstrdup(src->push_id);
	if (src->reason)
		dst->reason = xstrdup(src->reason);
	if (src->message)
		dst->message = xstrdup(src->message);
	return VEX_STORE_OK;

invalid:
	vex_ref_update_release(dst);
	return VEX_STORE_ERR_INVALID;
}

void vex_meta_state_init(struct vex_meta_state *state)
{
	memset(state, 0, sizeof(*state));
	state->next_epoch = 1;
	state->next_update = 1;
}

void vex_meta_state_release(struct vex_meta_state *state)
{
	size_t i;

	for (i = 0; i < state->epochs_nr; i++)
		free(state->epochs[i].push_id);
	for (i = 0; i < state->refs_nr; i++) {
		free(state->refs[i].name);
		vex_ref_target_release(&state->refs[i].target);
	}
	for (i = 0; i < state->ref_updates_nr; i++)
		vex_ref_update_release(&state->ref_updates[i].update);
	FREE_AND_NULL(state->epochs);
	FREE_AND_NULL(state->objects);
	FREE_AND_NULL(state->epoch_objects);
	FREE_AND_NULL(state->refs);
	FREE_AND_NULL(state->ref_updates);
	FREE_AND_NULL(state->commit_parents);
	state->epochs_nr = state->epochs_alloc = 0;
	state->objects_nr = state->objects_alloc = 0;
	state->epoch_objects_nr = state->epoch_objects_alloc = 0;
	state->refs_nr = state->refs_alloc = 0;
	state->ref_updates_nr = state->ref_updates_alloc = 0;
	state->commit_parents_nr = state->commit_parents_alloc = 0;
	state->next_epoch = 1;
	state->next_update = 1;
}

static int decode_target(char **fields, const struct git_hash_algo *hash_algo,
			 struct vex_ref_target *target)
{
	uint64_t type;
	int ret;

	if ((ret = parse_u64(fields[0], &type)) ||
	    type > VEX_REF_TARGET_SYMREF)
		return VEX_STORE_ERR_CORRUPT;
	memset(target, 0, sizeof(*target));
	target->type = type;
	if ((ret = decode_oid(fields[1], hash_algo, &target->oid)) ||
	    (ret = decode_string(fields[2], (char **)&target->symref))) {
		vex_ref_target_release(target);
		return ret;
	}
	if (target->type == VEX_REF_TARGET_SYMREF && !target->symref) {
		vex_ref_target_release(target);
		return VEX_STORE_ERR_CORRUPT;
	}
	return VEX_STORE_OK;
}

static void encode_target(struct strbuf *out, const struct vex_ref_target *target,
			  const struct git_hash_algo *hash_algo)
{
	strbuf_addf(out, "%d\t", target->type);
	encode_oid(out, target->type == VEX_REF_TARGET_OID ? &target->oid : NULL,
		   hash_algo);
	strbuf_addch(out, '\t');
	encode_string(out, target->type == VEX_REF_TARGET_SYMREF ? target->symref : NULL);
}

static int add_epoch(struct vex_meta_state *state, uint64_t id,
		     enum vex_epoch_state epoch_state, const char *push_id)
{
	struct vex_meta_epoch *epoch;

	if (epoch_state < VEX_EPOCH_OPEN || epoch_state > VEX_EPOCH_ABORTED)
		return VEX_STORE_ERR_CORRUPT;
	ALLOC_GROW(state->epochs, state->epochs_nr + 1, state->epochs_alloc);
	epoch = &state->epochs[state->epochs_nr++];
	epoch->id = id;
	epoch->state = epoch_state;
	epoch->push_id = NULL;
	if (decode_string(push_id, &epoch->push_id))
		return VEX_STORE_ERR_CORRUPT;
	return VEX_STORE_OK;
}

static int add_object(struct vex_meta_state *state, const struct object_id *oid,
		      enum object_type type, unsigned long size, uint64_t epoch,
		      enum vex_object_state object_state)
{
	struct vex_meta_object *object;

	if (type <= OBJ_NONE || type >= OBJ_MAX ||
	    (object_state != VEX_OBJECT_QUARANTINE &&
	     object_state != VEX_OBJECT_PUBLISHED))
		return VEX_STORE_ERR_CORRUPT;
	ALLOC_GROW(state->objects, state->objects_nr + 1, state->objects_alloc);
	object = &state->objects[state->objects_nr++];
	oidcpy(&object->oid, oid);
	object->type = type;
	object->size = size;
	object->epoch = epoch;
	object->state = object_state;
	return VEX_STORE_OK;
}

static int add_epoch_object(struct vex_meta_state *state, uint64_t epoch,
			    const struct object_id *oid)
{
	struct vex_meta_epoch_object *epoch_object;

	ALLOC_GROW(state->epoch_objects, state->epoch_objects_nr + 1,
		   state->epoch_objects_alloc);
	epoch_object = &state->epoch_objects[state->epoch_objects_nr++];
	epoch_object->epoch = epoch;
	oidcpy(&epoch_object->oid, oid);
	return VEX_STORE_OK;
}

static int add_ref(struct vex_meta_state *state, const char *name,
		   const struct vex_ref_target *target, uint64_t version)
{
	struct vex_meta_ref *ref;

	ALLOC_GROW(state->refs, state->refs_nr + 1, state->refs_alloc);
	ref = &state->refs[state->refs_nr++];
	memset(ref, 0, sizeof(*ref));
	ref->name = NULL;
	if (decode_string(name, &ref->name) || vex_ref_target_dup(&ref->target, target)) {
		free(ref->name);
		ref->name = NULL;
		vex_ref_target_release(&ref->target);
		return VEX_STORE_ERR_CORRUPT;
	}
	ref->version = version;
	return VEX_STORE_OK;
}

static int add_ref_update(struct vex_meta_state *state, char **fields,
			  const struct git_hash_algo *hash_algo)
{
	struct vex_meta_ref_update *entry;
	struct vex_ref_update *update;
	uint64_t id;
	int ret;

	if ((ret = parse_u64(fields[1], &id)))
		return ret;
	ALLOC_GROW(state->ref_updates, state->ref_updates_nr + 1,
		   state->ref_updates_alloc);
	entry = &state->ref_updates[state->ref_updates_nr++];
	memset(entry, 0, sizeof(*entry));
	entry->id = id;
	update = &entry->update;
	if ((ret = decode_string(fields[2], (char **)&update->refname)) ||
	    (ret = decode_target(fields + 3, hash_algo, &update->old_target)) ||
	    (ret = parse_u64(fields[6], &update->expected_version)) ||
	    (ret = decode_target(fields + 7, hash_algo, &update->new_target)) ||
	    (ret = parse_int(fields[10], &update->is_fast_forward)) ||
	    (ret = decode_string(fields[11], (char **)&update->actor)) ||
	    (ret = decode_string(fields[12], (char **)&update->push_id)) ||
	    (ret = decode_string(fields[13], (char **)&update->reason)) ||
	    (ret = decode_string(fields[14], (char **)&update->message))) {
		vex_ref_update_release(update);
		return ret;
	}
	if (!update->refname || !update->reason)
		return VEX_STORE_ERR_CORRUPT;
	return VEX_STORE_OK;
}

static int parse_state_line(struct vex_meta_state *state,
			    const struct vex_meta_fs *meta,
			    char *line, int *saw_repository, int *saw_next)
{
	char *fields[16];
	int fields_nr = split_fields(line, fields, ARRAY_SIZE(fields));
	uint64_t value;
	uint64_t id;
	uint64_t epoch;
	uint64_t object_state;
	unsigned long size;
	unsigned int position;
	struct object_id oid;
	struct object_id parent;
	struct vex_ref_target target;
	int ret;

	if (fields_nr < 1)
		return VEX_STORE_ERR_CORRUPT;
	if (!strcmp(fields[0], "R")) {
		if (fields_nr != 3 || strcmp(fields[1], meta->repo_id) ||
		    strcmp(fields[2], meta->hash_algo->name) || *saw_repository)
			return VEX_STORE_ERR_CORRUPT;
		*saw_repository = 1;
		return VEX_STORE_OK;
	}
	if (!strcmp(fields[0], "N")) {
		if (fields_nr != 3 || parse_u64(fields[1], &state->next_epoch) ||
		    parse_u64(fields[2], &state->next_update) || !state->next_epoch ||
		    !state->next_update || *saw_next)
			return VEX_STORE_ERR_CORRUPT;
		*saw_next = 1;
		return VEX_STORE_OK;
	}
	if (!strcmp(fields[0], "E")) {
		if (fields_nr != 4 || parse_u64(fields[1], &id) ||
		    parse_u64(fields[2], &value) ||
		    value > VEX_EPOCH_ABORTED)
			return VEX_STORE_ERR_CORRUPT;
		return add_epoch(state, id, value, fields[3]);
	}
	if (!strcmp(fields[0], "O")) {
		if (fields_nr != 6 || decode_oid(fields[1], meta->hash_algo, &oid) ||
		    parse_u64(fields[2], &value) || parse_ulong(fields[3], &size) ||
		    parse_u64(fields[4], &epoch) ||
		    parse_u64(fields[5], &object_state))
			return VEX_STORE_ERR_CORRUPT;
		return add_object(state, &oid, value, size, epoch, object_state);
	}
	if (!strcmp(fields[0], "Q")) {
		if (fields_nr != 3 || parse_u64(fields[1], &value) ||
		    decode_oid(fields[2], meta->hash_algo, &oid))
			return VEX_STORE_ERR_CORRUPT;
		return add_epoch_object(state, value, &oid);
	}
	if (!strcmp(fields[0], "F")) {
		if (fields_nr != 6 || decode_target(fields + 2, meta->hash_algo, &target) ||
		    parse_u64(fields[5], &value))
			return VEX_STORE_ERR_CORRUPT;
		ret = add_ref(state, fields[1], &target, value);
		vex_ref_target_release(&target);
		return ret;
	}
	if (!strcmp(fields[0], "U")) {
		if (fields_nr != 15)
			return VEX_STORE_ERR_CORRUPT;
		return add_ref_update(state, fields, meta->hash_algo);
	}
	if (!strcmp(fields[0], "C")) {
		struct vex_meta_commit_parent *entry;

		if (fields_nr != 4 || decode_oid(fields[1], meta->hash_algo, &oid) ||
		    parse_uint(fields[2], &position) ||
		    decode_oid(fields[3], meta->hash_algo, &parent))
			return VEX_STORE_ERR_CORRUPT;
		ALLOC_GROW(state->commit_parents, state->commit_parents_nr + 1,
			   state->commit_parents_alloc);
		entry = &state->commit_parents[state->commit_parents_nr++];
		oidcpy(&entry->commit_oid, &oid);
		entry->position = position;
		oidcpy(&entry->parent_oid, &parent);
		return VEX_STORE_OK;
	}
	return VEX_STORE_ERR_CORRUPT;
}

int vex_meta_fs_open(struct vex_meta_fs **meta_out, const char *root,
		       const char *repo_id, const struct git_hash_algo *hash_algo)
{
	struct vex_meta_fs *meta;

	CALLOC_ARRAY(meta, 1);
	meta->root = xstrdup(root);
	meta->repo_id = xstrdup(repo_id);
	meta->hash_algo = hash_algo;
	*meta_out = meta;
	return VEX_STORE_OK;
}

void vex_meta_fs_close(struct vex_meta_fs *meta)
{
	if (!meta)
		return;
	free(meta->root);
	free(meta->repo_id);
	free(meta);
}

int vex_meta_fs_lock(struct vex_meta_fs *meta,
		       struct vex_meta_fs_lock *lock)
{
	struct strbuf path = STRBUF_INIT;
	int ret = VEX_STORE_ERR;

	memset(lock, 0, sizeof(*lock));
	if (meta_ensure_parent(meta))
		goto done;
	meta_state_path(meta, &path);
	if (hold_lock_file_for_update(&lock->lock, path.buf, LOCK_NO_DEREF) < 0)
		goto done;
	lock->held = 1;
	ret = VEX_STORE_OK;
done:
	strbuf_release(&path);
	return ret;
}

void vex_meta_fs_unlock(struct vex_meta_fs_lock *lock)
{
	if (lock->held)
		rollback_lock_file(&lock->lock);
	lock->held = 0;
}

int vex_meta_fs_load(struct vex_meta_fs *meta, struct vex_meta_state *state)
{
	struct strbuf path = STRBUF_INIT;
	struct strbuf content = STRBUF_INIT;
	char *cursor;
	char *end;
	char *line;
	int saw_repository = 0;
	int saw_next = 0;
	int ret = VEX_STORE_OK;
	ssize_t bytes;

	vex_meta_state_init(state);
	meta_state_path(meta, &path);
	bytes = strbuf_read_file(&content, path.buf, 0);
	if (bytes < 0) {
		if (errno == ENOENT)
			goto done;
		ret = VEX_STORE_ERR;
		goto done;
	}
	cursor = content.buf;
	end = content.buf + content.len;
	line = cursor;
	cursor = memchr(cursor, '\n', end - cursor);
	if (!cursor || line == cursor) {
		ret = VEX_STORE_ERR_CORRUPT;
		goto done;
	}
	*cursor++ = '\0';
	if (strcmp(line, VEX_META_FS_VERSION)) {
		ret = VEX_STORE_ERR_CORRUPT;
		goto done;
	}
	while (cursor < end) {
		line = cursor;
		cursor = memchr(cursor, '\n', end - cursor);
		if (cursor)
			*cursor++ = '\0';
		else
			cursor = end;
		if (!*line)
			continue;
		ret = parse_state_line(state, meta, line,
				       &saw_repository, &saw_next);
		if (ret)
			goto done;
	}
	if (!saw_repository || !saw_next)
		ret = VEX_STORE_ERR_CORRUPT;
done:
	if (ret)
		vex_meta_state_release(state);
	strbuf_release(&path);
	strbuf_release(&content);
	return ret;
}

static void serialize_state(struct strbuf *out, const struct vex_meta_fs *meta,
			    const struct vex_meta_state *state)
{
	size_t i;

	strbuf_addf(out, "%s\nR\t%s\t%s\nN\t%"PRIu64"\t%"PRIu64"\n",
		    VEX_META_FS_VERSION, meta->repo_id, meta->hash_algo->name,
		    state->next_epoch, state->next_update);
	for (i = 0; i < state->epochs_nr; i++) {
		const struct vex_meta_epoch *epoch = &state->epochs[i];

		strbuf_addf(out, "E\t%"PRIu64"\t%d\t", epoch->id, epoch->state);
		encode_string(out, epoch->push_id);
		strbuf_addch(out, '\n');
	}
	for (i = 0; i < state->objects_nr; i++) {
		const struct vex_meta_object *object = &state->objects[i];

		strbuf_addstr(out, "O\t");
		encode_oid(out, &object->oid, meta->hash_algo);
		strbuf_addf(out, "\t%d\t%lu\t%"PRIu64"\t%d\n",
			    object->type, object->size, object->epoch, object->state);
	}
	for (i = 0; i < state->epoch_objects_nr; i++) {
		const struct vex_meta_epoch_object *entry = &state->epoch_objects[i];

		strbuf_addf(out, "Q\t%"PRIu64"\t", entry->epoch);
		encode_oid(out, &entry->oid, meta->hash_algo);
		strbuf_addch(out, '\n');
	}
	for (i = 0; i < state->refs_nr; i++) {
		const struct vex_meta_ref *ref = &state->refs[i];

		strbuf_addstr(out, "F\t");
		encode_string(out, ref->name);
		strbuf_addch(out, '\t');
		encode_target(out, &ref->target, meta->hash_algo);
		strbuf_addf(out, "\t%"PRIu64"\n", ref->version);
	}
	for (i = 0; i < state->ref_updates_nr; i++) {
		const struct vex_meta_ref_update *entry = &state->ref_updates[i];
		const struct vex_ref_update *update = &entry->update;

		strbuf_addf(out, "U\t%"PRIu64"\t", entry->id);
		encode_string(out, update->refname);
		strbuf_addch(out, '\t');
		encode_target(out, &update->old_target, meta->hash_algo);
		strbuf_addf(out, "\t%"PRIu64"\t", update->expected_version);
		encode_target(out, &update->new_target, meta->hash_algo);
		strbuf_addf(out, "\t%d\t", update->is_fast_forward);
		encode_string(out, update->actor);
		strbuf_addch(out, '\t');
		encode_string(out, update->push_id);
		strbuf_addch(out, '\t');
		encode_string(out, update->reason);
		strbuf_addch(out, '\t');
		encode_string(out, update->message);
		strbuf_addch(out, '\n');
	}
	for (i = 0; i < state->commit_parents_nr; i++) {
		const struct vex_meta_commit_parent *entry =
			&state->commit_parents[i];

		strbuf_addstr(out, "C\t");
		encode_oid(out, &entry->commit_oid, meta->hash_algo);
		strbuf_addf(out, "\t%u\t", entry->position);
		encode_oid(out, &entry->parent_oid, meta->hash_algo);
		strbuf_addch(out, '\n');
	}
}

int vex_meta_fs_save_locked(struct vex_meta_fs *meta,
			      struct vex_meta_fs_lock *lock,
			      const struct vex_meta_state *state)
{
	struct strbuf content = STRBUF_INIT;
	int fd;
	int ret = VEX_STORE_ERR;

	if (!lock->held)
		return VEX_STORE_ERR_STATE;
	serialize_state(&content, meta, state);
	fd = get_lock_file_fd(&lock->lock);
	if (write_in_full(fd, content.buf, content.len) < 0 ||
	    git_fsync(fd, FSYNC_HARDWARE_FLUSH) < 0)
		goto done;
	if (commit_lock_file(&lock->lock))
		goto done;
	lock->held = 0;
	ret = VEX_STORE_OK;
done:
	if (ret)
		vex_meta_fs_unlock(lock);
	strbuf_release(&content);
	return ret;
}

int vex_meta_fs_ensure(struct vex_meta_fs *meta)
{
	struct vex_meta_fs_lock lock;
	struct vex_meta_state state;
	int ret;

	if ((ret = vex_meta_fs_lock(meta, &lock)))
		return ret;
	if ((ret = vex_meta_fs_load(meta, &state))) {
		vex_meta_fs_unlock(&lock);
		return ret;
	}
	ret = vex_meta_fs_save_locked(meta, &lock, &state);
	vex_meta_state_release(&state);
	return ret;
}
