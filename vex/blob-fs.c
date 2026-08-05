#include "git-compat-util.h"
#include "vex/store-internal.h"
#include "hex.h"
#include "path.h"
#include "strbuf.h"
#include "wrapper.h"

/* The fs blob backend deliberately mirrors the final S3 key layout. */

struct vex_blob_fs {
	char *root;
	char *repo_id;
	const struct git_hash_algo *hash_algo;
};

static void append_oid(struct strbuf *path, const struct object_id *oid,
		       const struct git_hash_algo *hash_algo)
{
	char hex[GIT_MAX_HEXSZ + 1];

	hash_to_hex_algop_r(hex, oid->hash, hash_algo);
	strbuf_addstr(path, hex);
}

static void final_path(const struct vex_blob_fs *blob,
		       const struct object_id *oid, struct strbuf *path)
{
	strbuf_addf(path, "%s/blobs/git/v1/%s/objects/", blob->root,
		    blob->repo_id);
	append_oid(path, oid, blob->hash_algo);
}

static void staging_path(const struct vex_blob_fs *blob, uint64_t epoch,
			 const struct object_id *oid, struct strbuf *path)
{
	strbuf_addf(path, "%s/blobs/git/v1/%s/staging/%"PRIu64"/objects/",
		    blob->root, blob->repo_id, epoch);
	append_oid(path, oid, blob->hash_algo);
}

static void staging_dir(const struct vex_blob_fs *blob, uint64_t epoch,
			struct strbuf *path)
{
	strbuf_addf(path, "%s/blobs/git/v1/%s/staging/%"PRIu64,
		    blob->root, blob->repo_id, epoch);
}

static int ensure_parent(const char *path)
{
	return safe_create_leading_directories_no_share((char *)path) == SCLD_OK ?
		VEX_STORE_OK : VEX_STORE_ERR;
}

static int path_is_regular_file(const char *path)
{
	struct stat st;

	if (!stat(path, &st))
		return S_ISREG(st.st_mode) ? 1 : VEX_STORE_ERR_CORRUPT;
	if (errno == ENOENT || errno == ENOTDIR)
		return 0;
	return VEX_STORE_ERR;
}

static int write_staging_file(const char *path, const void *data,
			      unsigned long size)
{
	struct strbuf tmp = STRBUF_INIT;
	int fd = -1;
	int ret = VEX_STORE_ERR;

	if (ensure_parent(path))
		goto done;
	strbuf_addf(&tmp, "%s.tmp-XXXXXX", path);
	fd = git_mkstemp_mode(tmp.buf, 0600);
	if (fd < 0)
		goto done;
	if (write_in_full(fd, data, size) < 0 ||
	    git_fsync(fd, FSYNC_HARDWARE_FLUSH) < 0 || close(fd) < 0) {
		fd = -1;
		goto done;
	}
	fd = -1;
	if (rename(tmp.buf, path) < 0)
		goto done;
	ret = VEX_STORE_OK;
done:
	if (fd >= 0)
		close(fd);
	if (ret && tmp.len)
		unlink(tmp.buf);
	strbuf_release(&tmp);
	return ret;
}

static int remove_tree(const char *path)
{
	DIR *dir;
	struct dirent *entry;
	int ret = VEX_STORE_OK;

	dir = opendir(path);
	if (!dir)
		return errno == ENOENT ? VEX_STORE_OK : VEX_STORE_ERR;
	while ((entry = readdir(dir))) {
		struct strbuf child = STRBUF_INIT;
		struct stat st;

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		strbuf_addf(&child, "%s/%s", path, entry->d_name);
		if (lstat(child.buf, &st) < 0) {
			ret = VEX_STORE_ERR;
		} else if (S_ISDIR(st.st_mode)) {
			if (remove_tree(child.buf))
				ret = VEX_STORE_ERR;
		} else if (unlink(child.buf) < 0) {
			ret = VEX_STORE_ERR;
		}
		strbuf_release(&child);
	}
	if (closedir(dir) || rmdir(path))
		ret = VEX_STORE_ERR;
	return ret;
}

static int read_file(const char *path, void **data, unsigned long *size)
{
	struct stat st;
	void *contents;
	int fd;
	ssize_t bytes;

	*data = NULL;
	*size = 0;
	fd = open_nofollow(path, O_RDONLY);
	if (fd < 0)
		return errno == ENOENT ? VEX_STORE_ERR_NOT_FOUND : VEX_STORE_ERR;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
	    (uintmax_t)st.st_size > ULONG_MAX) {
		close(fd);
		return VEX_STORE_ERR_CORRUPT;
	}
	contents = xmallocz(st.st_size);
	bytes = read_in_full(fd, contents, st.st_size);
	if (close(fd) < 0 || bytes != st.st_size) {
		free(contents);
		return VEX_STORE_ERR;
	}
	*data = contents;
	*size = st.st_size;
	return VEX_STORE_OK;
}

