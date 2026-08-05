#ifndef VEX_BLOB_FS_H
#define VEX_BLOB_FS_H

#include "vex/store.h"

struct vex_blob_fs;
struct vex_meta_state;

int vex_blob_fs_open(struct vex_blob_fs **blob, const char *root,
		     const char *repo_id, const struct git_hash_algo *hash_algo);
void vex_blob_fs_close(struct vex_blob_fs *blob);
int vex_blob_fs_ensure(struct vex_blob_fs *blob);
int vex_blob_fs_put_staging(struct vex_blob_fs *blob, uint64_t epoch,
			    const struct object_id *oid,
			    const void *data, unsigned long size);
int vex_blob_fs_promote_epoch(struct vex_blob_fs *blob,
			      const struct vex_meta_state *state, uint64_t epoch);
int vex_blob_fs_compensate_epoch(struct vex_blob_fs *blob, uint64_t epoch);
int vex_blob_fs_has_final(struct vex_blob_fs *blob,
			  const struct object_id *oid);
int vex_blob_fs_read_final(struct vex_blob_fs *blob,
			   const struct object_id *oid,
			   void **data, unsigned long *size);
int vex_blob_fs_read_staging(struct vex_blob_fs *blob, uint64_t epoch,
			     const struct object_id *oid,
			     void **data, unsigned long *size);

#endif /* VEX_BLOB_FS_H */
