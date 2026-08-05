#ifndef VEX_STORE_H
#define VEX_STORE_H

/*
 * Vex object and ref storage interface.
 *
 * This is deliberately independent from an ODB source or ref backend.  The
 * latter use this interface to keep the blob store and the metadata store in
 * one publish boundary.  An epoch is private until a ref transaction commits;
 * a failed transaction compensates every object staged in that epoch.
 */

#include "hash.h"
#include "object.h"

#include <stddef.h>
#include <stdint.h>

struct vex_store;
struct vex_epoch;
struct vex_ref_transaction;
struct vex_blob_s3_options;
struct vex_meta_mysql_options;
struct strbuf;

enum vex_store_backend {
	VEX_STORE_BACKEND_FS = 1,
	/* Filesystem metadata with canonical object bytes in S3-compatible storage. */
	VEX_STORE_BACKEND_FS_S3,
	/* MySQL metadata with canonical object bytes in S3-compatible storage. */
	VEX_STORE_BACKEND_MYSQL_S3,
};

enum vex_store_result {
	VEX_STORE_OK = 0,
	VEX_STORE_ERR = -1,
	VEX_STORE_ERR_NOT_FOUND = -2,
	VEX_STORE_ERR_CONFLICT = -3,
	VEX_STORE_ERR_INVALID = -4,
	VEX_STORE_ERR_CORRUPT = -5,
	VEX_STORE_ERR_STATE = -6,
};

enum vex_object_state {
	VEX_OBJECT_QUARANTINE = 1,
	VEX_OBJECT_PUBLISHED,
};

enum vex_ref_target_type {
	/* Valid only for a CAS expectation. */
	VEX_REF_TARGET_ANY = 0,
	VEX_REF_TARGET_NONE,
	VEX_REF_TARGET_OID,
	VEX_REF_TARGET_SYMREF,
};

struct vex_store_options {
	enum vex_store_backend backend;
	/* Root containing the vex-store data, not a Git object directory. */
	const char *root;
	/* A canonical, lower-case 32-digit hexadecimal repository UUID. */
	const char *repo_id;
	const struct git_hash_algo *hash_algo;
	/* Required for either S3-byte backend. Repo/hash are derived here. */
	const struct vex_blob_s3_options *blob_s3;
	/* Required only for VEX_STORE_BACKEND_MYSQL_S3. */
	const struct vex_meta_mysql_options *meta_mysql;
};

struct vex_object_info {
	enum object_type type;
	unsigned long size;
	uint64_t epoch;
	enum vex_object_state state;
};

/*
 * A selected immutable derived pack.  The identifier is a pack checksum, not
 * a Git object ID, but uses object_id storage so the active repository hash
 * algorithm is carried consistently through the ODB path.
 */
struct vex_pack_artifact {
	struct object_id pack_id;
	uint64_t covering_generation;
};

/* Opaque TTL-bounded protection for one packed-object serving session. */
#define VEX_PACK_LEASE_ID_BYTES 16
struct vex_pack_lease {
	unsigned char id[VEX_PACK_LEASE_ID_BYTES];
	unsigned int active:1;
};

struct vex_ref_target {
	enum vex_ref_target_type type;
	struct object_id oid;
	/* Set only for VEX_REF_TARGET_SYMREF. */
	const char *symref;
};

struct vex_ref {
	const char *name;
	struct vex_ref_target target;
	uint64_t version;
};

struct vex_ref_update {
	const char *refname;
	/*
	 * Old target and version form a typed compare-and-swap.  A zero
	 * expected_version does not constrain the version.
	 */
	struct vex_ref_target old_target;
	uint64_t expected_version;
	struct vex_ref_target new_target;
	/* -1 means that fast-forward status was not calculated. */
	int is_fast_forward;
	const char *actor;
	const char *push_id;
	const char *reason;
	const char *message;
};

/*
 * A sweep plan is intentionally read-only.  It performs a complete object
 * graph mark from live ref tips, durable MySQL pins, current pack members,
 * and active clone-lease members, reading canonical bytes from the configured
 * blob backend. `sealed_candidates` is therefore an auditable input to a
 * later grace-period deletion phase, not a deletion authorization by itself.
 */
