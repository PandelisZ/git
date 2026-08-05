#ifndef ODB_SOURCE_VEX_H
#define ODB_SOURCE_VEX_H

#include "odb/source.h"

struct odb_source_vex;
struct vex_store;
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
bool repo_uses_vex_storage(const struct repository *repo);

#endif /* ODB_SOURCE_VEX_H */
