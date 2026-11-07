// Host mock of the TegraGM20B user client (CONTRACT.md v1) for NVK's nvkmd_darwin, interposed over IOKit: a "GM20B" that
// completes every submission at once (its fence page shows the seqno as soon as Exec returns). Enough for vkCreateDevice,
// metal2vulkan's device open and shader/pipeline creation (NAK SM53); nothing is executed. VmBind refuses what the kernel
// refuses (TegraGM20BUserClient.cpp's run() and vmBind()), so a bind past the end of a BO fails here as on the device.
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "TegraGM20BABI.h"
#include "mock_iokit.h"

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

// The kernel's user VA window (TegraGM20BGpu.h's TGM_USER_VA_START and TGM_KERNEL_VA), which GetInfo reports.
static const uint64_t kVaStart = 0x1000, kVaEnd = 1ull << 39;

struct bo {
    int fd;
    uint64_t bytes;
    void *import;
    bool coherent;
    uint32_t maps;
};
static struct bo bos[kMaxBo];
static atomic_uint nbo = 1;
static struct mock_memory memory;
static uint32_t submitted[16];
static uint32_t *fence; /* the fence page, one uint32 per channel at slot*16 */
static atomic_ulong calls[16];
static struct mock_exec_surfaces exec_surfaces;
static pthread_mutex_t exec_surfaces_lock = PTHREAD_MUTEX_INITIALIZER;

static io_service_t mock_get(mach_port_t main, CFDictionaryRef matching)
{
    if (matching)
        CFRelease(matching);
    return kService;
}

static kern_return_t mock_open(io_service_t s, task_port_t t, uint32_t type, io_connect_t *c)
{
    if (s != kService || type != TegraGM20BOpenType)
        return kIOReturnBadArgument;
    *c = kConn;
    return KERN_SUCCESS;
}

static kern_return_t mock_close(io_connect_t c)
{
    return KERN_SUCCESS;
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
    if (!token || token >= kMaxBo || !bos[token].bytes || (bos[token].fd < 0 && !bos[token].import))
        return NULL;
    *bytes = bos[token].bytes;
    *fd = bos[token].fd;
    return bos[token].import ? bos[token].import : (void *)1;
}

static void account(struct mock_memory_kind *kind, bool coherent, int64_t bytes)
{
    uint64_t *now = coherent ? &kind->coherent : &kind->cached;
    uint64_t *peak = coherent ? &kind->coherent_peak : &kind->cached_peak;
    *now += (uint64_t)bytes;
    if (*now > *peak)
        *peak = *now;
}

// The kernel's VmBind: run()'s size and reserved checks, then vmBind()'s checks of each op.
static kern_return_t vm_bind(const void *in, size_t insz)
{
    const struct TegraGM20BAbiVmBind *b = in;
    if (insz < offsetof(struct TegraGM20BAbiVmBind, ops) || !b->count || b->count > TegraGM20BMaxBindOps)
        return kIOReturnBadArgument;
    const size_t want = insz == sizeof(*b)
                            ? insz
                            : offsetof(struct TegraGM20BAbiVmBind, ops) + b->count * sizeof(struct TegraGM20BAbiBindOp);
    if (insz != want || b->reserved)
        return kIOReturnBadArgument;
    for (uint32_t i = 0; i < b->count; i++) {
        const struct TegraGM20BAbiBindOp *op = &b->ops[i];
        if (((op->va | op->range | op->bo_offset) & 0xfff) || !op->range || op->va < kVaStart || op->range > kVaEnd ||
            op->va > kVaEnd - op->range || op->kind > 0xff) {
            memory.bind_refusals++;
            return kIOReturnBadArgument;
        }
        if (op->op == TegraGM20BBindMapSparse)
            return kIOReturnUnsupported;
        if (op->op == TegraGM20BBindUnmap)
            continue;
        if (op->op != TegraGM20BBindMap)
            return kIOReturnBadArgument;
        const struct bo *bo = op->handle && op->handle < kMaxBo ? &bos[op->handle] : NULL;
        if (bo && bo->fd < 0 && !bo->import)
            bo = NULL;
        if (!bo || !bo->bytes || op->bo_offset > bo->bytes || op->range > bo->bytes - op->bo_offset) {
            memory.bind_refusals++;
            fprintf(stderr, "mock: VmBind refused: handle %llu offset %#llx range %#llx past its %#llx bytes\n",
                    (unsigned long long)op->handle, (unsigned long long)op->bo_offset, (unsigned long long)op->range,
                    (unsigned long long)(bo ? bo->bytes : 0));
            return kIOReturnBadArgument;
        }
    }
    return KERN_SUCCESS;
}

