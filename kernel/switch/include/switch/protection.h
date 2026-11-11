// SPDX-License-Identifier: GPL-2.0-only
/*
 * Content protection classes on iOS 27's Data volume (kernel/switch/vfs/protection.c), for the XNU 12377 hooks.
 */
#ifndef SWITCH_PROTECTION_H
#define SWITCH_PROTECTION_H

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mount;
struct uio;
struct vnode;
struct vnode_attr;
struct vfs_context;

__BEGIN_DECLS

/* Before launchd, once Data is mounted at /private/var. */
void switch_protection_capture(void);

/* MNT_CPROTECT for the Data volume, else 0: or-ed into the f_flags statfs and getfsstat copy out. */
uint32_t switch_protection_mount_flags(struct mount *mp);

bool switch_protection_volume(struct vnode *vp);

/* F_GETPROTECTIONCLASS on the Data volume. */
int switch_protection_get(struct vnode *vp, int32_t *pclass, struct vfs_context *ctx);

/* F_SETPROTECTIONCLASS on a local writable volume without MNT_CPROTECT, after its authorization and MAC check. */
int switch_protection_set(struct vnode *vp, int32_t pclass, struct vfs_context *ctx);

/*
 * setattrlist on a volume without MNT_CPROTECT, between authorization and vnode_setattr: begin takes the class off vap
 * once the caller may write the file (F_SETPROTECTIONCLASS's right), end labels the vnode after vnode_setattr.
 */
int switch_protection_setattr_begin(struct vnode *vp, struct vnode_attr *vap, int32_t *pclass, struct vfs_context *ctx);
int switch_protection_setattr_end(struct vnode *vp, int32_t pclass, struct vfs_context *ctx);

/* ATTR_CMN_DATA_PROTECT_FLAGS on a local writable volume without MNT_CPROTECT, whatever the filesystem said. */
void switch_protection_getattr(struct mount *mp, struct vnode *vp, uint32_t commonattr, struct vnode_attr *vap,
                               struct vfs_context *ctx);

/* getattrlistbulk asking the Data volume for classes packs each entry through getattrlist, where labels are read. */
bool switch_protection_bulk_by_entry(struct vnode *dvp, uint32_t commonattr);

/* vn_create: take the class a create asks for off vap before the filesystem sees it, then label the new vnode. */
int switch_protection_create_begin(struct vnode *dvp, struct vnode_attr *vap, int32_t *pclass);
int switch_protection_create_end(struct vnode *dvp, struct vnode *vp, int32_t pclass, struct vfs_context *ctx);

/* listxattr and flistxattr: vn_listxattr without the label. */
int switch_protection_listxattr(struct vnode *vp, struct uio *uio, size_t *size, int options, struct vfs_context *ctx);

__END_DECLS

#endif /* SWITCH_PROTECTION_H */
