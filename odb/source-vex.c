#include "git-compat-util.h"
#include "config.h"
#include "hex.h"
#include "vex/store.h"
#include "gettext.h"
#include "object-file.h"
#include "odb.h"
#include "odb/source-vex.h"
#include "odb/streaming.h"
#include "odb/transaction.h"
#include "repository.h"
#include "strbuf.h"

/*
 * The Vex source deliberately keeps source.path pointed at the thin
 * gitdir's objects directory.  A number of Git callers use that path for
 * discovery and diagnostics, but object bytes are never read from it here.
 */
struct odb_source_vex {
	struct odb_source base;
	struct vex_store *store;
};

struct odb_transaction_vex {
	struct odb_transaction base;
	struct vex_epoch *epoch;
};

struct odb_read_stream_vex {
	struct odb_read_stream base;
	unsigned char *data;
	size_t offset;
};

static char *vex_canonical_repo_id(const char *payload)
{
	char *repo_id;
	size_t nr = 0;

	if (!payload || !*payload)
		return NULL;

	repo_id = xmallocz(32);
	for (; *payload; payload++) {
		unsigned char c = *payload;

		if (c == '-')
			continue;
		if (!isxdigit(c) || nr == 32) {
			free(repo_id);
			return NULL;
		}
		repo_id[nr++] = tolower(c);
	}

	if (nr != 32) {
		free(repo_id);
		return NULL;
	}
	return repo_id;
}

int vex_store_open_for_repository(struct repository *repo, const char *gitdir,
					    struct vex_store **out)
{
	struct vex_store_options opts = {
		.backend = VEX_STORE_BACKEND_FS,
		.root = getenv("GIT_VEX_FS_ROOT"),
		.repo_id = NULL,
		.hash_algo = repo->hash_algo,
	};
	const char *configured_root = NULL;
	char *root = NULL;
	char *repo_id = vex_canonical_repo_id(repo->ref_storage_payload);
	int ret;

	*out = NULL;
	if (!repo_id)
		return VEX_STORE_ERR_INVALID;

	if ((!opts.root || !*opts.root) &&
	    !repo_config_get_value(repo, "vex.storeroot", &configured_root) &&
	    configured_root && *configured_root)
		opts.root = configured_root;
	if (!opts.root || !*opts.root) {
		root = xstrfmt("%s/vex-store", gitdir);
		opts.root = root;
	}
	opts.repo_id = repo_id;

	ret = vex_store_open(out, &opts);
	if (!ret)
		ret = vex_repo_ensure(*out);
	if (ret && *out) {
		vex_store_close(*out);
		*out = NULL;
	}

	free(root);
	free(repo_id);
	return ret;
}

static int vex_source_open(struct odb_source_vex *source)
{
	if (source->store)
		return 0;
	return vex_store_open_for_repository(source->base.odb->repo,
					       source->base.odb->repo->gitdir,
					       &source->store);
}

static struct odb_transaction_vex *vex_source_transaction(
		struct odb_source_vex *source)
{
	struct odb_transaction *transaction = source->base.odb->transaction;

	if (!transaction || transaction->source != &source->base)
		return NULL;
	return container_of(transaction, struct odb_transaction_vex, base);
}

static int vex_source_stage_object(struct odb_source_vex *source,
				     struct vex_epoch *epoch,
				     const struct object_id *oid,
				     enum object_type type,
				     const void *data, unsigned long size,
				     enum odb_write_object_flags flags)
{
	int ret;

	ret = vex_store_stage_object(source->store, epoch, oid, type, data, size);
	if (ret && !(flags & ODB_WRITE_OBJECT_SILENT))
		error(_("unable to stage Vex object %s"), oid_to_hex(oid));
	return ret;
}

static int vex_source_publish_object(struct odb_source_vex *source,
					 const struct object_id *oid,
					 enum object_type type,
					 const void *data, unsigned long size,
					 enum odb_write_object_flags flags)
{
	struct vex_epoch *epoch = NULL;
	int ret;

