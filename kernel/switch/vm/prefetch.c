// SPDX-License-Identifier: GPL-2.0-only
#define SWITCH_PREFETCH_IMPL 1
#include "prefetch_internal.h"
#include "prefetch_pace.h"

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/vm_param.h>
#include <os/log.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/vnode.h>
#include <vm/vnode_pager.h>
#if XNU_12377_121_6
#include <vm/vm_ubc.h>
#endif

extern kern_return_t thread_terminate(thread_t);

static const char *const k_cryptex_images[SWITCH_PREFETCH_CRYPTEX_COUNT] = {
    SWITCH_SYSTEM_CRYPTEX_IMAGE,
    SWITCH_APP_CRYPTEX_IMAGE,
};

static struct {
    struct switch_prefetch_ready ready;
    fsid_t fsids[SWITCH_PREFETCH_VOLUME_COUNT];
    uint32_t started;
    uint32_t thread_active;
    uint32_t thread_done;
    int done_event;
#if DEVELOPMENT || DEBUG
    thread_call_t diag_call;
    uint32_t diag_stop;
    uint64_t prev_demand_bytes[SWITCH_PREFETCH_VOLUME_COUNT];
    uint64_t prev_replay_reqs;
    uint64_t prev_replay_gt64k;
    uint64_t prev_demand_reqs;
    uint64_t prev_demand_gt64k;
    uint64_t prev_demand_abs;
    uint64_t prev_replay_abs;
#endif
} g_prefetch;

void switch_prefetch_list_path(char *out, size_t size)
{
    (void)snprintf(out, size, "%s.boot-prefetch", SWITCH_SYSTEM_CRYPTEX_IMAGE);
}

static inline mount_t switch_prefetch_mount_at(unsigned index)
{
    if (index >= SWITCH_PREFETCH_VOLUME_COUNT) {
        return NULL;
    }
    return (mount_t)__atomic_load_n(&g_prefetch.ready.mounts[index], __ATOMIC_ACQUIRE);
}

#if DEVELOPMENT || DEBUG
static uint64_t switch_abs_to_ms(uint64_t abs_ticks)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(abs_ticks, &ns);
    return ns / NSEC_PER_MSEC;
}

static uint64_t switch_delta(uint64_t cur, uint64_t prev)
{
    return cur >= prev ? cur - prev : 0u;
}

