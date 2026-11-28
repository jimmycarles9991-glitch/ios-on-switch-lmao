// SPDX-License-Identifier: GPL-2.0-only
/*
 * An iPad grafts the OS cryptex onto Preboot, so its dyld caches pass the shared region's volume check. Here the
 * cryptex is a mount of its own (SwitchRawMount.h), which the check accepts at /private/preboot/Cryptexes/OS. iOS
 * 17's dyld has no cryptex fallback for the DriverKit cache: it opens /System/DriverKit/System/Library/dyld, which the
 * boot's bindfs union shows. Such a vnode is judged by the file behind it for the volume only. dyld's code signature
 * (F_ADDFILESIGS on its fd) and the pages stay on the union's vnode, so every other check still sees that vnode.
 *
 * iOS 27's load-time translator rewrites any cache /chosen/a64x-cache-pretranslated does not list with the main
 * cache's plan, which is wrong for the DriverKit cache: a DriverKit process maps only the one translated offline.
 */

#include <switch/identity.h>
#include <switch/shared_cache.h>

#include <os/log.h>
#include <sys/mount_internal.h>
#include <sys/proc_internal.h>
#include <sys/vnode_internal.h>

#if CONFIG_A64X
#include <switch/a64x.h>
#endif

extern int bindfs_getbackingvnode(vnode_t in_vp, vnode_t *out_vpp);

bool switch_shared_cache_on_cryptex(proc_t p, vnode_t vp, mount_t cryptex)
{
    const mount_t mp = vnode_mount(vp);
    if (mp == cryptex) {
        return true;
    }
    vnode_t file = NULLVP;
    if (bindfs_boot_union_host(mp) == NULL || bindfs_getbackingvnode(vp, &file) != 0) {
        return false;
    }
    const bool on = vnode_mount(file) == cryptex;
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_DIAG_SHARED_CACHE) && SWITCH_DIAG_SHARED_CACHE
    printf("SwitchSharedCache: %s[%d] maps %s through the boot union at %s: its file is on %s %s: %s\n", p->p_comm,
           proc_getpid(p), vp->v_name ? vp->v_name : "?", vfs_statfs(mp)->f_mntonname,
           vfs_statfs(vnode_mount(file))->f_fstypename, vfs_statfs(vnode_mount(file))->f_mntonname,
           on ? "the OS cryptex" : "not the OS cryptex, refused");
#else
    (void)p;
#endif
    vnode_put(file);
    return on;
}

#if CONFIG_A64X
bool switch_shared_cache_unusable(proc_t p, vnode_t vp, bool driverkit)
{
    if (!driverkit) {
        return false;
    }
    const bool translated = a64x_cache_pretranslated(vp);
#if (DEVELOPMENT || DEBUG) && defined(SWITCH_DIAG_SHARED_CACHE) && SWITCH_DIAG_SHARED_CACHE
    printf("SwitchSharedCache: %s[%d] maps the DriverKit cache %s from %s %s: %s\n", p->p_comm, proc_getpid(p),
           vp->v_name ? vp->v_name : "?", vfs_statfs(vnode_mount(vp))->f_fstypename,
           vfs_statfs(vnode_mount(vp))->f_mntonname, translated ? "translated offline" : "not translated");
#endif
    if (translated) {
        return false;
    }
    os_log_error(OS_LOG_DEFAULT,
                 "SwitchSharedCache: %s[%d]: DriverKit shared cache %s is not pretranslated: not mapped", p->p_comm,
                 proc_getpid(p), vp->v_name ? vp->v_name : "?");
    return true;
}
#endif
