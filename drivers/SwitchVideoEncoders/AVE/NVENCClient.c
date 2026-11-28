// SPDX-License-Identifier: BSD-2-Clause-Patent
// See NVENCClient.h.
#include "NVENCClient.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <os/log.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "AVEEmitter.h"
#include "NVENCListener.h"
#include "../../../kits/SwitchServiceWait/SwitchServiceWait.h"

extern os_log_t gLog;

enum {
    kBuffers = 64,
};

typedef struct {
    uint32_t handle;
    uint8_t *va;
} Mapping;

typedef struct {
    uint32_t magic;
    uint32_t method;
    TegraNVENCSubmit submit;
} SubmitPacket;

_Static_assert(offsetof(SubmitPacket, submit) == 8, "tunnel header must be 8 bytes");
_Static_assert(sizeof(SubmitPacket) == 8 + sizeof(TegraNVENCSubmit), "submit packet must have no padding");

struct NVENCClient {
    io_connect_t connection;
    uint32_t selector;
    uint32_t features;
    Mapping mappings[kBuffers];
    NVENCListener *listener;
    SubmitPacket submitPacket;
    SubmitPacket tilePackets[AVE_TILE_SLOTS];
};

static void remember(NVENCClient *c, uint32_t handle, void *va)
{
    Mapping *freeSlot = NULL;
    for (unsigned i = 0; i < kBuffers; i++) {
        if (c->mappings[i].handle == handle) {
            c->mappings[i].va = va;
            return;
        }
        if (!freeSlot && !c->mappings[i].handle)
            freeSlot = &c->mappings[i];
    }
    if (freeSlot)
        *freeSlot = (Mapping){handle, va};
}

static void forget(NVENCClient *c, uint32_t handle)
{
    for (unsigned i = 0; i < kBuffers; i++)
        if (c->mappings[i].handle == handle)
            c->mappings[i] = (Mapping){0, NULL};
}

void *nvenc_buffer_va(const NVENCClient *c, uint32_t handle)
{
    for (unsigned i = 0; handle && i < kBuffers; i++)
        if (c->mappings[i].handle == handle)
            return c->mappings[i].va;
    return NULL;
}

static io_service_t service(void)
{
    return IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("TegraNVENC"));
}

bool nvenc_service_wait(void)
{
    return switch_service_wait("TegraNVENC", TEGRA_NVENC_ABSENT_RESOURCE, 20000);  // E3 comes seconds into boot
}

kern_return_t nvenc_tunnel(io_connect_t connection, uint32_t selector, uint32_t method, const void *in, size_t inSize,
                           void *out, size_t outSize)
{
    uint8_t buffer[8 + sizeof(TegraNVENCSubmit)];
    const uint32_t header[2] = {TEGRA_NVENC_TUNNEL_MAGIC, method};
    if (inSize > sizeof(buffer) - 8)
        return kIOReturnBadArgument;
    memcpy(buffer, header, 8);
    if (inSize)
        memcpy(buffer + 8, in, inSize);
    size_t size = outSize;
    return IOConnectCallStructMethod(connection, selector, buffer, 8 + inSize, out, out ? &size : NULL);
}

static kern_return_t call(NVENCClient *c, uint32_t selector, uint32_t method, const void *in, size_t inSize, void *out,
                          size_t outSize)
{
    return nvenc_tunnel(c->connection, selector, method, in, inSize, out, outSize);
}

NVENCClient *nvenc_open(int *error)
{
    const io_service_t s = service();
    NVENCClient *c = s ? calloc(1, sizeof(*c)) : NULL;
    kern_return_t kr = s ? kIOReturnNoMemory : kIOReturnNotFound;
    if (c)
        kr = IOServiceOpen(s, mach_task_self(), TEGRA_NVENC_OPEN_TYPE, &c->connection);
    if (s)
        IOObjectRelease(s);
    if (kr == KERN_SUCCESS) {
        // Which selectors reach the client is the sandbox's choice, the same in every connection of a process.
        static uint32_t admitted = UINT32_MAX;
        kr = kIOReturnNotPermitted;
        if (admitted != UINT32_MAX) {
            c->selector = admitted;
            kr = KERN_SUCCESS;
        }
        for (uint32_t s = 0; s < 64 && kr != KERN_SUCCESS; s++) {
            if (call(c, s, TEGRA_NVENC_TUNNEL_PROBE, NULL, 0, NULL, 0) == KERN_SUCCESS) {
                c->selector = admitted = s;
                kr = KERN_SUCCESS;
            }
        }
    }
    TegraNVENCInfo info;
    if (kr == KERN_SUCCESS) {
        kr = call(c, c->selector, TegraNVENCMethodGetInfo, NULL, 0, &info, sizeof(info));
        if (kr == KERN_SUCCESS && info.abi_version != TEGRA_NVENC_ABI_VERSION)
            kr = kIOReturnUnsupported;
    }
    if (kr != KERN_SUCCESS) {
        if (c && c->connection)
            IOServiceClose(c->connection);
        free(c);
        if (error)
            *error = kr;
        return NULL;
    }
    c->features = info.features;
    c->submitPacket.magic = TEGRA_NVENC_TUNNEL_MAGIC;
    c->submitPacket.method = TegraNVENCMethodSubmit;
    for (unsigned i = 0; i < AVE_TILE_SLOTS; i++) {
        c->tilePackets[i].magic = TEGRA_NVENC_TUNNEL_MAGIC;
        c->tilePackets[i].method = TegraNVENCMethodSubmit;
    }
    return c;
}

