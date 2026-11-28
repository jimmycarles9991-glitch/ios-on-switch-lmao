// SPDX-License-Identifier: GPL-2.0-only
#include "prefetch_internal.h"
#include "prefetch_naming.h"
#include "prefetch_pace.h"

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/vm_param.h>
#include <os/log.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/mount_internal.h>
#include <sys/param.h>
#include <sys/reboot.h>
#include <sys/vnode.h>
#include <uuid/uuid.h>

extern uuid_string_t bootsessionuuid_string;
extern int unlink1(vfs_context_t, vnode_t, user_addr_t, enum uio_seg, int);
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));

struct switch_naming {
    struct switch_log_group *groups;
    uint32_t count;
    mount_t mount;
    uint8_t volume;
};

static int compare_log_entries(const void *a, const void *b)
{
    return switch_prefetch_compare_log_entries((const struct switch_record_entry *)a,
                                               (const struct switch_record_entry *)b);
}

static int compare_named_files(const void *a, const void *b)
{
    return switch_prefetch_compare_named_files((const struct switch_named_file *)a,
                                               (const struct switch_named_file *)b);
}

static int compare_ranges_qsort(const void *a, const void *b)
{
    return switch_prefetch_compare_ranges((const struct switch_prefetch_range *)a,
                                          (const struct switch_prefetch_range *)b);
}

static int compare_runs_qsort(const void *a, const void *b)
{
    return switch_prefetch_compare_runs((const struct switch_prefetch_run *)a, (const struct switch_prefetch_run *)b);
}

static int name_logged_vnode(vnode_t vp, void *argument)
{
    if (get_system_inshutdown()) {
        return VNODE_RETURNED_DONE;
    }
    const struct switch_naming *naming = (const struct switch_naming *)argument;
    uint32_t lo = 0;
    uint32_t hi = naming->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if ((uintptr_t)naming->groups[mid].vp < (uintptr_t)vp) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    for (struct switch_log_group *hit = naming->groups + lo;
         hit < naming->groups + naming->count && hit->vp == (const void *)vp; ++hit) {
        if (hit->file_id != 0u || hit->volume != naming->volume || hit->vid != vnode_vid(vp)) {
            continue;
        }
        uint64_t file_id = 0;
        struct switch_prefetch_file_meta meta = {0};
        if (switch_prefetch_vnode_meta(vp, naming->mount, 0u, vfs_context_kernel(), &file_id, &meta)) {
            hit->file_id = file_id;
            hit->file_size = meta.file_size;
            hit->mtime_sec = meta.mtime_sec;
            hit->mtime_nsec = meta.mtime_nsec;
        }
    }
    return VNODE_RETURNED;
}

static int kernel_write_chunked(vnode_t vp, const void *buf, size_t len, off_t offset, vfs_context_t ctx)
{
    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        const size_t remain = len - done;
        const int chunk = remain > (1u << 20) ? (int)(1u << 20) : (int)remain;
        int resid = chunk;
        const int err = vn_rdwr(UIO_WRITE, vp, (caddr_t)(src + done), chunk, offset + (off_t)done, UIO_SYSSPACE,
                                IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (err != 0) {
            return err;
        }
        if (resid != 0) {
            return EIO;
        }
        done += (size_t)chunk;
    }
    return 0;
}

static int kernel_write_file(void *ctx_arg, const char *path, const void *header, size_t header_len, const void *runs,
                             size_t runs_len)
{
    vfs_context_t ctx = (vfs_context_t)ctx_arg;
    vnode_t vp = NULLVP;
    int error = vnode_open(path, O_CREAT | O_TRUNC | FWRITE | O_NOFOLLOW_ANY, 0644, 0, &vp, ctx);
    if (error != 0) {
        return error;
    }
    error = kernel_write_chunked(vp, header, header_len, 0, ctx);
    if (error == 0 && runs_len != 0u) {
        error = kernel_write_chunked(vp, runs, runs_len, (off_t)header_len, ctx);
    }
    if (error == 0) {
        error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
    }
    const int close_err = vnode_close(vp, FWRITE | FWASWRITTEN, ctx);
    return error != 0 ? error : close_err;
}

static int kernel_rename_file(void *ctx_arg __unused, const char *from, const char *to)
{
    return switch_vfs_rename(from, to);
}

static void kernel_unlink_file(void *ctx_arg, const char *path)
{
    (void)unlink1((vfs_context_t)ctx_arg, NULLVP, CAST_USER_ADDR_T(path), UIO_SYSSPACE, VNODE_REMOVE_NOFOLLOW_ANY);
}

#if DEVELOPMENT || DEBUG
static uint64_t switch_prefetch_save_ms(uint64_t from_abs, uint64_t to_abs)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(to_abs - from_abs, &ns);
    return ns / NSEC_PER_MSEC;
}
#endif

