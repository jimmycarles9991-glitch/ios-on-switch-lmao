// Host mock of the TegraGM20B user client (CONTRACT.md v1) for NVK's nvkmd_darwin, interposed over IOKit: a "GM20B" that
// accepts every call and completes every submission at once (its fence page shows the seqno as soon as Exec returns).
// Enough for vkCreateDevice, metal2vulkan's device open and shader/pipeline creation (NAK SM53); nothing is executed.
// r8-rawca: Map/Unmap (methods 12, 13; MOCK_NO_KERNEL_MAP=1 hides them) and Order (method 14, the kext's tgm_chan_order: every
// channel of the mock is one task's; MOCK_NO_ORDER=1 hides it). MOCK_GPU_US=<n>: an asynchronous GPU instead, one thread that
// runs the channels side by side (a thread each), each channel's submissions in kick order, n us each, a submission only once
// the acquires Order queued for it have passed (as the host's semaphore acquires): its fence word moves then, WaitFence
// sleeps until it does (a condition variable, as the kext's interrupt), and "mock GPU: deadlock" aborts if nothing can run.
// MOCK_GPU_LOG=<file>: every submission run, "ch seqno kick_us start_us end_us acquires", and the WaitFence calls by thread.
// MOCK_IGNORE_ACQUIRES=1: the GPU runs submissions without their acquires (ordercheck.py's negative control).
// MOCK_IMPORT_US=<n>: BoImport takes n us per 16 MiB (the kext's wiring; selcheck's pool test).
// SurfaceLayout (method 16; MOCK_NO_SURFACE_LAYOUT=1 hides it) decides a surface's layout as the kext does: at its first asking
// import, block-linear when that import writes it, is the surface's only import and its shape tiles, its rows then tiled in
// place (TegraGM20BLayout.h); pitch for good otherwise.
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "TegraGM20BABI.h"
#include "TegraGM20BLayout.h"
void pushcount_bind(uint64_t va, uint64_t range, int fd, void *import, uint64_t offset, uint64_t bytes);
void pushcount_exec(const struct TegraGM20BAbiExec *e);

#define DYLD_INTERPOSE(_replacement, _replacee)                                 \
    __attribute__((used)) static struct {                                       \
        const void *replacement;                                                \
        const void *replacee;                                                   \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(unsigned long)&_replacement, (const void *)(unsigned long)&_replacee};

enum {
    kService = 0x7e57,
    kConn = 0x7e58,
    kFenceToken = 0xfe000001,
    kMaxBo = 65536
};

struct bo {
    int fd;
    uint64_t bytes;
    void *import;
    uint32_t surface;
};
static struct bo bos[kMaxBo];
static atomic_uint nbo = 1;
static uint32_t submitted[16];
static uint32_t *fence; /* the fence page, one uint32 per channel at slot*16 */
static atomic_ulong calls[16];
static atomic_ulong import_maps;    /* VmBind map ops onto imported pages */
static atomic_ulong kind_maps[256]; /* VmBind map ops by PTE kind */

/* Surfaces' layouts (method 16), by surface ID */
enum {
    kSurfaces = 256
};

static struct surface_layout {
    uint32_t id;
    int tiled;
    struct tgm_layout layout;
} surfaces[kSurfaces];

static pthread_mutex_t surfaces_lock = PTHREAD_MUTEX_INITIALIZER;

/* Order and the asynchronous GPU (gpu_lock) */
enum {
    kOrderMax = TegraGM20BMaxOrderWaits,
    kJobs = 8192
};

struct acq {
    uint32_t ch, seqno;
};

struct job {
    uint32_t ch, seqno, nacq;
    struct acq acq[kOrderMax];
    struct TegraGM20BAbiExec *exec;
    uint64_t kick;
};

struct run {
    uint32_t ch, seqno, nacq;
    uint64_t kick, start, end;
};

static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gpu_cond = PTHREAD_COND_INITIALIZER;
static struct acq pending[16][kOrderMax];
static uint32_t npending[16], nchannels;

enum {
    kQueue = 1024
};
static struct job queue[16][kQueue]; /* per channel, in kick order: [head, tail) */
static uint32_t qhead[16], qtail[16], running;
static struct run runs[kJobs];
static uint32_t nruns;
static unsigned long order_calls, order_queued, waits_total, waits_submit;
static int gpu_us = -1;

