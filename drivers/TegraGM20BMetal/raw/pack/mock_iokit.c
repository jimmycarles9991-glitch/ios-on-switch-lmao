// Host mock of the TegraGM20B user client (CONTRACT.md v1) for NVK's nvkmd_darwin, interposed over IOKit: a "GM20B" that
// accepts every call and completes every submission at once (its fence page shows the seqno as soon as Exec returns).
// Enough for vkCreateDevice, metal2vulkan's device open and shader/pipeline creation (NAK SM53); nothing is executed.
// r8-rawca: Map/Unmap (methods 12, 13; MOCK_NO_KERNEL_MAP=1 hides them) and Order (method 14, the kext's tgm_chan_order: among the
// channels of one connection; MOCK_NO_ORDER=1 hides it). MOCK_GPU_US=<n>: an asynchronous GPU instead, one thread that
// runs the channels side by side (a thread each), each channel's submissions in kick order, n us each, a submission only once
// the acquires Order queued for it have passed (as the host's semaphore acquires): its fence word moves then, WaitFence
// sleeps until it does (a condition variable, as the kext's interrupt), and "mock GPU: deadlock" aborts if nothing can run.
// MOCK_GPU_LOG=<file>: every submission run, "ch seqno kick_us start_us end_us acquires", and the WaitFence calls by thread.
// MOCK_IGNORE_ACQUIRES=1: the GPU runs submissions without their acquires (ordercheck.py's negative control).
// MOCK_IMPORT_US=<n>: BoImport takes n us per 16 MiB (the kext's wiring; selcheck's pool test).
// MOCK_COMPRESSION=1: GetInfo says compressible kinds bind (TegraGM20BInfoCompression), and they do as the kext binds them
// (mock_comp.h: which binds, the contract's violations counted, MOCK_COMP_LINES=<n> comptag lines). Off by default.
// MOCK_CONTIG_US=<n>: a Contiguous or Compressible BoAlloc takes n us (the kext's contiguous search, or its zeroing).
// MOCK_COMP_REFUSE=<n>: the first n Compressible BoAllocs are refused with kIOReturnNoMemory (the pool short).
// MOCK_TILED_CACHE=1: GetInfo says every GR context holds the tiled cache's table (TegraGM20BInfoTiledCache), as the kext
// does unless booted with switch-no-tiled-cache. Off by default while NVK host builds without the bit stay in use.
// Each IOServiceOpen is a connection of its own, as another task's would be (Order stays among its channels; MOCK_TASK_WINDOWS=1:
// its own GPU VA window too), and ExecSurfaces (method 17; MOCK_NO_EXEC_SURFACES=1 hides it) orders as the kext's TegraGM20BFences.h: a
// submission that lists an IOSurface's import waits on the GPU for every other channel's, any connection's, last submission
// that listed the surface as written (surfaces by the surface ID their BoImport named), and one that lists it written becomes
// its channel's writer.
// SurfaceLayout (method 16; MOCK_NO_SURFACE_LAYOUT=1 hides it) decides a surface's layout as the kext does: at its first asking
// import, block-linear when that import writes it, is the surface's only import and its shape tiles, its rows then tiled in
// place (TegraGM20BLayout.h); pitch for good otherwise.
// Priority (method 18; MOCK_NO_PRIORITY=1 hides it): one per process, as the kext keeps one per connection
// (mock_iokit_priority).
// mock_iokit_kill_next(error): the next Exec on a 3D channel (a queue's, not NVK's upload channel) is accepted, then its
// channel dies as the kext kills one (method 11's error, and with mock_iokit_kill_next_at the fault VA GetStatus reports
// for an MMU fault): what it had in flight never runs, its kill record
// (TegraGM20BAbiFence) names those seqnos, its fence passes them, and Exec, WaitFence and Order on it answer
// kIOReturnNotResponding until the client makes a new channel (MOCK_NO_KILL_RECORD=1: no record, as a kext without
// TegraGM20BInfoKillRecord).
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
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
    kConn = 0x7e58, /* the first connection; the next ones follow it */
    kConns = 5,
    kFenceToken = 0xfe000001,
    kFenceWords = 16, /* a channel's fence record: 64 bytes */
    kMaxBo = 65536
};

