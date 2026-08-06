#ifndef VEX_BLOB_S3_H
#define VEX_BLOB_S3_H

/*
 * Minimal S3-compatible blob driver for Vex object bytes.
 *
 * It intentionally owns only immutable object bytes.  Refs, object metadata,
 * and epoch state remain the metadata store's responsibility.  The public
 * per-repository key shape is:
 *
 *   git/v1/<repo_uuid>/objects/<oid_hex>
 *
 * Writes first use the equivalent staging namespace below, then a caller
 * promotes each object using S3 CopyObject.  That keeps this driver usable by
 * a transactional metadata implementation without making S3 a ref database.
 */

#include "hash.h"

#include <stddef.h>
#include <stdint.h>

struct strbuf;
struct vex_blob_s3;

/*
 * One transient receive-side upload.  The caller owns `data` until
 * vex_blob_s3_put_staging_batch() returns.  This is deliberately a transport
 * batching shape only: every entry still lands at its own immutable staging
 * key and no Git payload is folded into metadata.
 */
struct vex_blob_s3_staged_object {
	const struct object_id *oid;
	const void *data;
	unsigned long size;
};

/* The two immutable files that make a Git pack usable. */
enum vex_blob_s3_pack_component {
	VEX_BLOB_S3_PACK_COMPONENT_PACK = 0,
	VEX_BLOB_S3_PACK_COMPONENT_IDX = 1,
};

enum vex_blob_s3_result {
	VEX_BLOB_S3_OK = 0,
	VEX_BLOB_S3_ERR = -1,
	VEX_BLOB_S3_ERR_NOT_FOUND = -2,
	VEX_BLOB_S3_ERR_INVALID = -3,
	VEX_BLOB_S3_ERR_UNAVAILABLE = -4,
};

struct vex_blob_s3_options {
	/* Path-style S3 endpoint, for example http://127.0.0.1:9000. */
	const char *endpoint;
	const char *bucket;
	const char *region;
	const char *access_key;
	const char *secret_key;
	/* Lower-case 32-hex UUID; a canonical hyphenated UUID is also accepted. */
	const char *repo_id;
	const struct git_hash_algo *hash_algo;
	/* Optional PEM CA bundle for a private HTTPS endpoint. */
	const char *ca_file;
	/* Set only for explicitly insecure local development endpoints. */
	int insecure_tls;
};

int vex_blob_s3_open(struct vex_blob_s3 **blob,
		     const struct vex_blob_s3_options *opts);
void vex_blob_s3_close(struct vex_blob_s3 *blob);

/* Verify that the configured bucket is reachable with the configured key. */
int vex_blob_s3_check_bucket(struct vex_blob_s3 *blob);

int vex_blob_s3_final_key(const struct vex_blob_s3 *blob,
			  const struct object_id *oid, struct strbuf *key);
int vex_blob_s3_staging_key(const struct vex_blob_s3 *blob, uint64_t epoch,
			    const struct object_id *oid, struct strbuf *key);

int vex_blob_s3_put_staging(struct vex_blob_s3 *blob, uint64_t epoch,
			    const struct object_id *oid,
			    const void *data, unsigned long size);
/*
 * Upload a bounded set of distinct objects concurrently.  On any failure it
 * removes every staging key attempted by this call before returning an error;
 * callers can therefore leave metadata untouched until this succeeds.
 */
int vex_blob_s3_put_staging_batch(
	struct vex_blob_s3 *blob, uint64_t epoch,
	const struct vex_blob_s3_staged_object *objects, size_t objects_nr);
/* Copy one staged object to its final content-addressed key, then delete it. */
int vex_blob_s3_promote_object(struct vex_blob_s3 *blob, uint64_t epoch,
			       const struct object_id *oid);
/*
 * Promote a bounded set of staging objects concurrently.  It never makes
 * metadata visible; callers still publish only after the entire batch has
 * succeeded.  A failed batch is safe to retry or compensate.
 */
int vex_blob_s3_promote_batch(struct vex_blob_s3 *blob, uint64_t epoch,
			      const struct object_id *oids, size_t oids_nr);
/* Idempotent compensation for one staged object. */
int vex_blob_s3_delete_staging(struct vex_blob_s3 *blob, uint64_t epoch,
			       const struct object_id *oid);
/* Idempotent compensation for an uncommitted newly-promoted final object. */
int vex_blob_s3_delete_final(struct vex_blob_s3 *blob,
			     const struct object_id *oid);

/* Returns 1 if present, 0 if absent, or a negative VEX_BLOB_S3_ERR_* value. */
int vex_blob_s3_has_final(struct vex_blob_s3 *blob,
			  const struct object_id *oid);
int vex_blob_s3_read_final(struct vex_blob_s3 *blob,
			   const struct object_id *oid,
			   void **data, unsigned long *size);
int vex_blob_s3_read_staging(struct vex_blob_s3 *blob, uint64_t epoch,
			     const struct object_id *oid,
			     void **data, unsigned long *size);

/*
 * Canonical raw receive-pack storage.  A receive first writes and validates a
 * pack/idx pair on its local WAL disk.  These APIs keep those bytes in S3,
 * never in metadata:
 *
 *   git/v1/<repo>/staging/<epoch>/packs/<packid>.{pack,idx}
 *   git/v1/<repo>/packs/<packid>.{pack,idx}
 *
 * `pack_id` is the raw pack checksum and must be exactly this repository's
 * hash length.  Staging also requires a non-zero epoch.  The stage call
 * accepts absolute regular WAL files only, verifies that the pack trailer and
 * idx footer bind both files to `pack_id`, uploads the two files directly,
 * and compensates both staging keys if either upload fails.  Git's full pack
 * and index semantic verification remains the receive path's responsibility.
 */
int vex_blob_s3_receive_pack_staging_key(
	const struct vex_blob_s3 *blob, uint64_t epoch,
	const unsigned char *pack_id, size_t pack_id_len,
	enum vex_blob_s3_pack_component component, struct strbuf *key);
int vex_blob_s3_receive_pack_final_key(
	const struct vex_blob_s3 *blob, const unsigned char *pack_id,
	size_t pack_id_len, enum vex_blob_s3_pack_component component,
	struct strbuf *key);
int vex_blob_s3_stage_receive_pack(struct vex_blob_s3 *blob, uint64_t epoch,
	const unsigned char *pack_id, size_t pack_id_len,
	const char *pack_path, const char *idx_path);

/*
 * Copy both staged components to their canonical final keys, then remove the
 * staging pair.  A failed copy or delete leaves no metadata visible and is
 * retryable.  Final-key cleanup is only for an uncommitted pack whose
 * metadata transaction did not seal it.
 */
int vex_blob_s3_promote_receive_pack(struct vex_blob_s3 *blob, uint64_t epoch,
	const unsigned char *pack_id, size_t pack_id_len);
int vex_blob_s3_delete_receive_pack_staging(
	struct vex_blob_s3 *blob, uint64_t epoch,
	const unsigned char *pack_id, size_t pack_id_len);
int vex_blob_s3_delete_receive_pack_final(
	struct vex_blob_s3 *blob, const unsigned char *pack_id,
	size_t pack_id_len);

/*
 * Stream one immutable derived-pack component into an already-created local
 * temporary file.  The key is derived from this blob handle's repository and
 * hash algorithm rather than accepted from the caller, so metadata cannot
 * redirect a cache read outside its own packs namespace.
 */
int vex_blob_s3_download_derived_pack(struct vex_blob_s3 *blob,
				      const unsigned char *pack_id,
				      size_t pack_id_len,
				      int index,
				      const char *destination);

#endif /* VEX_BLOB_S3_H */