	ret = vex_epoch_allocate(source->store, "git-odb", &epoch);
	if (!ret)
		ret = vex_source_stage_object(source, epoch, oid, type, data, size, flags);
	if (!ret)
		ret = vex_blob_promote(source->store, epoch);
	if (!ret)
		ret = vex_object_index_publish(source->store, epoch);
	if (!ret)
		ret = vex_epoch_seal(epoch);
	if (ret) {
		if (epoch)
			vex_epoch_compensate(epoch);
		if (!(flags & ODB_WRITE_OBJECT_SILENT))
			error(_("unable to publish Vex object %s"), oid_to_hex(oid));
	}
	if (epoch)
		vex_epoch_free(epoch);
	return ret;
}

static void vex_populate_object_info(struct odb_source_vex *source,
				       struct object_info *oi,
				       enum object_type type, unsigned long size,
				       void *data)
{
	if (!oi) {
		free(data);
		return;
	}

	if (oi->typep)
		*oi->typep = type;
	if (oi->sizep)
		*oi->sizep = size;
	if (oi->disk_sizep)
		*oi->disk_sizep = 0;
	if (oi->delta_base_oid)
		oidclr(oi->delta_base_oid, source->base.odb->repo->hash_algo);
	if (oi->contentp)
		*oi->contentp = data;
	else
		free(data);
	if (oi->mtimep)
		*oi->mtimep = 0;
	if (oi->source_infop)
		oi->source_infop->source = &source->base;
}

static int odb_source_vex_read_object_info(struct odb_source *base,
					     const struct object_id *oid,
					     struct object_info *oi,
					     enum object_info_flags flags UNUSED)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct odb_transaction_vex *transaction;
	struct vex_object_info info;
	enum object_type type;
	unsigned long size;
	void *data = NULL;
	int ret;

	if (vex_source_open(source))
		return -1;

	transaction = vex_source_transaction(source);
	if (oi && oi->contentp) {
		if (transaction)
			ret = vex_store_read_object_in_epoch(source->store, transaction->epoch,
							     oid, &type, &size, &data);
		else
			ret = vex_store_read_object(source->store, oid, &type, &size, &data);
		if (ret)
			return -1;
		vex_populate_object_info(source, oi, type, size, data);
		return 0;
	}

	ret = vex_object_index_get(source->store, oid, &info);
	if (ret || (info.state != VEX_OBJECT_PUBLISHED &&
		    (!transaction || info.epoch != vex_epoch_id(transaction->epoch))))
		return -1;
	vex_populate_object_info(source, oi, info.type, info.size, NULL);
	return 0;
}

static ssize_t odb_read_stream_vex_read(struct odb_read_stream *base,
					  char *buf, size_t len)
{
	struct odb_read_stream_vex *stream =
		container_of(base, struct odb_read_stream_vex, base);
	size_t available = base->size - stream->offset;

	if (len > available)
		len = available;
	memcpy(buf, stream->data + stream->offset, len);
	stream->offset += len;
	return len;
}

static int odb_read_stream_vex_close(struct odb_read_stream *base)
{
	struct odb_read_stream_vex *stream =
		container_of(base, struct odb_read_stream_vex, base);

	free(stream->data);
	return 0;
}

static int odb_source_vex_read_object_stream(struct odb_read_stream **out,
					       struct odb_source *base,
					       const struct object_id *oid)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct odb_transaction_vex *transaction;
	struct odb_read_stream_vex *stream;
	enum object_type type;
	unsigned long size;
	void *data = NULL;
	int ret;

	if (vex_source_open(source))
		return -1;
	transaction = vex_source_transaction(source);
	if (transaction)
		ret = vex_store_read_object_in_epoch(source->store, transaction->epoch,
						     oid, &type, &size, &data);
	else
		ret = vex_store_read_object(source->store, oid, &type, &size, &data);
	if (ret)
		return -1;

	stream = xcalloc(1, sizeof(*stream));
	stream->base.read = odb_read_stream_vex_read;
	stream->base.close = odb_read_stream_vex_close;
	stream->base.type = type;
	stream->base.size = size;
	stream->data = data;
	*out = &stream->base;
	return 0;
}

