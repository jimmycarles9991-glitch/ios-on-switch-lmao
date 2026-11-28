// SPDX-License-Identifier: GPL-2.0-only
/*
 * The flight recorder's store (--//kernel:recorder; state.h): captures wait in memory until a thread call at
 * THREAD_CALL_PRIORITY_LOW, which runs only when nothing else wants a core, compresses each into one file in mobile's
 * CrashReporter, so `idevicecrashreport` fetches it and neither the compression nor the write competes with the moment
 * it recorded. The call asks for IOPOL_THROTTLE; whether a kernel_task thread's I/O gets that tier is
 * open (the 25 Nov pack warm's reads, asked the same, went out at tier 0). A file holds one capture: a text header
 * (recorder.h's switch_recorder_header) written last, so a file pulled mid-write reads as incomplete, then the
 * stackshot and the ring as one raw deflate stream.
 *
 * The files are slots Switch-recorder-<n>.rec, as many as a tenth of the volume's free space holds
 * (switch_recorder_slots_for, read once a boot), and recorder.h's switch_recorder_pick_slot chooses which one the next
 * capture replaces: other boots' first, then this boot's automatic captures past its first ones; an owner's mark is
 * never replaced by an automatic capture.
 *
 * Memory: the captures waiting hold at most `budget` bytes, raw. When they fill it, an automatic capture gets no room for
 * its stackshot or its ring and is dropped before it stops any CPU; a mark takes the ring of the newest automatic
 * capture still waiting (it keeps its stackshot).
 */

#include <switch/recorder.h>

#include <sys/param.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/systm.h>
#include <sys/vnode_internal.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/policy_internal.h>
#include <kern/thread_call.h>
#include <libkern/zlib.h>
#include <mach/vm_param.h>
#include <os/log.h>
#include <string.h>
#include <uuid/uuid.h>

#include "state.h"

#if SWITCH_RECORDER

#define STORE_DIR       "/private/var/mobile/Library/Logs/CrashReporter"
#define STORE_MOBILE    501
#define STORE_OUT       (256u << 10) /* compressed bytes written at a time */
#define STORE_RAM_SHARE 32u          /* captures in memory take at most 1/32 of it */
#define STORE_RAM_MAX   (96ull << 20)
#define STORE_WBITS     15
#define STORE_MEMLEVEL  8

extern uuid_string_t bootsessionuuid_string;
extern int unlink1(vfs_context_t, vnode_t, user_addr_t, enum uio_seg, int);

static LCK_GRP_DECLARE(store_grp, "switch_recorder_store");
static LCK_MTX_DECLARE(store_mtx, &store_grp);

static struct {
    thread_call_t call;
    uint64_t budget;
    /* Under store_mtx. */
    struct switch_recorder_capture *head, *tail;
    uint32_t pending, capturing, seq;
    bool writing;
    uint64_t room;
    uint32_t dumps, dropped, no_ring, no_room;
    unsigned nslots;
    struct switch_recorder_last last;
    /* The store call's alone. */
    z_stream zs;
    uint8_t *zmem, *out;
    size_t zmem_size, zmem_used;
    struct switch_recorder_slot slots[SWITCH_RECORDER_SLOTS_MAX];
    bool scanned;
} store;

static void store_call(thread_call_param_t p0, thread_call_param_t p1);

static void *store_zalloc(__unused void *opaque, u_int items, u_int size)
{
    const size_t n = ((size_t)items * size + 15) & ~(size_t)15;
    void *p = NULL;

    if (store.zmem_used + n > store.zmem_size) {
        return NULL;
    }
    p = store.zmem + store.zmem_used;
    store.zmem_used += n;
    return p;
}

static void store_zfree(__unused void *opaque, __unused void *ptr)
{}

bool switch_recorder_store_init(void)
{
    store.zmem_size = zlib_deflate_memory_size(STORE_WBITS, STORE_MEMLEVEL) + 256;
    store.zmem = kalloc_data(store.zmem_size, Z_WAITOK);
    store.out = kalloc_data(STORE_OUT, Z_WAITOK);
    store.call =
        thread_call_allocate_with_options(store_call, NULL, THREAD_CALL_PRIORITY_LOW, THREAD_CALL_OPTIONS_ONCE);
    store.budget = MIN(STORE_RAM_MAX, max_mem / STORE_RAM_SHARE);
    if (store.zmem == NULL || store.out == NULL || store.call == NULL) {
        return false;
    }
    store.zs.zalloc = store_zalloc;
    store.zs.zfree = store_zfree;
    return deflateInit2(&store.zs, Z_BEST_SPEED, Z_DEFLATED, -STORE_WBITS, STORE_MEMLEVEL, Z_DEFAULT_STRATEGY) == Z_OK;
}