int vex_blob_fs_open(struct vex_blob_fs **blob_out, const char *root,
		       const char *repo_id, const struct git_hash_algo *hash_algo)
{
	struct vex_blob_fs *blob;

	CALLOC_ARRAY(blob, 1);
	blob->root = xstrdup(root);
	blob->repo_id = xstrdup(repo_id);
	blob->hash_algo = hash_algo;
	*blob_out = blob;
	return VEX_STORE_OK;
}

void vex_blob_fs_close(struct vex_blob_fs *blob)
{
	if (!blob)
		return;
	free(blob->root);
	free(blob->repo_id);
	free(blob);
}

int vex_blob_fs_ensure(struct vex_blob_fs *blob)
{
	struct strbuf path = STRBUF_INIT;
	int ret;

	strbuf_addf(&path, "%s/blobs/git/v1/%s/objects/.keep", blob->root,
		    blob->repo_id);
	ret = ensure_parent(path.buf);
	strbuf_release(&path);
	return ret;
}

int vex_blob_fs_put_staging(struct vex_blob_fs *blob, uint64_t epoch,
			      const struct object_id *oid,
			      const void *data, unsigned long size)
{
	struct strbuf path = STRBUF_INIT;
	int ret;

	staging_path(blob, epoch, oid, &path);
	ret = write_staging_file(path.buf, data, size);
	strbuf_release(&path);
	return ret;
}

int vex_blob_fs_promote_epoch(struct vex_blob_fs *blob,
				const struct vex_meta_state *state,
				uint64_t epoch)
{
	size_t i;

	for (i = 0; i < state->epoch_objects_nr; i++) {
		const struct vex_meta_epoch_object *entry = &state->epoch_objects[i];
		struct strbuf staged = STRBUF_INIT;
		struct strbuf final = STRBUF_INIT;
		int final_exists;
		int staged_exists;
		int ret = VEX_STORE_OK;

		if (entry->epoch != epoch)
			continue;
		staging_path(blob, epoch, &entry->oid, &staged);
		final_path(blob, &entry->oid, &final);
		final_exists = path_is_regular_file(final.buf);
		staged_exists = path_is_regular_file(staged.buf);
		if (final_exists < 0 || staged_exists < 0) {
			ret = final_exists < 0 ? final_exists : staged_exists;
		} else if (!final_exists && !staged_exists) {
			ret = VEX_STORE_ERR_NOT_FOUND;
		} else if (!final_exists) {
			if (ensure_parent(final.buf) || rename(staged.buf, final.buf) < 0)
				ret = VEX_STORE_ERR;
		} else if (staged_exists && unlink(staged.buf) < 0) {
			ret = VEX_STORE_ERR;
		}
		strbuf_release(&staged);
		strbuf_release(&final);
		if (ret)
			return ret;
	}
	return VEX_STORE_OK;
}

int vex_blob_fs_compensate_epoch(struct vex_blob_fs *blob, uint64_t epoch)
{
	struct strbuf path = STRBUF_INIT;
	int ret;

	staging_dir(blob, epoch, &path);
	ret = remove_tree(path.buf);
	strbuf_release(&path);
	return ret;
}

int vex_blob_fs_has_final(struct vex_blob_fs *blob,
			    const struct object_id *oid)
{
	struct strbuf path = STRBUF_INIT;
	int ret;

	final_path(blob, oid, &path);
	ret = path_is_regular_file(path.buf);
	strbuf_release(&path);
	return ret;
}

int vex_blob_fs_read_final(struct vex_blob_fs *blob,
			     const struct object_id *oid,
			     void **data, unsigned long *size)
{
	struct strbuf path = STRBUF_INIT;
	int ret;

	final_path(blob, oid, &path);
	ret = read_file(path.buf, data, size);
	strbuf_release(&path);
	return ret;
}

int vex_blob_fs_read_staging(struct vex_blob_fs *blob, uint64_t epoch,
			       const struct object_id *oid,
			       void **data, unsigned long *size)
{
	struct strbuf path = STRBUF_INIT;
	int ret;

	staging_path(blob, epoch, oid, &path);
	ret = read_file(path.buf, data, size);
	strbuf_release(&path);
	return ret;
}