static int odb_source_vex_for_each_object(struct odb_source *base UNUSED,
					     const struct object_info *request UNUSED,
					     odb_for_each_object_cb cb UNUSED,
					     void *cb_data UNUSED,
					     const struct odb_for_each_object_options *opts UNUSED)
{
	return error(_("Vex object enumeration is not implemented"));
}

static int odb_source_vex_count_objects(struct odb_source *base UNUSED,
					  enum odb_count_objects_flags flags UNUSED,
					  unsigned long *out)
{
	*out = 0;
	return error(_("Vex object enumeration is not implemented"));
}

static int odb_source_vex_find_abbrev_len(struct odb_source *base UNUSED,
					     const struct object_id *oid UNUSED,
					     unsigned min_length,
					     unsigned *out)
{
	*out = min_length;
	return error(_("Vex object abbreviation lookup is not implemented"));
}

static int odb_source_vex_freshen_object(struct odb_source *base,
					    const struct object_id *oid,
					    const time_t *mtime UNUSED)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);

	if (vex_source_open(source))
		return 0;
	return vex_store_has_object(source->store, oid) > 0;
}

static int odb_source_vex_write_object(struct odb_source *base,
					 const void *data, size_t size,
					 enum object_type type,
					 const struct object_id *oid,
					 const struct object_id *compat_oid UNUSED,
					 const time_t *mtime UNUSED,
					 enum odb_write_object_flags flags)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct odb_transaction_vex *transaction;

	if (vex_source_open(source))
		return -1;
	if (vex_store_has_object(source->store, oid) > 0)
		return 0;

	transaction = vex_source_transaction(source);
	if (transaction)
		return vex_source_stage_object(source, transaction->epoch, oid, type,
						 data, size, flags);
	return vex_source_publish_object(source, oid, type, data, size, flags);
}

static int vex_read_write_stream(struct odb_write_stream *stream, size_t size,
				   unsigned char **out)
{
	unsigned char *data = xmalloc(size ? size : 1);
	size_t offset = 0;

	while (offset < size) {
		ssize_t len = odb_write_stream_read(stream, data + offset, size - offset);

		if (len < 0 || !len || offset + len > size) {
			free(data);
			return -1;
		}
		offset += len;
	}

	if (!stream->is_finished) {
		unsigned char extra;
		ssize_t len = odb_write_stream_read(stream, &extra, 1);

		if (len || !stream->is_finished) {
			free(data);
			return -1;
		}
	}

	if (offset != size) {
		free(data);
		return -1;
	}
	*out = data;
	return 0;
}

static int odb_source_vex_write_object_stream(struct odb_source *base,
						struct odb_write_stream *stream,
						size_t size,
						struct object_id *oid)
{
	unsigned char *data;
	int ret;

	if (vex_read_write_stream(stream, size, &data))
		return error(_("invalid Vex object stream"));
	hash_object_file(base->odb->repo->hash_algo, data, size, OBJ_BLOB, oid);
	ret = odb_source_vex_write_object(base, data, size, OBJ_BLOB, oid,
						    NULL, NULL, 0);
	free(data);
	return ret;
}

static int odb_transaction_vex_write_object_stream(struct odb_transaction *base,
						      struct odb_write_stream *stream,
						      size_t size, struct object_id *oid)
{
	return odb_source_vex_write_object_stream(base->source, stream, size, oid);
}

static int odb_transaction_vex_commit(struct odb_transaction *base)
{
	struct odb_transaction_vex *transaction =
		container_of(base, struct odb_transaction_vex, base);
	struct odb_source_vex *source =
		container_of(base->source, struct odb_source_vex, base);
	int ret;

	ret = vex_blob_promote(source->store, transaction->epoch);
	if (!ret)
		ret = vex_object_index_publish(source->store, transaction->epoch);
	if (!ret)
		ret = vex_epoch_seal(transaction->epoch);
	if (ret) {
		vex_epoch_compensate(transaction->epoch);
		error(_("unable to publish Vex object transaction"));
	}
	vex_epoch_free(transaction->epoch);
	transaction->epoch = NULL;
	return ret;
}