uint32_t switch_recorder_store_begin(void)
{
    uint32_t seq = 0;

    lck_mtx_lock(&store_mtx);
    store.capturing++;
    seq = store.seq++;
    lck_mtx_unlock(&store_mtx);
    return seq;
}

/* Under store_mtx: the newest waiting automatic capture's ring freed, false when none has one. */
static bool store_drop_auto_ring(void)
{
    struct switch_recorder_capture *found = NULL;

    for (struct switch_recorder_capture *c = store.head; c != NULL; c = c->next) {
        if (c->ring != NULL && switch_recorder_automatic(c->head.reason)) {
            found = c;
        }
    }
    if (found == NULL) {
        return false;
    }
    kfree_data(found->ring, found->ring_alloc);
    store.room -= found->ring_alloc;
    found->ring = NULL;
    found->ring_alloc = 0;
    found->head.ring_bytes = 0;
    found->head.ring_lost = 2;
    store.no_ring++;
    return true;
}

bool switch_recorder_store_room(size_t bytes, bool mark)
{
    bool fits = false;

    lck_mtx_lock(&store_mtx);
    while (!(fits = store.room + bytes <= store.budget) && mark && store_drop_auto_ring()) {
    }
    if (fits) {
        store.room += bytes;
    }
    lck_mtx_unlock(&store_mtx);
    return fits;
}

void switch_recorder_store_unroom(size_t bytes)
{
    lck_mtx_lock(&store_mtx);
    store.room -= bytes;
    lck_mtx_unlock(&store_mtx);
}

void switch_recorder_capture_free(struct switch_recorder_capture *c)
{
    if (c->shot != NULL) {
        kfree_data(c->shot, c->shot_alloc);
    }
    if (c->ring != NULL) {
        kfree_data(c->ring, c->ring_alloc);
    }
    kfree_type(struct switch_recorder_capture, c);
}

void switch_recorder_store_push(struct switch_recorder_capture *c)
{
    lck_mtx_lock(&store_mtx);
    c->next = NULL;
    if (store.tail != NULL) {
        store.tail->next = c;
    } else {
        store.head = c;
    }
    store.tail = c;
    store.pending++;
    store.capturing--;
    if (c->ring == NULL && c->head.ring_lost == 2) {
        store.no_ring++;
    }
    lck_mtx_unlock(&store_mtx);
    thread_call_enter(store.call);
}

/* An automatic capture that got no room for anything: it holds no memory, so it only leaves the count. */
void switch_recorder_store_abandon(struct switch_recorder_capture *c)
{
    lck_mtx_lock(&store_mtx);
    store.capturing--;
    store.no_room++;
    lck_mtx_unlock(&store_mtx);
    switch_recorder_expect_done(c->head.tid);
    switch_recorder_capture_free(c);
}

bool switch_recorder_store_quiet(void)
{
    bool quiet = false;

    lck_mtx_lock(&store_mtx);
    quiet = store.capturing == 0 && store.pending == 0 && !store.writing;
    lck_mtx_unlock(&store_mtx);
    return quiet;
}

void switch_recorder_store_counts(struct switch_recorder_counts *n, struct switch_recorder_last *last)
{
    lck_mtx_lock(&store_mtx);
    n->captures = store.seq;
    n->dumps = store.dumps;
    n->dropped = store.dropped;
    n->no_ring = store.no_ring;
    n->no_room = store.no_room;
    n->pending = store.pending;
    n->pending_bytes = store.room;
    n->slots = store.nslots;
    *last = store.last;
    lck_mtx_unlock(&store_mtx);
}

static void store_path(char *out, size_t size, unsigned slot)
{
    snprintf(out, size, STORE_DIR "/Switch-recorder-%u.rec", slot);
}

/* The slots for this boot: as many as the volume's free space holds, each as its file's header says. Files past them,
 * left by a boot with more free space, are removed, so less free space never keeps more files. False while the
 * directory cannot be found (Data not mounted yet): the next capture scans again. */
