// SPDX-License-Identifier: GPL-2.0-only
/*
 * FSIOC_AUTH_FS answered for the files whose CDHashes the boot package lists in /chosen (kernel/switch/vfs/auth_fs.c),
 * for VNOP_IOCTL in every XNU.
 */
#ifndef SWITCH_AUTH_FS_H
#define SWITCH_AUTH_FS_H

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

#include <sys/fsctl.h>

#include "../../../../kits/SwitchCDHash/switch_cdhash.h"

struct vnode;

__BEGIN_DECLS

bool switch_auth_fs(struct vnode *vp, const void *data);
/* Once the cryptexes are mounted, before launchd (SwitchRawMount.h): their CDHash lists. */
void switch_auth_fs_init(void);
/* /chosen's CDHash list `property` (tools/package/adt_edit.py), empty when it is missing or not whole CDHashes. */
struct switch_cdhash_list switch_chosen_cdhashes(const char *property);

/* VNOP_IOCTL: true when FSIOC_AUTH_FS succeeds without asking the vnode's file system. */
static inline bool switch_auth_fs_answers(struct vnode *vp, unsigned long command, const void *data)
{
    return __improbable(command == FSIOC_AUTH_FS) && switch_auth_fs(vp, data);
}

__END_DECLS

#endif /* SWITCH_AUTH_FS_H */