static void switch_prefetch_diag_tick(thread_call_param_t p0 __unused, thread_call_param_t p1 __unused)
{
    if (__atomic_load_n(&g_prefetch.diag_stop, __ATOMIC_SEQ_CST) != 0u) {
        return;
    }
    struct switch_prefetch_pace_snapshot pace = {0};
    struct switch_cluster_io_snapshot io = {0};
    switch_vm_prefetch_pace_snapshot(&pace);
    switch_cluster_prefetch_snapshot(&io);

    const uint64_t cx_bytes =
        switch_delta(io.demand_bytes[SWITCH_PREFETCH_VOL_SYSTEM],
                     g_prefetch.prev_demand_bytes[SWITCH_PREFETCH_VOL_SYSTEM]) +
        switch_delta(io.demand_bytes[SWITCH_PREFETCH_VOL_APP], g_prefetch.prev_demand_bytes[SWITCH_PREFETCH_VOL_APP]);
    const uint64_t root_bytes =
        switch_delta(io.demand_bytes[SWITCH_PREFETCH_VOL_ROOT], g_prefetch.prev_demand_bytes[SWITCH_PREFETCH_VOL_ROOT]);
    const uint64_t data_bytes =
        switch_delta(io.demand_bytes[SWITCH_PREFETCH_VOL_DATA], g_prefetch.prev_demand_bytes[SWITCH_PREFETCH_VOL_DATA]);
    const uint64_t replay_reqs = switch_delta(io.replay_reqs, g_prefetch.prev_replay_reqs);
    const uint64_t replay_gt64k = switch_delta(io.replay_gt64k, g_prefetch.prev_replay_gt64k);
    const uint64_t demand_reqs = switch_delta(io.demand_reqs, g_prefetch.prev_demand_reqs);
    const uint64_t demand_gt64k = switch_delta(io.demand_gt64k, g_prefetch.prev_demand_gt64k);
    const uint64_t demand_abs = io.demand_abs + pace.demand_upl_abs;
    const uint64_t replay_ms = switch_abs_to_ms(switch_delta(pace.replay_abs, g_prefetch.prev_replay_abs));
    const uint64_t demand_ms = switch_abs_to_ms(switch_delta(demand_abs, g_prefetch.prev_demand_abs));

    for (unsigned v = 0; v < SWITCH_PREFETCH_VOLUME_COUNT; ++v) {
        g_prefetch.prev_demand_bytes[v] = io.demand_bytes[v];
    }
    g_prefetch.prev_replay_reqs = io.replay_reqs;
    g_prefetch.prev_replay_gt64k = io.replay_gt64k;
    g_prefetch.prev_demand_reqs = io.demand_reqs;
    g_prefetch.prev_demand_gt64k = io.demand_gt64k;
    g_prefetch.prev_demand_abs = demand_abs;
    g_prefetch.prev_replay_abs = pace.replay_abs;

    const uint64_t shift = PAGE_SHIFT;
    const uint64_t r_mib = (pace.replayed_pages << shift) >> 20;
    const uint64_t u_mib = (pace.used_pages << shift) >> 20;
    const uint64_t x_mib = (pace.reclaimed_pages << shift) >> 20;
    const uint64_t l_mib = (pace.lead_pages << shift) >> 20;
    const uint64_t b_mib = (pace.lead_bound << shift) >> 20;
    const uint64_t d_cx = cx_bytes >> 20;
    const uint64_t d_root = root_bytes >> 20;
    const uint64_t d_data = data_bytes >> 20;

    os_log(
        OS_LOG_DEFAULT,
        "SwitchBootPrefetch: %llu MiB paged in ahead (%llu used, %llu reclaimed unused, lead %llu of bound %llu), "
        "%llu MiB of demand reads by volume (%llu/%llu/%llu), card ms %llu/%llu, requests > 64 KiB replay %llu/%llu, "
        "demand %llu/%llu",
        r_mib, u_mib, x_mib, l_mib, b_mib, d_cx + d_root + d_data, d_cx, d_root, d_data, replay_ms, demand_ms,
        replay_gt64k, replay_reqs, demand_gt64k, demand_reqs);

    if (__atomic_load_n(&g_prefetch.diag_stop, __ATOMIC_SEQ_CST) == 0u) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(10u, NSEC_PER_SEC, &deadline);
        thread_call_enter_delayed(g_prefetch.diag_call, deadline);
    }
}
#endif

static bool switch_prefetch_mount_uuid(mount_t mp, fsid_t fsid, const char *pinned_uuid_str, uint8_t out[16],
                                       vfs_context_t ctx)
{
    mount_t busy_mp = switch_prefetch_busy_mount(mp, fsid);
    if (busy_mp != NULL) {
        struct vfs_attr vfa;
        VFSATTR_INIT(&vfa);
        VFSATTR_WANTED(&vfa, f_uuid);
        const bool ok = vfs_getattr(busy_mp, &vfa, ctx) == 0 && VFSATTR_IS_SUPPORTED(&vfa, f_uuid) &&
                        !switch_prefetch_uuid_is_zero(vfa.f_uuid);
        vfs_unbusy(busy_mp);
        if (ok) {
            memcpy(out, vfa.f_uuid, 16u);
            return true;
        }
    }
    return pinned_uuid_str != NULL && switch_prefetch_parse_uuid(pinned_uuid_str, out) &&
           !switch_prefetch_uuid_is_zero(out);
}