struct bo {
    uint64_t handle; /* 0: a free slot */
    int fd;
    uint64_t bytes;
    void *import;
    uint32_t surface;
    uint32_t flags, align_log2; /* BoAlloc's */
    uint32_t kind, cpu_maps;    /* its last bind's kind (kind_set), CPU mappings made */
    uint8_t kind_set;
    uint64_t lines; /* comptag lines it holds (mock_comp.h) */
};
/* Live BOs by slot. A handle is a serial above the slot (serial << 16 | slot): never reused, as the kext's; a slot (and the
 * map token, which is the slot) is, once its BO is freed */
static struct bo bos[kMaxBo];
static pthread_mutex_t bo_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t bo_serial;
static uint32_t submitted[16];
/* The fence page: per channel slot, at slot*64, its TegraGM20BAbiFence, TegraGM20BAbiFenceSemantic and
 * TegraGM20BAbiFenceIdentity, as the kext lays them out */
static uint32_t *fence;
static uint32_t dead[16]; /* method 11's error of a killed channel */
static uint32_t made[16]; /* channels made in each slot: a wait on one destroyed and made again answers NotResponding */
static uint64_t dead_va[16]; /* and its fault VA */
static uint32_t engines[16];
static uint8_t live[16]; /* slots in use: a connection gets its lowest free one, its seqnos continuing (the kext's) */
static uint32_t kill_next;

/* The slot of live handle `h`, or 0 */
static unsigned slot_of(uint64_t h)
{
    const unsigned slot = (unsigned)(h & (kMaxBo - 1));
    return slot && bos[slot].handle == h ? slot : 0;
}

/* `o` in a free slot, under a new handle: its slot, or 0 with every slot live */
static unsigned bo_new(struct bo o)
{
    static unsigned cursor = 1;
    unsigned slot = 0;
    pthread_mutex_lock(&bo_lock);
    for (unsigned n = 1; n < kMaxBo && !slot; n++) {
        if (!bos[cursor].handle)
            slot = cursor;
        cursor = cursor + 1 < kMaxBo ? cursor + 1 : 1;
    }
    if (slot) {
        o.handle = ++bo_serial << 16 | slot;
        bos[slot] = o;
    }
    pthread_mutex_unlock(&bo_lock);
    return slot;
}

static uint64_t kill_next_va;
enum { kMethods = 64 };
static atomic_ulong calls[kMethods];
static atomic_uint fail_left[kMethods]; /* mock_iokit_fail_next's, by method */
static atomic_uint fail_skip[kMethods]; /* mock_iokit_fail_after's calls that go through first */
static atomic_int fail_kr[kMethods];
static atomic_uint priority_now[2] = {2, 4}; /* Priority's (method 18): the connection's foreground and background */
static atomic_ulong import_maps;             /* VmBind map ops onto imported pages */
static atomic_ulong kind_maps[256];          /* VmBind map ops by PTE kind */
#include "mock_comp.h"

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
static atomic_uint nconns;

static int is_conn(mach_port_t c)
{
    return c >= kConn && c < kConn + kConns;
}

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
    int kill; /* MOCK_KILL's submission: the channel dies when the GPU reaches it */
};

struct run {
    uint32_t ch, seqno, nacq;
    uint64_t kick, start, end;
};

static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gpu_cond = PTHREAD_COND_INITIALIZER;
static struct acq pending[16][kOrderMax];
static uint32_t npending[16], nchannels;
static mach_port_t chan_conn[16]; /* the connection that made each channel: Order stays among one's */

/* ExecSurfaces: each channel's list for its next Exec, and each surface's writers (a channel's last submission that listed it
 * written) */
struct listed {
    uint32_t count, surface[TegraGM20BMaxExecSurfaces];
    uint64_t writes; /* bit i: surface[i] listed written */
    int given;
};
static struct listed lists[16];

static struct writer {
    uint32_t surface, ch, seqno;
} writers[256];

