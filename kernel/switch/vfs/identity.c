// SPDX-License-Identifier: GPL-2.0-only
/*
 * FrontBoard admits a daemon as a system process (lockdownd's Trust alert, SpringBoard's MIG services) only if statfs of
 * its executable gives the f_fsid of /System or of /private/preboot: BaseBoard's BSPathExistsOnSystemPartition, through
 * -[NSFileManager attributesOfFileSystemForPath:]. LaunchServices files a bundle under the container of the volume
 * getmntinfo(3) names for its device, and lsd's first-boot work removes every container but the root, system data,
 * Preboot and user data volumes after each reboot. So statfs and getfsstat64 describe the volume a file stands in for:
 *  - iOS 27's pretranslated Data copies stand in for System files: the System volume.
 *  - The cryptex mounts stand in for the grafts real iOS makes on Preboot (SwitchRawMount.h): Preboot.
 *  - The boot's bindfs unions (the overlay's /Applications among them) stand in for the directories they cover: those
 *    directories' volume.
 * stat keeps a file's own device: its file ids are its own volume's, and fsgetpath(st_dev, st_ino), which out-of-process
 * symbolication finds the shared cache by, resolves only there.
 */
#include <sys/mount_internal.h>
#include <sys/vnode_internal.h>

#include <switch/identity.h>
#include <switch/pretranslated.h>

extern mount_t native_owner_cryptex_mount[2];
extern mount_t native_owner_cryptex_host;

mount_t switch_vfs_volume_host(mount_t mp)
{
    const mount_t host = native_owner_cryptex_host;
    if (host != NULL && (mp == native_owner_cryptex_mount[0] || mp == native_owner_cryptex_mount[1])) {
        return host;
    }
    return bindfs_boot_union_host(mp);
}

mount_t switch_vfs_statfs_mount(vnode_t vp)
{
    const mount_t mp = vp->v_mount;
    if (mp == NULL) {
        return NULL;
    }
    const mount_t host = switch_vfs_volume_host(mp);
    if (host != NULL) {
        return host;
    }
    const mount_t system = switch_pretranslated_volume(vp);
    return system != NULL ? system : mp;
}
