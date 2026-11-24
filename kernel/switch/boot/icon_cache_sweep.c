// SPDX-License-Identifier: GPL-2.0-only
/*
 * Post-boot trash sweep for 24B119's icon cache rename-aside (<switch/icon_cache.h>). Armed before launchd only when
 * /private/var/Switch/icon-cache-trash exists, and started directly from IOPMrootDomain kPowerEventSystemBootCompleted
 * on a single throttled kernel worker (MAXPRI_THROTTLE + IOPOL_THROTTLE).
 */
#include <switch/icon_cache.h>

#if DEVELOPMENT || DEBUG
#include <kern/clock.h>
#endif
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <string.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/systm.h>
#include <sys/uio_internal.h>
#include <sys/vnode_internal.h>

/* osfmk/kern/sched.h and the MACH_KERNEL_PRIVATE section of thread.h are not exported to BSD. */
#ifndef MAXPRI_THROTTLE
#define MAXPRI_THROTTLE 4
#endif
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
                                                  thread_t *new_thread);
extern kern_return_t thread_terminate(thread_t thread);

enum {
    kSweepFullPath = 544,
    kSweepDirents = 2048,
    kSweepBatch = 2048,
    kSweepScratch = (SWITCH_ICON_CACHE_SWEEP_MAX_DEPTH + 1) * kSweepBatch,
    kSweepBufSize = kSweepFullPath + kSweepDirents + kSweepScratch,
};

struct icon_cache_sweep_ctx {
    mount_t mount;
    vfs_context_t vfs;
    char *full_path;
    char *dirents;
};

/*
 * Synchronizes pre-launchd arm() with IOPMrootDomain's System Boot Complete notification so the worker launches at
 * most once without taking a lock on the power-management workloop.
 */
static mount_t sweep_data_mount;
static uint32_t sweep_state;

static bool icon_cache_sweep_stopping(void)
{
    return vfs_shutdown_in_progress() || vfs_shutdown_finished();
}

static int icon_cache_sweep_full_path(struct icon_cache_sweep_ctx *ctx, const char *relative)
{
    const int size = snprintf(ctx->full_path, kSweepFullPath, "/private/var/%s", relative);
    return size < 0 || size >= kSweepFullPath ? ENAMETOOLONG : 0;
}

static int icon_cache_sweep_read_batch(void *context, const char *relative, char *names, size_t names_size,
                                       size_t *count)
{
    struct icon_cache_sweep_ctx *ctx = context;
    vnode_t dvp = NULLVP;
    *count = 0;
    if (icon_cache_sweep_stopping()) {
        return ECANCELED;
    }
    int error = icon_cache_sweep_full_path(ctx, relative);
    if (error) {
        return error;
    }

    error = vnode_open(ctx->full_path, FREAD | O_DIRECTORY | O_NOFOLLOW_ANY, 0, 0, &dvp, ctx->vfs);
    if (error) {
        return error;
    }
    if (vnode_mount(dvp) != ctx->mount || vnode_mountedhere(dvp) != NULL) {
        (void)vnode_close(dvp, FREAD, ctx->vfs);
        return EXDEV;
    }
    if (!vnode_isdir(dvp)) {
        (void)vnode_close(dvp, FREAD, ctx->vfs);
        return ENOTDIR;
    }

    UIO_STACKBUF(uio_buf, 1);
    uio_t uio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ, uio_buf, sizeof(uio_buf));
    size_t used = 0;
    bool full = false;
    for (int eof = 0, dcount = 0; !error && !eof && !full;) {
        uio_reset(uio, uio_offset(uio), UIO_SYSSPACE, UIO_READ);
        uio_addiov(uio, CAST_USER_ADDR_T(ctx->dirents), kSweepDirents);
        error = VNOP_READDIR(dvp, uio, 0, &eof, &dcount, ctx->vfs);
        const size_t filled = kSweepDirents - (size_t)uio_resid(uio);
        if (!filled) {
            eof = 1;
        }
        for (size_t at = 0; !error && at < filled;) {
            if (filled - at < offsetof(struct dirent, d_name)) {
                error = EIO;
                break;
            }
            struct dirent entry;
            memcpy(&entry, ctx->dirents + at, offsetof(struct dirent, d_name));
            if (entry.d_reclen < offsetof(struct dirent, d_name) || entry.d_reclen > filled - at) {
                error = EIO;
                break;
            }
            const char *name = ctx->dirents + at + offsetof(struct dirent, d_name);
            const size_t max_name = entry.d_reclen - offsetof(struct dirent, d_name);
            const size_t cap = entry.d_namlen < max_name ? entry.d_namlen : max_name;
            at += entry.d_reclen;
            if (!entry.d_ino) {
                continue;
            }
            const size_t nlen = strnlen(name, cap);
            if (!nlen || (nlen == 1 && name[0] == '.') || (nlen == 2 && name[0] == '.' && name[1] == '.')) {
                continue;
            }
            if (used + nlen + 1 > names_size) {
                if (!*count) {
                    error = ENAMETOOLONG;
                } else {
                    full = true;
                }
                break;
            }
            memcpy(names + used, name, nlen);
            names[used + nlen] = '\0';
            used += nlen + 1;
            ++(*count);
        }
    }

    const int close_error = vnode_close(dvp, FREAD, ctx->vfs);
    return error ? error : close_error;
}

