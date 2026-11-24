// SPDX-License-Identifier: GPL-2.0-only
/*
 * A shared cache map refused for its volume (<switch/cryptex_map.h>), debug kernels: the first
 * SWITCH_CRYPTEX_MAP_LINES refusals of a boot. A lookup the exemption did not make is printed as error -1 on "-".
 */
#include <switch/cryptex_map.h>

#include <os/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mount.h>
#include <sys/proc_internal.h>
#include <sys/vnode.h>

#if DEVELOPMENT || DEBUG

static uint32_t switch_cryptex_map_lines;

static const char *switch_cryptex_mount_name(vnode_t vp)
{
    const mount_t mp = vp != NULLVP ? vnode_mount(vp) : NULL;
    return mp != NULL ? vfs_statfs(mp)->f_mntonname : "-";
}

/* The exemption keeps no result from its Cryptexes/OS lookup, so it is made again here, under the same condition (the
 * Preboot lookup succeeded on another volume than the cache's). */
void switch_cryptex_map_refused(proc_t p, vnode_t cache, vnode_t preboot, int preboot_error)
{
    if (__atomic_fetch_add(&switch_cryptex_map_lines, 1u, __ATOMIC_RELAXED) >= SWITCH_CRYPTEX_MAP_LINES) {
        return;
    }
    vnode_t cryptex = NULLVP;
    int cryptex_error = -1;
    if (preboot_error == 0 && preboot != NULLVP && vnode_mount(cache) != vnode_mount(preboot)) {
        cryptex_error = vnode_lookup(SWITCH_CRYPTEX_PREBOOT_DIR "/OS", 0, &cryptex, vfs_context_current());
    }
    const bool found = cryptex_error == 0 && cryptex != NULLVP;
    os_log(OS_LOG_DEFAULT,
           "SwitchCryptex: map refused pid %d (%s) cache on %s preboot lookup %d on %s cryptex lookup %d on %s "
           "same-volume %d read-only %d",
           proc_getpid(p), proc_best_name(p), switch_cryptex_mount_name(cache), preboot_error,
           switch_cryptex_mount_name(preboot_error == 0 ? preboot : NULLVP), cryptex_error,
           switch_cryptex_mount_name(found ? cryptex : NULLVP), found && vnode_mount(cryptex) == vnode_mount(cache),
           found && (vfs_flags(vnode_mount(cryptex)) & MNT_RDONLY) != 0);
    if (found) {
        (void)vnode_put(cryptex);
    }
}

#endif /* DEVELOPMENT || DEBUG */
