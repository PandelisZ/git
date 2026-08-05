#ifndef VEX_META_MYSQL_H
#define VEX_META_MYSQL_H

/*
 * MySQL metadata driver for the Vex storage schema.
 *
 * This interface deliberately accepts object identifiers and blob keys, but
 * never object bytes.  Raw Git object and pack payloads remain the blob-store
 * responsibility; MySQL only carries the metadata declared in schema.sql.
 *
 * It is separate from vex_store for now.  vex_store currently owns an
 * in-process filesystem metadata representation, while this driver makes the
 * SQL transaction boundary explicit for the later metadata-backend vtable.
 */

#include <stddef.h>
#include <stdint.h>

#define VEX_META_MYSQL_REPO_ID_HEXSZ 32
#define VEX_META_MYSQL_OID_BYTES 32
#define VEX_META_MYSQL_VERSION_ABSENT UINT64_MAX

struct vex_meta_mysql;
struct vex_meta_state;
struct git_hash_algo;

enum vex_meta_mysql_result {
	VEX_META_MYSQL_OK = 0,
	VEX_META_MYSQL_ERR = -1,
	VEX_META_MYSQL_ERR_INVALID = -2,
	VEX_META_MYSQL_ERR_NOMEM = -3,
	VEX_META_MYSQL_ERR_CONNECT = -4,
	VEX_META_MYSQL_ERR_SCHEMA = -5,
	VEX_META_MYSQL_ERR_NOT_FOUND = -6,
	VEX_META_MYSQL_ERR_CONFLICT = -7,
	VEX_META_MYSQL_ERR_STATE = -8,
};

enum vex_meta_mysql_epoch_state {
	VEX_META_MYSQL_EPOCH_OPEN = 1,
	VEX_META_MYSQL_EPOCH_SEALED,
	VEX_META_MYSQL_EPOCH_SWEEPABLE,
	VEX_META_MYSQL_EPOCH_SWEPT,
	VEX_META_MYSQL_EPOCH_ABORTED,
};

enum vex_meta_mysql_object_state {
	VEX_META_MYSQL_OBJECT_QUARANTINE = 1,
	VEX_META_MYSQL_OBJECT_PUBLISHED,
};

/*
 * Returned metadata is sufficient to locate and validate a blob-store read.
 * blob_key is allocated by the driver and must be released with
 * vex_meta_mysql_object_release().  It is never a Git object payload.
 */
struct vex_meta_mysql_object {
	unsigned int object_type;
	uint64_t size;
	uint64_t generation;
	enum vex_meta_mysql_object_state state;
	char *blob_key;
};

struct vex_meta_mysql_options {
	const char *host;
	unsigned int port;
	const char *user;
	const char *password;
	const char *database;
	const char *unix_socket;
};

/*
 * Decode either 32 lower-case hexadecimal digits or a lower-case hyphenated
 * UUID into the BINARY(16) repository key used by schema.sql.
 */
int vex_meta_mysql_repo_id_parse(const char *repo_id,
				 unsigned char out[16]);

int vex_meta_mysql_open(struct vex_meta_mysql **out,
			const struct vex_meta_mysql_options *opts);
void vex_meta_mysql_close(struct vex_meta_mysql *db);
const char *vex_meta_mysql_error(const struct vex_meta_mysql *db);

int vex_meta_mysql_ping(struct vex_meta_mysql *db);

/* Verify the ten Vex metadata tables and objects.blob_key's metadata type. */
int vex_meta_mysql_schema_check(struct vex_meta_mysql *db,
				unsigned int *table_count);

/* Ensure the repository identity row exists without storing any Git bytes. */
int vex_meta_mysql_ensure_repository(struct vex_meta_mysql *db,
				     const char *repo_id,
				     const char *hash_algo,
				     const char *name);

/*
 * Allocate an open generation.  push_id is an optional lower-case UUID.  The
 * allocator uses a MySQL advisory lock per repository before inserting.
 */
int vex_meta_mysql_epoch_begin(struct vex_meta_mysql *db,
			       const char *repo_id, const char *push_id,
			       uint64_t *generation);

