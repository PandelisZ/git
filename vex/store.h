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
struct strbuf;

enum vex_store_backend {
	VEX_STORE_BACKEND_FS = 1,
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
};

struct vex_object_info {
	enum object_type type;
	unsigned long size;
	uint64_t epoch;
	enum vex_object_state state;
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

/* Build git/v1/<repo_uuid>/objects/<oid_hex> into key. */
int vex_blob_key(const struct vex_store *store, const struct object_id *oid,
		   struct strbuf *key);

int vex_epoch_allocate(struct vex_store *store, const char *push_id,
			 struct vex_epoch **epoch);
uint64_t vex_epoch_id(const struct vex_epoch *epoch);
int vex_epoch_seal(struct vex_epoch *epoch);
int vex_epoch_compensate(struct vex_epoch *epoch);
void vex_epoch_free(struct vex_epoch *epoch);

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
