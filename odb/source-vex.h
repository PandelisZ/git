#ifndef ODB_SOURCE_VEX_H
#define ODB_SOURCE_VEX_H

#include "odb/source.h"

struct object_id;
struct strbuf;
struct odb_source_vex;
struct vex_store;
struct vex_epoch;
struct odb_transaction;
struct odb_source_packed;
struct packed_git;
struct odb_vex_default_clone_pack;
struct repository;

/*
 * The Vex source keeps its Git-directory path for compatibility with
 * callers that need a thin on-disk gitdir, while its object bytes are owned
 * by the Vex store selected by the repository's ref-storage URI.
 */
struct odb_source *odb_source_vex_new(struct object_database *odb,
				       const char *path, bool local);

/*
 * Open the Vex store selected by the repository's vex:// URI.  gitdir is
 * used only to derive the local fs-backend fallback root.
 */
int vex_store_open_for_repository(struct repository *repo, const char *gitdir,
					    struct vex_store **out);

bool odb_source_is_vex(const struct odb_source *source);
struct odb_source_packed *odb_source_vex_packed_store(struct odb_source *source);

/*
 * Return only a complete current relay artifact.  This intentionally excludes
 * a retained raw base, which is safe for normal object lookup and pack-object
 * construction but can be stale after an incremental overlay.
 */
struct odb_source_packed *odb_source_vex_precomputed_relay_store(
	struct odb_source *source);
bool repo_uses_vex_storage(const struct repository *repo);

/*
 * Materialize the current, policy-scoped default-clone artifact into an
 * independent verified packed source.  The returned handle owns both that
 * cache and its metadata lease; close it before closing `source`.  It never
 * replaces the Vex source's complete-snapshot cache or its ordinary ODB
 * lookup path.
 */
int odb_source_vex_open_default_clone_pack(
	struct odb_source *source, struct odb_vex_default_clone_pack **out);
struct odb_source_packed *odb_source_vex_default_clone_pack_packed(
	const struct odb_vex_default_clone_pack *pack);
void odb_source_vex_close_default_clone_pack(
	struct odb_vex_default_clone_pack *pack);

/*
 * Return a local, post-receive delta pack only when it was published for the
 * exact old/new pair. The returned pack is owned by `source` and remains
 * valid until that source is closed or prepared again. It is a disposable
 * NVMe cache, never an authoritative Vex artifact.
 */
struct packed_git *odb_source_vex_fetch_delta_cache_pack(
	struct odb_source *source, const struct object_id *old_oid,
	const struct object_id *new_oid);

/*
 * Best-effort post-seal cache for a small normal fast-forward update. It is
 * local scratch only; a failure cannot affect durable Vex objects or refs.
 */
void odb_source_vex_maybe_build_fetch_delta_cache(
	struct repository *repo, const struct object_id *old_oid,
	const struct object_id *new_oid);

/*
 * Receive-pack starts its Vex ODB transaction in the parent process.  These
 * helpers let the Vex ref backend join that exact durable epoch rather than
 * allocating a second one.  `attach` returns 1 when a Vex receive transaction
 * was found, 0 when it was not, and a negative value on failure.
 */
int odb_vex_receive_transaction_attach(struct odb_transaction *transaction,
				       struct vex_store *store,
				       struct vex_epoch **epoch);
void odb_vex_receive_transaction_finish(struct odb_transaction *transaction);
void odb_vex_receive_transaction_abort(struct odb_transaction *transaction);
bool odb_vex_receive_transaction_is_active(
		const struct odb_transaction *transaction);

/*
 * Explicit, seed-oriented pack WAL handoff.  receive-pack asks for a trusted
 * per-epoch object directory, runs index-pack into it, then adopts the
 * verified pack before connectivity checks and hooks run.  Children inherit
 * only the pack id and derive the scratch location from the Vex epoch.
 */
bool odb_vex_receive_pack_wal_enabled(
		const struct odb_transaction *transaction);
int odb_vex_receive_transaction_prepare_pack_wal(
		struct odb_transaction *transaction, struct strbuf *objects_dir);
int odb_vex_receive_transaction_adopt_pack(
		struct odb_transaction *transaction, const char *objects_dir,
		const struct object_id *pack_id);

#endif /* ODB_SOURCE_VEX_H */
