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
#define VEX_META_MYSQL_LEASE_ID_BYTES 16
#define VEX_META_MYSQL_VERSION_ABSENT UINT64_MAX

struct vex_meta_mysql;
struct vex_meta_state;
struct git_hash_algo;
struct vex_ref_update;

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

/*
 * Metadata-only description of a receive batch.  The raw object data never
 * crosses this interface: callers first make each S3 staging key durable,
 * then atomically append these rows and generation claims.
 */
struct vex_meta_mysql_staged_object {
	const unsigned char *oid;
	size_t oid_len;
	unsigned int object_type;
	uint64_t size;
	const char *blob_key;
};

/* Metadata-only commit graph edge collected while staging a receive batch. */
struct vex_meta_mysql_commit_parent {
	const unsigned char *commit_oid;
	size_t commit_oid_len;
	unsigned int parent_pos;
	const unsigned char *parent_oid;
	size_t parent_oid_len;
};

/*
 * Metadata-only description of one derived pack which is safe to select for
 * the repository's current sealed generation.  pack_id uses the schema's
 * BINARY(32) form: SHA-1 IDs are zero-padded and SHA-256 IDs fill the array.
 * Neither key contains pack bytes; release it with
 * vex_meta_mysql_pack_release().
 */
struct vex_meta_mysql_pack {
	unsigned char pack_id[VEX_META_MYSQL_OID_BYTES];
	uint64_t covering_generation;
	char *pack_key;
	char *idx_key;
};

/*
 * Canonical receive-pack descriptor.  `pack_key` and `idx_key` name the
 * final immutable blob-store components; their staging pair is deliberately
 * not recorded here.  Pack bytes and index bytes never cross this API.
 *
 * The first direct path is intentionally seed-only: callers must set
 * full_snapshot so the sealed pair is safe to select for a full clone.
 */
struct vex_meta_mysql_receive_pack {
	const unsigned char *pack_id;
	size_t pack_id_len;
	const char *pack_key;
	const char *idx_key;
	unsigned int full_snapshot:1;
};

/*
 * Metadata for one object whose canonical bytes live in a sealed raw pack.
 * object_type and size are zero when the receive map was derived from the
 * pack index alone; callers then materialize the immutable pack/idx pair.
 * Release the strings with vex_meta_mysql_receive_pack_object_release().
 */
struct vex_meta_mysql_receive_pack_object {
	unsigned int object_type;
	uint64_t size;
	uint64_t generation;
	unsigned char pack_id[VEX_META_MYSQL_OID_BYTES];
	char *pack_key;
	char *idx_key;
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
 * A pin is metadata-only protection for a published Vex object.  Pinning is
 * deliberately separate from ref/audit state: callers use it for an explicit
 * restore or retention root, and normal ref updates must preserve it.
 */
typedef int (*vex_meta_mysql_pin_iter_fn)(
	const unsigned char oid[VEX_META_MYSQL_OID_BYTES], void *cb_data);

/* Pack and lease roots use the same padded BINARY(32) object-id shape. */
typedef int (*vex_meta_mysql_oid_iter_fn)(
	const unsigned char oid[VEX_META_MYSQL_OID_BYTES], void *cb_data);

/*
 * Recovery selects only epoch identifiers while the normal state transaction
 * is held.  The callback must not issue SQL on db or retain the pointer.
 */
typedef int (*vex_meta_mysql_generation_iter_fn)(uint64_t generation,
						  void *cb_data);

/* Metadata-only state for an object which has entered destructive sweep. */
struct vex_meta_mysql_sweep_tombstone {
	unsigned char oid[VEX_META_MYSQL_OID_BYTES];
	uint64_t generation;
	unsigned int due:1;
};

typedef int (*vex_meta_mysql_sweep_tombstone_iter_fn)(
	const struct vex_meta_mysql_sweep_tombstone *tombstone, void *cb_data);

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

/* Verify the fifteen Vex metadata tables and their payload-free key columns. */
int vex_meta_mysql_schema_check(struct vex_meta_mysql *db,
				unsigned int *table_count);

/* Ensure the repository identity row exists without storing any Git bytes. */
int vex_meta_mysql_ensure_repository(struct vex_meta_mysql *db,
				     const char *repo_id,
				     const char *hash_algo,
				     const char *name);

/*
 * Allocate an open generation.  push_id may be a lower-case UUID; descriptive
 * internal labels are represented as NULL in the normalized schema. The
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
 * Enumerate stale open generations while vex_meta_mysql_state_lock() is held.
 * The normal state adapter rewrites a repository snapshot on save, so callers
 * must mutate that same in-memory state before committing recovery.
 */
int vex_meta_mysql_state_for_each_expired_open_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		unsigned int min_age_seconds,
		vex_meta_mysql_generation_iter_fn fn, void *cb_data);

