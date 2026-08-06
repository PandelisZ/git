#include "git-compat-util.h"
#include "abspath.h"
#include "commit-reach.h"
#include "commit.h"
#include "config.h"
#include "dir.h"
#include "hash.h"
#include "hex.h"
#include "lockfile.h"
#include "pack.h"
#include "packfile.h"
#include "vex/blob-s3.h"
#include "vex/meta-mysql.h"
#include "vex/store.h"
#include "gettext.h"
#include "object-file.h"
#include "odb.h"
#include "odb/source-packed.h"
#include "odb/source-vex.h"
#include "odb/streaming.h"
#include "odb/transaction.h"
#include "parse.h"
#include "path.h"
#include "repository.h"
#include "run-command.h"
#include "strbuf.h"
#include "strvec.h"
#include "tempfile.h"
#include "trace.h"
#include "wrapper.h"

#define VEX_DERIVED_PACK_CACHE_LEASE_SECONDS 900
#define VEX_FETCH_DELTA_CACHE_MAX_PACKS 32
#define VEX_FETCH_DELTA_CACHE_MAX_PACK_BYTES (32UL * 1024UL * 1024UL)

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
	/*
	 * A receive-pack-local indexed WAL.  Unlike pack_cache below, this is
	 * never selectable by a reader: it exists only while the receive epoch is
	 * open so connectivity checks and hooks can inspect the exact incoming
	 * pack before its S3 components and ref update are sealed.
	 */
	struct odb_source_packed *receive_pack_cache;
	char *receive_pack_path;
	char *receive_pack_id;
	/*
	 * A read-only, disposable cache of one S3-derived pack.  It is never an
	 * object source of truth: cache misses and cache failures fall through to
	 * the canonical Vex store below.
	 */
	struct odb_source_packed *pack_cache;
	char *pack_cache_id;
	char *pack_cache_path;
	/* Protect the selected pack while this source can serve it locally. */
	struct vex_pack_lease pack_lease;
	/*
	 * A bounded, disposable NVMe cache of normal fast-forward receive deltas.
	 * It is deliberately independent from the MySQL/S3 source of truth: its
	 * only job is to keep an immediate post-push fetch from paying one S3 curl
	 * process per newly received object.
	 */
	struct odb_source_packed *fetch_delta_cache;
	char *fetch_delta_cache_path;
	/*
	 * A retained raw seed is an ODB base only after normal receives advance the
	 * generation beyond a complete relay artifact.  New objects still resolve
	 * through the canonical per-object store below; this cache supplies their
	 * immutable ancestors without turning the stale pack into a clone relay.
	 */
	struct odb_source_packed *raw_base_cache;
	char *raw_base_cache_id;
	char *raw_base_cache_path;
	struct vex_pack_lease raw_base_lease;
};

/*
 * A request-scoped default-clone relay cache.  This deliberately does not
 * live in odb_source_vex: the ordinary complete-snapshot cache remains the
 * only cache used by generic ODB reads.  The caller owns this handle and
 * closes it before its source, releasing both the packed source and lease.
 */
struct odb_vex_default_clone_pack {
	struct odb_source_vex *source;
	struct odb_source_packed *packed;
	char *pack_id;
	char *cache_path;
	struct vex_pack_lease lease;
};

