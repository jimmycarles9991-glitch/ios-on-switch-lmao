/* Explicit raw profile only. The physical APFS store is writable; each
 * filesystem mount has its own policy. No change to the FAT boot profile. */
#include "SwitchFiles.h"
#include "../../drivers/NativeSwitchBlock/SwitchCryptex.h"
#include "storage/block_backend.h"
#include <sys/fcntl.h>
#include <switch/auth_fs.h>

#define SWITCH_RAW_DATA_UUID    "D1B1D053-D0A6-496C-8625-A947A3DA863E"
#define SWITCH_RAW_PREBOOT_UUID "4AA4A761-5138-439D-9A30-D4138F6887FA"
extern int switch_raw_volume_status(const char *, dev_t *, char *, size_t, uint64_t *);
extern int native_owner_publish_raw_boot_manifest(dev_t, const char *, dev_t, const char *);

static int native_switch_raw_wait(const char *uuid, dev_t *device, char path[64])
{
    for (unsigned attempt = 0;; ++attempt) {
        uint64_t bytes = 0;
        int status = switch_raw_volume_status(uuid, device, path, 64, &bytes);
        if (status == SWITCH_BLOCK_OK)
            return bytes ? 0 : EINVAL;
        if (status != SWITCH_BLOCK_NOT_READY)
            return status == SWITCH_BLOCK_NOMEM ? ENOMEM : status == SWITCH_BLOCK_IO ? EIO : EINVAL;
        if (attempt == 100)
            return ETIMEDOUT;
        struct timespec interval = {.tv_sec = 0, .tv_nsec = 100000000};
        (void)msleep((void *)&native_owner_cache_stage, NULL, PWAIT, "switchRaw", &interval);
    }
}

static int native_switch_raw_mount(proc_t p, user_addr_t scratch, const char *path, const char *device_path,
                                   dev_t expected, int readonly, mount_t *observed_mount)
{
    uint8_t payload[0x150] = {0};
    user64_addr_t spec = scratch + 0x160;
    uint64_t flags = readonly ? MNT_RDONLY : 0;
    uint16_t mode = 1;
    uint32_t anonymous_owner = 99;
    memcpy(payload, &spec, sizeof(spec));
    memcpy(payload + 8, &flags, sizeof(flags));
    memcpy(payload + 16, &mode, sizeof(mode));
    memcpy(payload + 0x144, &anonymous_owner, sizeof(anonymous_owner));
    memcpy(payload + 0x148, &anonymous_owner, sizeof(anonymous_owner));
    struct mount_args request = {
        .type = scratch + 0x100, .path = scratch + 0x120, .flags = (int)flags, .data = scratch + 0x200};
    int32_t result = 0;
    int error;
    if ((error = copyout("apfs", request.type, 5)) || (error = copyout(path, request.path, strlen(path) + 1)) ||
        (error = copyout(device_path, spec, strlen(device_path) + 1)) ||
        (error = copyout(payload, request.data, sizeof(payload))))
        return error;
    error = mount(p, &request, &result);
    if (error)
        return error;
    vnode_t root = NULLVP;
    error = vnode_lookup(path, 0, &root, vfs_context_current());
    if (error)
        return error;
    mount_t mounted = vnode_mount(root);
    vnode_t device = vfs_devvp(mounted);
    dev_t actual = device ? vnode_specrdev(device) : NODEV;
    if (device)
        vnode_put(device);
    uint64_t actual_flags = vfs_flags(mounted);
    if (!vnode_isdir(root) || actual != expected || !!(actual_flags & MNT_RDONLY) != !!readonly ||
        (actual_flags & (MNT_IGNORE_OWNERSHIP | MNT_UNION)) || strcmp(vfs_statfs(mounted)->f_fstypename, "apfs"))
        error = EPERM;
    if (!error && observed_mount)
        *observed_mount = mounted;
    vnode_put(root);
    return error;
}