static int odb_transaction_vex_env(struct odb_transaction *base UNUSED,
					     struct strvec *env UNUSED)
{
	/*
	 * There is no portable process-local epoch handoff in the store API yet.
	 * Child Git processes therefore open the same Vex repository normally;
	 * their objects are published before a subsequent ref transaction can
	 * advance a tip.  Receive-pack epoch handoff is completed by protocol work.
	 */
	return 0;
}

static int odb_source_vex_begin_transaction(struct odb_source *base,
					      struct odb_transaction **out,
					      enum odb_transaction_flags flags UNUSED)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct odb_transaction_vex *transaction;
	int ret;

	if (vex_source_open(source))
		return error(_("unable to open Vex object store"));

	transaction = xcalloc(1, sizeof(*transaction));
	transaction->base.source = base;
	transaction->base.commit = odb_transaction_vex_commit;
	transaction->base.write_object_stream = odb_transaction_vex_write_object_stream;
	transaction->base.env = odb_transaction_vex_env;
	ret = vex_epoch_allocate(source->store, "git-odb", &transaction->epoch);
	if (ret) {
		free(transaction);
		return error(_("unable to allocate Vex object epoch"));
	}

	*out = &transaction->base;
	return 0;
}

static int odb_source_vex_read_alternates(struct odb_source *base UNUSED,
					     struct strvec *out UNUSED)
{
	return 0;
}

static int odb_source_vex_write_alternate(struct odb_source *base UNUSED,
					      const char *alternate UNUSED)
{
	return error(_("Vex source does not support alternates"));
}

static void odb_source_vex_close(struct odb_source *base)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);

	if (source->store)
		vex_store_close(source->store);
	source->store = NULL;
}

static void odb_source_vex_prepare(struct odb_source *base UNUSED,
					     enum odb_prepare_flags flags UNUSED)
{
}

static void odb_source_vex_free(struct odb_source *base)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);

	if (source->store)
		vex_store_close(source->store);
	odb_source_release(base);
	free(source);
}

struct odb_source *odb_source_vex_new(struct object_database *odb,
				       const char *path, bool local)
{
	struct odb_source_vex *source = xcalloc(1, sizeof(*source));

	odb_source_init(&source->base, odb, ODB_SOURCE_VEX, path, local);
	if (vex_source_open(source))
		die(_("unable to open Vex object store for '%s'"),
		    odb->repo->ref_storage_payload ? odb->repo->ref_storage_payload : "");

	source->base.free = odb_source_vex_free;
	source->base.close = odb_source_vex_close;
	source->base.prepare = odb_source_vex_prepare;
	source->base.read_object_info = odb_source_vex_read_object_info;
	source->base.read_object_stream = odb_source_vex_read_object_stream;
	source->base.for_each_object = odb_source_vex_for_each_object;
	source->base.count_objects = odb_source_vex_count_objects;
	source->base.find_abbrev_len = odb_source_vex_find_abbrev_len;
	source->base.freshen_object = odb_source_vex_freshen_object;
	source->base.write_object = odb_source_vex_write_object;
	source->base.write_object_stream = odb_source_vex_write_object_stream;
	source->base.begin_transaction = odb_source_vex_begin_transaction;
	source->base.read_alternates = odb_source_vex_read_alternates;
	source->base.write_alternate = odb_source_vex_write_alternate;

	return &source->base;
}

bool odb_source_is_vex(const struct odb_source *source)
{
	return source && source->type == ODB_SOURCE_VEX;
}

bool repo_uses_vex_storage(const struct repository *repo)
{
	return repo && repo->ref_storage_format == REF_STORAGE_FORMAT_VEX;
}