static bool switch_prefetch_expected_key(struct switch_prefetch_header *key)
{
    memset(key, 0, sizeof(*key));
    memcpy(key->magic, SWITCH_PREFETCH_LIST_V3_MAGIC, 8);
    key->version = SWITCH_PREFETCH_LIST_VERSION;
    key->header_bytes = sizeof(*key);
    vfs_context_t ctx = vfs_context_kernel();
    for (unsigned i = 0; i < SWITCH_PREFETCH_CRYPTEX_COUNT; ++i) {
        vnode_t vp = NULLVP;
        if (vnode_open(k_cryptex_images[i], FREAD | O_NOFOLLOW_ANY, 0, 0, &vp, ctx) != 0) {
            return false;
        }
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_data_size);
        VATTR_WANTED(&va, va_fileid);
        VATTR_WANTED(&va, va_modify_time);
        const bool ok = vnode_isreg(vp) && vnode_getattr(vp, &va, ctx) == 0 && VATTR_IS_SUPPORTED(&va, va_data_size) &&
                        VATTR_IS_SUPPORTED(&va, va_fileid) && VATTR_IS_SUPPORTED(&va, va_modify_time);
        vnode_close(vp, FREAD, ctx);
        if (!ok) {
            return false;
        }
        key->images[i].bytes = va.va_data_size;
        key->images[i].file_id = va.va_fileid;
        key->images[i].mtime_sec = va.va_modify_time.tv_sec;
        key->images[i].mtime_nsec = va.va_modify_time.tv_nsec;
    }
    if (!switch_prefetch_mount_uuid(switch_prefetch_mount_at(SWITCH_PREFETCH_VOL_ROOT),
                                    g_prefetch.fsids[SWITCH_PREFETCH_VOL_ROOT], SWITCH_OS_VOLUME_UUID, key->root_uuid,
                                    ctx)) {
        return false;
    }
    if (!switch_prefetch_mount_uuid(switch_prefetch_mount_at(SWITCH_PREFETCH_VOL_DATA),
                                    g_prefetch.fsids[SWITCH_PREFETCH_VOL_DATA], NULL, key->data_uuid, ctx)) {
        return false;
    }
    return true;
}