static int native_switch_raw_directory(proc_t p, user_addr_t scratch, const char *path, mount_t expected, int create)
{
    vnode_t directory = NULLVP;
    int error = vnode_lookup(path, 0, &directory, vfs_context_current());
    if (error == ENOENT && create) {
        struct mkdir_args request = {.path = scratch + 0x900, .mode = 0755};
        int32_t result = 0;
        if ((error = copyout(path, request.path, strlen(path) + 1)))
            return error;
        if ((error = mkdir(p, &request, &result)))
            return error;
        error = vnode_lookup(path, 0, &directory, vfs_context_current());
    }
    if (error)
        return error;
    if (vnode_mount(directory) != expected || !vnode_isdir(directory))
        error = ENOTDIR;
    vnode_put(directory);
    return error;
}

extern volatile int native_bindfs_boot_mount;

/* Read-only bindfs union of `source` over `point`: lookups try `source` first,
 * then fall through to the point's own entries. */
static int native_switch_raw_union(proc_t p, user_addr_t scratch, const char *source, const char *point)
{
    struct mount_args request = {
        .type = scratch + 0xa00, .path = scratch + 0xa20, .flags = MNT_RDONLY | MNT_UNION, .data = scratch + 0xb00};
    int32_t result = 0;
    int error;
    if ((error = copyout("bindfs", request.type, 7)) || (error = copyout(point, request.path, strlen(point) + 1)) ||
        (error = copyout(source, request.data, strlen(source) + 1)))
        return error;
    native_bindfs_boot_mount = 1;
    error = mount(p, &request, &result);
    native_bindfs_boot_mount = 0;
    return error;
}

#define SWITCH_RAW_STEP(step, call)                                  \
    do {                                                             \
        error = (call);                                              \
        if (error) {                                                 \
            printf("SwitchRawBoot: %s failed: %d\n", (step), error); \
            return error;                                            \
        }                                                            \
    } while (0)

/* Mount Preboot read-write, attach and mount the original System/App cryptex
 * images from it, and point launchd's dyld at the System cryptex cache. */