static kern_return_t mock_call(mach_port_t c, uint32_t sel, const void *in, size_t insz, void *out, size_t *outsz)
{
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
                                      .flags = TegraGM20BInfoHostImport | TegraGM20BInfoExecSurfaces,
                                      .chipset = 0x12b,
                                      .gpc_count = 1,
                                      .tpc_count = 2,
                                      .cls_copy = 0xb0b5,
                                      .cls_eng2d = 0x902d,
                                      .cls_eng3d = 0xb197,
                                      .cls_m2mf = 0xa140,
                                      .cls_compute = 0xb1c0,
                                      .cls_gpfifo = 0xb06f,
                                      .va_start = kVaStart,
                                      .va_end = kVaEnd,
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
        const bool coherent = a->flags & TegraGM20BBoCoherent;
        bos[h] = (struct bo){fd, a->bytes, NULL, coherent, 0};
        account(&memory.live, coherent, (int64_t)a->bytes);
        memory.allocs[coherent]++;
        struct TegraGM20BAbiBoAllocReply r = {.handle = h, .map_token = h};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodSurfaceWrites:
        return KERN_SUCCESS;
    case TegraGM20BMethodBoImport: {
        const struct TegraGM20BAbiBoImport *a = in;
        unsigned h = atomic_fetch_add(&nbo, 1);
        bos[h] = (struct bo){-1, a->bytes, (void *)(uintptr_t)a->address, false, 0};
        struct TegraGM20BAbiBoAllocReply r = {.handle = h, .map_token = 0};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoFree: {
        const struct TegraGM20BAbiBoFree *a = in;
        if (a->handle >= kMaxBo || !bos[a->handle].bytes)
            return kIOReturnBadArgument;
        struct bo *b = &bos[a->handle];
        if (b->fd >= 0) {
            close(b->fd);
            account(&memory.live, b->coherent, -(int64_t)b->bytes);
        }
        if (b->maps) {
            b->fd = -1;
            b->import = NULL;
        } else {
            *b = (struct bo){0};
        }
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodVmBind:
        return vm_bind(in, insz);
    case TegraGM20BMethodChannelDestroy:
        return KERN_SUCCESS;
    case TegraGM20BMethodChannelCreate: {
        static uint32_t next = 1;
        if (!fence)
            fence = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
        uint32_t ch = next++;
        struct TegraGM20BAbiChannelCreateReply r = {.channel = ch, .fence_token = kFenceToken, .fence_offset = ch * 16};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodExec: {
        const struct TegraGM20BAbiExec *e = in;
        uint32_t ch = e->channel & 15;
        if (e->count)
            submitted[ch]++;
        fence[ch * 4] = submitted[ch];
        struct TegraGM20BAbiExecReply r = {.seqno = submitted[ch]};
        memcpy(out, &r, sizeof(r));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodExecSurfaces: {
        const struct TegraGM20BAbiExecSurfaces *e = in;
        const size_t head = offsetof(struct TegraGM20BAbiExecSurfaces, uses);
        if (insz < head || e->count > TegraGM20BMaxExecSurfaces ||
            insz < head + e->count * sizeof(struct TegraGM20BAbiSurfaceUse))
            return kIOReturnBadArgument;
        pthread_mutex_lock(&exec_surfaces_lock);
        exec_surfaces.lists++;
        exec_surfaces.count = e->count;
        exec_surfaces.flags = e->flags;
        for (uint32_t i = 0; i < e->count; i++) {
            exec_surfaces.uses[i].handle = e->uses[i].handle;
            exec_surfaces.uses[i].access = e->uses[i].access;
        }
        pthread_mutex_unlock(&exec_surfaces_lock);
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodWaitFence:
        return KERN_SUCCESS;
    case TegraGM20BMethodGetTimestamp: {
        struct TegraGM20BAbiTimestamp t = {.ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW)};
        memcpy(out, &t, sizeof(t));
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodGetStatus: {
        const struct TegraGM20BAbiChannel *ch = in;
        struct TegraGM20BAbiStatus s = {.completed = submitted[ch->channel & 15],
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
    bos[type].maps++;
    account(&memory.mapped, bos[type].coherent, (int64_t)bytes);
    return KERN_SUCCESS;
}

// A mapping outlives its BO's free until the task unmaps it (IOKit keeps a task's mappings), and so does its charge.
static kern_return_t mock_unmap(io_connect_t c, uint32_t type, task_port_t t, mach_vm_address_t addr)
{
    if (type == kFenceToken || !type || type >= kMaxBo || !bos[type].maps)
        return KERN_SUCCESS;
    struct bo *b = &bos[type];
    munmap((void *)addr, b->bytes);
    account(&memory.mapped, b->coherent, -(int64_t)b->bytes);
    if (!--b->maps && b->fd < 0 && !b->import)
        *b = (struct bo){0};
    return KERN_SUCCESS;
}

void mock_iokit_memory(struct mock_memory *out)
{
    *out = memory;
}

void mock_iokit_exec_surfaces(struct mock_exec_surfaces *out)
{
    pthread_mutex_lock(&exec_surfaces_lock);
    *out = exec_surfaces;
    pthread_mutex_unlock(&exec_surfaces_lock);
}

uint64_t mock_iokit_import_handle(const void *address)
{
    for (unsigned h = atomic_load(&nbo); h-- > 1;)
        if (bos[h].import == address && bos[h].bytes)
            return h;
    return 0;
}

void mock_iokit_report(void)
{
    static const char *names[] = {"",       "GetInfo",   "BoAlloc",       "BoImport",
                                  "BoFree", "VmBind",    "ChannelCreate", "ChannelDestroy",
                                  "Exec",   "WaitFence", "GetTimestamp",  "GetStatus"};
    for (int i = 1; i < 12; i++)
        if (calls[i])
            fprintf(stderr, "mock: %s x%lu\n", names[i], (unsigned long)calls[i]);
    fprintf(
        stderr,
        "mock: BOs live %llu KiB cached (mapped, iokit_mapped: %llu KiB, peak %llu) + %llu KiB coherent (peak %llu); "
        "BoAllocs %llu cached + %llu coherent; %llu binds refused\n",
        (unsigned long long)(memory.live.cached >> 10), (unsigned long long)(memory.mapped.cached >> 10),
        (unsigned long long)(memory.mapped.cached_peak >> 10), (unsigned long long)(memory.live.coherent >> 10),
        (unsigned long long)(memory.live.coherent_peak >> 10), (unsigned long long)memory.allocs[0],
        (unsigned long long)memory.allocs[1], (unsigned long long)memory.bind_refusals);
}

DYLD_INTERPOSE(mock_get, IOServiceGetMatchingService)
DYLD_INTERPOSE(mock_open, IOServiceOpen)
DYLD_INTERPOSE(mock_close, IOServiceClose)
DYLD_INTERPOSE(mock_release, IOObjectRelease)
DYLD_INTERPOSE(mock_call, IOConnectCallStructMethod)
DYLD_INTERPOSE(mock_map, IOConnectMapMemory64)
DYLD_INTERPOSE(mock_unmap, IOConnectUnmapMemory64)
