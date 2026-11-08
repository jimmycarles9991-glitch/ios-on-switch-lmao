// SPDX-License-Identifier: GPL-2.0-only
/*
 * FSIOC_AUTH_FS answered for the files a trust module the boot package carries authenticates
 * (kernel/switch/vfs/auth_fs.c), for VNOP_IOCTL in every XNU.
 */
#ifndef SWITCH_AUTH_FS_H
#define SWITCH_AUTH_FS_H

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

#include <sys/fsctl.h>

struct vnode;

__BEGIN_DECLS

bool switch_auth_fs(struct vnode *vp, const void *data);
bool switch_trust_module_uuid(const uint8_t *cdhash, uint8_t uuid[16]);

/* VNOP_IOCTL: true when FSIOC_AUTH_FS succeeds without asking the vnode's file system. */
static inline bool switch_auth_fs_answers(struct vnode *vp, unsigned long command, const void *data)
{
    return __improbable(command == FSIOC_AUTH_FS) && switch_auth_fs(vp, data);
}

__END_DECLS

#endif /* SWITCH_AUTH_FS_H */