static bool store_scan(vfs_context_t ctx)
{
    char path[96], h[SWITCH_RECORDER_HEADER];
    uint64_t free_bytes = 0;
    unsigned removed = 0;
    vnode_t dvp = NULLVP;
    mount_t mp = NULL;

    if (vnode_lookup(STORE_DIR, 0, &dvp, ctx) != 0) {
        return false;
    }
    mp = vnode_mount(dvp);
    (void)vfs_update_vfsstat(mp, ctx, VFS_KERNEL_EVENT);
    free_bytes = (uint64_t)vfs_statfs(mp)->f_bavail * vfs_statfs(mp)->f_bsize;
    vnode_put(dvp);
    store.scanned = true;
    store.nslots = switch_recorder_slots_for(free_bytes);
    for (unsigned i = 0; i < store.nslots; i++) {
        vnode_t vp = NULLVP;
        int resid = (int)sizeof(h);

        store_path(path, sizeof(path), i);
        store.slots[i] = (struct switch_recorder_slot){0};
        if (vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx) != 0) {
            continue;
        }
        if (vn_rdwr(UIO_READ, vp, (caddr_t)h, (int)sizeof(h), 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx),
                    &resid, vfs_context_proc(ctx)) == 0 &&
            resid == 0) {
            store.slots[i] = switch_recorder_slot_of(h, sizeof(h), bootsessionuuid_string);
        }
        vnode_close(vp, FREAD, ctx);
    }
    for (unsigned i = store.nslots; i < SWITCH_RECORDER_SLOTS_MAX; i++) {
        store_path(path, sizeof(path), i);
        removed += unlink1(ctx, NULLVP, CAST_USER_ADDR_T(path), UIO_SYSSPACE, VNODE_REMOVE_NOFOLLOW_ANY) == 0;
    }
    os_log(OS_LOG_DEFAULT,
           "SwitchRecorder: store of %u slots in " STORE_DIR " (%llu MiB free), %u files past them removed",
           store.nslots, free_bytes >> 20, removed);
    return true;
}

/* An empty file at path, owned by mobile and readable by all, left open for writing in *vp. */
static int store_create(const char *path, vnode_t *vp, vfs_context_t ctx)
{
    struct vnode_attr va;
    int error = vnode_open(path, O_CREAT | FWRITE | O_NOFOLLOW, 0644, 0, vp, ctx);

    if (error) {
        return error;
    }
    VATTR_INIT(&va);
    VATTR_SET(&va, va_data_size, 0);
    VATTR_SET(&va, va_uid, STORE_MOBILE);
    VATTR_SET(&va, va_gid, STORE_MOBILE);
    VATTR_SET(&va, va_mode, 0644);
    error = vnode_setattr(*vp, &va, ctx);
    if (error) {
        vnode_close(*vp, FWRITE, ctx);
        *vp = NULLVP;
    }
    return error;
}

static int store_write(vnode_t vp, const void *buf, size_t bytes, off_t at, vfs_context_t ctx, uint64_t *ticks)
{
    const uint64_t t = mach_absolute_time();
    int resid = 0;
    const int error = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)buf, (int)bytes, at, UIO_SYSSPACE, IO_NOAUTH | IO_UNIT,
                              vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));

    *ticks += mach_absolute_time() - t;
    return error ? error : (resid ? EIO : 0);
}

/* One input of the stream through deflate, written as it fills the output buffer. */
static int store_deflate(vnode_t vp, const void *in, size_t bytes, bool last, off_t *at, vfs_context_t ctx,
                         uint64_t *zticks, uint64_t *wticks)
{
    int error = 0;
    int z = Z_OK;

    store.zs.next_in = (Bytef *)(uintptr_t)in;
    store.zs.avail_in = (uInt)bytes;
    do {
        const uint64_t t = mach_absolute_time();

        store.zs.next_out = store.out;
        store.zs.avail_out = STORE_OUT;
        z = deflate(&store.zs, last ? Z_FINISH : Z_NO_FLUSH);
        *zticks += mach_absolute_time() - t;
        if (z != Z_OK && z != Z_STREAM_END && z != Z_BUF_ERROR) {
            return EIO;
        }
        const size_t produced = STORE_OUT - store.zs.avail_out;
        if (produced != 0 && (error = store_write(vp, store.out, produced, *at, ctx, wticks)) != 0) {
            return error;
        }
        *at += (off_t)produced;
    } while (store.zs.avail_in != 0 || (last && z != Z_STREAM_END));
    return 0;
}

static uint64_t store_us(uint64_t ticks)
{
    uint64_t ns = 0;

    absolutetime_to_nanoseconds(ticks, &ns);
    return ns / NSEC_PER_USEC;
}

