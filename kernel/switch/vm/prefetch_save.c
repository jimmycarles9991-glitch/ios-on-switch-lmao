// SPDX-License-Identifier: GPL-2.0-only
#include "prefetch_internal.h"
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
#include <sys/ubc_internal.h>
#include <sys/vnode.h>
#include <uuid/uuid.h>

extern uuid_string_t bootsessionuuid_string;
extern int unlink1(vfs_context_t, vnode_t, user_addr_t, enum uio_seg, int);
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));

struct switch_log_group {
    struct vnode *vp;
    uint32_t vid;
    uint32_t first;
    uint32_t end;
    uint64_t file_id;
    uint64_t file_size;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    uint8_t volume;
};

struct switch_naming {
    struct switch_log_group *groups;
    uint32_t count;
    mount_t mount;
    uint8_t volume;
};

struct switch_run_ref {
    uint64_t file_id;
    uint32_t index;
    uint8_t volume;
};

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

static int compare_log_entries(const void *a, const void *b)
{
    const struct switch_record_entry *x = (const struct switch_record_entry *)a;
    const struct switch_record_entry *y = (const struct switch_record_entry *)b;
    if (x->vp != y->vp) {
        return x->vp < y->vp ? -1 : 1;
    }
    if (x->vid != y->vid) {
        return x->vid < y->vid ? -1 : 1;
    }
    return x->index < y->index ? -1 : (int)(x->index > y->index);
}