/* Names each group's vnode where XNU still has it (Data always, since its files change), and from the replay's own
 * files otherwise: a read-only volume's file the replay opened needs no lookup, and a vnode recycled since the
 * boot's read keeps its name. */
static uint32_t switch_prefetch_name_groups(struct switch_log_group *groups, uint32_t group_count,
                                            struct switch_named_file *files, uint32_t file_count, mount_t *mounts,
                                            const fsid_t *fsids, uint32_t mount_count)
{
    if (file_count != 0u) {
        qsort(files, file_count, sizeof(*files), compare_named_files);
    }
    uint32_t from_replay = switch_prefetch_name_from_files(groups, group_count, files, file_count, false);
    bool unnamed[SWITCH_PREFETCH_VOLUME_COUNT] = {false};
    for (uint32_t g = 0; g < group_count; ++g) {
        if (groups[g].file_id == 0u) {
            unnamed[groups[g].volume] = true;
        }
    }
    for (uint32_t v = 0; v < mount_count && v < SWITCH_PREFETCH_VOLUME_COUNT && !get_system_inshutdown(); ++v) {
        if (!unnamed[v]) {
            continue;
        }
        mount_t busy_mp = switch_prefetch_busy_mount(mounts[v], fsids[v]);
        if (busy_mp == NULL) {
            continue;
        }
        struct switch_naming naming = {groups, group_count, busy_mp, (uint8_t)v};
        (void)vnode_iterate(busy_mp, 0, name_logged_vnode, &naming);
        vfs_unbusy(busy_mp);
    }
    from_replay += switch_prefetch_name_from_files(groups, group_count, files, file_count, true);
    return from_replay;
}

