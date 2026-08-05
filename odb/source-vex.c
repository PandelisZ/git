#include "git-compat-util.h"
#include "config.h"
#include "hex.h"
#include "vex/blob-s3.h"
#include "vex/meta-mysql.h"
#include "vex/store.h"
#include "gettext.h"
#include "object-file.h"
#include "odb.h"
#include "odb/source-vex.h"
#include "odb/streaming.h"
#include "odb/transaction.h"
#include "repository.h"
#include "strbuf.h"
#include "strvec.h"

/*
 * The Vex source deliberately keeps source.path pointed at the thin
 * gitdir's objects directory.  A number of Git callers use that path for
 * discovery and diagnostics, but object bytes are never read from it here.
 */
struct odb_source_vex {
	struct odb_source base;
	struct vex_store *store;
	/* A child process's non-owning handle to the parent's receive epoch. */
	struct vex_epoch *inherited_epoch;
};

struct odb_transaction_vex {
	struct odb_transaction base;
	struct vex_epoch *epoch;
	/* The parent owns a receive-pack epoch and waits for the ref backend. */
	unsigned int receive_handoff:1;
	/* A receive helper attached its parent's epoch and must only detach. */
	unsigned int inherited_handoff:1;
	unsigned int handoff_finished:1;
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

static int vex_env_true(const char *name)
{
	const char *value = getenv(name);

	return value && (!strcmp(value, "1") || !strcmp(value, "true") ||
			 !strcmp(value, "yes"));
}

static int vex_env_port(const char *name, unsigned int *port)
{
	const char *value = getenv(name);
	char *end;
	unsigned long parsed;

	*port = 0;
	if (!value || !*value)
		return 0;
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno || !*value || *end || !parsed || parsed > UINT_MAX)
		return -1;
	*port = (unsigned int)parsed;
	return 0;
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
	const char *blob_backend = getenv("GIT_VEX_BLOB_BACKEND");
	const char *meta_backend = getenv("GIT_VEX_META_BACKEND");
	const char *backend = getenv("GIT_VEX_BACKEND");
	const char *s3_region = getenv("GIT_VEX_S3_REGION");
	struct vex_blob_s3_options blob_s3 = {
		.endpoint = getenv("GIT_VEX_S3_ENDPOINT"),
		.bucket = getenv("GIT_VEX_S3_BUCKET"),
		.region = s3_region && *s3_region ? s3_region : "us-east-1",
		.access_key = getenv("GIT_VEX_S3_ACCESS_KEY"),
		.secret_key = getenv("GIT_VEX_S3_SECRET_KEY"),
		.ca_file = getenv("GIT_VEX_S3_CA_FILE"),
		.insecure_tls = vex_env_true("GIT_VEX_S3_INSECURE_TLS"),
	};
	struct vex_meta_mysql_options meta_mysql = {
		.host = getenv("GIT_VEX_MYSQL_HOST"),
		.port = 0,
		.user = getenv("GIT_VEX_MYSQL_USER"),
		.password = getenv("GIT_VEX_MYSQL_PASSWORD"),
		.database = getenv("GIT_VEX_MYSQL_DATABASE"),
		.unix_socket = getenv("GIT_VEX_MYSQL_SOCKET"),
	};
	char *root = NULL;
	char *repo_id = vex_canonical_repo_id(repo->ref_storage_payload);
	int ret;

