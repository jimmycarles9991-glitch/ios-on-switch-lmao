// SPDX-License-Identifier: GPL-2.0-only
/*
 * The console's steps in bsd_init (<switch/boot.h>): the kexts it needs before the root mounts, the SD disk once IOBSD
 * exists, and the owner-signed root policy beside Apple's root authentication.
 */
#include <sys/mount_internal.h>
#include <sys/systm.h>
#include <sys/vnode_internal.h>
#include <libkern/OSKextLib.h>

#include "SwitchDiag.h"
#include "SwitchPowerTrail.h"
#include <switch/boot.h>
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern kern_return_t OSKextLoadKextWithIdentifier(const char *identifier);

extern int native_verified_block_os_root_authorized(dev_t, dev_t);

static volatile int switch_boot_apfs_status = -1;
static volatile uint32_t switch_boot_apfs_registered = 0;
static volatile uintptr_t switch_boot_apfs_vfsops = 0;
static volatile int switch_boot_mobile_graphics_status = -1;

static volatile int switch_boot_root_trust_enabled = 1;
static volatile int switch_boot_root_accepted_value = 0;
static volatile int switch_boot_root_apple_autherr = -1;
static volatile int switch_boot_root_decision = -1;
static volatile int switch_boot_root_readonly = -1;
static volatile dev_t switch_boot_root_mount_device = NODEV;
static volatile dev_t switch_boot_root_selected_device = NODEV;

__attribute__((noinline)) static void switch_boot_apfs_checkpoint(void)
{
    __asm__ volatile("" ::: "memory");
}

static void switch_boot_load_apfs(void)
{
    switch_boot_apfs_status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
    mount_list_lock();
    for (struct vfstable *entry = vfsconf; entry; entry = entry->vfc_next) {
        if (!strncmp(entry->vfc_name, "apfs", MFSNAMELEN)) {
            switch_boot_apfs_registered = 1;
            switch_boot_apfs_vfsops = (uintptr_t)entry->vfc_vfsops;
            break;
        }
    }
    mount_list_unlock();
    switch_boot_apfs_checkpoint();
}

__attribute__((noinline)) static void switch_boot_mobile_graphics_checkpoint(void)
{
    __asm__ volatile("" ::: "memory");
}

static void switch_boot_load_mobile_graphics(void)
{
    switch_boot_mobile_graphics_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily");
    switch_boot_mobile_graphics_checkpoint();
    if (switch_boot_mobile_graphics_status == 0) {
        extern void native_legacy_display_probe(void);
        native_legacy_display_probe();
    }
}

void switch_boot_bufferinit_done(void)
{
    extern void sheng_pmgr_run(void);
    sheng_pmgr_run();
}

void switch_boot_vfs_ready(void)
{
    switch_boot_load_apfs();
    switch_boot_load_mobile_graphics();
    /*
     * After IOMobileGraphicsFamily: IOGPUFamily then brings no new dependency (AppleARMPlatform, AMFI, IOSurface,
     * IOReportFamily are loaded) and has no personalities. QuartzCore polls 30 s for an IOAcceleratorES otherwise.
     */
    extern void switch_accel_stub_publish(void);
    switch_accel_stub_publish();
}

kern_return_t switch_boot_iokit_bsd_ready(kern_return_t result)
{
    /*
     * The original IOSurface personality requires the genuine IOBSD resource. The default-off framebuffer instrument
     * may start only after publication.
     */
    extern void native_legacy_display_late_probe(void);
    native_legacy_display_late_probe();
    return result;
}

void switch_boot_autoconf_done(void)
{
    /*
     * Publish the SD disk only once IOBSD exists. APFS's container start takes its container lock and then waits for
     * IOBSD; APFS's IOBSD-matched service takes the same lock in its start. If the disk appears first, the two IOKit
     * matching threads deadlock (four cores lose that race every time).
     */
    switch_root_boot_prepare();
}

void switch_boot_mountroot_begin(void)
{
    switch_root_event(6, 0, 0);
}

void switch_boot_mountroot_done(int err)
{
    if (!err) {
        switch_power_trail_mark(SWITCH_MARK_BOOT_ROOT);
    }
    switch_root_event(7, err, 0);
    if (err) {
        panic("native original root mount failed: %d", err);
    }
}

__attribute__((noinline)) static void switch_boot_root_accepted_checkpoint(int apple, int owner)
{
    __asm__ volatile("" : : "r"(apple), "r"(owner) : "memory");
}

__attribute__((noinline)) static void switch_boot_root_checkpoint(int apple, int owner, dev_t device)
{
    __asm__ volatile("" : : "r"(apple), "r"(owner), "r"(device) : "memory");
}

int switch_boot_root_authorized(int autherr)
{
    mount_t root_mount = vnode_mount(rootvnode);
    vnode_t device_vnode = vfs_devvp(root_mount);
    dev_t device = device_vnode ? vnode_specrdev(device_vnode) : NODEV;
    int readonly = (vfs_flags(root_mount) & MNT_RDONLY) != 0;
    /* Only the expected missing Apple personalization may use the explicit owner-signed development policy for this
     * exact root. */
    int owner =
        autherr == EAUTH && readonly && device_vnode && native_verified_block_os_root_authorized(device, rootdev);
    if (device_vnode) {
        vnode_put(device_vnode);
    }
    switch_boot_root_mount_device = device;
    switch_boot_root_selected_device = rootdev;
    switch_boot_root_apple_autherr = autherr;
    switch_boot_root_decision = owner;
    switch_boot_root_readonly = readonly;
    switch_boot_root_checkpoint(autherr, owner, device);
    switch_root_event(8, autherr, ((uint64_t)(uint32_t)device << 32) | (unsigned)owner);
    return owner;
}

void switch_boot_root_accepted(int autherr, int owner_authorized)
{
    switch_boot_root_accepted_value = owner_authorized;
    switch_boot_root_accepted_checkpoint(autherr, owner_authorized);
}