static int native_switch_raw_cache_prepare(proc_t p, user_addr_t scratch, user_addr_t *envp)
{
    vnode_t original_root = rootvnode;
    mount_t os_mount = vnode_mount(original_root);
    vnode_t device = vfs_devvp(os_mount);
    dev_t os_device = device ? vnode_specrdev(device) : NODEV;
    if (device)
        vnode_put(device);
    int error = 0;
    if (!(vfs_flags(os_mount) & MNT_RDONLY) || !native_verified_block_root_authorized(os_device)) {
        printf("SwitchRawBoot: root is not the read-only SD OS volume\n");
        return EAUTH;
    }
    dev_t preboot_device = NODEV;
    char path[64];
    mount_t preboot_mount = NULL;
    SWITCH_RAW_STEP("Preboot volume lookup", native_switch_raw_wait(SWITCH_RAW_PREBOOT_UUID, &preboot_device, path));
    SWITCH_RAW_STEP("/private/preboot lookup",
                    native_switch_raw_directory(p, scratch, "/private/preboot", os_mount, 0));
    SWITCH_RAW_STEP("Preboot mount",
                    native_switch_raw_mount(p, scratch, "/private/preboot", path, preboot_device, 0, &preboot_mount));
    static const char *const directories[] = {"/private/preboot/Cryptexes", "/private/preboot/Cryptexes/OS",
                                              "/private/preboot/Cryptexes/App",
#if XNU_12377_121_6
                                              /* The overlay's union point for the launchd plists 24B119 bootstraps
                                               * through /System/Cryptexes/Supplemental (commpage.c). */
                                              "/private/preboot/Cryptexes/Supplemental"
#endif
    };
    for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i)
        SWITCH_RAW_STEP(directories[i], native_switch_raw_directory(p, scratch, directories[i], preboot_mount, 1));
    dev_t cache_device = NODEV;
    /* Real iOS grafts these cryptexes onto the Preboot volume (libignition), so LaunchServices files their bundles
     * under the Preboot container and keeps them. We attach-and-mount instead, so record each cryptex mount_t and
     * the Preboot mount they stand in a graft on: kern_exec's launch-constraint skip and getfsstat64's volume answer
     * (vfs_syscalls.c native_switch_graft_mntonname) recognise them by mount_t. The mounts' own f_mntonname stays
     * their real path: getattrlist names a volume root after it, so changing it (r7a) made realpath(3) resolve
     * /private/preboot/Cryptexes/OS as .../Cryptexes/preboot and the rebuild registered no cryptex extension. */
    for (unsigned i = 0; i < 2; ++i) {
        dev_t image_volume = NODEV;
        mount_t cryptex_mount = NULL;
        SWITCH_RAW_STEP(i ? "App cryptex attach" : "System cryptex attach",
                        switch_cryptex_attach(i, preboot_device, &image_volume, path, sizeof(path)));
        error = native_switch_raw_mount(p, scratch, directories[i + 1], path, image_volume, 1, &cryptex_mount);
        /* r17-launch: a volume on the image's direct read map that does not mount is mounted through DiskImages. */
        if (error && !switch_cryptex_fallback(i, &image_volume, path, sizeof(path))) {
            printf("SwitchRawBoot: %s cryptex mount on its direct read map failed: %d\n", i ? "App" : "System", error);
            error = native_switch_raw_mount(p, scratch, directories[i + 1], path, image_volume, 1, &cryptex_mount);
        }
        SWITCH_RAW_STEP(i ? "App cryptex mount" : "System cryptex mount", error);
        native_owner_cryptex_mount[i] = cryptex_mount;
        switch_cryptex_mounted(i, cryptex_mount);  // SwitchCryptex.cpp: the cluster layer's record, the prefetch
        if (!i)
            cache_device = image_volume;
    }
    native_owner_cryptex_host = preboot_mount;
    switch_auth_fs_init();
    /* dyld looks for code that only the OS cryptex has under the cryptex prefix that libignition reports. Ignition is
     * off here, so show each directory holding such code through the root's own: every one of 21H461's and 24B119's
     * (Mach-O files of the cryptex outside its dyld cache whose path the System volume lacks), each where the selected
     * kind's cryptex has it. */
    static const char *const cryptex_only[] = {
        "/usr/lib",                                                     // libobjc-trampolines, libffi-trampolines, ...
        "/System/Library/PrivateFrameworks/PlugInKitDaemon.framework",  // pkd's framework, not in the cache
        "/System/Library/PrivateFrameworks/VisualTestKit.framework",
        "/System/DriverKit/System/Library",                                        // the DriverKit shared cache
        "/System/Library/PrivateFrameworks/DeviceRecoveryBrainSupport.framework",  // devicerecoveryd (KeepAlive)
        "/System/Library/PrivateFrameworks/PeriodicSystemMetricsCore.framework",
        "/System/Library/PrivateFrameworks/Nodes.framework",
        "/System/Library/PrivateFrameworks/NodesUI.framework",
        "/System/Library/PrivateFrameworks/WeaveReplay.framework",
        "/System/Library/PrivateFrameworks/_NutritionCore_AppIntents.framework",
        "/System/Library/PrivateFrameworks/iCloudDriveCore.framework/Frameworks",
        "/usr/libexec/Moments.framework",
        "/usr/libexec/AppleLatticeSupport.framework"};
    for (unsigned i = 0; i < sizeof(cryptex_only) / sizeof(cryptex_only[0]); ++i) {
        char source[128];
        snprintf(source, sizeof(source), "/private/preboot/Cryptexes/OS%s", cryptex_only[i]);
        vnode_t probe = NULLVP;
        if (vnode_lookup(source, 0, &probe, vfs_context_current()) == ENOENT)
            continue;
        if (probe)
            vnode_put(probe);
        if ((error = native_switch_raw_union(p, scratch, source, cryptex_only[i])))
            printf("SwitchRawBoot: %s cryptex union failed: %d\n", cryptex_only[i], error);
    }
    if (!native_owner_publish_raw_boot_manifest(os_device, SWITCH_OS_VOLUME_UUID, cache_device,
                                                SWITCH_SYSTEM_CRYPTEX_UUID))
        printf("SwitchRawBoot: boot-manifest-hash not published (continuing)\n");
    /* Same development dyld mode as the FAT profile; no signature-policy change. */
    dyld_flags |= UINT64_C(8);
    commpage_update_dyld_flags(dyld_flags);
    native_owner_cache_dyld_flags = dyld_flags;
    /* CacheFinder's existing override opens this exact mounted cryptex path. */
    static const char cache_environment[] =
        "DYLD_SHARED_CACHE_DIR=/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld";
    user64_addr_t environment_vector[2] = {scratch + 0x400, USER_ADDR_NULL};
    if ((error = copyout(cache_environment, environment_vector[0], sizeof(cache_environment))) ||
        (error = copyout(environment_vector, scratch + 0x500, sizeof(environment_vector))))
        return error;
    *envp = scratch + 0x500;
    native_owner_cache_selected_device = native_owner_cache_mounted_device = cache_device;
    native_owner_cache_readonly = native_owner_cache_authorized = native_owner_cache_root_recheck = 1;
    native_owner_cache_original_root = native_owner_cache_current_root = (uintptr_t)original_root;
    native_owner_cache_native_union = 0;
    native_owner_cache_stage = 6;
    native_owner_cache_status = native_owner_cache_mount_status = 0;
    return 0;
}