	*out = NULL;
	if (!repo_id)
		return VEX_STORE_ERR_INVALID;
	if (!meta_backend || !*meta_backend) {
		/* Keep the topology alias usable in process environments. */
		if (backend && !strcmp(backend, "mysql+s3"))
			meta_backend = "mysql";
		else
			meta_backend = "fs";
	}
	if (!strcmp(meta_backend, "mysql")) {
		if (vex_env_port("GIT_VEX_MYSQL_PORT", &meta_mysql.port)) {
			free(repo_id);
			return VEX_STORE_ERR_INVALID;
		}
		/* MySQL stores metadata only; a Vex MySQL store always has S3 bytes. */
		if (blob_backend && *blob_backend && strcmp(blob_backend, "s3")) {
			free(repo_id);
			return VEX_STORE_ERR_INVALID;
		}
		opts.backend = VEX_STORE_BACKEND_MYSQL_S3;
		opts.blob_s3 = &blob_s3;
		opts.meta_mysql = &meta_mysql;
	} else if (strcmp(meta_backend, "fs")) {
		free(repo_id);
		return VEX_STORE_ERR_INVALID;
	}
	if (blob_backend && *blob_backend) {
		if (!strcmp(blob_backend, "fs")) {
			if (opts.backend != VEX_STORE_BACKEND_FS) {
				free(repo_id);
				return VEX_STORE_ERR_INVALID;
			}
			/* The default, retained for explicitly-configured test fixtures. */
		} else if (!strcmp(blob_backend, "s3")) {
			if (opts.backend == VEX_STORE_BACKEND_FS)
				opts.backend = VEX_STORE_BACKEND_FS_S3;
			opts.blob_s3 = &blob_s3;
		} else {
			free(repo_id);
			return VEX_STORE_ERR_INVALID;
		}
	}

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
		if (opts.backend == VEX_STORE_BACKEND_MYSQL_S3)
			error(_("unable to initialize Vex MySQL metadata: %s"),
			      vex_store_error(*out));
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

/*
 * The lease is deliberately scoped to an already-selected vex:// repository.
 * The child verifies the repository id again before attaching, so a lease
 * cannot cross repository namespaces even when a process inherits its env.
 */
static int vex_source_parse_inherited_epoch(struct odb_source_vex *source,
					     uint64_t *epoch_id)
{
	const char *value = getenv("GIT_VEX_EPOCH");
	const char *repo_id;
	char *end;
	unsigned long long parsed;

	if (!value || !*value)
		return 0;
	repo_id = vex_store_repo_id(source->store);
	if (!skip_prefix(value, "v1:", &value) ||
	    !starts_with(value, repo_id))
		return -1;
	value += strlen(repo_id);
	if (*value++ != ':')
		return -1;
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno || !*value || *end || !parsed || parsed > UINT64_MAX)
		return -1;
	*epoch_id = parsed;
	return 1;
}

static int vex_source_inherited_epoch(struct odb_source_vex *source,
				      struct vex_epoch **epoch)
{
	uint64_t epoch_id;
	int ret;

	*epoch = NULL;
	if (source->inherited_epoch) {
		*epoch = source->inherited_epoch;
		return 0;
	}
	ret = vex_source_parse_inherited_epoch(source, &epoch_id);
	if (!ret)
		return 0;
	if (ret < 0 || vex_epoch_attach(source->store, epoch_id,
					  &source->inherited_epoch))
		return error(_("unable to attach inherited Vex receive epoch"));
	*epoch = source->inherited_epoch;
	return 0;
}

static int vex_source_active_epoch(struct odb_source_vex *source,
				   struct vex_epoch **epoch)
{
	struct odb_transaction_vex *transaction = vex_source_transaction(source);

	if (transaction) {
		*epoch = transaction->epoch;
		return 0;
	}
	return vex_source_inherited_epoch(source, epoch);
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
	struct vex_epoch *epoch;
	struct vex_object_info info;
	enum object_type type;
	unsigned long size;
	void *data = NULL;
	int ret;

	if (vex_source_open(source))
		return -1;

	if (vex_source_active_epoch(source, &epoch))
		return -1;
	if (oi && oi->contentp) {
		if (epoch)
			ret = vex_store_read_object_in_epoch(source->store, epoch,
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
		    (!epoch || info.epoch != vex_epoch_id(epoch))))
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
	struct vex_epoch *epoch;
	struct odb_read_stream_vex *stream;
	enum object_type type;
	unsigned long size;
	void *data = NULL;
	int ret;