struct odb_transaction_vex {
	struct odb_transaction base;
	struct vex_epoch *epoch;
	/*
	 * One receive can carry a very large pack.  When the explicit pack-WAL
	 * path is enabled, index-pack writes it below this trusted scratch
	 * directory and the parent source keeps it visible until the receive
	 * transaction reaches a terminal state.
	 */
	char *receive_pack_objects_path;
	struct object_id receive_pack_id;
	unsigned int receive_pack_ready:1;
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

static int vex_source_open(struct odb_source_vex *source);

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

static char *vex_source_receive_pack_objects_path(
		const struct odb_source_vex *source, uint64_t epoch_id)
{
	const char *scratch = getenv("GIT_VEX_SCRATCH");
	const char *repo_id;

	if (!source || !source->store || !epoch_id || !scratch ||
	    !is_absolute_path(scratch) || strpbrk(scratch, "\r\n"))
		return NULL;
	repo_id = vex_store_repo_id(source->store);
	if (!repo_id || !*repo_id)
		return NULL;
	return xstrfmt("%s/vex-receive/%s/%"PRIu64"/objects", scratch,
		       repo_id, epoch_id);
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
		ret = vex_store_ensure_metadata(*out);
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

static void vex_source_release_cached_pack(
	struct odb_source_vex *source, struct odb_source_packed **cache,
	char **pack_id, char **cache_path, struct vex_pack_lease *lease)
{
	int ret;

	if (cache && *cache) {
		odb_source_close(&(*cache)->base);
		odb_source_free(&(*cache)->base);
		*cache = NULL;
	}
	if (pack_id) {
		free(*pack_id);
		*pack_id = NULL;
	}
	if (cache_path) {
		free(*cache_path);
		*cache_path = NULL;
	}
	if (!lease || !lease->active)
		return;
	ret = source && source->store ?
		vex_store_pack_lease_release(source->store, lease) :
		VEX_STORE_ERR_INVALID;
	if (ret)
		trace_printf("vex: could not release derived pack lease repo=%s (%d)",
			     source && source->store ? vex_store_repo_id(source->store) : "unknown",
			     ret);
	/* A failed release is safely bounded by the durable lease TTL. */
	memset(lease, 0, sizeof(*lease));
}

static void vex_source_release_pack_cache(struct odb_source_vex *source)
{
	vex_source_release_cached_pack(source, &source->pack_cache,
				       &source->pack_cache_id,
				       &source->pack_cache_path,
				       &source->pack_lease);
}

static void vex_source_release_fetch_delta_cache(struct odb_source_vex *source)
{
	if (source->fetch_delta_cache) {
		odb_source_close(&source->fetch_delta_cache->base);
		odb_source_free(&source->fetch_delta_cache->base);
	}
	source->fetch_delta_cache = NULL;
	free(source->fetch_delta_cache_path);
	source->fetch_delta_cache_path = NULL;
}

static void vex_source_release_raw_base_cache(struct odb_source_vex *source)
{
	vex_source_release_cached_pack(source, &source->raw_base_cache,
				       &source->raw_base_cache_id,
				       &source->raw_base_cache_path,
				       &source->raw_base_lease);
}

static void vex_source_release_receive_pack_cache(struct odb_source_vex *source)
{
	if (source->receive_pack_cache) {
		odb_source_close(&source->receive_pack_cache->base);
		odb_source_free(&source->receive_pack_cache->base);
	}
	source->receive_pack_cache = NULL;
	free(source->receive_pack_id);
	source->receive_pack_id = NULL;
	free(source->receive_pack_path);
	source->receive_pack_path = NULL;
}

static unsigned int vex_source_pack_cache_lease_seconds(void)
{
	const char *value = getenv("GIT_VEX_TEST_PACK_LEASE_SECONDS");
	unsigned long ttl;

	/* Test-only override; production always uses the conservative default. */
	if (!value || !*value)
		return VEX_DERIVED_PACK_CACHE_LEASE_SECONDS;
	ttl = git_env_ulong("GIT_VEX_TEST_PACK_LEASE_SECONDS",
				    VEX_DERIVED_PACK_CACHE_LEASE_SECONDS);
	if (!ttl || ttl > 86400)
		return VEX_DERIVED_PACK_CACHE_LEASE_SECONDS;
	return ttl;
}

static int vex_source_derived_pack_cache_enabled(void)
{
	/* A planner test can exercise metadata roots without creating a cache lease. */
	return !git_env_bool("GIT_VEX_TEST_DISABLE_DERIVED_PACK_CACHE", 0);
}

static char *vex_source_pack_cache_root(const struct odb_source_vex *source)
{
	const char *scratch = getenv("GIT_VEX_SCRATCH");
	const char *fallback;

	if (scratch && *scratch && !strpbrk(scratch, "\r\n"))
		fallback = scratch;
	else
		fallback = source->base.odb->repo->gitdir;
	if (!fallback || !*fallback || !source->store)
		return NULL;
	return xstrfmt("%s/vex-pack-cache/%s", fallback,
		       vex_store_repo_id(source->store));
}

static int vex_source_fetch_delta_cache_enabled(void)
{
	const char *value = getenv("GIT_VEX_FETCH_DELTA_CACHE");

	/* The cache is enabled whenever the configured scratch volume exists. */
	return !value || vex_env_true("GIT_VEX_FETCH_DELTA_CACHE");
}

static char *vex_fetch_delta_cache_root_for_repository(
		const struct repository *repo)
{
	const char *scratch = getenv("GIT_VEX_SCRATCH");
	char *repo_id;
	char *root;

	if (!repo || !scratch || !is_absolute_path(scratch) ||
	    strpbrk(scratch, "\r\n"))
		return NULL;
	repo_id = vex_canonical_repo_id(repo->ref_storage_payload);
	if (!repo_id)
		return NULL;
	root = xstrfmt("%s/vex-fetch-delta/%s", scratch, repo_id);
	free(repo_id);
	return root;
}

static char *vex_source_fetch_delta_cache_objects_path(
		const struct odb_source_vex *source)
{
	char *root;
	char *objects_path;

	if (!source || !source->base.odb ||
	    !vex_source_fetch_delta_cache_enabled())
		return NULL;
	root = vex_fetch_delta_cache_root_for_repository(source->base.odb->repo);
	if (!root)
		return NULL;
	objects_path = xstrfmt("%s/objects", root);
	free(root);
	return objects_path;
}

static int vex_source_refresh_fetch_delta_cache(struct odb_source_vex *source)
{
	struct odb_source_packed *cache = NULL;
	struct packfile_list_entry *entry;
	char *objects_path;

	objects_path = vex_source_fetch_delta_cache_objects_path(source);
	if (!objects_path || !is_directory(objects_path)) {
		free(objects_path);
		vex_source_release_fetch_delta_cache(source);
		return VEX_STORE_ERR_NOT_FOUND;
	}
	if (source->fetch_delta_cache && source->fetch_delta_cache_path &&
	    !strcmp(source->fetch_delta_cache_path, objects_path)) {
		free(objects_path);
		return VEX_STORE_OK;
	}
	cache = odb_source_packed_new(source->base.odb, objects_path, false);
	odb_source_prepare(&cache->base, ODB_PREPARE_FLUSH_CACHES);
	entry = packfile_store_get_packs(cache);
	if (!entry) {
		odb_source_close(&cache->base);
		odb_source_free(&cache->base);
		free(objects_path);
		return VEX_STORE_ERR_NOT_FOUND;
	}
	vex_source_release_fetch_delta_cache(source);
	source->fetch_delta_cache = cache;
	source->fetch_delta_cache_path = objects_path;
	trace_printf("vex: activated local fetch-delta cache repo=%s",
		     vex_store_repo_id(source->store));
	return VEX_STORE_OK;
}

static int vex_fetch_delta_cache_manifest_content(
		const struct git_hash_algo *hash_algo,
		const struct object_id *pack_id,
		const struct object_id *old_oid,
		const struct object_id *new_oid,
		char *buffer, size_t buffer_len)
{
	char pack_hex[GIT_MAX_HEXSZ + 1];
	char old_hex[GIT_MAX_HEXSZ + 1];
	char new_hex[GIT_MAX_HEXSZ + 1];
	int len;

	if (!hash_algo || !pack_id || !old_oid || !new_oid || !buffer ||
	    !buffer_len)
		return -1;
	hash_to_hex_algop_r(pack_hex, pack_id->hash, hash_algo);
	hash_to_hex_algop_r(old_hex, old_oid->hash, hash_algo);
	hash_to_hex_algop_r(new_hex, new_oid->hash, hash_algo);
	len = xsnprintf(buffer, buffer_len,
			"vex-fetch-delta-v1\n%s\n%s\n%s\n",
			old_hex, new_hex, pack_hex);
	return len < 0 || (size_t)len >= buffer_len ? -1 : len;
}

static int vex_fetch_delta_cache_manifest_matches(
		const struct odb_source_vex *source, const char *manifest_dir,
		const struct packed_git *pack,
		const struct object_id *old_oid, const struct object_id *new_oid)
{
	const struct git_hash_algo *hash_algo = source->base.odb->repo->hash_algo;
	struct object_id pack_id;
	struct strbuf manifest_path = STRBUF_INIT;
	struct stat st;
	char expected[(GIT_MAX_HEXSZ + 1) * 3 + 32];
	char actual[sizeof(expected)];
	char pack_hex[GIT_MAX_HEXSZ + 1];
	int expected_len;
	int fd = -1;
	int ret = 0;

	if (!manifest_dir || !pack || !old_oid || !new_oid)
		goto out;
	oidread(&pack_id, pack->hash, hash_algo);
	expected_len = vex_fetch_delta_cache_manifest_content(
		hash_algo, &pack_id, old_oid, new_oid,
		expected, sizeof(expected));
	if (expected_len < 0)
		goto out;
	hash_to_hex_algop_r(pack_hex, pack_id.hash, hash_algo);
	strbuf_addf(&manifest_path, "%s/pack-%s.vex-fetch-delta", manifest_dir,
			   pack_hex);
	fd = open_nofollow(manifest_path.buf, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
	    st.st_size != expected_len ||
	    read_in_full(fd, actual, expected_len) != expected_len)
		goto out;
	ret = !memcmp(actual, expected, expected_len);

out:
	if (fd >= 0)
		close(fd);
	strbuf_release(&manifest_path);
	return ret;
}

struct packed_git *odb_source_vex_fetch_delta_cache_pack(
		struct odb_source *base, const struct object_id *old_oid,
		const struct object_id *new_oid)
{
	struct odb_source_vex *source;
	struct packfile_list_entry *entry;
	char *root = NULL;
	char *manifest_dir = NULL;
	struct packed_git *result = NULL;

	if (!odb_source_is_vex(base) || !old_oid || !new_oid)
		return NULL;
	source = container_of(base, struct odb_source_vex, base);
	/* Object parsing opens the Vex source before upload-pack reaches here.
	 * Do not force a cold raw-base materialization merely to probe this
	 * optional local relay cache. */
	if (!source->store || vex_source_refresh_fetch_delta_cache(source))
		goto out;
	root = vex_fetch_delta_cache_root_for_repository(source->base.odb->repo);
	if (!root)
		goto out;
	manifest_dir = xstrfmt("%s/manifests", root);
	if (!is_directory(manifest_dir))
		goto out;
	for (entry = packfile_store_get_packs(source->fetch_delta_cache);
	     entry; entry = entry->next) {
		if (entry->pack &&
		    vex_fetch_delta_cache_manifest_matches(source, manifest_dir, entry->pack,
						   old_oid, new_oid) &&
		    find_pack_entry_one(new_oid, entry->pack)) {
			result = entry->pack;
			break;
		}
	}

out:
	free(manifest_dir);
	free(root);
	return result;
}

/*
 * A pack cache belongs to the local Vex scratch volume, but each smart-HTTP
 * request has a fresh Git process.  Rehashing a large immutable pack in every
 * process makes a warm cache no better than a download-sized read.  Persist a
 * small, atomically-published verification witness after the first full hash.
 * The witness is tied to the expected pack id and the local file identity, so
 * a removed, replaced, or resized cache entry is verified again.
 */
#define VEX_PACK_CACHE_VERIFICATION_MAX (GIT_MAX_HEXSZ + 256)

static int vex_source_pack_cache_verification_payload(
	struct odb_source_vex *source, const char *pack_path,
	const struct vex_pack_artifact *artifact, char *buffer, size_t buffer_len)
{
	const struct git_hash_algo *hash_algo = source->base.odb->repo->hash_algo;
	struct strbuf idx_path = STRBUF_INIT;
	struct stat pack_st;
	struct stat idx_st;
	char pack_hex[GIT_MAX_HEXSZ + 1];
	size_t pack_path_len;
	int pack_fd = -1;
	int idx_fd = -1;
	int len = -1;

	if (!pack_path || !artifact || !hash_algo || !buffer || !buffer_len)
		return -1;
	pack_path_len = strlen(pack_path);
	if (pack_path_len <= 5 ||
	    strcmp(pack_path + pack_path_len - 5, ".pack"))
		goto out;
	strbuf_add(&idx_path, pack_path, pack_path_len - 5);
	strbuf_addstr(&idx_path, ".idx");
	pack_fd = open_nofollow(pack_path, O_RDONLY);
	idx_fd = open_nofollow(idx_path.buf, O_RDONLY);
	if (pack_fd < 0 || idx_fd < 0 || fstat(pack_fd, &pack_st) ||
	    fstat(idx_fd, &idx_st) || !S_ISREG(pack_st.st_mode) ||
	    !S_ISREG(idx_st.st_mode))
		goto out;
	hash_to_hex_algop_r(pack_hex, artifact->pack_id.hash, hash_algo);
	len = xsnprintf(buffer, buffer_len,
			"vex-pack-cache-v2\n%s\n%"PRIuMAX" %"PRIuMAX" %"PRIuMAX
			" %"PRIdMAX" %"PRIdMAX"\n%"PRIuMAX" %"PRIuMAX" %"PRIuMAX
			" %"PRIdMAX" %"PRIdMAX"\n",
			pack_hex, (uintmax_t)pack_st.st_dev, (uintmax_t)pack_st.st_ino,
			(uintmax_t)pack_st.st_size, (intmax_t)pack_st.st_mtime,
			(intmax_t)pack_st.st_ctime, (uintmax_t)idx_st.st_dev,
			(uintmax_t)idx_st.st_ino, (uintmax_t)idx_st.st_size,
			(intmax_t)idx_st.st_mtime, (intmax_t)idx_st.st_ctime);
	if (len < 0 || (size_t)len >= buffer_len)
		len = -1;

out:
	if (idx_fd >= 0)
		close(idx_fd);
	if (pack_fd >= 0)
		close(pack_fd);
	strbuf_release(&idx_path);
	return len;
}

static int vex_source_pack_cache_is_verified(
	struct odb_source_vex *source, struct packed_git *pack,
	const struct vex_pack_artifact *artifact, const char *verification_path)
{
	char expected[VEX_PACK_CACHE_VERIFICATION_MAX];
	char actual[VEX_PACK_CACHE_VERIFICATION_MAX];
	struct stat st;
	int expected_len;
	int fd = -1;
	int ret = 0;

	if (!pack || !verification_path)
		return 0;
	expected_len = vex_source_pack_cache_verification_payload(
		source, pack->pack_name, artifact, expected, sizeof(expected));
	if (expected_len < 0)
		return 0;
	fd = open_nofollow(verification_path, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
	    st.st_size != expected_len ||
	    read_in_full(fd, actual, expected_len) != expected_len)
		goto out;
	ret = !memcmp(actual, expected, expected_len);

out:
	if (fd >= 0)
		close(fd);
	return ret;
}

static int vex_source_write_pack_cache_verification(
	struct odb_source_vex *source, struct packed_git *pack,
	const struct vex_pack_artifact *artifact, const char *verification_path)
{
	char content[VEX_PACK_CACHE_VERIFICATION_MAX];
	char *template = NULL;
	struct tempfile *temp = NULL;
	int content_len;
	int ret = -1;

	if (!pack || !verification_path)
		return -1;
	content_len = vex_source_pack_cache_verification_payload(
		source, pack->pack_name, artifact, content, sizeof(content));
	if (content_len < 0)
		goto out;
	template = xstrfmt("%s-XXXXXX", verification_path);
	temp = mks_tempfile_m(template, 0600);
	if (!temp ||
	    write_in_full(get_tempfile_fd(temp), content, content_len) != content_len ||
	    rename_tempfile(&temp, verification_path))
		goto out;
	ret = 0;

out:
	delete_tempfile(&temp);
	free(template);
	return ret;
}

static int vex_source_verify_pack_checksum(struct odb_source_vex *source,
					    struct packed_git *pack,
					    const struct vex_pack_artifact *artifact)
{
	const struct git_hash_algo *hash_algo = source->base.odb->repo->hash_algo;
	struct git_hash_ctx ctx;
	struct stat st;
	unsigned char buffer[65536];
	unsigned char computed[GIT_MAX_RAWSZ];
	unsigned char trailer[GIT_MAX_RAWSZ];
	off_t remaining;
	int fd = -1;
	int ret = -1;

	if (!pack || !artifact || !hash_algo)
		return -1;
	fd = open_nofollow(pack->pack_name, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
	    st.st_size < hash_algo->rawsz)
		goto out;
	remaining = st.st_size - hash_algo->rawsz;
	git_hash_init(&ctx, hash_algo);
	while (remaining) {
		size_t to_read = remaining > (off_t)sizeof(buffer) ?
			sizeof(buffer) : (size_t)remaining;
		ssize_t read_len = read_in_full(fd, buffer, to_read);

		if (read_len != (ssize_t)to_read)
			goto out;
		git_hash_update(&ctx, buffer, to_read);
		remaining -= to_read;
	}
	if (read_in_full(fd, trailer, hash_algo->rawsz) != hash_algo->rawsz)
		goto out;
	git_hash_final(computed, &ctx);
	if (hasheq(computed, artifact->pack_id.hash, hash_algo) &&
	    hasheq(trailer, artifact->pack_id.hash, hash_algo))
		ret = 0;

out:
	if (fd >= 0)
		close(fd);
	return ret;
}

static struct odb_source_packed *vex_source_open_verified_pack_cache(
		struct odb_source_vex *source, const char *objects_path,
		const struct vex_pack_artifact *artifact,
		const char *verification_path)
{
	struct odb_source_packed *cache;
	struct packfile_list_entry *entry;

	cache = odb_source_packed_new(source->base.odb, objects_path, false);
	odb_source_prepare(&cache->base, ODB_PREPARE_FLUSH_CACHES);
	entry = packfile_store_get_packs(cache);
	if (!entry || entry->next) {
		odb_source_close(&cache->base);
		odb_source_free(&cache->base);
		return NULL;
	}
	if (verification_path && vex_source_pack_cache_is_verified(
		    source, entry->pack, artifact, verification_path))
		return cache;
	if (verify_pack_index(entry->pack) ||
	    vex_source_verify_pack_checksum(source, entry->pack, artifact)) {
		odb_source_close(&cache->base);
		odb_source_free(&cache->base);
		return NULL;
	}
	if (verification_path && vex_source_write_pack_cache_verification(
		    source, entry->pack, artifact, verification_path))
		trace_printf("vex: could not persist verified pack cache witness %s",
			     verification_path);
	return cache;
}

static int vex_source_install_receive_pack_cache(
		struct odb_source_vex *source, const char *objects_path,
		const struct object_id *pack_id)
{
	struct vex_pack_artifact artifact = { 0 };
	struct odb_source_packed *cache;
	char pack_hex[GIT_MAX_HEXSZ + 1];

	if (!objects_path || !is_absolute_path(objects_path) || !pack_id)
		return -1;
	oidcpy(&artifact.pack_id, pack_id);
	cache = vex_source_open_verified_pack_cache(source, objects_path,
					      &artifact, NULL);
	if (!cache)
		return -1;
	vex_source_release_receive_pack_cache(source);
	source->receive_pack_cache = cache;
	hash_to_hex_algop_r(pack_hex, pack_id->hash,
			    source->base.odb->repo->hash_algo);
	source->receive_pack_id = xstrdup(pack_hex);
	source->receive_pack_path = xstrdup(objects_path);
	return 0;
}

static struct tempfile *vex_source_pack_cache_tempfile(const char *pack_dir,
						const char *component)
{
	char *template = xstrfmt("%s/.vex-pack-%s-XXXXXX", pack_dir, component);
	struct tempfile *temp = mks_tempfile_m(template, 0600);

	free(template);
	return temp;
}

/*
 * Materialize one immutable artifact without changing the Vex source's
 * selected complete snapshot.  Both the normal ODB cache and the narrowly
 * scoped default-clone relay use this installer, but own their packed source
 * and lease independently.
 */
static int vex_source_materialize_pack_cache(
	struct odb_source_vex *source, const struct vex_pack_artifact *artifact,
	const char *pack_id, struct odb_source_packed **out_cache,
	char **out_objects_path)
{
	struct lock_file lock = LOCK_INIT;
	struct tempfile *pack_temp = NULL;
	struct tempfile *idx_temp = NULL;
	struct odb_source_packed *cache = NULL;
	char *root = NULL;
	char *objects_path = NULL;
	char *pack_dir = NULL;
	char *pack_path = NULL;
	char *idx_path = NULL;
	char *verification_path = NULL;
	char *lock_path = NULL;
	int ret = -1;

	if (!source || !artifact || !pack_id || !out_cache || !out_objects_path)
		return -1;
	*out_cache = NULL;
	*out_objects_path = NULL;
	root = vex_source_pack_cache_root(source);
	if (!root)
		goto out;
	objects_path = xstrfmt("%s/%s/objects", root, pack_id);
	pack_dir = xstrfmt("%s/pack", objects_path);
	pack_path = xstrfmt("%s/pack-%s.pack", pack_dir, pack_id);
	idx_path = xstrfmt("%s/pack-%s.idx", pack_dir, pack_id);
	verification_path = xstrfmt("%s/pack-%s.vex-verified", pack_dir,
					pack_id);
	lock_path = xstrfmt("%s/.vex-pack-install-%s", pack_dir, pack_id);
	if (safe_create_leading_directories_const(source->base.odb->repo,
						  pack_path) < 0)
		goto out;
	cache = vex_source_open_verified_pack_cache(source, objects_path, artifact,
						      verification_path);
	/*
	 * The index is the pack discovery point. A reader can adopt an immutable,
	 * verified pair without contending on the installer lock: the installer
	 * publishes .pack before .idx, so it either sees the old complete pair or
	 * the new complete pair. This also keeps a stale lock left by an interrupted
	 * installer from delaying every request after a cache was already published.
	 */
	if (!cache &&
	    hold_lock_file_for_update_timeout(&lock, lock_path, LOCK_NO_DEREF,
					      30000) < 0)
		goto out;
	/* An installer may have completed while this process waited for its lock. */
	if (!cache)
		cache = vex_source_open_verified_pack_cache(source, objects_path,
							      artifact, verification_path);
	if (!cache) {
		unlink(pack_path);
		unlink(idx_path);
		unlink(verification_path);
		pack_temp = vex_source_pack_cache_tempfile(pack_dir, "pack");
		idx_temp = vex_source_pack_cache_tempfile(pack_dir, "idx");
		if (!pack_temp || !idx_temp || close_tempfile_gently(pack_temp) < 0 ||
		    close_tempfile_gently(idx_temp) < 0)
			goto out;
		if (vex_store_download_pack(source->store, artifact,
					    get_tempfile_path(pack_temp),
					    get_tempfile_path(idx_temp)))
			goto out;
		if (rename_tempfile(&pack_temp, pack_path) ||
		    rename_tempfile(&idx_temp, idx_path)) {
			unlink(pack_path);
			unlink(idx_path);
			goto out;
		}
		cache = vex_source_open_verified_pack_cache(source, objects_path,
							      artifact, verification_path);
		if (!cache) {
			unlink(pack_path);
			unlink(idx_path);
			goto out;
		}
	}
	*out_cache = cache;
	cache = NULL;
	*out_objects_path = objects_path;
	objects_path = NULL;
	ret = 0;

out:
	if (cache) {
		odb_source_close(&cache->base);
		odb_source_free(&cache->base);
	}
	delete_tempfile(&idx_temp);
	delete_tempfile(&pack_temp);
	rollback_lock_file(&lock);
	free(lock_path);
	free(idx_path);
	free(pack_path);
	free(pack_dir);
	free(verification_path);
	free(objects_path);
	free(root);
	return ret;
}

static int vex_source_install_pack_cache(struct odb_source_vex *source,
					 const struct vex_pack_artifact *artifact,
					 const char *pack_id)
{
	struct odb_source_packed *cache = NULL;
	char *objects_path = NULL;
	int ret;

	ret = vex_source_materialize_pack_cache(source, artifact, pack_id,
						 &cache, &objects_path);
	if (ret)
		return ret;
	/* Only replace the normal cache after its replacement is fully verified. */
	vex_source_release_pack_cache(source);
	source->pack_cache = cache;
	source->pack_cache_id = xstrdup(pack_id);
	source->pack_cache_path = objects_path;
	return 0;
}

static int vex_source_install_raw_base_cache(
		struct odb_source_vex *source,
		const struct vex_pack_artifact *artifact, const char *pack_id)
{
	struct odb_source_packed *cache = NULL;
	char *objects_path = NULL;
	int ret;

	ret = vex_source_materialize_pack_cache(source, artifact, pack_id,
						 &cache, &objects_path);
	if (ret)
		return ret;
	/* Only replace the base after its immutable pair is fully verified. */
	vex_source_release_raw_base_cache(source);
	source->raw_base_cache = cache;
	source->raw_base_cache_id = xstrdup(pack_id);
	source->raw_base_cache_path = objects_path;
	return 0;
}

static int vex_source_refresh_pack_cache(struct odb_source_vex *source)
{
	struct vex_pack_artifact artifact;
	struct vex_pack_lease new_lease = { 0 };
	char pack_id[GIT_MAX_HEXSZ + 1];
	int ret;

	ret = vex_store_current_pack(source->store, &artifact);
	if (ret == VEX_STORE_ERR_NOT_FOUND) {
		vex_source_release_pack_cache(source);
		return VEX_STORE_ERR_NOT_FOUND;
	}
	if (ret)
		return ret;
	hash_to_hex_algop_r(pack_id, artifact.pack_id.hash,
			    source->base.odb->repo->hash_algo);
	if (source->pack_cache && source->pack_cache_id &&
	    !strcmp(source->pack_cache_id, pack_id) && source->pack_lease.active)
		return 0;
	/*
	 * Acquire before installing so a sweep can never see a locally serving
	 * cache without a durable retention root.  install_pack_cache() releases
	 * the old cache/lease only after the replacement has verified.
	 */
	ret = vex_store_pack_lease_acquire(source->store, &artifact,
					 vex_source_pack_cache_lease_seconds(),
					 &new_lease);
	if (ret)
		return ret;
	if (source->pack_cache && source->pack_cache_id &&
	    !strcmp(source->pack_cache_id, pack_id)) {
		source->pack_lease = new_lease;
		return 0;
	}
	ret = vex_source_install_pack_cache(source, &artifact, pack_id);
	if (ret) {
		int release_ret = vex_store_pack_lease_release(source->store,
							       &new_lease);

		if (release_ret)
			trace_printf("vex: could not release failed derived pack lease repo=%s (%d)",
				     vex_store_repo_id(source->store), release_ret);
		return ret;
	}
	source->pack_lease = new_lease;
	if (!ret)
		trace_printf("vex: activated derived pack cache repo=%s pack=%s generation=%"PRIu64,
			     vex_store_repo_id(source->store), pack_id,
			     artifact.covering_generation);
	return ret;
}

static int vex_source_refresh_raw_base_cache(struct odb_source_vex *source)
{
	struct vex_pack_artifact artifact;
	struct vex_pack_lease new_lease = { 0 };
	char pack_id[GIT_MAX_HEXSZ + 1];
	int ret;

	ret = vex_store_base_receive_pack(source->store, &artifact);
	if (ret == VEX_STORE_ERR_NOT_FOUND) {
		vex_source_release_raw_base_cache(source);
		return ret;
	}
	if (ret)
		return ret;
	hash_to_hex_algop_r(pack_id, artifact.pack_id.hash,
			    source->base.odb->repo->hash_algo);
	if (source->raw_base_cache && source->raw_base_cache_id &&
	    !strcmp(source->raw_base_cache_id, pack_id) &&
	    source->raw_base_lease.active)
		return 0;
	ret = vex_store_pack_lease_acquire(source->store, &artifact,
					 vex_source_pack_cache_lease_seconds(),
					 &new_lease);
	if (ret)
		return ret;
	if (source->raw_base_cache && source->raw_base_cache_id &&
	    !strcmp(source->raw_base_cache_id, pack_id)) {
		source->raw_base_lease = new_lease;
		return 0;
	}
	ret = vex_source_install_raw_base_cache(source, &artifact, pack_id);
	if (ret) {
		int release_ret = vex_store_pack_lease_release(source->store,
							       &new_lease);

		if (release_ret)
			trace_printf("vex: could not release failed raw base lease repo=%s (%d)",
				     vex_store_repo_id(source->store), release_ret);
		return ret;
	}
	source->raw_base_lease = new_lease;
	trace_printf("vex: activated raw base pack cache repo=%s pack=%s generation=%"PRIu64,
		     vex_store_repo_id(source->store), pack_id,
		     artifact.covering_generation);
	return 0;
}

static int vex_source_install_inherited_receive_pack(
		struct odb_source_vex *source);

static int vex_source_open(struct odb_source_vex *source)
{
	int ret;

	if (source->store)
		return 0;
	ret = vex_store_open_for_repository(source->base.odb->repo,
					    source->base.odb->repo->gitdir,
					    &source->store);
	if (ret)
		return ret;
	/*
	 * The ref backend needs only the durable metadata namespace for an
	 * advertisement.  An actual ODB operation restores the complete store
	 * readiness boundary here: direct callers and writers check S3 plus stale
	 * epoch recovery, while a gateway read child may use its startup witness.
	 */
	ret = vex_repo_ensure(source->store);
	if (ret) {
		vex_store_close(source->store);
		source->store = NULL;
		return ret;
	}
	if (vex_source_install_inherited_receive_pack(source)) {
		vex_store_close(source->store);
		source->store = NULL;
		return error(_("unable to activate inherited Vex receive pack"));
	}
	/*
	 * This cache is not part of the Vex durability contract. A missing or
	 * malformed scratch entry simply leaves the canonical MySQL/S3 read path
	 * in charge of the request.
	 */
	ret = vex_source_refresh_fetch_delta_cache(source);
	if (ret && ret != VEX_STORE_ERR_NOT_FOUND)
		trace_printf("vex: fetch-delta cache refresh unavailable repo=%s (%d)",
			     vex_store_repo_id(source->store), ret);
	/*
	 * Derived relay caches are optional, but a retained raw receive pack is a
	 * canonical ODB base after an incremental overlay.  Keep that path live
	 * even when a test disables the performance cache.
	 */
	if (!vex_source_derived_pack_cache_enabled()) {
		ret = vex_source_refresh_raw_base_cache(source);
		if (ret && ret != VEX_STORE_ERR_NOT_FOUND) {
			vex_store_close(source->store);
			source->store = NULL;
			return error(_("unable to activate Vex raw base pack"));
		}
		return 0;
	}
	ret = vex_source_refresh_pack_cache(source);
	if (ret == VEX_STORE_ERR_NOT_FOUND) {
		ret = vex_source_refresh_raw_base_cache(source);
		if (ret && ret != VEX_STORE_ERR_NOT_FOUND) {
			vex_store_close(source->store);
			source->store = NULL;
			return error(_("unable to activate Vex raw base pack"));
		}
	} else if (ret) {
		trace_printf("vex: derived pack cache unavailable repo=%s (%d)",
			     vex_store_repo_id(source->store), ret);
	}
	return 0;
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

/*
 * Hooks and connectivity helpers run as fresh Git processes.  They inherit a
 * pack id, not a caller-controlled path: the only accepted local location is
 * the scratch path deterministically derived from the Vex repository and the
 * already-authenticated receive epoch.
 */
static int vex_source_install_inherited_receive_pack(
		struct odb_source_vex *source)
{
	const char *value = getenv("GIT_VEX_RECEIVE_PACK");
	const char *end;
	struct object_id pack_id;
	char *objects_path = NULL;
	uint64_t epoch_id;
	int ret;

	if (!value || !*value)
		return 0;
	if (source->receive_pack_cache)
		return 0;
	ret = vex_source_parse_inherited_epoch(source, &epoch_id);
	if (ret != 1)
		return -1;
	if (parse_oid_hex_algop(value, &pack_id, &end,
				source->base.odb->repo->hash_algo) || *end)
		return -1;
	objects_path = vex_source_receive_pack_objects_path(source, epoch_id);
	if (!objects_path || vex_source_install_receive_pack_cache(source,
						       objects_path, &pack_id)) {
		free(objects_path);
		return -1;
	}
	free(objects_path);
	return 0;
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

	/*
	 * A receive transaction remains owned by the parent until receive-pack
	 * returns, but its ref backend may already have sealed the epoch.  Reads
	 * after that handoff (for example, the best-effort delta-cache builder)
	 * must use canonical visibility rather than trying to refresh or read an
	 * inactive epoch.
	 */
	if (transaction && !transaction->handoff_finished) {
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
					     enum object_info_flags flags)
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
	if ((flags & OBJECT_INFO_SECOND_READ) &&
	    vex_source_derived_pack_cache_enabled()) {
		ret = vex_source_refresh_pack_cache(source);
		if (ret == VEX_STORE_ERR_NOT_FOUND)
			ret = vex_source_refresh_raw_base_cache(source);
		if (ret && ret != VEX_STORE_ERR_NOT_FOUND)
			trace_printf("vex: derived pack cache refresh unavailable repo=%s (%d)",
				     vex_store_repo_id(source->store), ret);
	}
	if (source->receive_pack_cache &&
	    !odb_source_read_object_info(&source->receive_pack_cache->base, oid,
					 oi, flags))
		return 0;
	if (source->fetch_delta_cache &&
	    !odb_source_read_object_info(&source->fetch_delta_cache->base, oid,
					 oi, flags))
		return 0;
	if (source->pack_cache &&
	    !odb_source_read_object_info(&source->pack_cache->base, oid, oi, flags))
		return 0;
	if (source->raw_base_cache &&
	    !odb_source_read_object_info(&source->raw_base_cache->base, oid, oi,
					 flags))
		return 0;

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

	ret = vex_object_index_get_visible(source->store, epoch, oid, &info);
	if (ret)
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
	if (source->receive_pack_cache &&
	    !odb_source_read_object_stream(out, &source->receive_pack_cache->base,
					   oid))
		return 0;
	if (source->fetch_delta_cache &&
	    !odb_source_read_object_stream(out, &source->fetch_delta_cache->base,
					   oid))
		return 0;
	if (source->pack_cache &&
	    !odb_source_read_object_stream(out, &source->pack_cache->base, oid))
		return 0;
	if (source->raw_base_cache &&
	    !odb_source_read_object_stream(out, &source->raw_base_cache->base, oid))
		return 0;
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
	if (source->receive_pack_cache) {
		ret = odb_source_for_each_object(&source->receive_pack_cache->base,
						 request, cb, cb_data, opts);
		if (ret)
			return ret;
	}
	if (source->fetch_delta_cache) {
		ret = odb_source_for_each_object(&source->fetch_delta_cache->base,
						 request, cb, cb_data, opts);
		if (ret)
			return ret;
	}
	if (source->pack_cache) {
		ret = odb_source_for_each_object(&source->pack_cache->base, request,
						 cb, cb_data, opts);
		if (ret)
			return ret;
	}
	if (source->raw_base_cache) {
		ret = odb_source_for_each_object(&source->raw_base_cache->base,
						 request, cb, cb_data, opts);
		if (ret)
			return ret;
	}
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
					  enum odb_count_objects_flags flags,
					  unsigned long *out)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct vex_epoch *epoch;
	unsigned long count = 0;
	unsigned long source_count;
	int ret;

	if (vex_source_open(source))
		return -1;
	if (source->receive_pack_cache) {
		ret = odb_source_count_objects(&source->receive_pack_cache->base,
						       flags, &source_count);
		if (ret < 0)
			return ret;
		count += source_count;
	}
	if (source->fetch_delta_cache) {
		ret = odb_source_count_objects(&source->fetch_delta_cache->base,
						       flags, &source_count);
		if (ret < 0)
			return ret;
		count += source_count;
	}
	if (source->pack_cache) {
		ret = odb_source_count_objects(&source->pack_cache->base, flags,
						       &source_count);
		if (ret < 0)
			return ret;
		count += source_count;
	}
	if (source->raw_base_cache) {
		ret = odb_source_count_objects(&source->raw_base_cache->base, flags,
						       &source_count);
		if (ret < 0)
			return ret;
		count += source_count;
	}
	if (vex_source_active_epoch(source, &epoch))
		return -1;
	ret = vex_store_count_objects(source->store, epoch, &source_count);
	if (ret)
		return error(_("unable to count Vex objects"));
	*out = count + source_count;
	return 0;
}

static int odb_source_vex_find_abbrev_len(struct odb_source *base,
					     const struct object_id *oid,
					     unsigned min_length,
					     unsigned *out)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct vex_epoch *epoch;
	unsigned len = min_length;
	int ret;

	if (vex_source_open(source))
		return -1;
	if (source->receive_pack_cache) {
		ret = odb_source_find_abbrev_len(&source->receive_pack_cache->base,
						      oid, len, &len);
		if (ret < 0)
			return ret;
	}
	if (source->fetch_delta_cache) {
		ret = odb_source_find_abbrev_len(&source->fetch_delta_cache->base,
						      oid, len, &len);
		if (ret < 0)
			return ret;
	}
	if (source->pack_cache) {
		ret = odb_source_find_abbrev_len(&source->pack_cache->base, oid,
						      len, &len);
		if (ret < 0)
			return ret;
	}
	if (source->raw_base_cache) {
		ret = odb_source_find_abbrev_len(&source->raw_base_cache->base, oid,
						      len, &len);
		if (ret < 0)
			return ret;
	}
	if (vex_source_active_epoch(source, &epoch))
		return -1;
	ret = vex_store_find_abbrev_len(source->store, epoch, oid, len, &len);
	if (ret)
		return error(_("unable to find Vex object abbreviation"));
	*out = len;
	return 0;
}

static int odb_source_vex_freshen_object(struct odb_source *base,
					    const struct object_id *oid,
					    const time_t *mtime)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	struct vex_epoch *epoch;
	struct vex_object_info info;

