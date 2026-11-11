// SPDX-License-Identifier: GPL-2.0-only
/*
 * The volume statfs, fstatfs and getfsstat64 describe for a file that stands in for one of Apple's
 * (kernel/switch/vfs/identity.c), for every XNU.
 */
#ifndef SWITCH_IDENTITY_H
#define SWITCH_IDENTITY_H

#include <sys/cdefs.h>

struct mount;
struct vnode;

__BEGIN_DECLS

struct mount *switch_vfs_statfs_mount(struct vnode *vp);
struct mount *switch_vfs_volume_host(struct mount *mp);
struct mount *bindfs_boot_union_host(struct mount *mp);

__END_DECLS

#endif /* SWITCH_IDENTITY_H */