	if (vex_source_open(source))
		return -1;
	if (vex_source_active_epoch(source, &epoch))
		return -1;
	if (epoch)
		ret = vex_store_read_object_in_epoch(source->store, epoch,
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

struct vex_for_each_object_data {
	struct odb_source_vex *source;
	const struct object_info *request;
	odb_for_each_object_cb cb;
	void *cb_data;
};

static int vex_for_each_object_cb(const struct object_id *oid,
				  const struct vex_object_info *info UNUSED,
				  void *cb_data)
{
	struct vex_for_each_object_data *data = cb_data;

	if (data->request) {
		struct object_info oi = *data->request;

		/*
		 * Keep the object-info contract: a request can include content, not
		 * only metadata supplied by the Vex index.
		 */
		if (odb_source_vex_read_object_info(&data->source->base, oid,
						    &oi, 0))
			return -1;
		return data->cb(oid, &oi, data->cb_data);
	}

	return data->cb(oid, NULL, data->cb_data);
}

static int odb_source_vex_for_each_object(struct odb_source *base,
					     const struct object_info *request,
					     odb_for_each_object_cb cb,
					     void *cb_data,
					     const struct odb_for_each_object_options *opts)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct vex_epoch *epoch;
	struct vex_for_each_object_data data = {
		.source = source,
		.request = request,
		.cb = cb,
		.cb_data = cb_data,
	};
	int ret;

	if ((opts->flags & ODB_FOR_EACH_OBJECT_PROMISOR_ONLY) ||
	    ((opts->flags & ODB_FOR_EACH_OBJECT_LOCAL_ONLY) && !base->local))
		return 0;
	if (vex_source_open(source))
		return -1;
	if (vex_source_active_epoch(source, &epoch))
		return -1;
	ret = vex_store_for_each_object(source->store,
					epoch,
					opts->prefix, opts->prefix_hex_len,
					vex_for_each_object_cb, &data);
	if (ret < 0)
		return error(_("unable to enumerate Vex objects"));
	return ret;
}

static int odb_source_vex_count_objects(struct odb_source *base,
					  enum odb_count_objects_flags flags UNUSED,
					  unsigned long *out)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct vex_epoch *epoch;
	int ret;

	if (vex_source_open(source))
		return -1;
	if (vex_source_active_epoch(source, &epoch))
		return -1;
	ret = vex_store_count_objects(source->store,
				      epoch, out);
	if (ret)
		return error(_("unable to count Vex objects"));
	return 0;
}

static int odb_source_vex_find_abbrev_len(struct odb_source *base,
					     const struct object_id *oid,
					     unsigned min_length,
					     unsigned *out)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct vex_epoch *epoch;
	int ret;

	if (vex_source_open(source))
		return -1;
	if (vex_source_active_epoch(source, &epoch))
		return -1;
	ret = vex_store_find_abbrev_len(source->store,
					 epoch,
					 oid, min_length, out);
	if (ret)
		return error(_("unable to find Vex object abbreviation"));
	return 0;
}