/* Read an epoch state, or return VEX_META_MYSQL_ERR_NOT_FOUND. */
int vex_meta_mysql_epoch_get_state(struct vex_meta_mysql *db,
				   const char *repo_id, uint64_t generation,
				   enum vex_meta_mysql_epoch_state *state);

/*
 * Abort one open epoch atomically.  Its generation claims are removed and
 * only now-unclaimed quarantine objects (plus their commit-parent rows) are
 * deleted; shared claims and already-published objects survive.  Blob-store
 * staging cleanup remains the caller's responsibility.
 */
int vex_meta_mysql_epoch_abort(struct vex_meta_mysql *db,
			       const char *repo_id, uint64_t generation);

/*
 * Stage an object metadata row and its generation claim.  oid may be a
 * 20-byte SHA-1 or 32-byte SHA-256 ID; SHA-1 values are zero-padded to the
 * BINARY(32) schema representation.  blob_key names a blob-store location.
 */
int vex_meta_mysql_stage_object(struct vex_meta_mysql *db,
			 const char *repo_id, uint64_t generation,
			 const unsigned char *oid, size_t oid_len,
			 unsigned int object_type, uint64_t size,
			 const char *blob_key);

/*
 * Fetch metadata only if it is public, or if visible_generation is the same
 * still-open epoch that claimed it.  Pass zero for normal public reads.  The
 * ownership check prevents another receive-pack from observing quarantine
 * objects by guessing a generation number.
 */
int vex_meta_mysql_object_get_visible(struct vex_meta_mysql *db,
				      const char *repo_id,
				      const unsigned char *oid, size_t oid_len,
				      uint64_t visible_generation,
				      struct vex_meta_mysql_object *object);
void vex_meta_mysql_object_release(struct vex_meta_mysql_object *object);

/* Store only commit-graph edge metadata; neither object contains Git bytes. */
int vex_meta_mysql_add_commit_parent(struct vex_meta_mysql *db,
			     const char *repo_id,
			     const unsigned char *commit_oid, size_t commit_oid_len,
			     unsigned int parent_pos,
			     const unsigned char *parent_oid, size_t parent_oid_len);

/* Make every object claimed by an open generation visible, then seal it. */
int vex_meta_mysql_epoch_seal(struct vex_meta_mysql *db,
			      const char *repo_id, uint64_t generation);

/*
 * Atomically create/update one direct ref and append its immutable audit row.
 * expected_version is VEX_META_MYSQL_VERSION_ABSENT for a create-only CAS.
 * new_oid must already be a published object in this repository; blob
 * durability is checked by the caller before this metadata operation.
 * Symbolic refs, deletions, and a multi-ref transaction belong in the future
 * common metadata-backend interface rather than this standalone probe API.
 */
int vex_meta_mysql_ref_cas_direct(struct vex_meta_mysql *db,
				  const char *repo_id, const char *refname,
				  uint64_t expected_version,
				  const unsigned char *new_oid, size_t new_oid_len,
				  const char *reason, const char *message,
				  uint64_t *new_version);

/*
 * Store adapter boundary.  These methods project the existing Vex metadata
 * state machine onto the normalized MySQL schema while a repository-row lock
 * and SQL transaction are held.  They never accept or return Git bytes.
 *
 * A regular Git build still exports unavailable stubs, so selecting the
 * MySQL backend fails clearly unless Git was built with VEX_MYSQL=YesPlease.
 */
int vex_meta_mysql_state_lock(struct vex_meta_mysql *db, const char *repo_id);
void vex_meta_mysql_state_unlock(struct vex_meta_mysql *db);
int vex_meta_mysql_state_load(struct vex_meta_mysql *db, const char *repo_id,
			       const struct git_hash_algo *hash_algo,
			       struct vex_meta_state *state);
int vex_meta_mysql_state_save_locked(struct vex_meta_mysql *db,
				      const char *repo_id,
				      const struct git_hash_algo *hash_algo,
				      const struct vex_meta_state *state);

#endif /* VEX_META_MYSQL_H */