bool nvenc_info(NVENCClient *c, TegraNVENCInfo *info)
{
    return call(c, c->selector, TegraNVENCMethodGetInfo, NULL, 0, info, sizeof(*info)) == KERN_SUCCESS;
}

void nvenc_close(NVENCClient *c)
{
    if (!c)
        return;
    nvenc_listener_stop(c->listener);
    c->listener = NULL;
    IOServiceClose(c->connection);  // the kernel waits for this connection's jobs and frees its buffers
    free(c);
}

// The first failure is logged: its IOReturn tells NVENC's memory running out from a mapping the kernel refused.
static void logOnce(int *flag, const char *what, kern_return_t kr, uint64_t size)
{
    if (__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED))
        return;
    os_log_error(gLog ? gLog : OS_LOG_DEFAULT, "AVE: %{public}s %#x (%llu bytes)", what, kr, (unsigned long long)size);
}

int nvenc_alloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    static int allocLogged;
    NVENCClient *c = ctx;
    const TegraNVENCBufferAlloc in = {size};
    TegraNVENCBufferReply reply;
    const kern_return_t kr = call(c, c->selector, TegraNVENCMethodBufferAlloc, &in, sizeof(in), &reply, sizeof(reply));
    if (kr) {
        logOnce(&allocLogged, "NVENC buffer alloc failed", kr, size);
        return -1;
    }
    *handle = reply.handle;
    *va = (void *)(uintptr_t)reply.address;
    remember(c, reply.handle, *va);
    return 0;
}

void nvenc_free(void *ctx, uint32_t handle)
{
    NVENCClient *c = ctx;
    const TegraNVENCBuffer b = {handle, 0};
    call(c, c->selector, TegraNVENCMethodBufferFree, &b, sizeof(b), NULL, 0);  // the kernel unmaps it from this task
    forget(c, handle);
}

TegraNVENCSubmit *nvenc_submit_buffer(NVENCClient *c)
{
    return c ? &c->submitPacket.submit : NULL;
}

TegraNVENCSubmit *nvenc_tile_submit_buffer(NVENCClient *c, uint32_t slot)
{
    return c && slot < AVE_TILE_SLOTS ? &c->tilePackets[slot].submit : NULL;
}

int nvenc_submit(void *ctx, const TegraNVENCSubmit *submit, uint32_t *fence, uint32_t *input)
{
    NVENCClient *c = ctx;
    TegraNVENCFence reply;
    const SubmitPacket *packet = NULL;
    if (submit == &c->submitPacket.submit) {
        packet = &c->submitPacket;
    } else {
        for (unsigned i = 0; i < AVE_TILE_SLOTS; i++) {
            if (submit == &c->tilePackets[i].submit) {
                packet = &c->tilePackets[i];
                break;
            }
        }
    }
    kern_return_t kr;
    if (packet) {
        size_t size = sizeof(reply);
        kr = IOConnectCallStructMethod(c->connection, c->selector, packet, sizeof(*packet), &reply, &size);
    } else {
        kr = call(c, c->selector, TegraNVENCMethodSubmit, submit, sizeof(*submit), &reply, sizeof(reply));
    }
    if (kr)
        return kr;
    *fence = reply.fence;
    if (input)
        *input = reply.input;
    TegraNVENCRange status;
    if (c->listener && nvenc_status_range(submit, &status)) {
        const uint8_t *base = nvenc_buffer_va(c, status.handle);
        nvenc_listener_submitted(c->listener, reply.fence, status, base ? base + status.offset : NULL);
    }
    return 0;
}

int nvenc_wait(void *ctx, const TegraNVENCWait *wait)
{
    NVENCClient *c = ctx;
    return call(c, c->selector, TegraNVENCMethodWait, wait, sizeof(*wait), NULL, 0) ? -1 : 0;
}

bool nvenc_listen(NVENCClient *c, dispatch_queue_t queue, NVENCDone done, void *context)
{
    if (!c || c->listener || !(c->features & TEGRA_NVENC_FEATURE_COMPLETION))
        return false;
    c->listener = nvenc_listener_start(c->connection, c->selector, queue, done, context);
    return c->listener != NULL;
}

void nvenc_stop_listening(NVENCClient *c)
{
    if (!c)
        return;
    nvenc_listener_stop(c->listener);
    c->listener = NULL;
}

bool nvenc_completions_on_port(const NVENCClient *c)
{
    return c && nvenc_listener_on_port(c->listener);
}