struct vex_sweep_plan {
	uint64_t published_objects;
	uint64_t ref_roots;
	uint64_t pin_roots;
	uint64_t pack_roots;
	uint64_t lease_roots;
	uint64_t reachable_objects;
	uint64_t sealed_candidates;
};

/*
 * Object iteration is an index operation: it never reads raw object bytes.
 * A NULL epoch exposes only published objects.  Passing the current epoch
 * additionally exposes its quarantined objects, which is needed while Git is
 * constructing a transaction.  Objects owned by every other open epoch
 * remain invisible.
 */
typedef int (*vex_object_iter_fn)(const struct object_id *oid,
				  const struct vex_object_info *info,
				  void *cb_data);
typedef int (*vex_ref_iter_fn)(const struct vex_ref *ref, void *cb_data);
typedef int (*vex_ref_update_iter_fn)(const struct vex_ref_update *update,
					uint64_t id, void *cb_data);

int vex_store_open(struct vex_store **store,
		     const struct vex_store_options *opts);
void vex_store_close(struct vex_store *store);

/* Verify/create the metadata row and on-disk namespace for this repository. */
int vex_repo_ensure(struct vex_store *store);
const char *vex_store_repo_id(const struct vex_store *store);
const struct git_hash_algo *vex_store_hash_algo(const struct vex_store *store);
const char *vex_store_result_string(int result);
/* A backend diagnostic for an open store; it never exposes credentials. */
const char *vex_store_error(const struct vex_store *store);

/* Build git/v1/<repo_uuid>/objects/<oid_hex> into key. */
int vex_blob_key(const struct vex_store *store, const struct object_id *oid,
		   struct strbuf *key);

int vex_epoch_allocate(struct vex_store *store, const char *push_id,
			 struct vex_epoch **epoch);
uint64_t vex_epoch_id(const struct vex_epoch *epoch);

/*
 * Attach a process-local handle to an already-open epoch.  This is the
 * receive-pack handoff primitive: a child that inherited the lease may stage
 * and read only that epoch's quarantined objects, but cannot publish it on
 * its own.  The handle must be released with vex_epoch_detach().
 */
int vex_epoch_attach(struct vex_store *store, uint64_t epoch_id,
		     struct vex_epoch **epoch);

/*
 * Release a local epoch handle without changing durable epoch state.  This is
 * used by a child process after it has staged data for its parent, and by the
 * parent after the joined ref transaction has sealed or compensated the
 * epoch.  Call vex_epoch_free() for an owner that should compensate an open
 * epoch on cleanup.
 */
void vex_epoch_detach(struct vex_epoch *epoch);
int vex_epoch_seal(struct vex_epoch *epoch);
int vex_epoch_compensate(struct vex_epoch *epoch);
void vex_epoch_free(struct vex_epoch *epoch);

/*
 * Reclaim stale open MySQL+S3 epochs left by an abruptly terminated receive
 * worker.  `min_age_seconds` is a safety grace period; zero is reserved for
 * controlled test recovery.  The normal Vex backend calls this during store
 * initialization with its configured bounded grace period.
 */
int vex_store_recover_expired_epochs(struct vex_store *store,
				      unsigned int min_age_seconds,
				      unsigned int *recovered);

/*
 * Stage raw Git object content.  The implementation verifies that oid is the
 * hash of type, size, and data before it writes the staging blob.
 */
int vex_blob_put_staging(struct vex_store *store,
			   struct vex_epoch *epoch,
			   const struct object_id *oid,
			   enum object_type type, const void *data,
			   unsigned long size);
int vex_blob_promote(struct vex_store *store, struct vex_epoch *epoch);
int vex_blob_compensate(struct vex_store *store, struct vex_epoch *epoch);

int vex_object_index_put_quarantine(struct vex_store *store,
			      struct vex_epoch *epoch,
			      const struct object_id *oid,
			      enum object_type type, unsigned long size);
int vex_object_index_publish(struct vex_store *store,
			       struct vex_epoch *epoch);
int vex_object_index_get(struct vex_store *store,
			   const struct object_id *oid,
			   struct vex_object_info *info);