/*
 * Advance an open receive epoch's durable activity timestamp.  The caller
 * must own vex_meta_mysql_state_lock() for this repository; that prevents a
 * concurrent state-snapshot rewrite from losing the heartbeat.
 */
int vex_meta_mysql_state_refresh_receive_heartbeat_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		uint64_t generation);

/*
 * These destructive-sweep methods are valid only while the normal state
 * transaction owns the repository row.  MySQL server time is the authority
 * for both generation age and the tombstone delay.
 */
int vex_meta_mysql_state_for_each_expired_sealed_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		unsigned int min_age_seconds,
		vex_meta_mysql_generation_iter_fn fn, void *cb_data);
int vex_meta_mysql_state_for_each_sweep_tombstone_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		vex_meta_mysql_sweep_tombstone_iter_fn fn, void *cb_data);
int vex_meta_mysql_state_for_each_pending_sweep_blob_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		vex_meta_mysql_oid_iter_fn fn, void *cb_data);
int vex_meta_mysql_state_mark_sweep_tombstone_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len, uint64_t generation,
		unsigned int tombstone_seconds, int *marked);
int vex_meta_mysql_state_cancel_sweep_tombstone_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len, int *cancelled);
int vex_meta_mysql_state_mark_sweep_metadata_deleted_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len);
/*
 * Recheck a post-state-commit S3 deletion while the repository row is locked.
 * A newly republished object cancels the old completed-metadata tombstone
 * before callers can delete its canonical immutable bytes.
 */
int vex_meta_mysql_state_prepare_sweep_blob_delete_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len, int *ready);
int vex_meta_mysql_state_mark_sweep_blob_deleted_locked(
		struct vex_meta_mysql *db, const char *repo_id,
		const unsigned char *oid, size_t oid_len);

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
 * Atomically append a bounded set of object records and claims for one open
 * generation.  Existing content-addressed records must match type, size, and
 * key exactly; otherwise the whole batch rolls back.
 */
int vex_meta_mysql_stage_objects(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct vex_meta_mysql_staged_object *objects, size_t objects_nr);

/*
 * Bulk-register an already-S3-staged raw receive pack without materializing
 * individual object bytes or rows in objects/generation_objects.  The
 * caller supplies a trusted, absolute, regular non-symlink TSV manifest:
 *
 *   <canonical lower-case oid hex> TAB <type 1..4> TAB <decimal size> LF
 *
 * SHA-1 rows use 40 hex digits and are padded only inside MySQL's BINARY(32)
 * representation; SHA-256 rows use 64.  `expected_members` is an integrity
 * check and permits an idempotent retry of the exact building envelope.
 */
int vex_meta_mysql_receive_pack_stage_manifest(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct git_hash_algo *hash_algo,
	const struct vex_meta_mysql_receive_pack *pack,
	const char *manifest_path, uint64_t expected_members);

/*
 * Resolve raw-pack membership if the envelope is sealed, or if
 * visible_generation owns the still-building receive.  Normal public reads
 * pass zero.  This returns metadata only, never a pack payload.
 */
int vex_meta_mysql_receive_pack_object_get_visible(
	struct vex_meta_mysql *db, const char *repo_id,
	const unsigned char *oid, size_t oid_len, uint64_t visible_generation,
	struct vex_meta_mysql_receive_pack_object *object);
void vex_meta_mysql_receive_pack_object_release(
	struct vex_meta_mysql_receive_pack_object *object);

/* Atomically append bounded commit-parent metadata after its objects exist. */
int vex_meta_mysql_stage_commit_parents(
	struct vex_meta_mysql *db, const char *repo_id,
	const struct vex_meta_mysql_commit_parent *parents, size_t parents_nr);

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

/*
 * Select a sealed derived pack only when it covers the latest sealed
 * repository generation.  This equality guard keeps a builder which raced a
 * newer push from becoming readable merely because its row says "sealed".
 */
int vex_meta_mysql_current_sealed_pack(struct vex_meta_mysql *db,
				       const char *repo_id,
				       const struct git_hash_algo *hash_algo,
				       struct vex_meta_mysql_pack *pack);

/*
 * Select the deliberately partial, derived artifact for a default clone.
 * It has the same current-generation equality guard as a complete snapshot,
 * but can never be returned by vex_meta_mysql_current_sealed_pack().  The
 * caller must retain the ordinary pack-objects path for requests outside the
 * partial artifact's advertised policy.
 */
int vex_meta_mysql_current_sealed_default_clone_pack(
	struct vex_meta_mysql *db, const char *repo_id,
	const struct git_hash_algo *hash_algo, struct vex_meta_mysql_pack *pack);

/*
 * Find the oldest sealed raw receive snapshot retained by this repository.
 * It is a read-only canonical base for a later loose-object overlay, never a
 * current-generation relay selection.  Obsolete raw packs remain readable
 * because their received-pack membership is still a durable recovery root.
 */