static uint64_t now_us(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

static int passed(uint32_t ch, uint32_t seqno)
{
    return (int32_t)(fence[ch * 4] - seqno) >= 0;
}

static int async_gpu(void)
{
    if (gpu_us < 0)
        gpu_us = getenv("MOCK_GPU_US") ? atoi(getenv("MOCK_GPU_US")) : 0;
    return gpu_us > 0;
}

/* gpu_lock held: `waiter`'s next submission waits for `source` to pass `seqno` (one per source, the later seqno) */
static int order_add(uint32_t waiter, uint32_t source, uint32_t seqno)
{
    for (uint32_t i = 0; i < npending[waiter]; i++)
        if (pending[waiter][i].ch == source) {
            if ((int32_t)(seqno - pending[waiter][i].seqno) > 0)
                pending[waiter][i].seqno = seqno;
            return 0;
        }
    if (npending[waiter] == kOrderMax)
        return -1;
    pending[waiter][npending[waiter]++] = (struct acq){source, seqno};
    return 0;
}

/* gpu_lock held: the head of channel ch's queue may run (its acquires passed; MOCK_IGNORE_ACQUIRES: always) */
static int head_ready(uint32_t ch, int ignore_acquires)
{
    if (qhead[ch] == qtail[ch])
        return 0;
    const struct job *j = &queue[ch][qhead[ch] % kQueue];
    for (uint32_t k = 0; k < j->nacq && !ignore_acquires; k++)
        if (!passed(j->acq[k].ch, j->acq[k].seqno))
            return 0;
    return 1;
}

/* One per channel, as the host's PBDMAs and the engines' time slices run channels side by side: the channel's submissions in
 * order, each once its acquires passed, each taking MOCK_GPU_US. */
static void *gpu_thread(void *arg)
{
    const uint32_t ch = (uint32_t)(uintptr_t)arg;
    char name[32];
    snprintf(name, sizeof name, "mock.gpu.%u", ch);
    pthread_setname_np(name);
    const int ignore_acquires = getenv("MOCK_IGNORE_ACQUIRES") != NULL; /* the checker's negative control */
    pthread_mutex_lock(&gpu_lock);
    for (;;) {
        if (!head_ready(ch, ignore_acquires)) {
            if (qhead[ch] != qtail[ch] && !running) { /* nothing runs that could move a fence: none ever will */
                int any = 0;
                for (uint32_t c = 1; c < 16 && !any; c++)
                    any = head_ready(c, ignore_acquires);
                if (!any) {
                    const struct job *j = &queue[ch][qhead[ch] % kQueue];
                    fprintf(stderr, "mock GPU: deadlock: ch %u seqno %u waits on ch %u fence %u (at %u)\n", j->ch,
                            j->seqno, j->acq[0].ch, j->acq[0].seqno, fence[j->acq[0].ch * 4]);
                    abort();
                }
            }
            pthread_cond_wait(&gpu_cond, &gpu_lock);
            continue;
        }
        struct job j = queue[ch][qhead[ch] % kQueue];
        running++;
        pthread_mutex_unlock(&gpu_lock);
        const uint64_t start = now_us();
        pushcount_exec(j.exec);
        usleep(gpu_us);
        const uint64_t end = now_us();
        free(j.exec);
        pthread_mutex_lock(&gpu_lock);
        running--;
        qhead[ch]++;
        fence[ch * 4] = j.seqno;
        if (nruns < kJobs)
            runs[nruns++] = (struct run){j.ch, j.seqno, j.nacq, j.kick, start, end};
        pthread_cond_broadcast(&gpu_cond);
    }
    return NULL;
}

static void gpu_log(void)
{
    const char *path = getenv("MOCK_GPU_LOG");
    FILE *f = path ? fopen(path, "w") : NULL;
    if (!f)
        return;
    pthread_mutex_lock(&gpu_lock);
    for (uint32_t i = 0; i < nruns; i++)
        fprintf(f, "run %u %u %llu %llu %llu %u\n", runs[i].ch, runs[i].seqno, (unsigned long long)runs[i].kick,
                (unsigned long long)runs[i].start, (unsigned long long)runs[i].end, runs[i].nacq);
    fprintf(f, "order calls %lu acquires %lu\nwaitfence total %lu submit-thread %lu\n", order_calls, order_queued,
            waits_total, waits_submit);
    pthread_mutex_unlock(&gpu_lock);
    fclose(f);
}

/* Only the GPU's class is mocked; IOSurfaceRoot and the rest go to the real IOKit. */
static io_service_t mock_get(mach_port_t main, CFDictionaryRef matching)
{
    CFStringRef cls = matching ? CFDictionaryGetValue(matching, CFSTR("IOProviderClass")) : NULL;
    char name[128] = "";
    if (cls)
        CFStringGetCString(cls, name, sizeof name, kCFStringEncodingUTF8);
    if (strcmp(name, "IOAcceleratorES") && strcmp(name, "AGXAccelerator") &&
        strcmp(name, "TegraGM20BUserClientService") && strncmp(name, "Tegra", 5))
        return IOServiceGetMatchingService(main, matching);
    if (matching)
        CFRelease(matching);
    return kService;
}

static kern_return_t mock_open(io_service_t s, task_port_t t, uint32_t type, io_connect_t *c)
{
    if (s != kService)
        return IOServiceOpen(s, t, type, c);
    if (s != kService || type != TegraGM20BOpenType)
        return kIOReturnBadArgument;
    *c = kConn;
    return KERN_SUCCESS;
}

static kern_return_t mock_close(io_connect_t c)
{
    return c == kConn ? KERN_SUCCESS : IOServiceClose(c);
}

static kern_return_t mock_release(io_object_t o)
{
    return (o == kService || o == kConn) ? KERN_SUCCESS : IOObjectRelease(o);
}

static void *backing(uint32_t token, uint64_t *bytes, int *fd)
{
    if (token == kFenceToken) {
        *bytes = 4096;
        *fd = -1;
        return fence;
    }
    if (!token || token >= kMaxBo || !bos[token].bytes)
        return NULL;
    *bytes = bos[token].bytes;
    *fd = bos[token].fd;
    return bos[token].import ? bos[token].import : (void *)1;
}

static kern_return_t mock_map(io_connect_t c, uint32_t type, task_port_t t, mach_vm_address_t *addr,
                              mach_vm_size_t *size, IOOptionBits opt);
static kern_return_t mock_unmap(io_connect_t c, uint32_t type, task_port_t t, mach_vm_address_t addr);

/* The layout of the surface `bo` imports, decided at its first ask */
static uint32_t surface_layout(const struct bo *bo, const struct TegraGM20BAbiSurfaceLayout *a)
{
    pthread_mutex_lock(&surfaces_lock);
    struct surface_layout *s = NULL;
    for (unsigned i = 0; i < kSurfaces && !s; i++)
        if (surfaces[i].id == bo->surface)
            s = &surfaces[i];
    if (!s) {
        for (unsigned i = 0; i < kSurfaces && !s; i++)
            if (!surfaces[i].id)
                s = &surfaces[i];
        unsigned imports = 0;
        for (unsigned h = 1; h < kMaxBo; h++)
            imports += bos[h].surface == bo->surface;
        struct tgm_layout l = {0};
        const int tiles = (a->flags & TegraGM20BLayoutWrites) && imports == 1 &&
                          tgm_layout_tiled(a->width, a->height, a->bpp, &l) && l.bytes <= bo->bytes &&
                          (uint64_t)a->bytes_per_row * a->height <= bo->bytes && a->bytes_per_row >= a->width * a->bpp;
        *s = (struct surface_layout){bo->surface, tiles, l};
        if (tiles) { /* the rows tiled in place, as the kext's copy-engine move does */
            uint8_t *rows = malloc((size_t)a->bytes_per_row * a->height);
            memcpy(rows, bo->import, (size_t)a->bytes_per_row * a->height);
            memset(bo->import, 0, l.bytes);
            tgm_layout_write(&l, bo->import, 0, 0, a->width, a->height, rows, a->bytes_per_row);
            free(rows);
        }
    }
    const uint32_t layout = !s->tiled ? 0
                            : s->layout.width == a->width && s->layout.height == a->height && s->layout.bpp == a->bpp
                                ? TegraGM20BLayoutTiled
                                : TegraGM20BLayoutOtherShape;
    pthread_mutex_unlock(&surfaces_lock);
    return layout;
}

static kern_return_t mock_call(mach_port_t c, uint32_t sel, const void *in, size_t insz, void *out, size_t *outsz)
{
    static atomic_int logged;
    if (!atomic_exchange(&logged, 1))
        atexit(gpu_log);
    if (c != kConn)
        return IOConnectCallStructMethod(c, sel, in, insz, out, outsz);
    const uint32_t *w = in;
    if (insz >= 8 && w[0] == TegraGM20BTunnelMagic) {
        sel = w[1];
        in = w + 2;
        insz -= 8;
        w = in;
    }
    if (sel == TegraGM20BTunnelProbe)
        return KERN_SUCCESS;
    if (sel < 16)
        atomic_fetch_add(&calls[sel], 1);
    switch (sel) {
    case TegraGM20BMethodGetInfo: {
        struct TegraGM20BAbiInfo i = {.abi_version = TegraGM20BAbiVersion,
                                      .flags = TegraGM20BInfoHostImport |
                                               (getenv("MOCK_NO_KERNEL_MAP") ? 0 : TegraGM20BInfoKernelMap) |
                                               (getenv("MOCK_NO_ORDER") ? 0 : TegraGM20BInfoOrder) |
                                               (getenv("MOCK_NO_SURFACE_LAYOUT") ? 0 : TegraGM20BInfoSurfaceLayout),
                                      .chipset = 0x12e,
                                      .gpc_count = 1,
                                      .tpc_count = 2,
                                      .cls_copy = 0xb0b5,
                                      .cls_eng2d = 0x902d,
                                      .cls_eng3d = 0xb197,
                                      .cls_m2mf = 0xa140,
                                      .cls_compute = 0xb1c0,
                                      .cls_gpfifo = 0xb06f,
                                      .va_start = 0x1000,
                                      .va_end = 1ull << 39,
                                      .page_size = 4096,
                                      .big_page_size = 0x20000,
                                      .max_bo_bytes = 256ull << 20};
        strcpy(i.chip_name, "GM20B");
        strcpy(i.device_name, "NVIDIA Tegra X1 (GM20B)");
        if (*outsz != sizeof(i))
            return kIOReturnBadArgument;
        memcpy(out, &i, sizeof(i));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoAlloc: {
        const struct TegraGM20BAbiBoAlloc *a = in;
        unsigned h = atomic_fetch_add(&nbo, 1);
        char name[64];
        snprintf(name, sizeof(name), "/nvkmock.%d.%u", getpid(), h);
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        shm_unlink(name);
        if (fd < 0 || ftruncate(fd, (off_t)a->bytes))
            return kIOReturnNoMemory;
        bos[h] = (struct bo){fd, a->bytes, NULL};
        struct TegraGM20BAbiBoAllocReply r = {.handle = h, .map_token = h};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodSurfaceWrites:
        return KERN_SUCCESS;
    case TegraGM20BMethodBoImport: {
        const struct TegraGM20BAbiBoImport *a = in;
        static const char *slow =
            NULL; /* MOCK_IMPORT_US=<n>: n us per 16 MiB wired, as the kext's BoImport (r7l: 10.5 ms) */
        if (!slow)
            slow = getenv("MOCK_IMPORT_US") ? getenv("MOCK_IMPORT_US") : "";
        if (*slow)
            usleep((useconds_t)((uint64_t)atoi(slow) * a->bytes / (16u << 20)));
        unsigned h = atomic_fetch_add(&nbo, 1);
        bos[h] = (struct bo){-1, a->bytes, (void *)(uintptr_t)a->address, a->surface_id};
        struct TegraGM20BAbiBoAllocReply r = {.handle = h, .map_token = 0};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodSurfaceLayout: {
        const struct TegraGM20BAbiSurfaceLayout *a = in;
        if (getenv("MOCK_NO_SURFACE_LAYOUT") || a->handle >= kMaxBo || !bos[a->handle].surface)
            return kIOReturnBadArgument;
        struct TegraGM20BAbiSurfaceLayoutReply r = {.layout = surface_layout(&bos[a->handle], a)};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoFree: {
        const struct TegraGM20BAbiBoFree *a = in;
        if (a->handle < kMaxBo && bos[a->handle].fd >= 0)
            close(bos[a->handle].fd);
        if (a->handle < kMaxBo)
            bos[a->handle] = (struct bo){0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodVmBind: {
        const struct TegraGM20BAbiVmBind *b = in;
        for (uint32_t i = 0; i < b->count && i < TegraGM20BMaxBindOps; i++) {
            const struct TegraGM20BAbiBindOp *op = &b->ops[i];
            const struct bo *o = (op->op == TegraGM20BBindMap && op->handle < kMaxBo) ? &bos[op->handle] : NULL;
            if (o && o->import)
                atomic_fetch_add(&import_maps, 1);
            if (op->op == TegraGM20BBindMap)
                atomic_fetch_add(&kind_maps[op->kind & 255], 1);
            pushcount_bind(op->va, op->range, o ? o->fd : -1, o ? o->import : NULL, op->bo_offset, o ? o->bytes : 0);
        }
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodChannelDestroy:
        return KERN_SUCCESS;
    case TegraGM20BMethodChannelCreate: {
        static uint32_t next = 1;
        if (!fence)
            fence = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
        uint32_t ch = next++;
        pthread_mutex_lock(&gpu_lock);
        nchannels = ch;
        pthread_mutex_unlock(&gpu_lock);
        struct TegraGM20BAbiChannelCreateReply r = {.channel = ch, .fence_token = kFenceToken, .fence_offset = ch * 16};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodExec: {
        const struct TegraGM20BAbiExec *e = in;
        uint32_t ch = e->channel & 15;
        pthread_mutex_lock(&gpu_lock);
        if (!e->count) {
            struct TegraGM20BAbiExecReply r = {.seqno = submitted[ch]};
            pthread_mutex_unlock(&gpu_lock);
            memcpy(out, &r, sizeof(r));
            return KERN_SUCCESS;
        }
        struct job j = {.ch = ch, .seqno = ++submitted[ch], .kick = now_us()};
        for (uint32_t i = 0; i < npending[ch];
             i++) /* the acquires Order queued: its first entry (one already passed: none) */
            if (!passed(pending[ch][i].ch, pending[ch][i].seqno))
                j.acq[j.nacq++] = pending[ch][i];
        npending[ch] = 0;
        struct TegraGM20BAbiExecReply r = {.seqno = j.seqno};
        if (async_gpu()) {
            static pthread_t threads[16];
            if (!threads[ch])
                pthread_create(&threads[ch], NULL, gpu_thread, (void *)(uintptr_t)ch);
            j.exec = malloc(insz);
            memcpy(j.exec, e, insz);
            if (qtail[ch] - qhead[ch] == kQueue) {
                fprintf(stderr, "mock GPU: queue full\n");
                abort();
            }
            queue[ch][qtail[ch]++ % kQueue] = j;
            pthread_cond_broadcast(&gpu_cond);
            pthread_mutex_unlock(&gpu_lock);
        } else {
            pthread_mutex_unlock(&gpu_lock);
            pushcount_exec(e);
            pthread_mutex_lock(&gpu_lock);
            fence[ch * 4] = j.seqno;
            pthread_mutex_unlock(&gpu_lock);
        }
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodWaitFence: {
        const struct TegraGM20BAbiWaitFence *w = in;
        char name[64] = "";
        pthread_getname_np(pthread_self(), name, sizeof name);
        pthread_mutex_lock(&gpu_lock);
        waits_total++;
        if (strstr(name, "raw.submit"))
            waits_submit++;
        while (!passed(w->channel & 15, w->seqno))
            pthread_cond_wait(&gpu_cond, &gpu_lock);
        pthread_mutex_unlock(&gpu_lock);
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodOrder: {
        const struct TegraGM20BAbiOrder *o = in;
        uint32_t ch = o->channel & 15, flags = o->flags, seqno = o->seqno, n = 0;
        if (getenv("MOCK_NO_ORDER") || !flags || (flags & ~3u) || o->reserved)
            return kIOReturnBadArgument;
        pthread_mutex_lock(&gpu_lock);
        order_calls++;
        if (flags & TegraGM20BOrderBeforeTask) {
            if (!seqno)
                seqno = submitted[ch];
            if ((int32_t)(seqno - submitted[ch]) > 0) {
                pthread_mutex_unlock(&gpu_lock);
                return kIOReturnBadArgument;
            }
            if (passed(ch, seqno))
                flags &= ~TegraGM20BOrderBeforeTask;
        }
        int bad = 0;
        for (uint32_t other = 1; other <= nchannels && !bad; other++) {
            if (other == ch)
                continue;
            if ((flags & TegraGM20BOrderAfterTask) && !passed(other, submitted[other])) {
                bad = order_add(ch, other, submitted[other]);
                n += !bad;
            }
            if (!bad && (flags & TegraGM20BOrderBeforeTask)) {
                bad = order_add(other, ch, seqno);
                n += !bad;
            }
        }
        order_queued += n;
        pthread_mutex_unlock(&gpu_lock);
        struct TegraGM20BAbiOrderReply r = {.waits = n};
        memcpy(out, &r, sizeof(r));
        return bad ? kIOReturnNoResources : KERN_SUCCESS;
    }
    case TegraGM20BMethodMap: {
        const struct TegraGM20BAbiMap *m = in;
        if (getenv("MOCK_NO_KERNEL_MAP"))
            return kIOReturnBadArgument;
        mach_vm_address_t addr = m->address;
        mach_vm_size_t size = 0;
        kern_return_t kr = mock_map(kConn, m->token, mach_task_self(), &addr, &size,
                                    (m->flags & TegraGM20BMapFixed) ? 0 : kIOMapAnywhere);
        struct TegraGM20BAbiMapReply r = {.address = addr, .bytes = size};
        memcpy(out, &r, sizeof(r));
        return kr;
    }
    case TegraGM20BMethodUnmap: {
        const struct TegraGM20BAbiUnmap *u = in;
        if (getenv("MOCK_NO_KERNEL_MAP"))
            return kIOReturnBadArgument;
        return mock_unmap(kConn, u->token, mach_task_self(), u->address);
    }
    case TegraGM20BMethodGetTimestamp: {
        struct TegraGM20BAbiTimestamp t = {.ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)};
        memcpy(out, &t, sizeof(t));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodGetStatus: {
        const struct TegraGM20BAbiChannel *ch = in;
        struct TegraGM20BAbiStatus s = {.completed = fence[(ch->channel & 15) * 4],
                                        .submitted = submitted[ch->channel & 15]};
        memcpy(out, &s, sizeof(s));
        return KERN_SUCCESS;
    }
    }
    return kIOReturnBadArgument;
}

static kern_return_t mock_map(io_connect_t c, uint32_t type, task_port_t t, mach_vm_address_t *addr,
                              mach_vm_size_t *size, IOOptionBits opt)
{
    if (c != kConn)
        return IOConnectMapMemory64(c, type, t, addr, size, opt);
    uint64_t bytes;
    int fd;
    void *b = backing(type, &bytes, &fd);
    if (!b)
        return kIOReturnBadArgument;
    if (type == kFenceToken && (opt & kIOMapAnywhere)) {
        *addr = (mach_vm_address_t)fence;
        *size = 4096;
        return KERN_SUCCESS;
    }
    void *at = (opt & kIOMapAnywhere) ? NULL : (void *)*addr;
    void *m = mmap(at, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | (at ? MAP_FIXED : 0), fd, 0);
    if (m == MAP_FAILED)
        return kIOReturnNoMemory;
    *addr = (mach_vm_address_t)m;
    *size = bytes;
    return KERN_SUCCESS;
}

static kern_return_t mock_unmap(io_connect_t c, uint32_t type, task_port_t t, mach_vm_address_t addr)
{
    if (c != kConn)
        return IOConnectUnmapMemory64(c, type, t, addr);
    uint64_t bytes;
    int fd;
    if (type == kFenceToken)
        return KERN_SUCCESS;
    if (!backing(type, &bytes, &fd))
        return KERN_SUCCESS;
    munmap((void *)addr, bytes);
    return KERN_SUCCESS;
}

unsigned long mock_iokit_calls(unsigned method)
{
    return method < 16 ? calls[method] : 0;
}

unsigned long mock_iokit_import_maps(void)
{
    return import_maps;
}

unsigned long mock_iokit_kind_maps(unsigned kind)
{
    return kind < 256 ? kind_maps[kind] : 0;
}

void mock_iokit_report(void)
{
    static const char *names[] = {"",       "GetInfo",   "BoAlloc",       "BoImport",
                                  "BoFree", "VmBind",    "ChannelCreate", "ChannelDestroy",
                                  "Exec",   "WaitFence", "GetTimestamp",  "GetStatus"};
    for (int i = 1; i < 12; i++)
        if (calls[i])
            fprintf(stderr, "mock: %s x%lu\n", names[i], (unsigned long)calls[i]);
}

DYLD_INTERPOSE(mock_get, IOServiceGetMatchingService)
DYLD_INTERPOSE(mock_open, IOServiceOpen)
DYLD_INTERPOSE(mock_close, IOServiceClose)
DYLD_INTERPOSE(mock_release, IOObjectRelease)
DYLD_INTERPOSE(mock_call, IOConnectCallStructMethod)
DYLD_INTERPOSE(mock_map, IOConnectMapMemory64)
DYLD_INTERPOSE(mock_unmap, IOConnectUnmapMemory64)