/* Blob-before-metadata convenience operation used by ODB write paths. */
int vex_store_stage_object(struct vex_store *store,
			     struct vex_epoch *epoch,
			     const struct object_id *oid,
			     enum object_type type, const void *data,
			     unsigned long size);
/* Returns 1 when a published object exists, 0 when it does not, or an error. */
int vex_store_has_object(struct vex_store *store,
			   const struct object_id *oid);
int vex_store_read_object(struct vex_store *store,
			    const struct object_id *oid,
			    enum object_type *type, unsigned long *size,
			    void **data);
int vex_store_read_object_in_epoch(struct vex_store *store,
			     const struct vex_epoch *epoch,
			     const struct object_id *oid,
			     enum object_type *type, unsigned long *size,
			     void **data);

/*
 * Find a derived pack that is sealed for exactly the current repository
 * generation.  Its payload remains in S3; callers may materialize it only as
 * a disposable local read cache with vex_store_download_pack().
 */
int vex_store_current_pack(struct vex_store *store,
			   struct vex_pack_artifact *artifact);
int vex_store_download_pack(struct vex_store *store,
			    const struct vex_pack_artifact *artifact,
			    const char *pack_destination,
			    const char *idx_destination);

/*
 * Hold an expiring metadata lease for a derived pack during server-side
 * serving. The lease never stores pack/object bytes and can be released early
 * once the packed source is closed. ttl_seconds must be in the bounded range
 * accepted by the MySQL driver.
 */
int vex_store_pack_lease_acquire(struct vex_store *store,
				 const struct vex_pack_artifact *artifact,
				 unsigned int ttl_seconds,
				 struct vex_pack_lease *lease);
int vex_store_pack_lease_release(struct vex_store *store,
				 struct vex_pack_lease *lease);

/*
 * Enumerate visible metadata records in object-id order.  A nonzero callback
 * return stops iteration and is returned to the caller.  When prefix is set,
 * only object IDs matching its first prefix_hex_len characters are yielded.
 */
int vex_store_for_each_object(struct vex_store *store,
			      const struct vex_epoch *epoch,
			      const struct object_id *prefix,
			      size_t prefix_hex_len,
			      vex_object_iter_fn fn, void *cb_data);
int vex_store_count_objects(struct vex_store *store,
			    const struct vex_epoch *epoch, unsigned long *out);
int vex_store_find_abbrev_len(struct vex_store *store,
			     const struct vex_epoch *epoch,
			     const struct object_id *oid,
			     unsigned min_length, unsigned *out);

/* Durable, MySQL-only retain/release roots for published objects. */
int vex_store_pin_object(struct vex_store *store, const struct object_id *oid,
			 const char *reason);
int vex_store_unpin_object(struct vex_store *store, const struct object_id *oid,
			   const char *reason);

/*
 * Build a conservative, non-mutating sweep plan.  Only the active
 * MySQL+S3 Vex backend implements this first safety slice.
 */
int vex_store_sweep_plan(struct vex_store *store, struct vex_sweep_plan *plan);

int vex_ref_read(struct vex_store *store, const char *refname,
		   struct vex_ref *ref);
/* Release strings returned by vex_ref_read(). */
void vex_ref_clear(struct vex_ref *ref);
int vex_ref_iterate(struct vex_store *store, const char *prefix,
		      vex_ref_iter_fn fn, void *cb_data);

int vex_ref_transaction_begin(struct vex_store *store,
				struct vex_epoch *epoch,
				struct vex_ref_transaction **transaction);
int vex_ref_transaction_update(struct vex_ref_transaction *transaction,
				 const struct vex_ref_update *update);
/*
 * Commit first verifies every CAS, then makes blobs durable and atomically
 * publishes object metadata, refs, ref_updates, and the sealed epoch.
 */
int vex_ref_transaction_commit(struct vex_ref_transaction *transaction);
void vex_ref_transaction_abort(struct vex_ref_transaction *transaction);

/* Immutable audit rows.  Ref transactions append these automatically. */
int vex_ref_updates_append(struct vex_store *store,
			     const struct vex_ref_update *update,
			     uint64_t *id);
int vex_ref_updates_iterate(struct vex_store *store,
			      vex_ref_update_iter_fn fn, void *cb_data);

#endif /* VEX_STORE_H */