	if (vex_source_open(source))
		return 0;
	if (source->receive_pack_cache &&
	    odb_source_freshen_object(&source->receive_pack_cache->base, oid, mtime))
		return 1;
	if (source->fetch_delta_cache &&
	    odb_source_freshen_object(&source->fetch_delta_cache->base, oid, mtime))
		return 1;
	if (source->pack_cache &&
	    odb_source_freshen_object(&source->pack_cache->base, oid, mtime))
		return 1;
	if (source->raw_base_cache &&
	    odb_source_freshen_object(&source->raw_base_cache->base, oid, mtime))
		return 1;
	if (vex_source_active_epoch(source, &epoch))
		return 0;
	if (epoch && !vex_object_index_get_visible(source->store, epoch, oid,
						      &info))
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

static void vex_transaction_release_receive_pack_wal(
		struct odb_source_vex *source,
		struct odb_transaction_vex *transaction)
{
	char *expected = NULL;
	struct strbuf path = STRBUF_INIT;

	if (!transaction->receive_pack_objects_path)
		return;
	if (source && transaction->epoch)
		expected = vex_source_receive_pack_objects_path(source,
							     vex_epoch_id(transaction->epoch));
	if (expected && !strcmp(expected, transaction->receive_pack_objects_path)) {
		/* No child can use this receive cache after the parent transaction ends. */
		if (source->receive_pack_path &&
		    !strcmp(expected, source->receive_pack_path))
			vex_source_release_receive_pack_cache(source);
		strbuf_addstr(&path, expected);
		if (remove_dir_recursively(&path, 0))
			trace_printf("vex: could not remove completed receive pack WAL %s",
				     expected);
	}
	strbuf_release(&path);
	free(expected);
	free(transaction->receive_pack_objects_path);
	transaction->receive_pack_objects_path = NULL;
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
		/*
		 * receive-pack's unpack-objects child owns the final local batch
		 * flush.  Its successful exit is the handoff proof the parent needs
		 * before connectivity checks, hooks, and ref publication run.
		 */
		ret = vex_store_flush_staged(source->store, transaction->epoch);
		if (ret)
			error(_("unable to flush Vex receive object batch"));
		vex_transaction_release_receive_pack_wal(source, transaction);
		vex_epoch_detach(transaction->epoch);
		transaction->epoch = NULL;
		return ret;
	}
	if (transaction->receive_handoff) {
		if (!transaction->handoff_finished) {
			ret = vex_epoch_compensate(transaction->epoch);
			if (ret)
				error(_("unable to compensate unfinished Vex receive epoch"));
		} else {
			ret = 0;
		}
		vex_transaction_release_receive_pack_wal(source, transaction);
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
	vex_transaction_release_receive_pack_wal(source, transaction);
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
	if (source->receive_pack_id)
		strvec_pushf(env, "GIT_VEX_RECEIVE_PACK=%s",
			     source->receive_pack_id);
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

bool odb_vex_receive_pack_wal_enabled(
		const struct odb_transaction *base)
{
	return odb_vex_receive_transaction_is_active(base) &&
		vex_env_true("GIT_VEX_RECEIVE_PACK_WAL");
}

int odb_vex_receive_transaction_prepare_pack_wal(
		struct odb_transaction *base, struct strbuf *objects_dir)
{
	struct odb_transaction_vex *transaction = vex_receive_transaction(base);
	struct odb_source_vex *source;
	char *objects_path = NULL;
	char *pack_dir = NULL;
	char *pack_probe = NULL;
	struct stat st;
	int ret = -1;

	if (!objects_dir || !odb_vex_receive_pack_wal_enabled(base) ||
	    !transaction || transaction->receive_pack_ready)
		return -1;
	source = container_of(base->source, struct odb_source_vex, base);
	objects_path = vex_source_receive_pack_objects_path(source,
						     vex_epoch_id(transaction->epoch));
	if (!objects_path)
		goto out;
	/* Creating a child makes safe_create_leading_directories create `pack`. */
	pack_dir = xstrfmt("%s/pack", objects_path);
	pack_probe = xstrfmt("%s/.vex-receive-pack", pack_dir);
	if (safe_create_leading_directories_const(source->base.odb->repo,
					    pack_probe) != SCLD_OK)
		goto out;
	if (lstat(pack_dir, &st) || !S_ISDIR(st.st_mode))
		goto out;
	strbuf_reset(objects_dir);
	strbuf_addstr(objects_dir, objects_path);
	ret = 0;
out:
	free(pack_probe);
	free(pack_dir);
	free(objects_path);
	return ret;
}

int odb_vex_receive_transaction_adopt_pack(
		struct odb_transaction *base, const char *objects_dir,
		const struct object_id *pack_id)
{
	struct odb_transaction_vex *transaction = vex_receive_transaction(base);
	struct odb_source_vex *source;
	char *expected = NULL;
	int ret = -1;

	if (!objects_dir || !pack_id || !odb_vex_receive_pack_wal_enabled(base) ||
	    !transaction || transaction->receive_pack_ready)
		return -1;
	source = container_of(base->source, struct odb_source_vex, base);
	expected = vex_source_receive_pack_objects_path(source,
						    vex_epoch_id(transaction->epoch));
	if (!expected || strcmp(expected, objects_dir) ||
	    vex_source_install_receive_pack_cache(source, expected, pack_id)) {
		vex_source_release_receive_pack_cache(source);
		goto out;
	}
	/* Retain the derived path even if S3/metadata staging fails, so the
	 * terminal receive cleanup can reclaim this ephemeral index-pack WAL. */
	transaction->receive_pack_objects_path = xstrdup(expected);
	if (vex_store_stage_receive_pack(source->store, transaction->epoch,
					 pack_id, expected)) {
		vex_source_release_receive_pack_cache(source);
		goto out;
	}
	oidcpy(&transaction->receive_pack_id, pack_id);
	transaction->receive_pack_ready = 1;
	ret = 0;
out:
	free(expected);
	return ret;
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
	if (transaction->receive_pack_ready &&
	    vex_epoch_adopt_receive_pack(*epoch, &transaction->receive_pack_id)) {
		vex_epoch_detach(*epoch);
		*epoch = NULL;
		return -1;
	}
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

	/* Defensive fallback for a helper that wrote through an inherited epoch
	 * without opening its normal ODB transaction. */
	if (source->inherited_epoch &&
	    vex_store_flush_staged(source->store, source->inherited_epoch))
		die(_("unable to flush Vex receive object batch during source close"));
	vex_epoch_detach(source->inherited_epoch);
	source->inherited_epoch = NULL;
	vex_source_release_receive_pack_cache(source);
	vex_source_release_fetch_delta_cache(source);
	vex_source_release_pack_cache(source);
	vex_source_release_raw_base_cache(source);
	if (source->store)
		vex_store_close(source->store);
	source->store = NULL;
}

static void odb_source_vex_prepare(struct odb_source *base,
					     enum odb_prepare_flags flags)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);
	int ret;