/* The capture into its slot: false when it had none. */
static bool store_persist(struct switch_recorder_capture *c, struct switch_recorder_last *last, vfs_context_t ctx)
{
    struct switch_recorder_head *h = &c->head;
    const bool mark = h->reason == SWITCH_RECORDER_MARK;
    char path[96], text[SWITCH_RECORDER_HEADER];
    uint64_t zticks = 0, wticks = 0;
    off_t at = SWITCH_RECORDER_HEADER;
    vnode_t vp = NULLVP;
    int error = 0;
    int slot = 0;

    if (!store.scanned && !store_scan(ctx)) {
        os_log(OS_LOG_DEFAULT, "SwitchRecorder: capture %u (%s) dropped: no " STORE_DIR " yet", h->seq,
               switch_recorder_reason_name(h->reason));
        return false;
    }
    if (store.nslots == 0) {
        os_log(OS_LOG_DEFAULT,
               "SwitchRecorder: capture %u (%s) dropped: a tenth of the volume's free space holds no slot", h->seq,
               switch_recorder_reason_name(h->reason));
        return false;
    }
    if ((slot = switch_recorder_pick_slot(store.slots, store.nslots, mark)) < 0) {
        os_log(OS_LOG_DEFAULT, "SwitchRecorder: capture %u (%s) dropped: every slot holds a mark or a kept capture",
               h->seq, switch_recorder_reason_name(h->reason));
        return false;
    }
    store_path(path, sizeof(path), (unsigned)slot);
    if ((error = store_create(path, &vp, ctx)) != 0) {
        os_log(OS_LOG_DEFAULT, "SwitchRecorder: capture %u: %s refused (%d)", h->seq, path, error);
        return false;
    }
    (void)deflateReset(&store.zs);
    error = store_deflate(vp, c->shot, h->shot_bytes, c->ring == NULL, &at, ctx, &zticks, &wticks);
    if (error == 0 && c->ring != NULL) {
        error = store_deflate(vp, c->ring, h->ring_bytes, true, &at, ctx, &zticks, &wticks);
    }
    h->deflate_bytes = (uint64_t)at - SWITCH_RECORDER_HEADER;
    if (error == 0 && !switch_recorder_header(text, h)) {
        error = EOVERFLOW;
    }
    if (error == 0) {
        error = store_write(vp, text, sizeof(text), 0, ctx, &wticks);
    }
    vnode_close(vp, FWRITE, ctx);
    store.slots[slot] =
        (struct switch_recorder_slot){.used = error == 0, .this_boot = true, .mark = mark, .seq = h->seq};
    if (error != 0) {
        os_log(OS_LOG_DEFAULT, "SwitchRecorder: capture %u: writing %s failed (%d)", h->seq, path, error);
        return false;
    }
    *last = (struct switch_recorder_last){
        .head = *h, .slot = (unsigned)slot, .deflate_us = store_us(zticks), .write_us = store_us(wticks)};
    os_log(OS_LOG_DEFAULT,
           "SwitchRecorder: dump %u (%s ask %u, thread %llu, value %llu) in slot %u: stackshot %llu KiB and ring %llu "
           "KiB as %llu KiB, deflate %llu us, write %llu us, %llu ms after its trigger",
           h->seq, switch_recorder_reason_name(h->reason), h->ask, h->tid, h->value, slot, h->shot_bytes >> 10,
           h->ring_bytes >> 10, h->deflate_bytes >> 10, last->deflate_us, last->write_us,
           store_us(mach_absolute_time() - h->at) / 1000);
    return true;
}

static void store_call(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
    const int iopol = proc_get_thread_policy(current_thread(), TASK_POLICY_INTERNAL, TASK_POLICY_IOPOL);
    vfs_context_t ctx = vfs_context_kernel();

    proc_set_thread_policy(current_thread(), TASK_POLICY_INTERNAL, TASK_POLICY_IOPOL, IOPOL_THROTTLE);
    for (;;) {
        struct switch_recorder_last last = {0};
        struct switch_recorder_capture *c = NULL;
        bool written = false;

        lck_mtx_lock(&store_mtx);
        if ((c = store.head) != NULL) {
            store.head = c->next;
            if (store.head == NULL) {
                store.tail = NULL;
            }
            store.writing = true;
        }
        lck_mtx_unlock(&store_mtx);
        if (c == NULL) {
            break;
        }
        written = store_persist(c, &last, ctx);
        lck_mtx_lock(&store_mtx);
        store.writing = false;
        store.pending--;
        store.room -= c->shot_alloc + c->ring_alloc;
        if (written) {
            store.dumps++;
            store.last = last;
        } else {
            store.dropped++;
        }
        lck_mtx_unlock(&store_mtx);
        switch_recorder_expect_done(c->head.tid);
        switch_recorder_capture_free(c);
    }
    proc_set_thread_policy(current_thread(), TASK_POLICY_INTERNAL, TASK_POLICY_IOPOL, iopol);
}

#endif /* SWITCH_RECORDER */