static int icon_cache_sweep_inspect(void *context, const char *relative, enum switch_icon_cache_entry_kind *kind)
{
    struct icon_cache_sweep_ctx *ctx = context;
    vnode_t vp = NULLVP;
    *kind = SWITCH_ICON_CACHE_ENTRY_MISSING;
    if (icon_cache_sweep_stopping()) {
        return ECANCELED;
    }
    int error = icon_cache_sweep_full_path(ctx, relative);
    if (error) {
        return error;
    }

    error = vnode_lookup(ctx->full_path, VNODE_LOOKUP_NOFOLLOW | VNODE_LOOKUP_NOFOLLOW_ANY, &vp, ctx->vfs);
    if (error == ENOENT) {
        return 0;
    }
    if (error == ELOOP) {
        *kind = SWITCH_ICON_CACHE_ENTRY_SYMLINK;
        return 0;
    }
    if (error) {
        return error;
    }

    if (vnode_mount(vp) != ctx->mount || vnode_mountedhere(vp) != NULL) {
        *kind = SWITCH_ICON_CACHE_ENTRY_FOREIGN;
    } else {
        switch (vnode_vtype(vp)) {
        case VLNK:
            *kind = SWITCH_ICON_CACHE_ENTRY_SYMLINK;
            break;
        case VREG:
            *kind = SWITCH_ICON_CACHE_ENTRY_REG;
            break;
        case VDIR:
            *kind = SWITCH_ICON_CACHE_ENTRY_DIR;
            break;
        default:
            *kind = SWITCH_ICON_CACHE_ENTRY_OTHER;
            break;
        }
    }
    vnode_put(vp);
    return 0;
}

static int icon_cache_sweep_remove(struct icon_cache_sweep_ctx *ctx, const char *relative, bool is_dir)
{
    if (icon_cache_sweep_stopping()) {
        return ECANCELED;
    }
    int error = icon_cache_sweep_full_path(ctx, relative);
    if (error) {
        return error;
    }

    struct nameidata nd = {0};
    NDINIT(&nd, DELETE, is_dir ? OP_RMDIR : OP_UNLINK, LOCKPARENT | NOFOLLOW, UIO_SYSSPACE,
           CAST_USER_ADDR_T(ctx->full_path), ctx->vfs);
    nd.ni_flag |= NAMEI_NOFOLLOW_ANY;
    error = namei(&nd);
    if (error) {
        return error;
    }

    vnode_t dvp = nd.ni_dvp;
    vnode_t vp = nd.ni_vp;
    if (dvp == NULLVP || vp == NULLVP) {
        error = ENOENT;
    } else if (vnode_mount(dvp) != ctx->mount || vnode_mount(vp) != ctx->mount || vnode_isvroot(vp) ||
               vnode_mountedhere(vp) != NULL) {
        error = EXDEV;
    } else if (vfs_isrdonly(vnode_mount(dvp))) {
        error = EROFS;
    } else if (dvp == vp) {
        error = EINVAL;
    } else if (vnode_vtype(vp) == VLNK) {
        error = ELOOP;
    } else if (is_dir) {
        if (!vnode_isdir(vp)) {
            error = ENOTDIR;
        } else {
            error = VNOP_RMDIR(dvp, vp, &nd.ni_cnd, ctx->vfs);
        }
    } else {
        if (vnode_isdir(vp)) {
            error = EISDIR;
        } else if (!vnode_isreg(vp)) {
            error = EPERM;
        } else {
            vnode_link_lock(vp);
            error = VNOP_REMOVE(dvp, vp, &nd.ni_cnd, VNODE_REMOVE_SKIP_NAMESPACE_EVENT, ctx->vfs);
            if (!error && (vp->v_flag & VISHARDLINK)) {
                vnode_update_identity(vp, NULLVP, NULL, 0, 0, VNODE_UPDATE_PARENT);
            }
            vnode_link_unlock(vp);
        }
    }

    /* nameidone must precede vnode_put(dvp) while releasing any parent node lock. */
    nameidone(&nd);
    if (dvp != NULLVP) {
        vnode_put(dvp);
    }
    if (vp != NULLVP) {
        vnode_put(vp);
    }
    return error;
}