static uint32_t nwriters;

enum {
    kQueue = 1024
};
static struct job queue[16][kQueue]; /* per channel, in kick order: [head, tail) */
static uint32_t qhead[16], qtail[16], running;
static bool paused[16];
static struct run runs[kJobs];
static uint32_t nruns;
static unsigned long order_calls, order_queued, waits_total, waits_submit;
static int gpu_us = -1;
static void kill_channel(uint32_t ch, uint32_t error);

/* MOCK_KILL=<channel>:<seqno>: that submission faults the GPU when it runs and the kernel kills its channel (kill_channel,
 * GetStatus error 2, a GR exception), so commits can go on past the kill while it is in flight. gpu_lock held. */
static int kill_due(uint32_t ch, uint32_t seqno)
{
    static int parsed;
    static unsigned kch, kseq;
    if (!parsed) {
        const char *k = getenv("MOCK_KILL");
        parsed = 1;
        if (!k || sscanf(k, "%u:%u", &kch, &kseq) != 2)
            kch = 0;
    }
    return kch && ch == (kch & 15) && seqno == kseq;
}

static uint64_t now_us(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

static int passed(uint32_t ch, uint32_t seqno)
{
    return (int32_t)(fence[ch * kFenceWords] - seqno) >= 0;
}

static int async_gpu(void)
{
    if (gpu_us < 0)
        gpu_us = getenv("MOCK_GPU_US") ? atoi(getenv("MOCK_GPU_US")) : 0;
    return gpu_us > 0;
}

bool mock_iokit_seed_sequence(uint32_t channel, uint32_t sequence)
{
    pthread_mutex_lock(&gpu_lock);
    const bool ready = channel < 16 && live[channel] && !dead[channel] &&
                       qhead[channel] == qtail[channel] && passed(channel, submitted[channel]);
    if (ready)
        submitted[channel] = fence[channel * kFenceWords] = sequence;
    pthread_mutex_unlock(&gpu_lock);
    return ready;
}

void mock_iokit_pause(uint32_t channel, bool pause)
{
    pthread_mutex_lock(&gpu_lock);
    paused[channel] = pause;
    pthread_cond_broadcast(&gpu_cond);
    pthread_mutex_unlock(&gpu_lock);
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

/* gpu_lock held: channel ch's Exec of `seqno` takes its list (ExecSurfaces): an acquire on each other channel's last writer of
 * a listed surface that may still run, then it is the writer of the surfaces it lists written */
static void surface_acquires(uint32_t ch, uint32_t seqno)
{
    struct listed *l = &lists[ch];
    if (!l->given)
        return;
    for (uint32_t i = 0; i < l->count; i++)
        for (uint32_t w = 0; w < nwriters; w++)
            if (writers[w].surface == l->surface[i] && writers[w].ch != ch && !passed(writers[w].ch, writers[w].seqno))
                order_add(ch, writers[w].ch, writers[w].seqno);
    for (uint32_t i = 0; i < l->count; i++) {
        if (!(l->writes >> i & 1))
            continue;
        uint32_t w = 0;
        while (w < nwriters && !(writers[w].surface == l->surface[i] && writers[w].ch == ch))
            w++;
        if (w == nwriters && nwriters < sizeof writers / sizeof writers[0])
            nwriters++;
        if (w < nwriters)
            writers[w] = (struct writer){l->surface[i], ch, seqno};
    }
    l->given = 0;
}

/* gpu_lock held: the head of channel ch's queue may run (its acquires passed; MOCK_IGNORE_ACQUIRES: always) */
static int head_ready(uint32_t ch, int ignore_acquires)
{
    if (qhead[ch] == qtail[ch] || paused[ch])
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
            if (qhead[ch] != qtail[ch] && !running && !paused[ch]) { /* nothing runs that could move a fence: none ever will */
                int any = 0;
                for (uint32_t c = 1; c < 16 && !any; c++)
                    any = head_ready(c, ignore_acquires);
                if (!any) {
                    const struct job *j = &queue[ch][qhead[ch] % kQueue];
                    fprintf(stderr, "mock GPU: deadlock: ch %u seqno %u waits on ch %u fence %u (at %u)\n", j->ch,
                            j->seqno, j->acq[0].ch, j->acq[0].seqno, fence[j->acq[0].ch * kFenceWords]);
                    abort();
                }
            }
            pthread_cond_wait(&gpu_cond, &gpu_lock);
            continue;
        }
        struct job j = queue[ch][qhead[ch] % kQueue];
        if (dead[ch] || j.kill) { /* killed with it in flight, or MOCK_KILL's: it never runs */
            if (!dead[ch])
                kill_channel(ch, 2);
            qhead[ch]++;
            free(j.exec);
            continue;
        }
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
        if (!dead[ch])
            fence[ch * kFenceWords] = j.seqno;
        if (nruns < kJobs)
            runs[nruns++] = (struct run){j.ch, j.seqno, j.nacq, j.kick, start, end};
        pthread_cond_broadcast(&gpu_cond);
    }
    return NULL;
}

