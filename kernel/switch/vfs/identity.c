// SPDX-License-Identifier: GPL-2.0-only
/*
 * FrontBoard admits a daemon as a system process (lockdownd's Trust alert, SpringBoard's MIG services) only if statfs of
 * its executable gives the f_fsid of /System or of /private/preboot: BaseBoard's BSPathExistsOnSystemPartition, through
 * -[NSFileManager attributesOfFileSystemForPath:]. So statfs describes the volume a file stands in for:
 *  - iOS 27's pretranslated Data copies stand in for System files: the System volume.
 *  - The cryptex mounts stand in for the grafts real iOS makes on Preboot (SwitchRawMount.h): Preboot.
 * stat keeps a cryptex file's own device, unlike a graft's: its file ids are the cryptex volume's, and
 * fsgetpath(st_dev, st_ino), which out-of-process symbolication finds the shared cache by, resolves only there.
 */
#include <sys/mount_internal.h>
#include <sys/vnode_internal.h>

#include <switch/identity.h>
#include <switch/pretranslated.h>

extern mount_t native_owner_cryptex_mount[2];
extern mount_t native_owner_cryptex_host;

mount_t switch_vfs_statfs_mount(vnode_t vp)
{
    const mount_t mp = vp->v_mount;
    const mount_t host = native_owner_cryptex_host;
    if (mp == NULL) {
        return NULL;
    }
    if (host != NULL && (mp == native_owner_cryptex_mount[0] || mp == native_owner_cryptex_mount[1])) {
        return host;
    }
    const mount_t system = switch_pretranslated_volume(vp);
    return system != NULL ? system : mp;
}
