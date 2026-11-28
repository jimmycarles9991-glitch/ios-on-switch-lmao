// SPDX-License-Identifier: GPL-2.0-only
/* Between pretranslated.c, which captures the tree, and pretranslated_updates.c, which applies a deploy's updates to
 * its index first. */
#pragma once
#if CONFIG_A64X
#include <sys/mount.h>
#include <sys/vnode.h>
#include <switch/auth_fs.h>

#include "../../../kits/SwitchPretranslated/switch_pretranslated_updates.h"

#define SWITCH_PT_LOOKUP (VNODE_LOOKUP_NOFOLLOW_ANY | VNODE_LOOKUP_NOCROSSMOUNT)
/* Folders from the update tree's root down to one update, the root included. */
#define SWITCH_PT_MAX_UPDATE_DEPTH 16u

struct switch_pt_volumes {
    mount_t system;
    mount_t data;
    const struct switch_cdhash_list *trusted;
};

/* What the tables protect once the updates are in: per update taken, its System file and the tree copy it replaced;
 * the folders that hold the updates taken. */
struct switch_pt_applied {
    uint64_t systems[SWITCH_PT_MAX_UPDATES];
    uint64_t replaced[SWITCH_PT_MAX_UPDATES];
    uint32_t updates;
    uint64_t dirs[SWITCH_PT_MAX_UPDATES * SWITCH_PT_MAX_UPDATE_DEPTH];
    uint32_t dir_count;
};

int switch_pt_owned_by_root(vnode_t vp, enum vtype type, vfs_context_t ctx, uint64_t *id, off_t *size);

/* The boot package's updates, applied to the index's bytes (`raw`, which `index` was parsed from) before its tables are
 * built from it. Nonzero (the list's switch_pt_error): a list there that does not parse, and the tree must stay off. */
int switch_pt_apply_updates(vnode_t tree, uint8_t *raw, const struct switch_pt_index *index,
                            const struct switch_pt_volumes *volumes, struct switch_pt_applied *applied,
                            vfs_context_t ctx);
#endif
