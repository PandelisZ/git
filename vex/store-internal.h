#ifndef VEX_STORE_INTERNAL_H
#define VEX_STORE_INTERNAL_H

#include "vex/store.h"
#include "vex/blob-fs.h"
#include "vex/blob-s3.h"
#include "vex/meta-fs.h"
#include "vex/meta-mysql.h"
#include "lockfile.h"

#include <time.h>

struct vex_meta_fs;
struct vex_blob_fs;

enum vex_epoch_state {
	VEX_EPOCH_OPEN = 1,
	VEX_EPOCH_SEALED,
	VEX_EPOCH_SWEEPABLE,
	VEX_EPOCH_SWEPT,
	VEX_EPOCH_ABORTED,
};

struct vex_meta_epoch {
	uint64_t id;
	enum vex_epoch_state state;
	char *push_id;
	/* MySQL state snapshots preserve this so crash TTLs never reset on save. */
	char *created_at;
	/*
	 * A live receive worker advances this durable activity timestamp.  Stale
	 * epoch recovery must age this value rather than the allocation time so a
	 * large, continuously receiving push is not mistaken for a dead worker.
	 */
	char *receive_heartbeat_at;
	/* The sealed-generation grace must survive normal state rewrites too. */
	char *sealed_at;
};

struct vex_meta_object {
	struct object_id oid;
	enum object_type type;
	unsigned long size;
	uint64_t epoch;
	enum vex_object_state state;
};

struct vex_meta_epoch_object {
	uint64_t epoch;
	struct object_id oid;
};

struct vex_meta_ref {
	char *name;
	struct vex_ref_target target;
	uint64_t version;
};

struct vex_meta_ref_update {
	uint64_t id;
	struct vex_ref_update update;
};

struct vex_meta_commit_parent {
	struct object_id commit_oid;
	unsigned int position;
	struct object_id parent_oid;
};

struct vex_meta_state {
	uint64_t next_epoch;
	uint64_t next_update;
	struct vex_meta_epoch *epochs;
	size_t epochs_nr;
	size_t epochs_alloc;
	struct vex_meta_object *objects;
	size_t objects_nr;
	size_t objects_alloc;
	struct vex_meta_epoch_object *epoch_objects;
	size_t epoch_objects_nr;
	size_t epoch_objects_alloc;
	struct vex_meta_ref *refs;
	size_t refs_nr;
	size_t refs_alloc;
	struct vex_meta_ref_update *ref_updates;
	size_t ref_updates_nr;
	size_t ref_updates_alloc;
	struct vex_meta_commit_parent *commit_parents;
	size_t commit_parents_nr;
	size_t commit_parents_alloc;
};

struct vex_meta_fs_lock {
	struct lock_file lock;
	int held;
};

/*
 * A store write holds one metadata serialization boundary.  Filesystem
 * stores use a lockfile; MySQL stores use a transaction scoped repository
 * row lock.  Keeping this shape here lets the transactional store code stay
 * backend-neutral without making raw blob bytes part of the SQL driver.
 */
struct vex_meta_lock {
	struct vex_meta_fs_lock fs;
	int held;
};

struct vex_store {
	enum vex_store_backend backend;
	char *root;
	char *repo_id;
	const struct git_hash_algo *hash_algo;
	struct vex_meta_fs *meta;
	struct vex_meta_mysql *meta_mysql;
	struct vex_blob_fs *blob_fs;
	struct vex_blob_s3 *blob_s3;
	/* A successful repository metadata ensure is immutable per store handle. */
	unsigned int metadata_checked:1;
	unsigned int blob_s3_checked:1;
	/* Avoid repeating the stale-epoch scan on every operation in one store. */
	unsigned int epoch_recovery_checked:1;
};

/*
 * Receive-side data held only until the next bounded S3/MySQL append.  It is
 * process-local by design: a receive helper flushes before its inherited ODB
 * transaction ends, while the parent sees only durable quarantine metadata.
 */
struct vex_pending_stage_object {
	struct object_id oid;
	enum object_type type;
	unsigned long size;
	void *data;
};

struct vex_epoch {
	struct vex_store *store;
	uint64_t id;
	int active;
	/* An attached child must never compensate its parent's durable epoch. */
	int owner;
	/* Local throttle only; MySQL is the durable heartbeat authority. */
	time_t last_heartbeat;
	struct vex_pending_stage_object *pending_stages;
	size_t pending_stages_nr;
	size_t pending_stages_alloc;
	size_t pending_stage_bytes;
	unsigned int flushing_pending_stages:1;
	/*
	 * The receive-pack WAL path stages one immutable pack/idx pair rather than
	 * an unbounded number of loose S3 objects.  Its membership remains in
	 * MySQL; this transient handle tells ref publication/compensation which
	 * pack epoch protocol to use.
	 */
	struct object_id receive_pack_id;
	unsigned int receive_pack_staged:1;
	unsigned int receive_pack_promoted:1;
};

struct vex_ref_transaction {
	struct vex_store *store;
	struct vex_epoch *epoch;
	struct vex_ref_update *updates;
	size_t updates_nr;
	size_t updates_alloc;
	int committed;
};

void vex_ref_target_release(struct vex_ref_target *target);
int vex_ref_target_dup(struct vex_ref_target *dst,
			 const struct vex_ref_target *src);
int vex_ref_target_equal(const struct vex_ref_target *a,
			   const struct vex_ref_target *b);
void vex_ref_update_release(struct vex_ref_update *update);
int vex_ref_update_dup(struct vex_ref_update *dst,
			 const struct vex_ref_update *src);

#endif /* VEX_STORE_INTERNAL_H */
