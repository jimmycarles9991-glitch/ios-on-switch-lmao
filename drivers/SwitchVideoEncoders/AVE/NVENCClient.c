// SPDX-License-Identifier: BSD-2-Clause-Patent
// See NVENCClient.h.
#include "NVENCClient.h"
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <os/log.h>
#include <stdlib.h>
#include <string.h>

struct NVENCClient {
    io_connect_t connection;
    uint32_t selector;
};

static io_service_t service(void)
{
    return IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("TegraNVENC"));
}

bool nvenc_service_present(void)
{
    const io_service_t s = service();
    if (s)
        IOObjectRelease(s);
    return s != 0;
}

static kern_return_t call(NVENCClient *c, uint32_t selector, uint32_t method, const void *in, size_t inSize, void *out,
                          size_t outSize)
{
    uint8_t buffer[8 + sizeof(TegraNVENCSubmit)];
    const uint32_t header[2] = {TEGRA_NVENC_TUNNEL_MAGIC, method};
    if (inSize > sizeof(buffer) - 8)
        return kIOReturnBadArgument;
    memcpy(buffer, header, 8);
    if (inSize)
        memcpy(buffer + 8, in, inSize);
    size_t size = outSize;
    return IOConnectCallStructMethod(c->connection, selector, buffer, 8 + inSize, out, out ? &size : NULL);
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
    IOServiceClose(c->connection);  // the kernel waits for this connection's jobs and frees its buffers
    free(c);
}

// The first failure is logged: its IOReturn tells NVENC's memory running out from a mapping the kernel refused.
static void logOnce(int *flag, const char *what, kern_return_t kr, uint64_t size)
{
    if (__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED))
        return;
    os_log_error(os_log_create("com.brunostjohn.switch.ave", "encoder"), "AVE: %{public}s %#x (%llu bytes)", what, kr,
                 (unsigned long long)size);
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
    return 0;
}

void nvenc_free(void *ctx, uint32_t handle)
{
    NVENCClient *c = ctx;
    const TegraNVENCBuffer b = {handle, 0};
    call(c, c->selector, TegraNVENCMethodBufferFree, &b, sizeof(b), NULL, 0);  // the kernel unmaps it from this task
}

int nvenc_submit(void *ctx, const TegraNVENCSubmit *submit, uint32_t *fence, uint32_t *input)
{
    NVENCClient *c = ctx;
    TegraNVENCFence reply;
    const kern_return_t kr =
        call(c, c->selector, TegraNVENCMethodSubmit, submit, sizeof(*submit), &reply, sizeof(reply));
    if (kr)
        return kr;
    *fence = reply.fence;
    if (input)
        *input = reply.input;
    return 0;
}

int nvenc_wait(void *ctx, const TegraNVENCWait *wait)
{
    NVENCClient *c = ctx;
    return call(c, c->selector, TegraNVENCMethodWait, wait, sizeof(*wait), NULL, 0) ? -1 : 0;
}