/* gpu_lock held: the kext's chan_killed */
static void kill_channel(uint32_t ch, uint32_t error)
{
    if (!getenv("MOCK_NO_KILL_RECORD")) {
        fence[ch * kFenceWords + 1] = fence[ch * kFenceWords] + 1;
        fence[ch * kFenceWords + 2] = submitted[ch];
        fence[ch * kFenceWords + 3] = error;
    }
    dead[ch] = error;
    fence[ch * kFenceWords] = submitted[ch];
    pthread_cond_broadcast(&gpu_cond);
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
    const unsigned n = atomic_fetch_add(&nconns, 1);
    if (n >= kConns)
        return kIOReturnNoResources;
    *c = kConn + n;
    return KERN_SUCCESS;
}

static kern_return_t mock_close(io_connect_t c)
{
    return is_conn(c) ? KERN_SUCCESS : IOServiceClose(c);
}

static kern_return_t mock_release(io_object_t o)
{
    return (o == kService || is_conn(o)) ? KERN_SUCCESS : IOObjectRelease(o);
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

static uint32_t *fence_page(void)
{
    if (!fence)
        fence = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    return fence;
}

#include "mock_submission.h"
#ifdef SWITCH_MOCK_JOB_INTERFACE
#include "mock_jobs.h"
#endif

static kern_return_t mock_call(mach_port_t c, uint32_t sel, const void *in, size_t insz, void *out, size_t *outsz)
{
    static atomic_int logged;
    if (!atomic_exchange(&logged, 1))
        atexit(gpu_log);
    if (!is_conn(c))
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
    if (sel < kMethods)
        atomic_fetch_add(&calls[sel], 1);
    if (sel < kMethods && atomic_load(&fail_left[sel])) {
        if (atomic_load(&fail_skip[sel])) {
            atomic_fetch_sub(&fail_skip[sel], 1);
        } else {
            atomic_fetch_sub(&fail_left[sel], 1);
            return atomic_load(&fail_kr[sel]);
        }
    }
    switch (sel) {
    case TegraGM20BMethodGetInfo: {
        /* MOCK_TASK_WINDOWS=1: each connection a GPU VA window of its own, below 2^39 (NAK's QMD addresses): the first's
         * [4 KiB, 256 GiB), each next one 64 GiB after it. Else every connection the whole [4 KiB, 512 GiB). */
        const uint64_t n = getenv("MOCK_TASK_WINDOWS") ? c - kConn : 0,
                       at = n ? (1ull << 38) + (n - 1) * (1ull << 36) : 0,
                       window = n                             ? 1ull << 36
                                : getenv("MOCK_TASK_WINDOWS") ? 1ull << 38
                                                              : 1ull << 39;
        struct TegraGM20BAbiInfo i = {.abi_version = TegraGM20BAbiVersion,
                                      .flags = TegraGM20BInfoHostImport | TegraGM20BInfoCpuAccess |
                                               (getenv("MOCK_NO_EXEC_SURFACES") ? 0 : TegraGM20BInfoExecSurfaces) |
                                               (getenv("MOCK_NO_KERNEL_MAP") ? 0 : TegraGM20BInfoKernelMap) |
                                               (getenv("MOCK_NO_ORDER") ? 0 : TegraGM20BInfoOrder) |
                                               (getenv("MOCK_NO_SURFACE_LAYOUT") ? 0 : TegraGM20BInfoSurfaceLayout) |
                                               (getenv("MOCK_NO_KILL_RECORD") ? 0 : TegraGM20BInfoKillRecord) |
                                               (getenv("MOCK_TILED_CACHE") ? TegraGM20BInfoTiledCache : 0) |
                                               (getenv("MOCK_COMPRESSION") ? TegraGM20BInfoCompression : 0) |
                                               (getenv("MOCK_NO_PRIORITY") ? 0 : TegraGM20BInfoPriority) |
                                               (getenv("MOCK_NATIVE_JOBS") ? TegraGM20BInfoSubmitAddress : 0),
                                      .chipset = 0x12e,
                                      .gpc_count = 1,
                                      .tpc_count = 2,
                                      .cls_copy = 0xb0b5,
                                      .cls_eng2d = 0x902d,
                                      .cls_eng3d = 0xb197,
                                      .cls_m2mf = 0xa140,
                                      .cls_compute = 0xb1c0,
                                      .cls_gpfifo = 0xb06f,
                                      .va_start = at + 0x1000,
                                      .va_end = at + window,
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
        if ((a->flags & (TegraGM20BBoContiguous | TegraGM20BBoCompressible)) && getenv("MOCK_CONTIG_US"))
            usleep((useconds_t)atoi(getenv("MOCK_CONTIG_US")));
        const kern_return_t refused = comp_alloc(a);
        if (refused != KERN_SUCCESS)
            return refused;
        static atomic_uint names;
        char name[64];
        snprintf(name, sizeof(name), "/nvkmock.%d.%u", getpid(), atomic_fetch_add(&names, 1));
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        shm_unlink(name);
        if (fd < 0 || ftruncate(fd, (off_t)a->bytes)) {
            if (fd >= 0)
                close(fd);
            return kIOReturnNoMemory;
        }
        const unsigned h =
            bo_new((struct bo){.fd = fd, .bytes = a->bytes, .flags = a->flags, .align_log2 = a->align_log2});
        if (!h) {
            close(fd);
            return kIOReturnNoMemory;
        }
        struct TegraGM20BAbiBoAllocReply r = {.handle = bos[h].handle, .map_token = h};
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
        const unsigned h = bo_new((struct bo){
            .fd = -1, .bytes = a->bytes, .import = (void *)(uintptr_t)a->address, .surface = a->surface_id});
        if (!h)
            return kIOReturnNoMemory;
        struct TegraGM20BAbiBoAllocReply r = {.handle = bos[h].handle, .map_token = 0};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodSurfaceLayout: {
        const struct TegraGM20BAbiSurfaceLayout *a = in;
        const unsigned h = slot_of(a->handle);
        if (getenv("MOCK_NO_SURFACE_LAYOUT") || !bos[h].surface)
            return kIOReturnBadArgument;
        struct TegraGM20BAbiSurfaceLayoutReply r = {.layout = surface_layout(&bos[h], a)};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoFree: {
        const struct TegraGM20BAbiBoFree *a = in;
        const unsigned h = slot_of(a->handle);
        if (!h)
            return KERN_SUCCESS;
        if (bos[h].fd >= 0)
            close(bos[h].fd);
        comp_unbind(0, 0, h);
        pthread_mutex_lock(&bo_lock);
        bos[h] = (struct bo){0};
        pthread_mutex_unlock(&bo_lock);
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodVmBind: {
        const struct TegraGM20BAbiVmBind *b = in;
        for (uint32_t i = 0; i < b->count && i < TegraGM20BMaxBindOps; i++) {
            const struct TegraGM20BAbiBindOp *op = &b->ops[i];
            const int map = op->op == TegraGM20BBindMap || op->op == TegraGM20BBindMapVolatile;
            const unsigned h = map ? slot_of(op->handle) : 0;
            struct bo *o = h ? &bos[h] : NULL;
            if (o) {
                const kern_return_t kr = comp_bind(h, o, op);
                if (kr != KERN_SUCCESS)
                    return kr;
            } else if (op->op == TegraGM20BBindUnmap) {
                comp_unbind(op->va, op->range, 0);
            }
            if (o && o->import)
                atomic_fetch_add(&import_maps, 1);
            if (map)
                atomic_fetch_add(&kind_maps[op->kind & 255], 1);
            pushcount_bind(op->va, op->range, o ? o->fd : -1, o ? o->import : NULL, op->bo_offset, o ? o->bytes : 0);
        }
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodChannelDestroy: {
        const struct TegraGM20BAbiChannel *d = in;
        pthread_mutex_lock(&gpu_lock);
        live[d->channel & 15] = 0;
        pthread_mutex_unlock(&gpu_lock);
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodChannelCreate: {
        fence_page();
        pthread_mutex_lock(&gpu_lock);
        uint32_t ch = 1;
        while (ch < 16 && (live[ch] || (chan_conn[ch] && chan_conn[ch] != c)))
            ch++;
        if (ch == 16) {
            pthread_mutex_unlock(&gpu_lock);
            return kIOReturnNoResources;
        }
        live[ch] = 1;
        engines[ch] = ((const struct TegraGM20BAbiChannelCreate *)in)->engines;
        if (ch > nchannels)
            nchannels = ch;
        chan_conn[ch & 15] = c;
        dead[ch & 15] = 0;
        dead_va[ch & 15] = 0;
        made[ch & 15]++;
        memset(&fence[(ch & 15) * kFenceWords + 1], 0, (kFenceWords - 1) * sizeof(uint32_t));
        const uint64_t uid = (uint64_t)made[ch & 15] << 4 | (ch & 15);
        memcpy(&fence[(ch & 15) * kFenceWords + 8], &uid, sizeof uid);
        pthread_cond_broadcast(&gpu_cond);
        pthread_mutex_unlock(&gpu_lock);
        struct TegraGM20BAbiChannelCreateReply r = {
            .channel = ch, .fence_token = kFenceToken, .fence_offset = ch * kFenceWords * 4};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodSubmitAddress:
        return mock_submit(c, in, insz, out, outsz);
#ifdef SWITCH_MOCK_JOB_INTERFACE
    case TegraGM20BMethodResourceSetCreate:
        return mock_set_create(in, insz, out);
    case TegraGM20BMethodResourceSetRelease:
        return mock_set_release(in, insz);
    case TegraGM20BMethodSubmitJob:
        return mock_job(c, in, insz, out, outsz);
#endif
    case TegraGM20BMethodExec: {
        const struct TegraGM20BAbiExec *e = in;
        uint32_t ch = e->channel & 15;
        pthread_mutex_lock(&gpu_lock);
        if (dead[ch]) {
            pthread_mutex_unlock(&gpu_lock);
            return kIOReturnNotResponding;
        }
        if (!e->count) {
            struct TegraGM20BAbiExecReply r = {.seqno = submitted[ch]};
            pthread_mutex_unlock(&gpu_lock);
            memcpy(out, &r, sizeof(r));
            return KERN_SUCCESS;
        }
        struct job j = {.ch = ch, .seqno = ++submitted[ch], .kick = now_us()};
        surface_acquires(ch, j.seqno);
        for (uint32_t i = 0; i < npending[ch];
             i++) /* the acquires Order and ExecSurfaces queued: its first entry (one already passed: none) */
            if (!passed(pending[ch][i].ch, pending[ch][i].seqno))
                j.acq[j.nacq++] = pending[ch][i];
        npending[ch] = 0;
        struct TegraGM20BAbiExecReply r = {.seqno = j.seqno};
        j.kill = kill_due(ch, j.seqno);
        if (j.kill && !async_gpu())
            kill_channel(ch, 2);
        else if (kill_next && (engines[ch] & TegraGM20BEngine3D)) {
            kill_channel(ch, kill_next);
            dead_va[ch] = kill_next_va;
            kill_next = 0;
            kill_next_va = 0;
        }
        if (dead[ch]) {
            pthread_mutex_unlock(&gpu_lock);
            memcpy(out, &r, sizeof(r));
            return KERN_SUCCESS;
        }
        if (async_gpu() || paused[ch]) {
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
            fence[ch * kFenceWords] = j.seqno;
            pthread_mutex_unlock(&gpu_lock);
        }
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodExecSurfaces: {
        const struct TegraGM20BAbiExecSurfaces *e = in;
        const size_t head = offsetof(struct TegraGM20BAbiExecSurfaces, uses);
        if (getenv("MOCK_NO_EXEC_SURFACES") || insz < head || e->count > TegraGM20BMaxExecSurfaces ||
            insz < head + e->count * sizeof(struct TegraGM20BAbiSurfaceUse))
            return kIOReturnBadArgument;
        pthread_mutex_lock(&gpu_lock);
        struct listed *l = &lists[e->channel & 15];
        *l = (struct listed){.given = 1};
        for (uint32_t i = 0; i < e->count && !(e->flags & TegraGM20BExecSurfacesAll); i++) {
            const unsigned h = slot_of(e->uses[i].handle);
            const uint32_t surface = bos[h].import ? bos[h].surface : 0;
            if (!surface)
                continue; /* not a live import of a surface */
            if (e->uses[i].access & TegraGM20BSurfaceWrite)
                l->writes |= 1ull << l->count;
            l->surface[l->count++] = surface;
        }
        pthread_mutex_unlock(&gpu_lock);
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
        const uint32_t was = made[w->channel & 15];
        while (!dead[w->channel & 15] && made[w->channel & 15] == was && !passed(w->channel & 15, w->seqno))
            pthread_cond_wait(&gpu_cond, &gpu_lock);
        const int killed = dead[w->channel & 15] != 0 || made[w->channel & 15] != was;
        pthread_mutex_unlock(&gpu_lock);
        return killed ? kIOReturnNotResponding : KERN_SUCCESS;
    }
    case TegraGM20BMethodOrder: {
        const struct TegraGM20BAbiOrder *o = in;
        uint32_t ch = o->channel & 15, flags = o->flags, seqno = o->seqno, n = 0;
        if (getenv("MOCK_NO_ORDER") || !flags || (flags & ~3u) || o->reserved)
            return kIOReturnBadArgument;
        pthread_mutex_lock(&gpu_lock);
        if (dead[ch]) {
            pthread_mutex_unlock(&gpu_lock);
            return kIOReturnNotResponding;
        }
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
            if (other == ch || chan_conn[other] != chan_conn[ch])
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
    case TegraGM20BMethodCpuAcquire:
    case TegraGM20BMethodCpuRelease: {
        const struct TegraGM20BAbiCpuAccess *a = in;
        if (insz < sizeof(*a) || !slot_of(a->handle))
            return kIOReturnBadArgument;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodCpuComplete: {
        const struct TegraGM20BAbiCpuComplete *q = in;
        if (insz < sizeof(*q) || *outsz != sizeof(struct TegraGM20BAbiCpuCompleteReply))
            return kIOReturnBadArgument;
        const struct TegraGM20BAbiCpuCompleteReply r = {.current_count = q->range_count};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodMap: {
        const struct TegraGM20BAbiMap *m = in;
        if (getenv("MOCK_NO_KERNEL_MAP"))
            return kIOReturnBadArgument;
        mach_vm_address_t addr = m->address;
        mach_vm_size_t size = 0;
        kern_return_t kr =
            mock_map(c, m->token, mach_task_self(), &addr, &size, (m->flags & TegraGM20BMapFixed) ? 0 : kIOMapAnywhere);
        struct TegraGM20BAbiMapReply r = {.address = addr, .bytes = size};
        memcpy(out, &r, sizeof(r));
        return kr;
    }
    case TegraGM20BMethodUnmap: {
        const struct TegraGM20BAbiUnmap *u = in;
        if (getenv("MOCK_NO_KERNEL_MAP"))
            return kIOReturnBadArgument;
        return mock_unmap(c, u->token, mach_task_self(), u->address);
    }
    case TegraGM20BMethodGetTimestamp: {
        struct TegraGM20BAbiTimestamp t = {.ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)};
        memcpy(out, &t, sizeof(t));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodPriority: {
        const struct TegraGM20BAbiPriority *p = in;
        if (insz != sizeof(*p) || p->priority > TegraGM20BMaxPriority || p->background_priority > TegraGM20BMaxPriority)
            return kIOReturnBadArgument;
        atomic_store(&priority_now[0], p->priority);
        atomic_store(&priority_now[1], p->background_priority);
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodGetStatus: {
        const struct TegraGM20BAbiChannel *ch = in;
        struct TegraGM20BAbiStatus s = {.completed = fence[(ch->channel & 15) * kFenceWords],
                                        .submitted = submitted[ch->channel & 15],
                                        .error = dead[ch->channel & 15],
                                        .fault_va = dead_va[ch->channel & 15]};
        memcpy(out, &s, sizeof(s));
        return KERN_SUCCESS;
    }
    }
    return kIOReturnBadArgument;
}

static kern_return_t mock_map(io_connect_t c, uint32_t type, task_port_t t, mach_vm_address_t *addr,
                              mach_vm_size_t *size, IOOptionBits opt)
{
    if (!is_conn(c))
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
    if (type < kMaxBo && comp_cpu_map(type, &bos[type]))
        return kIOReturnBadArgument;
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
    if (!is_conn(c))
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

void mock_iokit_kill_next(uint32_t error)
{
    pthread_mutex_lock(&gpu_lock);
    kill_next = error;
    pthread_mutex_unlock(&gpu_lock);
}

void mock_iokit_kill_next_at(uint32_t error, uint64_t fault_va)
{
    pthread_mutex_lock(&gpu_lock);
    kill_next = error;
    kill_next_va = fault_va;
    pthread_mutex_unlock(&gpu_lock);
}

/* The `count` calls of `method` after the next `skip` answer `kr` and do nothing: the kernel refusing them (an Exec or a
 * ChannelCreate, a WaitFence that fails with no kill) */
void mock_iokit_fail_after(unsigned method, unsigned skip, unsigned count, int kr)
{
    if (method >= kMethods)
        return;
    atomic_store(&fail_kr[method], kr);
    atomic_store(&fail_skip[method], skip);
    atomic_store(&fail_left[method], count);
}

void mock_iokit_fail_next(unsigned method, unsigned count, int kr)
{
    mock_iokit_fail_after(method, 0, count, kr);
}

unsigned long mock_iokit_calls(unsigned method)
{
    return method < kMethods ? calls[method] : 0;
}

void mock_iokit_priority(uint32_t out[2])
{
    out[0] = atomic_load(&priority_now[0]);
    out[1] = atomic_load(&priority_now[1]);
}

/* The GPU's runs so far (MOCK_GPU_US), up to `most`: channel, seqno, kick, start and end (us), acquires */
unsigned mock_iokit_runs(uint64_t (*out)[6], unsigned most)
{
    pthread_mutex_lock(&gpu_lock);
    unsigned n = 0;
    for (; n < nruns && n < most; n++) {
        out[n][0] = runs[n].ch;
        out[n][1] = runs[n].seqno;
        out[n][2] = runs[n].kick;
        out[n][3] = runs[n].start;
        out[n][4] = runs[n].end;
        out[n][5] = runs[n].nacq;
    }
    pthread_mutex_unlock(&gpu_lock);
    return n;
}

/* The live BOs' bytes: allocations (BoAlloc) and imports of memory that is not an IOSurface's */
void mock_iokit_held(uint64_t *allocated, uint64_t *imported)
{
    *allocated = *imported = 0;
    for (unsigned h = 1; h < kMaxBo; h++) {
        if (bos[h].fd >= 0 && bos[h].bytes && !bos[h].import)
            *allocated += bos[h].bytes;
        else if (bos[h].import && !bos[h].surface)
            *imported += bos[h].bytes;
    }
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