static int compare_run_refs(const void *a, const void *b)
{
    const struct switch_run_ref *x = (const struct switch_run_ref *)a;
    const struct switch_run_ref *y = (const struct switch_run_ref *)b;
    if (x->volume != y->volume) {
        return x->volume < y->volume ? -1 : 1;
    }
    if (x->file_id != y->file_id) {
        return x->file_id < y->file_id ? -1 : 1;
    }
    return x->index < y->index ? -1 : (int)(x->index > y->index);
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
    const struct switch_naming *naming = (const struct switch_naming *)argument;
    uint32_t lo = 0;
    uint32_t hi = naming->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (naming->groups[mid].vp < vp) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    for (struct switch_log_group *hit = naming->groups + lo; hit < naming->groups + naming->count && hit->vp == vp;
         ++hit) {
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

static uint32_t collect_kept_replayed(const struct switch_prefetch_run *old_runs, uint32_t replayed_runs,
                                      const uint8_t *replayed_bits, mount_t *mounts, const fsid_t *fsids,
                                      uint32_t mount_count, struct switch_prefetch_range *out_ranges,
                                      uint64_t *pages_out, uint32_t *kept_out, uint32_t *dropped_out,
                                      uint32_t *evicted_out)
{
    *kept_out = 0;
    *dropped_out = 0;
    *evicted_out = 0;
    if (old_runs == NULL || replayed_runs == 0u) {
        return 0u;
    }
    const size_t refs_bytes = (size_t)replayed_runs * sizeof(struct switch_run_ref);
    struct switch_run_ref *refs = (struct switch_run_ref *)kalloc_data(refs_bytes, Z_WAITOK);
    if (refs == NULL) {
        return 0u;
    }
    uint32_t ref_count = 0;
    for (uint32_t i = 0; i < replayed_runs; ++i) {
        if (replayed_bits != NULL && !switch_prefetch_bit_test(replayed_bits, i)) {
            continue;
        }
        refs[ref_count].file_id = old_runs[i].file_id;
        refs[ref_count].index = i;
        refs[ref_count].volume = old_runs[i].volume;
        ++ref_count;
    }
    if (ref_count == 0u) {
        kfree_data(refs, refs_bytes);
        return 0u;
    }
    qsort(refs, ref_count, sizeof(*refs), compare_run_refs);

    vfs_context_t ctx = vfs_context_kernel();
    uint32_t n = 0;
    for (uint32_t i = 0; i < ref_count;) {
        const uint8_t vol = refs[i].volume;
        const uint64_t file_id = refs[i].file_id;
        uint32_t j = i + 1u;
        while (j < ref_count && refs[j].volume == vol && refs[j].file_id == file_id) {
            ++j;
        }
        if (vol >= mount_count || mounts[vol] == NULL) {
            i = j;
            continue;
        }
        vnode_t vp = NULLVP;
        struct switch_prefetch_file_meta meta = {0};
        if (!switch_prefetch_vget_regular(mounts[vol], fsids[vol], file_id, ctx, &vp, &meta)) {
            i = j;
            continue;
        }
        void *control = (void *)ubc_getobject(vp, UBC_FLAGS_NONE);
        for (uint32_t k = i; k < j; ++k) {
            const struct switch_prefetch_run *run = &old_runs[refs[k].index];
            if (!switch_prefetch_run_matches_file(run, meta.file_size, meta.mtime_sec, meta.mtime_nsec)) {
                continue;
            }
            struct switch_prefetch_pagein in;
            if (!switch_prefetch_run_pagein(run, SWITCH_PREFETCH_LIST_PAGE, PAGE_SIZE, meta.file_size, &in)) {
                continue;
            }
            const enum switch_prefetch_run_state state = switch_vm_prefetch_check_run(control, in.offset, in.length);
            if (state == SWITCH_PREFETCH_RUN_UNUSED) {
                ++*dropped_out;
                continue;
            }
            if (state == SWITCH_PREFETCH_RUN_EVICTED) {
                ++*evicted_out;
            }
            ++*kept_out;
            out_ranges[n].file_id = run->file_id;
            out_ranges[n].file_size = run->file_size;
            out_ranges[n].mtime_sec = run->mtime_sec;
            out_ranges[n].mtime_nsec = run->mtime_nsec;
            out_ranges[n].first = run->page;
            out_ranges[n].end = run->page + run->pages;
            out_ranges[n].when = run->when;
            out_ranges[n].volume = run->volume;
            *pages_out += run->pages;
            ++n;
        }
        vnode_put(vp);
        i = j;
    }
    kfree_data(refs, refs_bytes);
    return n;
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

void switch_prefetch_save_next(struct switch_record_entry *log, uint32_t appended,
                               const struct switch_prefetch_run *old_runs, uint32_t replayed_runs,
                               const uint8_t *replayed_bits, const struct switch_prefetch_header *expected_key,
                               mount_t *mounts, const fsid_t *fsids, uint32_t mount_count)
{
#if DEVELOPMENT || DEBUG
    const uint64_t start_abs = mach_absolute_time();
#endif
    const uint32_t entries = appended < SWITCH_PREFETCH_LOG_ENTRIES ? appended : SWITCH_PREFETCH_LOG_ENTRIES;
    const uint32_t max_ranges = entries + replayed_runs;
    if (max_ranges == 0u) {
        return;
    }
    const size_t groups_bytes = (size_t)(entries ? entries : 1u) * sizeof(struct switch_log_group);
    const size_t ranges_bytes = (size_t)max_ranges * sizeof(struct switch_prefetch_range);
    struct switch_log_group *groups = (struct switch_log_group *)kalloc_data(groups_bytes, Z_WAITOK);
    struct switch_prefetch_range *ranges = (struct switch_prefetch_range *)kalloc_data(ranges_bytes, Z_WAITOK);
    if (groups == NULL || ranges == NULL) {
        if (groups != NULL) {
            kfree_data(groups, groups_bytes);
        }
        if (ranges != NULL) {
            kfree_data(ranges, ranges_bytes);
        }
        os_log(OS_LOG_DEFAULT, "SwitchBootPrefetch: list not saved: %d (%u reads logged)", ENOMEM, appended);
        return;
    }

    uint32_t group_count = 0;
    bool vol_logged[SWITCH_PREFETCH_VOLUME_COUNT] = {false};
    if (entries != 0u) {
        qsort(log, entries, sizeof(*log), compare_log_entries);
        for (uint32_t i = 0; i < entries;) {
            uint32_t j = i + 1u;
            while (j < entries && log[j].vp == log[i].vp && log[j].vid == log[i].vid && log[j].index == log[i].index) {
                ++j;
            }
            if (log[i].index < mount_count && log[i].index < SWITCH_PREFETCH_VOLUME_COUNT) {
                groups[group_count].vp = log[i].vp;
                groups[group_count].vid = log[i].vid;
                groups[group_count].first = i;
                groups[group_count].end = j;
                groups[group_count].file_id = 0;
                groups[group_count].file_size = 0;
                groups[group_count].mtime_sec = 0;
                groups[group_count].mtime_nsec = 0;
                groups[group_count].volume = (uint8_t)log[i].index;
                vol_logged[log[i].index] = true;
                ++group_count;
            }
            i = j;
        }
        for (uint32_t v = 0; v < mount_count && v < SWITCH_PREFETCH_VOLUME_COUNT; ++v) {
            if (!vol_logged[v]) {
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
    }

    uint32_t n = 0;
    uint64_t total_pages = 0;
    for (uint32_t g = 0; g < group_count; ++g) {
        if (groups[g].file_id == 0u) {
            continue;
        }
        for (uint32_t i = groups[g].first; i < groups[g].end; ++i) {
            if (log[i].when == 0u || log[i].length == 0u || log[i].index >= mount_count ||
                log[i].offset > UINT64_MAX - log[i].length - SWITCH_PREFETCH_LIST_PAGE) {
                continue;
            }
            const uint64_t first = log[i].offset / SWITCH_PREFETCH_LIST_PAGE;
            const uint64_t end =
                (log[i].offset + log[i].length + SWITCH_PREFETCH_LIST_PAGE - 1u) / SWITCH_PREFETCH_LIST_PAGE;
            if (end <= first || end > UINT32_MAX) {
                continue;
            }
            ranges[n].file_id = groups[g].file_id;
            ranges[n].file_size = groups[g].file_size;
            ranges[n].mtime_sec = groups[g].mtime_sec;
            ranges[n].mtime_nsec = groups[g].mtime_nsec;
            ranges[n].first = (uint32_t)first;
            ranges[n].end = (uint32_t)end;
            ranges[n].when = log[i].when;
            ranges[n].volume = groups[g].volume;
            total_pages += end - first;
            ++n;
        }
    }
    kfree_data(groups, groups_bytes);

    uint32_t kept = 0;
    uint32_t dropped_unused = 0;
    uint32_t evicted = 0;
    n += collect_kept_replayed(old_runs, replayed_runs, replayed_bits, mounts, fsids, mount_count, ranges + n,
                               &total_pages, &kept, &dropped_unused, &evicted);

    struct switch_prefetch_run *runs = NULL;
    size_t runs_bytes = 0;
    uint32_t built = 0;
    uint32_t files = 0;
    if (n != 0u) {
        qsort(ranges, n, sizeof(*ranges), compare_ranges_qsort);
        const uint64_t cap =
            switch_prefetch_run_capacity(n, total_pages, SWITCH_PREFETCH_MAX_RUN_PAGES, SWITCH_PREFETCH_MAX_GAP_PAGES);
        if (cap <= SWITCH_PREFETCH_MAX_RUNS * 4ULL) {
            runs_bytes = (size_t)cap * sizeof(*runs);
            runs = (struct switch_prefetch_run *)kalloc_data(runs_bytes, Z_WAITOK);
            if (runs != NULL) {
                built = switch_prefetch_merge_runs(ranges, n, runs, cap, SWITCH_PREFETCH_MAX_RUN_PAGES,
                                                   SWITCH_PREFETCH_MAX_GAP_PAGES, &files);
                qsort(runs, built, sizeof(*runs), compare_runs_qsort);
            }
        }
    }
    kfree_data(ranges, ranges_bytes);

    const uint32_t count =
        runs != NULL ? switch_prefetch_cap_runs(runs, built, SWITCH_PREFETCH_CAP_PAGES, SWITCH_PREFETCH_MAX_RUNS) : 0u;
    int error = count == 0u ? ENOENT : 0;
    char path[192];
    switch_prefetch_list_path(path, sizeof(path));
    if (error == 0) {
        struct switch_prefetch_header header = *expected_key;
        header.runs = count;
        header.files = files;
        header.checksum = switch_prefetch_fnv((const uint8_t *)runs, (uint64_t)count * sizeof(*runs));
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
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - start_abs, &ns);
        const uint64_t cx_mib = ((vol_pages[SWITCH_PREFETCH_VOL_SYSTEM] + vol_pages[SWITCH_PREFETCH_VOL_APP]) *
                                 SWITCH_PREFETCH_LIST_PAGE) >>
                                20;
        const uint64_t root_mib = (vol_pages[SWITCH_PREFETCH_VOL_ROOT] * SWITCH_PREFETCH_LIST_PAGE) >> 20;
        const uint64_t data_mib = (vol_pages[SWITCH_PREFETCH_VOL_DATA] * SWITCH_PREFETCH_LIST_PAGE) >> 20;
        os_log(OS_LOG_DEFAULT,
               "SwitchBootPrefetch: list saved: %u runs, %llu MiB (%llu/%llu/%llu MiB cx/root/Data; %u kept of the "
               "last list, %u dropped unused, %u evicted) in %llu ms",
               count, (saved_pages * SWITCH_PREFETCH_LIST_PAGE) >> 20, cx_mib, root_mib, data_mib, kept, dropped_unused,
               evicted, ns / NSEC_PER_MSEC);
    }
#endif
}
