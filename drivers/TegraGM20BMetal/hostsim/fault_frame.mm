// SPDX-License-Identifier: MIT
// A GPU fault fails the command buffers it takes and nothing else, as Metal's does: through the frontend, m2v and NVK over
// raw/pack's mock GPU, whose mock_iokit_kill_next kills a channel as the kext's client fault isolation does (the work it had
// in flight never runs, its fence passes it, its kill record names it, and the channel answers kIOReturnNotResponding).
//  - the command buffer the kill takes fails with MTLCommandBufferErrorDomain and the kernel error's Metal code (an MMU fault
//    a page fault, a hang a timeout);
//  - one still running when a later one's kill comes fails with it, as a victim of Metal's recovery;
//  - the queue gets a new channel, and the next command buffers on the same queue and device complete;
//  - memory written before the fault keeps its bytes, and the next command buffer reads it.
// usage: fault_frame <cache dir> <host libvulkan_nouveau.dylib>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstring>
#include "NativeBridgeMetalFrontend.h"
#include "TegraGM20BABI.h"
#include "bridge/NativeMetalObjects.h"

extern "C" unsigned long mock_iokit_calls(unsigned method);
extern "C" void mock_iokit_kill_next(uint32_t error);

namespace {
int failures;

void missing(const char *, const char *)
{}

void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "OK  " : "FAIL", what);
    failures += !ok;
}

constexpr NSUInteger kBytes = 64 * 1024;

id<MTLCommandBuffer> copy(id<MTLCommandQueue> queue, id<MTLBuffer> from, id<MTLBuffer> to)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (!cb)
        return nil;
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:from sourceOffset:0 toBuffer:to destinationOffset:0 size:kBytes];
    [b endEncoding];
    [cb commit];
    return cb;
}

bool failedWith(id<MTLCommandBuffer> cb, MTLCommandBufferError code)
{
    [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusError && [cb.error.domain isEqualToString:MTLCommandBufferErrorDomain] &&
           cb.error.code == (NSInteger)code;
}

bool completes(id<MTLCommandBuffer> cb)
{
    [cb waitUntilCompleted];
    if (cb.error)
        printf("     %s\n", cb.error.localizedDescription.UTF8String);
    return cb && cb.status == MTLCommandBufferStatusCompleted;
}

bool holds(id<MTLBuffer> buffer, uint8_t value)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(buffer.contents);
    for (NSUInteger i = 0; i < kBytes; i++)
        if (bytes[i] != value)
            return false;
    return true;
}

struct Counts {
    unsigned long creates, destroys;

    static Counts now()
    {
        return {mock_iokit_calls(TegraGM20BMethodChannelCreate), mock_iokit_calls(TegraGM20BMethodChannelDestroy)};
    }
};

void faultedCommandBufferFails(id<MTLDevice> dev, id<MTLCommandQueue> queue)
{
    id<MTLBuffer> source = [dev newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> before = [dev newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> after = [dev newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    memset(source.contents, 0x5a, kBytes);
    memset(before.contents, 0xa5, kBytes);
    expect(completes(copy(queue, source, after)), "a command buffer before the fault completes");

    const Counts was = Counts::now();
    mock_iokit_kill_next(1);
    expect(failedWith(copy(queue, source, after), MTLCommandBufferErrorPageFault),
           "the command buffer an MMU fault takes fails with MTLCommandBufferErrorPageFault");
    id<MTLCommandBuffer> next = copy(queue, before, after);
    expect(next != nil, "the queue makes command buffers after the fault (no DeviceLost)");
    expect(completes(next), "the next command buffer on the queue completes");
    const Counts is = Counts::now();
    expect(is.creates == was.creates + 1 && is.destroys == was.destroys + 1, "the queue got one new channel");
    expect(holds(before, 0xa5) && holds(source, 0x5a), "memory written before the fault keeps its bytes");
    expect([dev newCommandQueue] != nil && completes(copy([dev newCommandQueue], source, after)),
           "the device makes queues whose work completes");
}

void runningCommandBufferIsAVictim(id<MTLDevice> dev, id<MTLCommandQueue> queue)
{
    id<MTLBuffer> a = [dev newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> b = [dev newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> running = copy(queue, a, b);
    [running waitUntilScheduled];
    mock_iokit_kill_next(4);
    id<MTLCommandBuffer> faulting = copy(queue, b, a);
    expect(failedWith(faulting, MTLCommandBufferErrorTimeout),
           "the command buffer a hang's kill takes fails with MTLCommandBufferErrorTimeout");
    expect(failedWith(running, MTLCommandBufferErrorTimeout), "one still running when the kill came fails with it");
    expect(completes(copy(queue, a, b)), "the queue runs on after it");
}
}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 3) {
            fprintf(stderr, "usage: fault_frame <cache dir> <host libvulkan_nouveau.dylib>\n");
            return 2;
        }
        setenv("MOCK_GPU_US", "20000", 0);
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[2], argv[1], 0, missing);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        faultedCommandBufferFails(dev, queue);
        runningCommandBufferIsAVictim(dev, queue);
    }
    printf("%s\n", failures ? "FAILED" : "ALL OK");
    return failures ? 1 : 0;
}
