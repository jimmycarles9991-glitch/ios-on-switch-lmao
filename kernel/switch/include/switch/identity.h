// SPDX-License-Identifier: GPL-2.0-only
/*
 * The volume statfs and fstatfs describe for a file that stands in for one of Apple's (kernel/switch/vfs/identity.c),
 * for every XNU.
 */
#ifndef SWITCH_IDENTITY_H
#define SWITCH_IDENTITY_H

#include <sys/cdefs.h>

struct mount;
struct vnode;

__BEGIN_DECLS

struct mount *switch_vfs_statfs_mount(struct vnode *vp);

__END_DECLS

#endif /* SWITCH_IDENTITY_H */
