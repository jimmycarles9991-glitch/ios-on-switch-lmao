// SPDX-License-Identifier: GPL-2.0-only
#include "xnu_decmp_reprio_harness.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#undef DEVELOPMENT
#undef DEBUG
#define DEVELOPMENT 0
#define DEBUG       0

#define PAGE_SIZE              16384u
#define atop(x)                ((uint64_t)(x) / PAGE_SIZE)
#define page_aligned(x)        (((x) % PAGE_SIZE) == 0)
#define assert(e)              ((void)0)
#define assertf(e, ...)        ((void)0)
#define KERN_SUCCESS           0
#define UPL_TRACKED_BY_OBJECT  0x1
#define UPL_EXPEDITE_SUPPORTED 0x2
#define UPL_DECMP_REAL_IO      0x4
#define UPL_SHADOWED           0x8
#define UPL_REPRIO_INFO_SHIFT  32
#define UPL_REPRIO_INFO_MASK   0xffffffffULL

typedef uint32_t zalloc_flags_t;
#define Z_WAITOK 0x0000u
#define Z_NOWAIT 0x0001u
#define Z_NOFAIL 0x8000u

struct vnode;

struct vm_object {
    struct vm_object *shadow;
    void *pager;
};
typedef struct vm_object *vm_object_t;

struct upl {
    int flags;
    struct upl *decmp_io_upl;
    uint64_t u_offset;
    uint32_t u_size;
    uint64_t *upl_reprio_info;
    vm_object_t map_object;
};
typedef struct upl *upl_t;

struct switch_test_link {
    void *next;
};

struct io_reprioritize_req {
    uint64_t blkno;
    uint32_t len;
    int priority;
    struct vnode *devvp;
    struct switch_test_link iorr_elm;
    struct switch_test_link io_reprioritize_list;
};
typedef struct io_reprioritize_req *io_reprioritize_req_t;

static struct xnu_decmp_reprio_run *g_run;
static unsigned g_noblock_calls;
static int io_reprioritize_req_zone;
static int io_reprioritize_q;
static int io_reprioritize_list;

static int upl_try_lock(upl_t upl)
{
    (void)upl;
    return 1;
}

static void upl_unlock(upl_t upl)
{
    (void)upl;
}

static int vnode_pager_get_object_devvp(void *pager, uintptr_t *devvp)
{
    (void)pager;
    *devvp = 0x5d;
    return KERN_SUCCESS;
}

static void *switch_test_kalloc(size_t size, zalloc_flags_t flags)
{
    ++*((flags & Z_NOWAIT) ? &g_run->nowait_allocs : &g_run->blocking_allocs);
    return g_run->fail_kalloc ? NULL : calloc(1, size);
}

static void *zalloc_flags(int zone, zalloc_flags_t flags)
{
    (void)zone;
    ++*((flags & Z_NOWAIT) ? &g_run->nowait_allocs : &g_run->blocking_allocs);
    return calloc(1, sizeof(struct io_reprioritize_req));
}

static void *zalloc_noblock(int zone)
{
    (void)zone;
    ++g_run->nowait_allocs;
    return ++g_noblock_calls == g_run->fail_req_at ? NULL : calloc(1, sizeof(struct io_reprioritize_req));
}

static void switch_test_enqueue(io_reprioritize_req_t req)
{
    if (g_run->enqueued < sizeof(g_run->enqueued_blknos) / sizeof(g_run->enqueued_blknos[0]))
        g_run->enqueued_blknos[g_run->enqueued] = req->blkno;
    ++g_run->enqueued;
    free(req);
}

static void mpsc_daemon_enqueue(int *queue, struct switch_test_link *elm, int options)
{
    (void)queue;
    (void)options;
    switch_test_enqueue((io_reprioritize_req_t)(void *)((char *)elm - offsetof(struct io_reprioritize_req, iorr_elm)));
}

#define kalloc_data(size, flags)            switch_test_kalloc((size), (flags))
#define kfree_data(p, size)                 free(p)
#define MPSC_QUEUE_DISABLE_PREEMPTION       0
#define IO_REPRIORITIZE_LIST_LOCK()         ((void)0)
#define IO_REPRIORITIZE_LIST_UNLOCK()       ((void)0)
#define IO_REPRIO_THREAD_WAKEUP()           ((void)0)
#define queue_enter(head, elt, type, field) switch_test_enqueue(elt)

#define vm_decmp_upl_reprioritize vm_decmp_upl_reprioritize_10063
#include "decmp_reprioritize_10063.h"
#undef vm_decmp_upl_reprioritize

#define vm_decmp_upl_reprioritize vm_decmp_upl_reprioritize_12377
#include "decmp_reprioritize_12377.h"
#undef vm_decmp_upl_reprioritize

void xnu_decmp_reprio(int tree, int nowait, const uint64_t *info, unsigned pages, struct xnu_decmp_reprio_run *run)
{
    g_run = run;
    g_noblock_calls = 0;
    struct vm_object object = {NULL, &object};
    uint64_t *words = calloc(pages, sizeof(*words));
    memcpy(words, info, pages * sizeof(*words));
    struct upl io = {UPL_DECMP_REAL_IO, NULL, 0, pages * PAGE_SIZE, words, &object};
    struct upl request = {UPL_TRACKED_BY_OBJECT | UPL_EXPEDITE_SUPPORTED, &io, 0, pages * PAGE_SIZE, NULL, &object};
    const zalloc_flags_t flags = nowait ? Z_NOWAIT : Z_WAITOK;
    if (tree == 10063)
        vm_decmp_upl_reprioritize_10063(&request, 0, flags);
    else
        vm_decmp_upl_reprioritize_12377(&request, 0, flags);
    free(words);
    (void)io_reprioritize_q;
    (void)io_reprioritize_list;
}