int vex_meta_mysql_base_receive_pack(struct vex_meta_mysql *db,
	const char *repo_id, const struct git_hash_algo *hash_algo,
	struct vex_meta_mysql_pack *pack);
void vex_meta_mysql_pack_release(struct vex_meta_mysql_pack *pack);

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
 * Normal per-object receive publication boundary. The caller has already
 * promoted its staged S3 objects, then holds vex_meta_mysql_state_lock().
 * This verifies the complete receive ref transaction, publishes the
 * generation's object claims, appends its audit records, obsoletes stale
 * derived packs, queues a replacement, and seals the generation in that same
 * MySQL transaction.
 */
int vex_meta_mysql_receive_epoch_seal_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct git_hash_algo *hash_algo,
	const struct vex_ref_update *updates, size_t updates_nr);

/*
 * Direct seed publication boundary.  The caller first promotes the staged
 * S3 pack/idx pair, then holds vex_meta_mysql_state_lock().  This verifies
 * ref CASes and that each new OID is a member of the building raw pack,
 * applies refs/audit rows, and atomically seals both generation and envelope.
 * It deliberately does not call the legacy state snapshot save path.
 */
int vex_meta_mysql_receive_pack_seal_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	const struct git_hash_algo *hash_algo,
	const struct vex_ref_update *updates, size_t updates_nr);

/* Return the building raw pack id for recovery before S3 compensation. */
int vex_meta_mysql_receive_pack_get_building_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	unsigned char out_pack_id[VEX_META_MYSQL_OID_BYTES]);

/*
 * Remove a building raw-pack mapping and abort its generation while locked.
 * On success out_pack_id receives the normalized BINARY(32) pack id so the
 * caller can compensate its staged/final S3 pair after committing.
 * VEX_META_MYSQL_ERR_NOT_FOUND means this generation is not a raw-pack epoch.
 */
int vex_meta_mysql_receive_pack_abort_locked(
	struct vex_meta_mysql *db, const char *repo_id, uint64_t generation,
	unsigned char out_pack_id[VEX_META_MYSQL_OID_BYTES]);

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
 * Pinning accepts only a published object.  Pins are idempotent by
 * (repo_id, oid, reason), never contain Git object bytes, and do not expire
 * unless a future retention operation assigns an expiry.
 */
int vex_meta_mysql_pin_object(struct vex_meta_mysql *db,
			       const char *repo_id,
			       const unsigned char *oid, size_t oid_len,
			       const char *reason);
int vex_meta_mysql_unpin_object(struct vex_meta_mysql *db,
				 const char *repo_id,
				 const unsigned char *oid, size_t oid_len,
				 const char *reason);

/* Enumerate only non-expired pins.  The callback must not issue SQL on db. */
int vex_meta_mysql_for_each_active_pin(struct vex_meta_mysql *db,
					const char *repo_id,
					vex_meta_mysql_pin_iter_fn fn,
					void *cb_data);

/*
 * Clone leases protect every object in a pack while an upload-pack/cache
 * reader can still serve it. `lease_id` is an opaque random BINARY(16) token;
 * expiry is a bounded crash fallback, not a source of object data.
 */
int vex_meta_mysql_clone_lease_acquire(struct vex_meta_mysql *db,
				       const char *repo_id,
				       const unsigned char *pack_id, size_t pack_id_len,
				       const unsigned char lease_id[VEX_META_MYSQL_LEASE_ID_BYTES],
				       unsigned int ttl_seconds);
int vex_meta_mysql_clone_lease_release(struct vex_meta_mysql *db,
				       const char *repo_id,
				       const unsigned char lease_id[VEX_META_MYSQL_LEASE_ID_BYTES]);

/* Enumerate objects retained by current sealed packs and non-expired leases. */
int vex_meta_mysql_for_each_active_pack_object(struct vex_meta_mysql *db,
						const char *repo_id,
						vex_meta_mysql_oid_iter_fn fn,
						void *cb_data);
int vex_meta_mysql_for_each_active_clone_lease_object(
					struct vex_meta_mysql *db, const char *repo_id,
					vex_meta_mysql_oid_iter_fn fn, void *cb_data);

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
/* Commit a locked metadata-only mutation without rewriting ordinary state. */
int vex_meta_mysql_state_commit_locked(struct vex_meta_mysql *db);
int vex_meta_mysql_state_load(struct vex_meta_mysql *db, const char *repo_id,
			       const struct git_hash_algo *hash_algo,
			       struct vex_meta_state *state);
/* Load just the public ref projection for read-only ref backend operations. */
int vex_meta_mysql_state_load_refs(struct vex_meta_mysql *db, const char *repo_id,
				    const struct git_hash_algo *hash_algo,
				    struct vex_meta_state *state);
int vex_meta_mysql_state_save_locked(struct vex_meta_mysql *db,
				      const char *repo_id,
				      const struct git_hash_algo *hash_algo,
				      const struct vex_meta_state *state);

#endif /* VEX_META_MYSQL_H */