/* Userland overlay (overlay/manifest.json, tools/package/overlay.py): files the owner keeps on Data under
 * /private/var/Switch/overlay<absolute path>, shown read-only over sealed system directories. ADT /chosen
 * "switch-overlay" lists NUL-terminated "<directory>[:<boot-arg>[|<boot-arg>...]]"; every listed directory unless all
 * of its boot-args are set (each a switch-no-<feature>; features sharing a directory each name their own) gets the same
 * read-only bindfs union as the cryptex directories above, so lookups find the
 * owner's file first and everything else falls through to the sealed entries. Called after the Data mount and
 * before launchd starts. The union reports its files as root:wheel (BINDM_FORCE_ROOT: the owner copies them
 * without root); code in them still needs its CDHash in the trust cache, which the packager adds as a module of its
 * own. A directory with nothing deployed is skipped; a failed union is logged and boot continues. */
#include <pexpert/device_tree.h>

static void native_switch_raw_overlay(proc_t p, user_addr_t scratch)
{
    DTEntry chosen;
    const char *list = NULL;
    unsigned int size = 0;
    if (SecureDTLookupEntry(0, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, "switch-overlay", (const void **)&list, &size) != kSuccess)
        return;
    unsigned shown = 0;
    for (unsigned at = 0; at < size;) {
        const size_t length = strnlen(list + at, size - at);
        char directory[128], source[176];
        if (!length || length >= sizeof(directory) || list[at] != '/') {
            at += (unsigned)length + 1;
            continue;
        }
        memcpy(directory, list + at, length);
        directory[length] = '\0';
        at += (unsigned)length + 1;
        char *gate = strchr(directory, ':');
        if (gate) {
            bool off = true;
            *gate++ = '\0';
            for (char *name = gate; name && off;) {
                char *next = strchr(name, '|');
                uint32_t value = 0;
                if (next)
                    *next++ = '\0';
                off = PE_parse_boot_argn(name, &value, sizeof(value)) && value;
                name = next;
            }
            if (off)
                continue;
        }
        snprintf(source, sizeof(source), "/private/var/Switch/overlay%s", directory);
        vnode_t probe = NULLVP;
        int error = vnode_lookup(source, 0, &probe, vfs_context_current());
        if (error == ENOENT)
            continue;
        if (!error) {
            if (!vnode_isdir(probe))
                error = ENOTDIR;
            vnode_put(probe);
        }
        if (!error)
            error = native_switch_raw_union(p, scratch, source, directory);
        if (error)
            printf("SwitchOverlay: %s over %s failed: %d\n", source, directory, error);
        else
            ++shown;
    }
    if (shown)
        printf("SwitchOverlay: %u director%s from /private/var/Switch/overlay\n", shown, shown == 1 ? "y" : "ies");
}
