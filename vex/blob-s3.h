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

#include <stdint.h>

struct strbuf;
struct vex_blob_s3;

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
/* Copy one staged object to its final content-addressed key, then delete it. */
int vex_blob_s3_promote_object(struct vex_blob_s3 *blob, uint64_t epoch,
			       const struct object_id *oid);
/* Idempotent compensation for one staged object. */
int vex_blob_s3_delete_staging(struct vex_blob_s3 *blob, uint64_t epoch,
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

#endif /* VEX_BLOB_S3_H */
