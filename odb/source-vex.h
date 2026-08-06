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
bool repo_uses_vex_storage(const struct repository *repo);

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