	if (vex_source_open(source))
		return;
	if ((flags & ODB_PREPARE_FLUSH_CACHES) &&
	    vex_source_derived_pack_cache_enabled()) {
		ret = vex_source_refresh_pack_cache(source);
		if (ret == VEX_STORE_ERR_NOT_FOUND)
			ret = vex_source_refresh_raw_base_cache(source);
		if (ret && ret != VEX_STORE_ERR_NOT_FOUND)
			trace_printf("vex: derived pack cache refresh unavailable repo=%s (%d)",
				     vex_store_repo_id(source->store), ret);
	}
	if (source->receive_pack_cache)
		odb_source_prepare(&source->receive_pack_cache->base, flags);
	if (source->fetch_delta_cache)
		odb_source_prepare(&source->fetch_delta_cache->base, flags);
	if (source->pack_cache)
		odb_source_prepare(&source->pack_cache->base, flags);
	if (source->raw_base_cache)
		odb_source_prepare(&source->raw_base_cache->base, flags);
}

static void odb_source_vex_free(struct odb_source *base)
{
	struct odb_source_vex *source = container_of(base, struct odb_source_vex, base);

	odb_source_vex_close(base);
	odb_source_release(base);
	free(source);
}

struct odb_source *odb_source_vex_new(struct object_database *odb,
				       const char *path, bool local)
{
	struct odb_source_vex *source = xcalloc(1, sizeof(*source));

	odb_source_init(&source->base, odb, ODB_SOURCE_VEX, path, local);
	/*
	 * Ref advertisement does not read objects.  Defer the S3/cache setup until
	 * an ODB operation actually needs it so protocol-v2 ls-refs does not pay
	 * for a 596 MiB raw-base cache activation on every RPC.
	 */

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

struct odb_source_packed *odb_source_vex_packed_store(struct odb_source *base)
{
	struct odb_source_vex *source;

	if (!odb_source_is_vex(base))
		return NULL;
	source = container_of(base, struct odb_source_vex, base);
	if (vex_source_open(source))
		return NULL;
	return source->receive_pack_cache ? source->receive_pack_cache :
		source->pack_cache ? source->pack_cache : source->raw_base_cache;
}

struct odb_source_packed *odb_source_vex_precomputed_relay_store(
	struct odb_source *base)
{
	struct odb_source_vex *source;

	if (!odb_source_is_vex(base))
		return NULL;
	source = container_of(base, struct odb_source_vex, base);
	if (vex_source_open(source))
		return NULL;
	/* A raw base is a lookup layer, never a whole-pack relay after overlays. */
	return source->pack_cache;
}

int odb_source_vex_open_default_clone_pack(
	struct odb_source *base, struct odb_vex_default_clone_pack **out)
{
	struct odb_source_vex *source;
	struct vex_pack_artifact artifact = { 0 };
	struct vex_pack_lease lease = { 0 };
	struct odb_source_packed *cache = NULL;
	struct odb_vex_default_clone_pack *handle;
	char *cache_path = NULL;
	char pack_id[GIT_MAX_HEXSZ + 1];
	int ret;

	if (!out)
		return VEX_STORE_ERR_INVALID;
	*out = NULL;
	if (!odb_source_is_vex(base))
		return VEX_STORE_ERR_INVALID;
	source = container_of(base, struct odb_source_vex, base);
	if ((ret = vex_source_open(source)))
		return ret;
	/* A closed source cannot safely own a request-scoped lease or cache. */
	if (!source->store)
		return VEX_STORE_ERR_STATE;
	ret = vex_store_current_default_clone_pack(source->store, &artifact);
	if (ret)
		return ret;
	hash_to_hex_algop_r(pack_id, artifact.pack_id.hash,
			    source->base.odb->repo->hash_algo);
	/*
	 * Acquire before downloading so sweep never sees the local relay source
	 * without a durable root.  This lease belongs solely to `handle`; it does
	 * not refresh, replace, or otherwise affect source->pack_cache.
	 */
	ret = vex_store_pack_lease_acquire(source->store, &artifact,
					 vex_source_pack_cache_lease_seconds(), &lease);
	if (ret)
		return ret;
	ret = vex_source_materialize_pack_cache(source, &artifact, pack_id,
						 &cache, &cache_path);
	if (ret) {
		vex_source_release_cached_pack(source, &cache, NULL, &cache_path,
					       &lease);
		return ret;
	}
	handle = xcalloc(1, sizeof(*handle));
	handle->source = source;
	handle->packed = cache;
	handle->pack_id = xstrdup(pack_id);
	handle->cache_path = cache_path;
	handle->lease = lease;
	*out = handle;
	trace_printf("vex: activated default-clone pack cache repo=%s pack=%s generation=%"PRIu64,
		     vex_store_repo_id(source->store), pack_id,
		     artifact.covering_generation);
	return VEX_STORE_OK;
}

struct odb_source_packed *odb_source_vex_default_clone_pack_packed(
	const struct odb_vex_default_clone_pack *pack)
{
	return pack ? pack->packed : NULL;
}

void odb_source_vex_close_default_clone_pack(
	struct odb_vex_default_clone_pack *pack)
{
	if (!pack)
		return;
	vex_source_release_cached_pack(pack->source, &pack->packed,
				       &pack->pack_id, &pack->cache_path, &pack->lease);
	free(pack);
}

static int vex_fetch_delta_cache_count_packs(const char *pack_dir)
{
	struct dirent *entry;
	DIR *dir;
	int count = 0;

	dir = opendir(pack_dir);
	if (!dir)
		return 0;
	while ((entry = readdir(dir))) {
		const char *name = entry->d_name;

		if (starts_with(name, "pack-") && ends_with(name, ".idx"))
			count++;
	}
	closedir(dir);
	return count;
}

static void vex_fetch_delta_cache_prune_orphans(const char *pack_dir)
{
	struct dirent *entry;
	DIR *dir;

	dir = opendir(pack_dir);
	if (!dir)
		return;
	while ((entry = readdir(dir))) {
		const char *name = entry->d_name;
		char *path;

		/* These names are owned only by this cache's interrupted builders. */
		if (!starts_with(name, ".vex-fetch-delta-pack-") &&
		    !starts_with(name, "tmp_pack_"))
			continue;
		path = xstrfmt("%s/%s", pack_dir, name);
		unlink(path);
		free(path);
	}
	closedir(dir);
}

static int vex_fetch_delta_cache_remove_oldest_pack(const char *pack_dir,
		const char *manifest_dir)
{
	struct dirent *entry;
	struct stat oldest_st = { 0 };
	DIR *dir;
	char *oldest = NULL;

	dir = opendir(pack_dir);
	if (!dir)
		return -1;
	while ((entry = readdir(dir))) {
		const char *name = entry->d_name;
		struct stat st;
		char *path;

		if (!starts_with(name, "pack-") || !ends_with(name, ".idx"))
			continue;
		path = xstrfmt("%s/%s", pack_dir, name);
		if (lstat(path, &st) || !S_ISREG(st.st_mode)) {
			free(path);
			continue;
		}
		if (!oldest || st.st_mtime < oldest_st.st_mtime ||
		    (st.st_mtime == oldest_st.st_mtime && strcmp(path, oldest) < 0)) {
			free(oldest);
			oldest = path;
			oldest_st = st;
		} else {
			free(path);
		}
	}
	closedir(dir);
	if (!oldest)
		return -1;
	oldest[strlen(oldest) - strlen(".idx")] = '\0';
	{
		const char *pack_basename = strrchr(oldest, '/');
		char *idx_path = xstrfmt("%s.idx", oldest);
		char *pack_path = xstrfmt("%s.pack", oldest);
		char *rev_path = xstrfmt("%s.rev", oldest);
		char *bitmap_path = xstrfmt("%s.bitmap", oldest);
		char *manifest_path;
		int unlink_failed = 0;

		pack_basename = pack_basename ? pack_basename + 1 : oldest;
		manifest_path = xstrfmt("%s/%s.vex-fetch-delta", manifest_dir,
					pack_basename);

		/* Stop direct relay selection before withdrawing generic ODB discovery. */
		if (unlink(manifest_path) && errno != ENOENT)
			unlink_failed = 1;
		/* Stop new packed-source discovery; existing open sources stay valid. */
		if (unlink(idx_path) && errno != ENOENT)
			unlink_failed = 1;
		unlink(pack_path);
		unlink(rev_path);
		unlink(bitmap_path);
		free(manifest_path);
		free(bitmap_path);
		free(rev_path);
		free(pack_path);
		free(idx_path);
		if (unlink_failed) {
			free(oldest);
			return -1;
		}
	}
	free(oldest);
	return 0;
}

static int vex_fetch_delta_cache_make_room(const char *pack_dir,
		const char *manifest_dir)
{
	int count;

	vex_fetch_delta_cache_prune_orphans(pack_dir);
	count = vex_fetch_delta_cache_count_packs(pack_dir);
	while (count >= VEX_FETCH_DELTA_CACHE_MAX_PACKS) {
		if (vex_fetch_delta_cache_remove_oldest_pack(pack_dir, manifest_dir))
			return -1;
		count--;
	}
	return 0;
}

static int vex_fetch_delta_cache_read_index_pack_report(
		int fd, const struct git_hash_algo *hash_algo,
		struct object_id *pack_id)
{
	char report[GIT_MAX_HEXSZ + 6];
	const char *oid_hex;
	size_t len;

	if (!hash_algo || !pack_id)
		return -1;
	len = hash_algo->hexsz + 6;
	if (read_in_full(fd, report, len) != len || report[len - 1] != '\n')
		return -1;
	report[len - 1] = '\0';
	if (!skip_prefix(report, "pack\t", &oid_hex) &&
	    !skip_prefix(report, "keep\t", &oid_hex))
		return -1;
	return strlen(oid_hex) != hash_algo->hexsz ||
		get_oid_hex_algop(oid_hex, pack_id, hash_algo) ? -1 : 0;
}

static int vex_fetch_delta_cache_write_manifest(const char *manifest_dir,
		const struct git_hash_algo *hash_algo,
		const struct object_id *pack_id,
		const struct object_id *old_oid,
		const struct object_id *new_oid)
{
	struct strbuf manifest_path = STRBUF_INIT;
	char content[(GIT_MAX_HEXSZ + 1) * 3 + 32];
	char pack_hex[GIT_MAX_HEXSZ + 1];
	char *template = NULL;
	struct tempfile *temp = NULL;
	int content_len;
	int ret = -1;

	if (!manifest_dir || !hash_algo)
		goto out;
	content_len = vex_fetch_delta_cache_manifest_content(
		hash_algo, pack_id, old_oid, new_oid, content, sizeof(content));
	if (content_len < 0)
		goto out;
	hash_to_hex_algop_r(pack_hex, pack_id->hash, hash_algo);
	strbuf_addf(&manifest_path, "%s/pack-%s.vex-fetch-delta", manifest_dir,
			   pack_hex);
	template = xstrfmt("%s/.vex-fetch-delta-manifest-XXXXXX", manifest_dir);
	temp = mks_tempfile_m(template, 0600);
	if (!temp ||
	    write_in_full(get_tempfile_fd(temp), content, content_len) != content_len ||
	    rename_tempfile(&temp, manifest_path.buf))
		goto out;
	ret = 0;

out:
	delete_tempfile(&temp);
	free(template);
	strbuf_release(&manifest_path);
	return ret;
}

static int vex_fetch_delta_cache_is_fast_forward(struct repository *repo,
		const struct object_id *old_oid, const struct object_id *new_oid)
{
	struct commit *old_commit;
	struct commit *new_commit;

	old_commit = lookup_commit_reference_gently(repo, old_oid, 1);
	new_commit = lookup_commit_reference_gently(repo, new_oid, 1);
	if (!old_commit || !new_commit)
		return 0;
	return repo_in_merge_bases(repo, old_commit, new_commit) > 0;
}

/*
 * Materialize a small, thick incremental pack under the shared scratch
 * volume. This is a performance-only after-commit action: all failures leave
 * the already-sealed Vex ref transaction untouched and readers fall back to
 * the canonical MySQL/S3 object source.
 */
static int vex_fetch_delta_cache_build(struct repository *repo,
		const struct object_id *old_oid, const struct object_id *new_oid)
{
	struct child_process pack_objects = CHILD_PROCESS_INIT;
	struct child_process index_pack = CHILD_PROCESS_INIT;
	struct lock_file lock = LOCK_INIT;
	struct tempfile *pack_temp = NULL;
	struct object_id pack_id;
	struct stat st;
	char *root = NULL;
	char *objects_path = NULL;
	char *pack_dir = NULL;
	char *manifest_dir = NULL;
	char *probe = NULL;
	char *pack_probe = NULL;
	char *template = NULL;
	char *lock_path = NULL;
	uintmax_t pack_bytes = 0;
	char buffer[8192];
	int pack_started = 0;
	int pack_too_large = 0;
	int pack_io_failed = 0;
	int pack_fd = -1;
	int index_pack_started = 0;
	int index_pack_report_ok = 0;
	int ret = -1;

	root = vex_fetch_delta_cache_root_for_repository(repo);
	if (!root)
		goto out;
	objects_path = xstrfmt("%s/objects", root);
	pack_dir = xstrfmt("%s/pack", objects_path);
	manifest_dir = xstrfmt("%s/manifests", root);
	pack_probe = xstrfmt("%s/.vex-fetch-delta-probe", pack_dir);
	probe = xstrfmt("%s/.vex-fetch-delta-probe", manifest_dir);
	if (safe_create_leading_directories_const(repo, pack_probe) != SCLD_OK ||
	    safe_create_leading_directories_const(repo, probe) != SCLD_OK ||
	    !is_directory(pack_dir) || !is_directory(manifest_dir))
		goto out;
	lock_path = xstrfmt("%s/.vex-fetch-delta-install", root);
	if (hold_lock_file_for_update_timeout(&lock, lock_path, LOCK_NO_DEREF,
					      30000) < 0 ||
	    vex_fetch_delta_cache_make_room(pack_dir, manifest_dir))
		goto out;
	template = xstrfmt("%s/.vex-fetch-delta-pack-XXXXXX", pack_dir);
	pack_temp = mks_tempfile_m(template, 0600);
	if (!pack_temp)
		goto out;

	strvec_pushl(&pack_objects.args, "pack-objects", "--revs", "--stdout",
			     "--delta-base-offset", "--no-reuse-delta", "--include-tag",
			     NULL);
	strvec_pushf(&pack_objects.env, "GIT_DIR=%s", repo->gitdir);
	pack_objects.in = -1;
	pack_objects.out = -1;
	/* A bounded-cache overflow is expected and must not taint push output. */
	pack_objects.no_stderr = 1;
	pack_objects.git_cmd = 1;
	if (start_command(&pack_objects))
		goto out;
	pack_started = 1;
	if (write_in_full(pack_objects.in, oid_to_hex(new_oid),
			  repo->hash_algo->hexsz) < 0 ||
	    write_in_full(pack_objects.in, "\n^", 2) < 0 ||
	    write_in_full(pack_objects.in, oid_to_hex(old_oid),
			  repo->hash_algo->hexsz) < 0 ||
	    write_in_full(pack_objects.in, "\n", 1) < 0) {
		close(pack_objects.in);
		pack_objects.in = -1;
		close(pack_objects.out);
		pack_objects.out = -1;
		(void)finish_command(&pack_objects);
		pack_started = 0;
		goto out;
	}
	close(pack_objects.in);
	pack_objects.in = -1;
	for (;;) {
		ssize_t bytes = xread(pack_objects.out, buffer, sizeof(buffer));

		if (!bytes)
			break;
		if (bytes < 0) {
			pack_io_failed = 1;
			break;
		}
		if ((uintmax_t)bytes >
			VEX_FETCH_DELTA_CACHE_MAX_PACK_BYTES - pack_bytes) {
			pack_too_large = 1;
			break;
		}
		if (write_in_full(get_tempfile_fd(pack_temp), buffer, bytes) != bytes) {
			pack_io_failed = 1;
			break;
		}
		pack_bytes += bytes;
	}
	close(pack_objects.out);
	pack_objects.out = -1;
	if (finish_command(&pack_objects)) {
		pack_started = 0;
		goto out;
	}
	pack_started = 0;
	if (pack_too_large || pack_io_failed || !pack_bytes ||
	    fstat(get_tempfile_fd(pack_temp), &st) || st.st_size <= 0 ||
	    (uintmax_t)st.st_size != pack_bytes)
		goto out;
	if (close_tempfile_gently(pack_temp) < 0)
		goto out;
	pack_fd = open_nofollow(get_tempfile_path(pack_temp), O_RDONLY);
	if (pack_fd < 0)
		goto out;
	strvec_pushl(&index_pack.args, "index-pack", "--stdin", NULL);
	strvec_pushf(&index_pack.env, "GIT_DIR=%s", repo->gitdir);
	strvec_pushf(&index_pack.env, "GIT_OBJECT_DIRECTORY=%s", objects_path);
	index_pack.in = pack_fd;
	index_pack.out = -1;
	index_pack.no_stderr = 1;
	index_pack.git_cmd = 1;
	if (start_command(&index_pack))
		goto out;
	pack_fd = -1;
	index_pack_started = 1;
	index_pack_report_ok = !vex_fetch_delta_cache_read_index_pack_report(
		index_pack.out, repo->hash_algo, &pack_id);
	close(index_pack.out);
	index_pack.out = -1;
	if (finish_command(&index_pack)) {
		index_pack_started = 0;
		goto out;
	}
	index_pack_started = 0;
	if (!index_pack_report_ok || vex_fetch_delta_cache_write_manifest(
			manifest_dir, repo->hash_algo, &pack_id, old_oid, new_oid))
		goto out;
	trace_printf("vex: published local fetch-delta cache old=%s new=%s bytes=%"PRIuMAX,
		     oid_to_hex(old_oid), oid_to_hex(new_oid), (uintmax_t)st.st_size);
	ret = 0;

out:
	if (pack_started) {
		if (pack_objects.in >= 0)
			close(pack_objects.in);
		if (pack_objects.out >= 0)
			close(pack_objects.out);
		(void)finish_command(&pack_objects);
	}
	if (index_pack_started) {
		if (index_pack.out >= 0)
			close(index_pack.out);
		(void)finish_command(&index_pack);
	}
	if (pack_fd >= 0)
		close(pack_fd);
	delete_tempfile(&pack_temp);
	rollback_lock_file(&lock);
	free(lock_path);
	free(template);
	free(pack_probe);
	free(probe);
	free(manifest_dir);
	free(pack_dir);
	free(objects_path);
	free(root);
	return ret;
}

void odb_source_vex_maybe_build_fetch_delta_cache(struct repository *repo,
		const struct object_id *old_oid, const struct object_id *new_oid)
{
	if (!repo_uses_vex_storage(repo) || !old_oid || !new_oid ||
	    is_null_oid(old_oid) || is_null_oid(new_oid) ||
	    !vex_source_fetch_delta_cache_enabled())
		return;
	if (!vex_fetch_delta_cache_is_fast_forward(repo, old_oid, new_oid))
		return;
	if (vex_fetch_delta_cache_build(repo, old_oid, new_oid))
		trace_printf("vex: skipped local fetch-delta cache old=%s new=%s",
			     oid_to_hex(old_oid), oid_to_hex(new_oid));
}

bool repo_uses_vex_storage(const struct repository *repo)
{
	return repo && repo->ref_storage_format == REF_STORAGE_FORMAT_VEX;
}