static int switch_prefetch_read_all(vnode_t vp, void *buf, size_t len, off_t offset, vfs_context_t ctx)
{
    uint8_t *dst = (uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        const size_t remain = len - done;
        const int chunk = remain > (1u << 20) ? (int)(1u << 20) : (int)remain;
        int resid = chunk;
        const int err = vn_rdwr(UIO_READ, vp, (caddr_t)(dst + done), chunk, offset + (off_t)done, UIO_SYSSPACE,
                                IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
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

static struct switch_prefetch_run *switch_prefetch_load_list(const struct switch_prefetch_header *expected,
                                                             uint32_t *count_out, size_t *alloc_bytes_out,
                                                             const char **why_out)
{
    *count_out = 0;
    *alloc_bytes_out = 0;
    *why_out = "absent";
    char path[192];
    switch_prefetch_list_path(path, sizeof(path));
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    if (vnode_open(path, FREAD | O_NOFOLLOW_ANY, 0, 0, &vp, ctx) != 0) {
        return NULL;
    }
    struct switch_prefetch_run *runs = NULL;
    size_t body_bytes = 0;
    const char *why = NULL;
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    struct switch_prefetch_header header;
    char magic[8];
    if (!vnode_isreg(vp) || vnode_getattr(vp, &va, ctx) != 0 || !VATTR_IS_SUPPORTED(&va, va_data_size)) {
        why = "not a regular file";
    } else if (va.va_data_size < 8u) {
        why = "too short";
    } else if (switch_prefetch_read_all(vp, magic, sizeof(magic), 0, ctx) != 0) {
        why = "read failed";
    } else if (memcmp(magic, SWITCH_PREFETCH_LIST_V2_MAGIC, 8) == 0) {
        why = "another format";
    } else if (va.va_data_size < sizeof(header)) {
        why = "too short";
    } else if (switch_prefetch_read_all(vp, &header, sizeof(header), 0, ctx) != 0) {
        why = "read failed";
    } else if ((why = switch_prefetch_header_refusal(&header, expected, SWITCH_PREFETCH_MAX_RUNS)) != NULL) {
    } else if (va.va_data_size != sizeof(header) + (size_t)header.runs * sizeof(struct switch_prefetch_run)) {
        why = "torn (short list)";
    } else {
        body_bytes = (size_t)header.runs * sizeof(struct switch_prefetch_run);
        runs = (struct switch_prefetch_run *)kalloc_data(body_bytes, Z_WAITOK);
        if (runs == NULL) {
            why = "no memory";
        } else if (switch_prefetch_read_all(vp, runs, body_bytes, (off_t)sizeof(header), ctx) != 0) {
            why = "read failed";
        }
    }
    vnode_close(vp, FREAD, ctx);
    if (why == NULL) {
        why = switch_prefetch_validate_runs(&header, runs, body_bytes);
    }
    if (why != NULL) {
        if (runs != NULL) {
            kfree_data(runs, body_bytes);
        }
        *why_out = why;
        return NULL;
    }
    *count_out = header.runs;
    *alloc_bytes_out = body_bytes;
    *why_out = NULL;
    return runs;
}

static uint32_t switch_prefetch_replay_runs(const struct switch_prefetch_run *runs, uint32_t run_count,
                                            uint8_t *replayed_bits)
{
    uint64_t list_bytes = 0;
    for (uint32_t i = 0; i < run_count; ++i) {
        list_bytes += (uint64_t)runs[i].pages * SWITCH_PREFETCH_LIST_PAGE;
    }
    const uint64_t list_vm_pages = (list_bytes + PAGE_SIZE - 1u) >> PAGE_SHIFT;
    const uint64_t bound = switch_vm_prefetch_begin(list_vm_pages, current_thread());
    if (bound == 0u) {
        switch_vm_prefetch_end_replay();
        return 0u;
    }
    switch_cluster_record_set_skip_thread(current_thread());

    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    uint64_t vp_file_id = 0;
    uint8_t vp_volume = 0xffu;
    struct switch_prefetch_file_meta vp_meta = {0};
    bool vp_valid = false;
    uint32_t replayed = 0;

    for (uint32_t i = 0; i < run_count; ++i) {
        if (!switch_vm_prefetch_can_issue_now()) {
            /* Release any vnode iocount before blocking so unmount never waits on the replay thread. */
            if (vp != NULLVP) {
                vnode_put(vp);
                vp = NULLVP;
            }
            vp_volume = 0xffu;
            vp_valid = false;
            if (!switch_vm_prefetch_wait_can_issue()) {
                break;
            }
        }
        const struct switch_prefetch_run *run = &runs[i];
        mount_t mp = switch_prefetch_mount_at(run->volume);
        if (mp == NULL) {
            continue;
        }
        if (run->file_id != vp_file_id || run->volume != vp_volume) {
            if (vp != NULLVP) {
                vnode_put(vp);
                vp = NULLVP;
            }
            vp_file_id = run->file_id;
            vp_volume = run->volume;
            vp_valid =
                switch_prefetch_vget_regular(mp, g_prefetch.fsids[run->volume], run->file_id, ctx, &vp, &vp_meta);
        } else if (vp_valid && (run->volume == SWITCH_PREFETCH_VOL_DATA || vnode_isrecycled(vp))) {
            /* Re-validate writable Data vnodes across consecutive runs so truncation or unlink skips cleanly. */
            if (!switch_prefetch_vnode_meta(vp, mp, run->file_id, ctx, NULL, &vp_meta)) {
                vnode_put(vp);
                vp = NULLVP;
                vp_valid = false;
            }
        }
        if (!vp_valid ||
            !switch_prefetch_run_matches_file(run, vp_meta.file_size, vp_meta.mtime_sec, vp_meta.mtime_nsec)) {
            continue;
        }
        struct switch_prefetch_pagein in;
        if (!switch_prefetch_run_pagein(run, SWITCH_PREFETCH_LIST_PAGE, PAGE_SIZE, vp_meta.file_size, &in)) {
            continue;
        }
        int err = 0;
        const int rc = vnode_pagein(vp, NULL, 0, in.offset, (upl_size_t)in.length, 0, &err);
        if (rc == PAGER_SUCCESS) {
            if (replayed_bits != NULL) {
                switch_prefetch_bit_set(replayed_bits, i);
            }
            replayed = i + 1u;
        }
    }
    if (vp != NULLVP) {
        vnode_put(vp);
    }
    switch_cluster_record_set_skip_thread(NULL);
    switch_vm_prefetch_end_replay();
    switch_vm_prefetch_wait_inflight_drain();
    return replayed;
}

static void switch_prefetch_thread(void *parameter, wait_result_t w __unused)
{
    struct switch_record_entry *log = (struct switch_record_entry *)parameter;
    const size_t log_bytes = (size_t)SWITCH_PREFETCH_LOG_ENTRIES * sizeof(*log);
    struct switch_prefetch_header key;
    struct switch_prefetch_run *runs = NULL;
    uint8_t *replayed_bits = NULL;
    size_t replayed_bits_bytes = 0;
    uint32_t run_count = 0;
    size_t runs_bytes = 0;
    uint32_t replayed = 0;
    bool have_key = false;

    if (switch_vm_prefetch_boot_stop_reason() == SWITCH_PREFETCH_STOP_NONE) {
        have_key = switch_prefetch_expected_key(&key);
        if (!have_key) {
            os_log(OS_LOG_DEFAULT, "SwitchBootPrefetch: off this boot (volume identity unavailable)");
        } else if (switch_vm_prefetch_boot_stop_reason() == SWITCH_PREFETCH_STOP_NONE) {
            const char *why = NULL;
            runs = switch_prefetch_load_list(&key, &run_count, &runs_bytes, &why);
            if (runs == NULL) {
                os_log(OS_LOG_DEFAULT, "SwitchBootPrefetch: no list to replay (%s); this boot records one", why);
            } else if (run_count != 0u) {
                replayed_bits_bytes = ((size_t)run_count + 7u) / 8u;
                replayed_bits = (uint8_t *)kalloc_data(replayed_bits_bytes, Z_WAITOK | Z_ZERO);
            }
        }
    }

#if DEVELOPMENT || DEBUG
    if (have_key && switch_vm_prefetch_boot_stop_reason() == SWITCH_PREFETCH_STOP_NONE) {
        g_prefetch.diag_call = thread_call_allocate_with_options(switch_prefetch_diag_tick, NULL,
                                                                 THREAD_CALL_PRIORITY_LOW, THREAD_CALL_OPTIONS_ONCE);
        if (g_prefetch.diag_call != NULL) {
            uint64_t deadline = 0;
            clock_interval_to_deadline(10u, NSEC_PER_SEC, &deadline);
            thread_call_enter_delayed(g_prefetch.diag_call, deadline);
        }
    }
#endif

    if (runs != NULL && switch_vm_prefetch_replay_stop_reason() == SWITCH_PREFETCH_STOP_NONE) {
        replayed = switch_prefetch_replay_runs(runs, run_count, replayed_bits);
    }

    const uint32_t stop_reason = switch_vm_prefetch_wait_stop();

#if DEVELOPMENT || DEBUG
    if (g_prefetch.diag_call != NULL) {
        thread_call_t call = g_prefetch.diag_call;
        __atomic_store_n(&g_prefetch.diag_stop, 1u, __ATOMIC_SEQ_CST);
        do {
            thread_call_cancel_wait(call);
        } while (!thread_call_free(call));
        g_prefetch.diag_call = NULL;
    }
#endif

    switch_vm_prefetch_finish();

    const uint32_t appended = switch_cluster_record_stop();
    if (stop_reason == SWITCH_PREFETCH_STOP_BOOT_COMPLETE && have_key && log != NULL) {
        mount_t mounts[SWITCH_PREFETCH_VOLUME_COUNT];
        for (unsigned v = 0; v < SWITCH_PREFETCH_VOLUME_COUNT; ++v) {
            mounts[v] = switch_prefetch_mount_at(v);
        }
        switch_prefetch_save_next(log, appended, runs, replayed, replayed_bits, &key, mounts, g_prefetch.fsids,
                                  SWITCH_PREFETCH_VOLUME_COUNT);
    }

    if (log != NULL) {
        kfree_data(log, log_bytes);
    }
    if (replayed_bits != NULL) {
        kfree_data(replayed_bits, replayed_bits_bytes);
    }
    if (runs != NULL) {
        kfree_data(runs, runs_bytes);
    }

    __atomic_store_n(&g_prefetch.thread_done, 1u, __ATOMIC_SEQ_CST);
    thread_wakeup((event_t)&g_prefetch.done_event);
    thread_terminate(current_thread());
}

static void switch_prefetch_start(void)
{
    if (__atomic_exchange_n(&g_prefetch.started, 1u, __ATOMIC_ACQ_REL) != 0u) {
        return;
    }
    /* Publish thread_active before reading boot_stop (seq_cst Dekker pair with switch_prefetch_shutdown). */
    __atomic_store_n(&g_prefetch.thread_active, 1u, __ATOMIC_SEQ_CST);
    if (switch_vm_prefetch_boot_stop_reason() != SWITCH_PREFETCH_STOP_NONE) {
        __atomic_store_n(&g_prefetch.thread_done, 1u, __ATOMIC_SEQ_CST);
        thread_wakeup((event_t)&g_prefetch.done_event);
        return;
    }
    const size_t log_bytes = (size_t)SWITCH_PREFETCH_LOG_ENTRIES * sizeof(struct switch_record_entry);
    struct switch_record_entry *log = (struct switch_record_entry *)kalloc_data(log_bytes, Z_WAITOK | Z_ZERO);
    if (log == NULL) {
        os_log(OS_LOG_DEFAULT, "SwitchBootPrefetch: off this boot (no memory for recording log)");
        __atomic_store_n(&g_prefetch.thread_done, 1u, __ATOMIC_SEQ_CST);
        thread_wakeup((event_t)&g_prefetch.done_event);
        return;
    }
    if (switch_vm_prefetch_boot_stop_reason() != SWITCH_PREFETCH_STOP_NONE) {
        kfree_data(log, log_bytes);
        __atomic_store_n(&g_prefetch.thread_done, 1u, __ATOMIC_SEQ_CST);
        thread_wakeup((event_t)&g_prefetch.done_event);
        return;
    }
    switch_cluster_record_start(log, SWITCH_PREFETCH_LOG_ENTRIES);
    if (!switch_vm_prefetch_start_thread(switch_prefetch_thread, log)) {
        (void)switch_cluster_record_stop();
        kfree_data(log, log_bytes);
        __atomic_store_n(&g_prefetch.thread_done, 1u, __ATOMIC_SEQ_CST);
        thread_wakeup((event_t)&g_prefetch.done_event);
    }
}

void switch_cryptex_mounted(unsigned index, mount_t mount)
{
    if (index >= SWITCH_PREFETCH_CRYPTEX_COUNT || mount == NULL || switch_prefetch_mount_at(index) != NULL ||
        switch_prefetch_ready_has_mount(&g_prefetch.ready, mount)) {
        return;
    }
    g_prefetch.fsids[index] = vfs_statfs(mount)->f_fsid;
    switch_cluster_register_cryptex_mount(index, mount);
    if (switch_prefetch_ready_add(&g_prefetch.ready, index, mount)) {
        switch_prefetch_start();
    }
}

void switch_prefetch_volumes_mounted(mount_t root_mount, mount_t data_mount)
{
    if (root_mount == NULL || data_mount == NULL || root_mount == data_mount) {
        return;
    }
    if ((vfs_flags(root_mount) & MNT_RDONLY) == 0 || (vfs_flags(data_mount) & MNT_RDONLY) != 0) {
        return;
    }
    if (strcmp(vfs_statfs(root_mount)->f_fstypename, "apfs") != 0 ||
        strcmp(vfs_statfs(data_mount)->f_fstypename, "apfs") != 0) {
        return;
    }
    if (switch_prefetch_mount_at(SWITCH_PREFETCH_VOL_ROOT) != NULL ||
        switch_prefetch_mount_at(SWITCH_PREFETCH_VOL_DATA) != NULL ||
        switch_prefetch_ready_has_mount(&g_prefetch.ready, root_mount) ||
        switch_prefetch_ready_has_mount(&g_prefetch.ready, data_mount)) {
        return;
    }
    g_prefetch.fsids[SWITCH_PREFETCH_VOL_ROOT] = vfs_statfs(root_mount)->f_fsid;
    g_prefetch.fsids[SWITCH_PREFETCH_VOL_DATA] = vfs_statfs(data_mount)->f_fsid;
    switch_cluster_register_mount(SWITCH_PREFETCH_VOL_ROOT, root_mount);
    switch_cluster_register_mount(SWITCH_PREFETCH_VOL_DATA, data_mount);
    const bool root_ready = switch_prefetch_ready_add(&g_prefetch.ready, SWITCH_PREFETCH_VOL_ROOT, root_mount);
    const bool data_ready = switch_prefetch_ready_add(&g_prefetch.ready, SWITCH_PREFETCH_VOL_DATA, data_mount);
    if (root_ready || data_ready) {
        switch_prefetch_start();
    }
}

void switch_boot_io_report(void)
{}

void switch_boot_io_complete(void)
{
    (void)switch_vm_prefetch_stop(SWITCH_PREFETCH_STOP_BOOT_COMPLETE);
}

void switch_prefetch_shutdown(void)
{
    (void)switch_vm_prefetch_stop(SWITCH_PREFETCH_STOP_SHUTDOWN);
    if (__atomic_load_n(&g_prefetch.thread_active, __ATOMIC_SEQ_CST) == 0u) {
        return;
    }
    while (__atomic_load_n(&g_prefetch.thread_done, __ATOMIC_SEQ_CST) == 0u) {
        assert_wait((event_t)&g_prefetch.done_event, THREAD_UNINT);
        if (__atomic_load_n(&g_prefetch.thread_done, __ATOMIC_SEQ_CST) != 0u) {
            clear_wait(current_thread(), THREAD_AWAKENED);
            break;
        }
        thread_block(THREAD_CONTINUE_NULL);
    }
}
