// SPDX-License-Identifier: GPL-2.0-only
#include "prefetch_internal.h"
#include "prefetch_pace.h"

#include <sys/mount.h>
#include <sys/mount_internal.h>
#include <sys/vnode.h>

bool switch_prefetch_vnode_meta(vnode_t vp, mount_t expected_mount, uint64_t expected_file_id, vfs_context_t ctx,
                                uint64_t *file_id_out, struct switch_prefetch_file_meta *meta_out)
{
    if (vp == NULLVP || expected_mount == NULL || vnode_mount(vp) != expected_mount || !vnode_isreg(vp) ||
        vnode_isnamedstream(vp) || vnode_issystem(vp) || vnode_isswap(vp) || vnode_isrecycled(vp)) {
        return false;
    }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_fileid);
    VATTR_WANTED(&va, va_data_size);
    VATTR_WANTED(&va, va_modify_time);
    VATTR_WANTED(&va, va_nlink);
    if (vnode_getattr(vp, &va, ctx) != 0 || !VATTR_IS_SUPPORTED(&va, va_fileid) ||
        !VATTR_IS_SUPPORTED(&va, va_data_size) || !VATTR_IS_SUPPORTED(&va, va_modify_time)) {
        return false;
    }
    const bool nlink_supported = VATTR_IS_SUPPORTED(&va, va_nlink) != 0;
    if (!switch_prefetch_vnode_eligible(true, false, false, false, vnode_isrecycled(vp) != 0,
                                        vnode_mount(vp) == expected_mount, va.va_fileid, expected_file_id,
                                        va.va_data_size, nlink_supported, va.va_nlink)) {
        return false;
    }
    if (file_id_out != NULL) {
        *file_id_out = va.va_fileid;
    }
    meta_out->file_size = va.va_data_size;
    meta_out->mtime_sec = va.va_modify_time.tv_sec;
    meta_out->mtime_nsec = va.va_modify_time.tv_nsec;
    return true;
}

mount_t switch_prefetch_busy_mount(mount_t expected_mp, fsid_t fsid)
{
    if (expected_mp == NULL) {
        return NULL;
    }
    /* Look up under mount_list_lock + mount_iterref so a forced unmount cannot free expected_mp before vfs_busy. */
    mount_t mp = mount_list_lookupby_fsid(&fsid, 0, 1);
    if (mp == NULL) {
        return NULL;
    }
    if (mp != expected_mp || vfs_busy(mp, LK_NOWAIT) != 0) {
        mount_iterdrop(mp);
        return NULL;
    }
    mount_iterdrop(mp);
    return mp;
}

bool switch_prefetch_vget_regular(mount_t mp, fsid_t fsid, uint64_t file_id, vfs_context_t ctx, vnode_t *vp_out,
                                  struct switch_prefetch_file_meta *meta_out)
{
    *vp_out = NULLVP;
    if (mp == NULL || file_id == 0u) {
        return false;
    }
    /* Hold vfs_busy only across VFS_VGET so unmount never blocks on replay or save. */
    mount_t busy_mp = switch_prefetch_busy_mount(mp, fsid);
    if (busy_mp == NULL) {
        return false;
    }
    vnode_t vp = NULLVP;
    const int err = VFS_VGET(busy_mp, (ino64_t)file_id, &vp, ctx);
    vfs_unbusy(busy_mp);
    if (err != 0 || vp == NULLVP) {
        return false;
    }
    if (!switch_prefetch_vnode_meta(vp, busy_mp, file_id, ctx, NULL, meta_out)) {
        vnode_put(vp);
        return false;
    }
    *vp_out = vp;
    return true;
}
