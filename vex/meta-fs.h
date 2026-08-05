#ifndef VEX_META_FS_H
#define VEX_META_FS_H

#include "vex/store.h"

struct vex_meta_fs;
struct vex_meta_fs_lock;
struct vex_meta_state;

int vex_meta_fs_open(struct vex_meta_fs **meta, const char *root,
		     const char *repo_id, const struct git_hash_algo *hash_algo);
void vex_meta_fs_close(struct vex_meta_fs *meta);
int vex_meta_fs_ensure(struct vex_meta_fs *meta);
int vex_meta_fs_lock(struct vex_meta_fs *meta, struct vex_meta_fs_lock *lock);
void vex_meta_fs_unlock(struct vex_meta_fs_lock *lock);
int vex_meta_fs_load(struct vex_meta_fs *meta, struct vex_meta_state *state);
int vex_meta_fs_save_locked(struct vex_meta_fs *meta,
			    struct vex_meta_fs_lock *lock,
			    const struct vex_meta_state *state);
void vex_meta_state_init(struct vex_meta_state *state);
void vex_meta_state_release(struct vex_meta_state *state);

#endif /* VEX_META_FS_H */