static int icon_cache_sweep_unlink(void *context, const char *relative)
{
    return icon_cache_sweep_remove(context, relative, false);
}

static int icon_cache_sweep_rmdir(void *context, const char *relative)
{
    return icon_cache_sweep_remove(context, relative, true);
}

static void icon_cache_sweep_worker(__unused void *arg, __unused wait_result_t wait)
{
#if DEVELOPMENT || DEBUG
    const uint64_t started = mach_absolute_time();
#endif
    thread_set_thread_name(current_thread(), "SwitchIconCacheSweep");
    throttle_set_thread_io_policy(IOPOL_THROTTLE);
    if (icon_cache_sweep_stopping()) {
        thread_terminate(current_thread());
        return;
    }

    mount_t data_mount = os_atomic_load(&sweep_data_mount, acquire);
    vfs_context_t vfs = vfs_context_kernel();
    vnode_t data_root = NULLVP;
    int verify_error =
        data_mount == NULL ? EINVAL : vnode_lookup("/private/var", VNODE_LOOKUP_NOFOLLOW_ANY, &data_root, vfs);
    if (!verify_error) {
        if (vnode_mount(data_root) != data_mount || !vnode_isvroot(data_root) || (vfs_flags(data_mount) & MNT_RDONLY)) {
            verify_error = EXDEV;
        }
        vnode_put(data_root);
    }
    if (verify_error) {
        printf("SwitchIconCache: trash sweep skipped: Data mount check failed (%d)\n", verify_error);
        thread_terminate(current_thread());
        return;
    }

    char *buf = kalloc_data(kSweepBufSize, Z_WAITOK | Z_ZERO);
    if (!buf) {
        printf("SwitchIconCache: trash sweep skipped: out of memory\n");
        thread_terminate(current_thread());
        return;
    }
    struct icon_cache_sweep_ctx ctx = {
        .mount = data_mount,
        .vfs = vfs,
        .full_path = buf,
        .dirents = buf + kSweepFullPath,
    };

    const struct switch_icon_cache_sweep_ops ops = {
        .context = &ctx,
        .scratch = buf + kSweepFullPath + kSweepDirents,
        .scratch_stride = kSweepBatch,
        .read_batch = icon_cache_sweep_read_batch,
        .inspect = icon_cache_sweep_inspect,
        .unlink = icon_cache_sweep_unlink,
        .rmdir = icon_cache_sweep_rmdir,
    };
    struct switch_icon_cache_sweep_result result;
    switch_icon_cache_sweep(&ops, &result);
    kfree_data(buf, kSweepBufSize);

    if (result.error == ECANCELED || (result.error && icon_cache_sweep_stopping())) {
        thread_terminate(current_thread());
        return;
    }
    if (result.error) {
        printf("SwitchIconCache: trash sweep stopped: %d (%u files, %u dirs removed)\n", result.error,
               result.files_removed, result.dirs_removed);
    }
#if DEVELOPMENT || DEBUG
    else {
        uint64_t elapsed_ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - started, &elapsed_ns);
        printf("SwitchIconCache: trash sweep done (%u files, %u dirs removed, %llu ms)\n", result.files_removed,
               result.dirs_removed, elapsed_ns / NSEC_PER_MSEC);
    }
#endif
    thread_terminate(current_thread());
}

static void icon_cache_sweep_launch(void)
{
    thread_t worker = THREAD_NULL;
    const kern_return_t kr = kernel_thread_start_priority(icon_cache_sweep_worker, NULL, MAXPRI_THROTTLE, &worker);
    if (kr != KERN_SUCCESS) {
        printf("SwitchIconCache: trash sweep thread start failed: %d\n", kr);
        return;
    }
    thread_deallocate(worker);
}

static void icon_cache_sweep_advance(uint32_t event_bit)
{
    uint32_t cur = os_atomic_load(&sweep_state, acquire);
    for (;;) {
        uint32_t next = 0;
        const bool start = switch_icon_cache_state_step(cur, event_bit, &next);
        if (next == cur) {
            return;
        }
        if (os_atomic_cmpxchgv(&sweep_state, cur, next, &cur, acq_rel)) {
            if (start) {
                icon_cache_sweep_launch();
            }
            return;
        }
    }
}

void switch_icon_cache_sweep_arm(void *data_mount, bool trash_pending)
{
    if (!trash_pending || data_mount == NULL) {
        return;
    }
    os_atomic_store(&sweep_data_mount, (mount_t)data_mount, release);
    icon_cache_sweep_advance(SWITCH_ICON_CACHE_STATE_PENDING);
}

void switch_icon_cache_boot_complete(void)
{
    icon_cache_sweep_advance(SWITCH_ICON_CACHE_STATE_BOOT_DONE);
}