static int odb_source_vex_freshen_object(struct odb_source *base,
					    const struct object_id *oid,
					    const time_t *mtime UNUSED)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct vex_epoch *epoch;
	struct vex_object_info info;

	if (vex_source_open(source))
		return 0;
	if (vex_source_active_epoch(source, &epoch))
		return 0;
	if (epoch && !vex_object_index_get(source->store, oid, &info) &&
	    (info.state == VEX_OBJECT_PUBLISHED ||
	     info.epoch == vex_epoch_id(epoch)))
		return 1;
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
	struct vex_epoch *epoch;

	if (vex_source_open(source))
		return -1;
	if (vex_store_has_object(source->store, oid) > 0)
		return 0;

	if (vex_source_active_epoch(source, &epoch))
		return -1;
	if (epoch)
		return vex_source_stage_object(source, epoch, oid, type,
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

	/*
	 * receive-pack keeps this ODB transaction open while its ref backend joins
	 * the same epoch.  That backend has already sealed or compensated the
	 * epoch; this final call merely releases the parent's local handle.
	 */
	if (transaction->inherited_handoff) {
		vex_epoch_detach(transaction->epoch);
		transaction->epoch = NULL;
		return 0;
	}
	if (transaction->receive_handoff) {
		if (!transaction->handoff_finished) {
			ret = vex_epoch_compensate(transaction->epoch);
			if (ret)
				error(_("unable to compensate unfinished Vex receive epoch"));
		} else {
			ret = 0;
		}
		vex_epoch_detach(transaction->epoch);
		transaction->epoch = NULL;
		return ret;
	}

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

static int odb_transaction_vex_env(struct odb_transaction *base,
					     struct strvec *env)
{
	struct odb_transaction_vex *transaction =
		container_of(base, struct odb_transaction_vex, base);
	struct odb_source_vex *source =
		container_of(base->source, struct odb_source_vex, base);

	if (transaction->receive_handoff || transaction->inherited_handoff)
		strvec_pushf(env, "GIT_VEX_EPOCH=v1:%s:%"PRIu64,
			     vex_store_repo_id(source->store),
			     vex_epoch_id(transaction->epoch));
	return 0;
}

static int odb_source_vex_begin_transaction(struct odb_source *base,
					      struct odb_transaction **out,
					      enum odb_transaction_flags flags)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct odb_transaction_vex *transaction;
	struct vex_epoch *inherited_epoch = NULL;
	int ret;

	if (vex_source_open(source))
		return error(_("unable to open Vex object store"));

	transaction = xcalloc(1, sizeof(*transaction));
	transaction->base.source = base;
	transaction->base.commit = odb_transaction_vex_commit;
	transaction->base.write_object_stream = odb_transaction_vex_write_object_stream;
	transaction->base.env = odb_transaction_vex_env;
	transaction->receive_handoff = !!(flags & ODB_TRANSACTION_RECEIVE);
	ret = vex_source_inherited_epoch(source, &inherited_epoch);
	if (!ret && inherited_epoch) {
		/* Transfer the child's non-owning lease into its ODB transaction. */
		transaction->epoch = inherited_epoch;
		source->inherited_epoch = NULL;
		transaction->inherited_handoff = 1;
	} else if (!ret) {
		ret = vex_epoch_allocate(source->store, "git-odb", &transaction->epoch);
	}
	if (ret) {
		free(transaction);
		return error(_("unable to allocate Vex object epoch"));
	}

	*out = &transaction->base;
	return 0;
}

static struct odb_transaction_vex *vex_receive_transaction(
		struct odb_transaction *base)
{
	struct odb_source_vex *source;

	if (!base || !base->source || !odb_source_is_vex(base->source))
		return NULL;
	source = container_of(base->source, struct odb_source_vex, base);
	if (base->source != &source->base)
		return NULL;
	return container_of(base, struct odb_transaction_vex, base);
}

bool odb_vex_receive_transaction_is_active(
		const struct odb_transaction *base)
{
	struct odb_transaction_vex *transaction =
		vex_receive_transaction((struct odb_transaction *)base);

	return transaction && transaction->receive_handoff &&
		transaction->epoch &&
		!transaction->handoff_finished;
}

int odb_vex_receive_transaction_attach(struct odb_transaction *base,
				       struct vex_store *store,
				       struct vex_epoch **epoch)
{
	struct odb_transaction_vex *transaction = vex_receive_transaction(base);
	int ret;

	if (!transaction)
		return 0;
	if (!transaction->receive_handoff)
		return 0;
	if (!store || !epoch || transaction->handoff_finished ||
	    !transaction->epoch)
		return -1;
	ret = vex_epoch_attach(store, vex_epoch_id(transaction->epoch), epoch);
	if (ret)
		return -1;
	return 1;
}

void odb_vex_receive_transaction_finish(struct odb_transaction *base)
{
	struct odb_transaction_vex *transaction = vex_receive_transaction(base);

	if (transaction && transaction->receive_handoff)
		transaction->handoff_finished = 1;
}

void odb_vex_receive_transaction_abort(struct odb_transaction *base)
{
	struct odb_transaction_vex *transaction = vex_receive_transaction(base);

	if (!transaction || !transaction->receive_handoff ||
	    transaction->handoff_finished)
		return;
	if (transaction->epoch && vex_epoch_compensate(transaction->epoch))
		error(_("unable to compensate Vex receive epoch"));
	transaction->handoff_finished = 1;
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

	vex_epoch_detach(source->inherited_epoch);
	source->inherited_epoch = NULL;
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

	vex_epoch_detach(source->inherited_epoch);
	source->inherited_epoch = NULL;
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