void switch_prefetch_save_next(struct switch_record_entry **log_p, size_t log_bytes, uint32_t appended,
                               struct switch_named_file *files, uint32_t file_count,
                               const struct switch_prefetch_header *expected_key, mount_t *mounts, const fsid_t *fsids,
                               uint32_t mount_count)
{
#if DEVELOPMENT || DEBUG
    const uint64_t start_abs = mach_absolute_time();
#endif
    struct switch_record_entry *log = *log_p;
    const uint32_t entries = appended < SWITCH_PREFETCH_LOG_ENTRIES ? appended : SWITCH_PREFETCH_LOG_ENTRIES;
    if (entries == 0u) {
        return;
    }

    qsort(log, entries, sizeof(*log), compare_log_entries);
    const uint32_t group_cap = switch_prefetch_count_groups(log, entries, mount_count);
    struct switch_log_group *groups = NULL;
    size_t groups_bytes = 0;
    uint32_t group_count = 0;
    uint32_t from_replay = 0;
    if (group_cap != 0u) {
        groups_bytes = (size_t)group_cap * sizeof(struct switch_log_group);
        groups = (struct switch_log_group *)kalloc_data(groups_bytes, Z_WAITOK);
        if (groups == NULL) {
            os_log(OS_LOG_DEFAULT, "SwitchBootPrefetch: list not saved: %d (%u reads logged)", ENOMEM, appended);
            return;
        }
        group_count = switch_prefetch_group_log(log, entries, mount_count, groups);
        from_replay = switch_prefetch_name_groups(groups, group_count, files, file_count, mounts, fsids, mount_count);
    }
#if DEVELOPMENT || DEBUG
    const uint64_t named_abs = mach_absolute_time();
#else
    (void)from_replay;
#endif
    uint32_t named_entries = 0;
    for (uint32_t g = 0; g < group_count; ++g) {
        if (groups[g].file_id != 0u) {
            named_entries += groups[g].end - groups[g].first;
        }
    }
    struct switch_prefetch_range *ranges = NULL;
    size_t ranges_bytes = 0;
    if (named_entries != 0u) {
        ranges_bytes = (size_t)named_entries * sizeof(struct switch_prefetch_range);
        ranges = (struct switch_prefetch_range *)kalloc_data(ranges_bytes, Z_WAITOK);
        if (ranges == NULL) {
            kfree_data(groups, groups_bytes);
            os_log(OS_LOG_DEFAULT, "SwitchBootPrefetch: list not saved: %d (%u reads logged)", ENOMEM, appended);
            return;
        }
    }

    uint64_t total_pages = 0;
    uint64_t unnamed_bytes = 0;
    const uint32_t n =
        group_count != 0u ? switch_prefetch_log_ranges(log, groups, group_count, ranges, &total_pages, &unnamed_bytes)
                          : 0u;
    if (groups != NULL) {
        kfree_data(groups, groups_bytes);
    }
    kfree_data(log, log_bytes);
    *log_p = NULL;

    struct switch_prefetch_run *runs = NULL;
    size_t runs_bytes = 0;
    uint32_t built = 0;
    uint32_t list_files = 0;
    if (n != 0u) {
        qsort(ranges, n, sizeof(*ranges), compare_ranges_qsort);
        uint64_t cap =
            switch_prefetch_run_capacity(n, total_pages, SWITCH_PREFETCH_MAX_RUN_PAGES, SWITCH_PREFETCH_MAX_GAP_PAGES);
        if (cap > SWITCH_PREFETCH_MAX_RUNS) {
            cap = SWITCH_PREFETCH_MAX_RUNS;
        }
        runs_bytes = (size_t)cap * sizeof(*runs);
        runs = (struct switch_prefetch_run *)kalloc_data(runs_bytes, Z_WAITOK);
        if (runs != NULL) {
            built = switch_prefetch_merge_runs(ranges, n, runs, cap, SWITCH_PREFETCH_MAX_RUN_PAGES,
                                               SWITCH_PREFETCH_MAX_GAP_PAGES, &list_files);
            qsort(runs, built, sizeof(*runs), compare_runs_qsort);
        }
    }
    if (ranges != NULL) {
        kfree_data(ranges, ranges_bytes);
    }

    const uint32_t count =
        runs != NULL ? switch_prefetch_cap_runs(runs, built, SWITCH_PREFETCH_CAP_PAGES, SWITCH_PREFETCH_MAX_RUNS) : 0u;
    /* Shutdown waits for this thread (switch_prefetch_shutdown). Once reboot_kernel sets system_inshutdown, the naming
     * above stops at its next vnode or volume, so what it built is incomplete: nothing is written and the last list
     * stays. Tested first, so a save a reboot cut short says so even when it named nothing. */
    int error = get_system_inshutdown() ? ECANCELED : count == 0u ? ENOENT : 0;
    char path[192];
    switch_prefetch_list_path(path, sizeof(path));
#if DEVELOPMENT || DEBUG
    const uint64_t write_abs = mach_absolute_time();
#endif
    if (error == 0) {
        struct switch_prefetch_header header = *expected_key;
        header.runs = count;
        header.files = list_files;
        static const struct switch_prefetch_file_ops ops = {
            .write_file = kernel_write_file,
            .rename_file = kernel_rename_file,
            .unlink_file = kernel_unlink_file,
        };
        error =
            switch_prefetch_save_atomic(path, bootsessionuuid_string, &header, runs, count, &ops, vfs_context_kernel());
    }

#if DEVELOPMENT || DEBUG
    uint64_t saved_pages = 0;
    uint64_t vol_pages[SWITCH_PREFETCH_VOLUME_COUNT] = {0};
    for (uint32_t i = 0; i < count; ++i) {
        saved_pages += runs[i].pages;
        if (runs[i].volume < SWITCH_PREFETCH_VOLUME_COUNT) {
            vol_pages[runs[i].volume] += runs[i].pages;
        }
    }
#endif
    if (runs != NULL) {
        kfree_data(runs, runs_bytes);
    }

    if (error != 0) {
        os_log(OS_LOG_DEFAULT, "SwitchBootPrefetch: %s not saved: %d (%u reads logged)", path, error, appended);
    }
#if DEVELOPMENT || DEBUG
    else {
        const uint64_t end_abs = mach_absolute_time();
        const uint64_t cx_mib = ((vol_pages[SWITCH_PREFETCH_VOL_SYSTEM] + vol_pages[SWITCH_PREFETCH_VOL_APP]) *
                                 SWITCH_PREFETCH_LIST_PAGE) >>
                                20;
        const uint64_t root_mib = (vol_pages[SWITCH_PREFETCH_VOL_ROOT] * SWITCH_PREFETCH_LIST_PAGE) >> 20;
        const uint64_t data_mib = (vol_pages[SWITCH_PREFETCH_VOL_DATA] * SWITCH_PREFETCH_LIST_PAGE) >> 20;
        os_log(
            OS_LOG_DEFAULT,
            "SwitchBootPrefetch: list saved: %u runs, %llu MiB (%llu/%llu/%llu MiB cx/root/Data; %u files named from "
            "the replay, %llu MiB unnamed) in %llu ms (sort and naming %llu, runs %llu, write %llu)",
            count, (saved_pages * SWITCH_PREFETCH_LIST_PAGE) >> 20, cx_mib, root_mib, data_mib, from_replay,
            unnamed_bytes >> 20, switch_prefetch_save_ms(start_abs, end_abs),
            switch_prefetch_save_ms(start_abs, named_abs), switch_prefetch_save_ms(named_abs, write_abs),
            switch_prefetch_save_ms(write_abs, end_abs));
    }
#endif
}
