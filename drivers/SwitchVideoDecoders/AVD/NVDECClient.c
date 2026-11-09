// SPDX-License-Identifier: BSD-2-Clause-Patent
// See NVDECClient.h.
#include "NVDECClient.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <os/log.h>
#include <stdlib.h>
#include <string.h>

struct NVDECClient {
    io_connect_t connection;
    uint32_t selector, codecs, features;
};

static io_service_t service(void)
{
    return IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("TegraNVDEC"));
}

bool nvdec_service_present(void)
{
    const io_service_t s = service();
    if (s)
        IOObjectRelease(s);
    return s != 0;
}

static bool serviceDecodes(const char *property)
{
    CFMutableDictionaryRef matching = IOServiceMatching("TegraNVDEC");
    if (!matching)
        return false;
    CFStringRef key = CFStringCreateWithCString(NULL, property, kCFStringEncodingASCII);
    const void *keys[] = {key}, *values[] = {kCFBooleanTrue};
    CFDictionaryRef match =
        CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(matching, CFSTR(kIOPropertyMatchKey), match);
    CFRelease(match);
    CFRelease(key);
    const io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    if (s)
        IOObjectRelease(s);
    return s != 0;
}

uint32_t nvdec_service_codecs(void)
{
    return (serviceDecodes(TEGRA_NVDEC_DECODES_H264) ? 1u << TEGRA_NVDEC_CODEC_H264 : 0) |
           (serviceDecodes(TEGRA_NVDEC_DECODES_VP9) ? 1u << TEGRA_NVDEC_CODEC_VP9 : 0) |
           (serviceDecodes(TEGRA_NVDEC_DECODES_HEVC) ? 1u << TEGRA_NVDEC_CODEC_HEVC : 0);
}

static kern_return_t call(NVDECClient *c, uint32_t selector, uint32_t method, const void *in, size_t inSize, void *out,
                          size_t outSize)
{
    uint8_t buffer[8 + sizeof(TegraNVDECSubmit)];
    const uint32_t header[2] = {TEGRA_NVDEC_TUNNEL_MAGIC, method};
    if (inSize > sizeof(buffer) - 8)
        return kIOReturnBadArgument;
    memcpy(buffer, header, 8);
    if (inSize)
        memcpy(buffer + 8, in, inSize);
    size_t size = outSize;
    return IOConnectCallStructMethod(c->connection, selector, buffer, 8 + inSize, out, out ? &size : NULL);
}

NVDECClient *nvdec_open(int *error)
{
    const io_service_t s = service();
    NVDECClient *c = s ? calloc(1, sizeof(*c)) : NULL;
    kern_return_t kr = s ? kIOReturnNoMemory : kIOReturnNotFound;
    if (c)
        kr = IOServiceOpen(s, mach_task_self(), TEGRA_NVDEC_OPEN_TYPE, &c->connection);
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
            if (call(c, s, TEGRA_NVDEC_TUNNEL_PROBE, NULL, 0, NULL, 0) == KERN_SUCCESS) {
                c->selector = admitted = s;
                kr = KERN_SUCCESS;
            }
        }
    }
    TegraNVDECInfo info;
    if (kr == KERN_SUCCESS) {
        kr = call(c, c->selector, TegraNVDECMethodGetInfo, NULL, 0, &info, sizeof(info));
        if (kr == KERN_SUCCESS && info.abi_version != TEGRA_NVDEC_ABI_VERSION)
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
    c->codecs = info.codecs;
    c->features = info.features;
    return c;
}

uint32_t nvdec_codecs(const NVDECClient *c)
{
    return c->codecs;
}

bool nvdec_can_present(const NVDECClient *c)
{
    return c->features & TEGRA_NVDEC_FEATURE_PRESENT;
}

int nvdec_present(NVDECClient *c, const TegraNVDECPresent *present)
{
    return call(c, c->selector, TegraNVDECMethodPresent, present, sizeof(*present), NULL, 0);
}

void nvdec_close(NVDECClient *c)
{
    if (!c)
        return;
    IOServiceClose(c->connection);  // the kernel waits for this connection's jobs and frees its buffers
    free(c);
}

// The first failure is logged: its IOReturn tells NVDEC's memory running out from a mapping the kernel refused.
static void logOnce(int *flag, const char *what, kern_return_t kr, uint64_t size)
{
    if (__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED))
        return;
    os_log_error(os_log_create("com.brunostjohn.switch.avd", "decoder"), "AVD: %{public}s %#x (%llu bytes)", what, kr,
                 (unsigned long long)size);
}

int nvdec_alloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    static int allocLogged;
    NVDECClient *c = ctx;
    const TegraNVDECBufferAlloc in = {size};
    TegraNVDECBufferReply reply;
    const kern_return_t kr = call(c, c->selector, TegraNVDECMethodBufferAlloc, &in, sizeof(in), &reply, sizeof(reply));
    if (kr) {
        logOnce(&allocLogged, "NVDEC buffer alloc failed", kr, size);
        return -1;
    }
    *handle = reply.handle;
    *va = (void *)(uintptr_t)reply.address;
    return 0;
}

void nvdec_free(void *ctx, uint32_t handle)
{
    NVDECClient *c = ctx;
    const TegraNVDECBuffer b = {handle, 0};
    call(c, c->selector, TegraNVDECMethodBufferFree, &b, sizeof(b), NULL, 0);  // the kernel unmaps it from this task
}

int nvdec_submit(void *ctx, const TegraNVDECSubmit *submit, uint32_t *fence)
{
    NVDECClient *c = ctx;
    TegraNVDECFence reply;
    if (call(c, c->selector, TegraNVDECMethodSubmit, submit, sizeof(*submit), &reply, sizeof(reply)))
        return -1;
    *fence = reply.fence;
    return 0;
}

int nvdec_wait(void *ctx, const TegraNVDECWait *wait)
{
    NVDECClient *c = ctx;
    return call(c, c->selector, TegraNVDECMethodWait, wait, sizeof(*wait), NULL, 0) ? -1 : 0;
}
