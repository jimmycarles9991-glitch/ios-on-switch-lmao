// SPDX-License-Identifier: MIT
// raw GM20B path, over the mock GPU: Metal's contracts the facade (runtime/RawMetal.mm) keeps where the raw path cannot draw.
// Build it with -fsanitize=address.
// - Without an m2v device (no nvk dylib given): a command buffer that never reaches a GPU (the kernel refuses the raw path's
//   channel, and there is no m2v to replay it) runs its scheduled handlers, before its completed ones; and one dropped
//   uncommitted with an encoder open is freed with the encoder; a buffer fill runs raw.
// - With one: newBufferWithBytes: while the raw pool has no room (the kernel refuses its allocations) gives m2v's buffer with
//   the bytes in it; and the buffers a compute pass names, as arguments and in arrays, are marked written by the GPU once it
//   completes (on m2v, which the replay hands their twins), so the CPU's next read of them invalidates its cached lines; and a
//   command buffer whose raw submission the kernel refused after part of it ran completes with an error, not replayed.
//   Command buffers and their encoders answer globalTraceObjectID with IDs of their own, and stay raw.
// - Either way: the queues' GPU priorities reach the kernel (Priority, method 18) as the most urgent any live queue asked for,
//   with no m2v queue made for them.
// usage: contractcheck <pack> <cache dir> [<nvk dylib>]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <IOKit/IOReturn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "TegraGM20BABI.h"
#include "bridge/NativeMetalObjects.h"

extern "C" void mock_iokit_fail_next(unsigned method, unsigned count, int kr);
extern "C" void mock_iokit_fail_after(unsigned method, unsigned skip, unsigned count, int kr);
extern "C" unsigned long mock_iokit_calls(unsigned method);
extern "C" void mock_iokit_priority(uint32_t out[2]);

static int failed;

static void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failed += !ok;
}

static void missing(const char *, const char *)
{}

static void handlersRun(id<MTLDevice> dev)
{
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLBuffer> a = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared],
                  b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    mock_iokit_fail_next(TegraGM20BMethodChannelCreate, 1, kIOReturnNoResources);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:4096];
    [e endEncoding];
    __block std::string order;
    NSLock *lock = [NSLock new];
    [cb addScheduledHandler:^(id<MTLCommandBuffer>) {
        [lock lock];
        order += 's';
        [lock unlock];
    }];
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        [lock lock];
        order += 'c';
        [lock unlock];
    }];
    [cb commit];
    [cb waitUntilCompleted];
    usleep(100000);  // handlers run on queues of their own
    [lock lock];
    printf("handlers of a command buffer no GPU ran: \"%s\" (status %lu)\n", order.c_str(), (unsigned long)cb.status);
    expect(order == "sc", "its scheduled handler ran, before its completed one");
    [lock unlock];
}

// A command buffer of more blit encoders than one Exec takes, whose second Exec the kernel refuses: what ran is not run again
// on m2v; it completes with an error
static void partialNotReplayed(id<MTLDevice> dev)
{
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLBuffer> a = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared],
                  b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    auto counters = [&] {
        return (NSDictionary<NSString *, NSNumber *> *)[(id)dev performSelector:@selector(switchRawCounters)];
    };
    {  // the raw channel open (its context's Exec) before the command buffer under test
        id<MTLCommandBuffer> warm = [q commandBuffer];
        id<MTLBlitCommandEncoder> e = [warm blitCommandEncoder];
        [e copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:64];
        [e endEncoding];
        [warm commit];
        [warm waitUntilCompleted];
    }
    const unsigned long m2vBefore = counters()[@"m2v"].unsignedLongValue,
                        execs = mock_iokit_calls(TegraGM20BMethodExec);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    for (int i = 0; i < 300; i++) {
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:64];
        [e endEncoding];
    }
    mock_iokit_fail_after(TegraGM20BMethodExec, 1, 1, kIOReturnNoMemory);
    [cb commit];
    [cb waitUntilCompleted];
    const unsigned long m2v = counters()[@"m2v"].unsignedLongValue - m2vBefore;
    printf("a command buffer the kernel ran part of: status %lu, %lu on m2v, %lu Exec calls\n",
           (unsigned long)cb.status, m2v, mock_iokit_calls(TegraGM20BMethodExec) - execs);
    expect(cb.status == MTLCommandBufferStatusError && !m2v, "it completes with an error, not replayed on m2v");
}

// A command buffer dropped uncommitted with an encoder still open: both are freed (the command buffer holds its open encoder
// weakly, the encoder it)
static void openEncoderFreed(id<MTLDevice> dev)
{
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLBuffer> a = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared],
                  b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    __weak id weakCb = nil, weakEncoder = nil;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:64];
        weakCb = cb;
        weakEncoder = e;
    }
    printf("a command buffer dropped with an encoder open: command buffer %s, encoder %s\n", weakCb ? "kept" : "freed",
           weakEncoder ? "kept" : "freed");
    expect(!weakCb && !weakEncoder, "both are freed");
}

// fillBuffer:range:value: on the raw path (no m2v to replay it here): it completes, its range holds the value
static void fillRaw(id<MTLDevice> dev)
{
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    memset(b.contents, 0xee, 4096);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e fillBuffer:b range:NSMakeRange(5, 1000) value:0x5a];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const uint8_t *p = (const uint8_t *)b.contents;
    unsigned wrong = 0;
    for (unsigned i = 0; i < 4096; i++)
        wrong += p[i] != (i >= 5 && i < 1005 ? 0x5a : 0xee);
    printf("a fill on the raw path: status %lu, %u bytes wrong\n", (unsigned long)cb.status, wrong);
    expect(cb.status == MTLCommandBufferStatusCompleted && !wrong, "it completes with its range filled");
}

// globalTraceObjectID (MPS's kdebug traces ask encoders): every command buffer and encoder answers its own nonzero ID from one
// count, the same one each time, and asking keeps the command buffer raw
static void traceIds(id<MTLDevice> dev)
{
    SEL trace = sel_registerName("globalTraceObjectID");
    auto idOf = [&](id object) {
        return ((uint64_t (*)(id, SEL))objc_msgSend)(object, trace);
    };
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLBuffer> b = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    const uint64_t ids[3] = {idOf(cb), idOf(blit), idOf(cb)};
    [blit fillBuffer:b range:NSMakeRange(0, 256) value:1];
    [blit endEncoding];
    id<MTLComputeCommandEncoder> compute = [cb computeCommandEncoder];
    const uint64_t computeId = idOf(compute);
    [compute endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const char *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchRawPath")).UTF8String;
    printf("trace IDs: command buffer %llu (again %llu), blit %llu, compute %llu; path %s\n",
           (unsigned long long)ids[0], (unsigned long long)ids[2], (unsigned long long)ids[1],
           (unsigned long long)computeId, path);
    expect(ids[0] && ids[0] == ids[2] && ids[1] && computeId && ids[1] != ids[0] && computeId != ids[1] &&
               computeId != ids[0] && idOf(blit) == ids[1],
           "each answers its own nonzero ID, the same each time");
    expect(cb.status == MTLCommandBufferStatusCompleted && !strcmp(path, "raw"), "the command buffer stays raw");
}

static void bytesWithoutRoom(id<MTLDevice> dev)
{
    std::vector<uint8_t> bytes(64 << 20);  // past any chunk the pool has: an allocation of its own
    for (size_t i = 0; i < bytes.size(); i++)
        bytes[i] = uint8_t(i * 7 + 3);
    // the write-combined pool's allocation, then the pool chunk's import of host pages
    mock_iokit_fail_next(TegraGM20BMethodBoAlloc, 1, kIOReturnNoMemory);
    mock_iokit_fail_next(TegraGM20BMethodBoImport, 1, kIOReturnNoMemory);
    id<MTLBuffer> b = [dev newBufferWithBytes:bytes.data() length:bytes.size() options:MTLResourceStorageModeShared];
    mock_iokit_fail_next(TegraGM20BMethodBoAlloc, 0, 0);
    mock_iokit_fail_next(TegraGM20BMethodBoImport, 0, 0);
    printf("newBufferWithBytes: without room: %s\n", b ? object_getClassName(b) : "nil");
    expect(b && strcmp(object_getClassName(b), "RMBuffer"), "not the raw pool's");
    expect(b && b.contents && !memcmp(b.contents, bytes.data(), bytes.size()), "m2v's buffer, holding the bytes");
}

// newBufferWithBytesNoCopy: over memory 4 KiB-aligned but not 16 KiB, and from inside a page (iconservicesagent's on 27): raw
// buffers at the caller's bytes, a raw fill lands exactly in their range, and their m2v twins are over the same bytes
static void noCopyInsidePages(id<MTLDevice> dev)
{
    void *pages = nullptr;
    posix_memalign(&pages, 16384, 65536);
    memset(pages, 0xee, 65536);
    uint8_t *base = static_cast<uint8_t *>(pages);

    struct {
        size_t offset, length;
    } cases[] = {{4096, 21760}, {0x4a00, 3584}};

    id<MTLCommandQueue> q = [dev newCommandQueue];
    for (const auto &c : cases) {
        id<MTLBuffer> b = [dev newBufferWithBytesNoCopy:base + c.offset
                                                 length:c.length
                                                options:MTLResourceStorageModeShared
                                            deallocator:nil];
        const bool raw = b && !strcmp(object_getClassName(b), "RMBuffer");
        printf("no-copy buffer at +0x%zx, %zu bytes: %s\n", c.offset, c.length, b ? object_getClassName(b) : "nil");
        expect(raw && b.contents == base + c.offset, "a raw buffer at the caller's bytes");
        if (!raw)
            continue;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e fillBuffer:b range:NSMakeRange(0, c.length) value:0x5a];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        unsigned wrong = 0;
        for (size_t i = 0; i < 65536; i++)
            wrong += base[i] != (i >= c.offset && i < c.offset + c.length ? 0x5a : 0xee);
        expect(cb.status == MTLCommandBufferStatusCompleted && !wrong, "a fill of it writes its bytes and no others");
        memset(base + c.offset, 0xee, c.length);
        id<MTLBuffer> twin = ((id (*)(id, SEL))objc_msgSend)(b, sel_registerName("rmTwin"));
        expect(twin && twin.contents == base + c.offset && twin.length == c.length, "its twin is over the same bytes");
    }
}

// RMBuffer's _gpuDirty (std::atomic<bool>), read through the runtime: the facade has no accessor for it
static bool gpuDirty(id<MTLBuffer> b)
{
    Ivar dirty = class_getInstanceVariable(object_getClass(b), "_gpuDirty");
    return dirty && *reinterpret_cast<const bool *>(reinterpret_cast<const uint8_t *>((__bridge void *)b) +
                                                    ivar_getOffset(dirty));
}

static void computeWrites(id<MTLDevice> dev)
{
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLBuffer> one = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared],
                  two = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared],
                  three = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setBuffer:one offset:0 atIndex:0];
    const id<MTLBuffer> pair[2] = {two, three};
    const NSUInteger offsets[2] = {0, 0};
    [e setBuffers:pair offsets:offsets withRange:NSMakeRange(1, 2)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    usleep(100000);
    printf("a compute pass's buffers after it completed: dirty %d (argument), %d %d (array)\n", gpuDirty(one),
           gpuDirty(two), gpuDirty(three));
    expect(gpuDirty(one), "a buffer it names as an argument is marked written");
    expect(gpuDirty(two) && gpuDirty(three), "buffers it names in an array are marked written");
}

@protocol QueuePrioritySPI
- (BOOL)setGPUPriority:(NSUInteger)priority;
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority;
- (NSUInteger)getGPUPriority;
- (NSUInteger)getBackgroundGPUPriority;
@end

static bool kernelPriority(uint32_t priority, uint32_t background)
{
    uint32_t now[2];
    mock_iokit_priority(now);
    return now[0] == priority && now[1] == background;
}

static void queuePriorities(id<MTLDevice> dev)
{
    id<MTLCommandQueue, QueuePrioritySPI> render = (id)[dev newCommandQueue], other = (id)[dev newCommandQueue];
    expect([render setGPUPriority:0] && kernelPriority(0, 4), "a queue's priority 0 reaches the kernel");
    expect([other setBackgroundGPUPriority:1] && kernelPriority(0, 1), "another's background priority 1 too");
    expect(![other setGPUPriority:6] && kernelPriority(0, 1), "a priority Metal does not have is refused");
    expect([render getGPUPriority] == 0 && [other getBackgroundGPUPriority] == 1, "each queue answers its own");
    Ivar twin = class_getInstanceVariable(object_getClass(render), "_twin");
    expect(twin && !object_getIvar(render, twin) && !object_getIvar(other, twin), "no m2v queue made for them");
    @autoreleasepool {
        render = nil;
    }
    expect(kernelPriority(2, 1), "the most urgent live queue's once the urgent one is gone");
    @autoreleasepool {
        other = nil;
    }
    expect(kernelPriority(2, 4), "Metal's defaults once no queue asks");
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 3) {
            fprintf(stderr, "usage: contractcheck <pack> <cache dir> [<nvk dylib>]\n");
            return 2;
        }
        setenv("NVDEC_WRITETHROUGH", "1",
               0);  // what the GPU writes lands in the memory behind it (fillRaw reads it back)
        id<MTLDevice> fallback = nil;
        if (argc > 3) {
            setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
            if (!(fallback = native_bridge_metal_open(argv[3], argv[2], 0, missing))) {
                fprintf(stderr, "NO M2V DEVICE: %s\n", nmb_last_error());
                return 1;
            }
        }
        NSString *why = nil;
        id<MTLDevice> dev = raw_metal_device_open(argv[1], 0, fallback, &why);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", why.UTF8String);
            return 1;
        }
        queuePriorities(dev);
        if (fallback) {
            noCopyInsidePages(dev);
            bytesWithoutRoom(dev);
            computeWrites(dev);
            partialNotReplayed(dev);
        } else {
            fillRaw(dev);  // first: handlersRun refuses the raw channel for a second
            traceIds(dev);
            handlersRun(dev);
            openEncoderFreed(dev);
        }
        printf("%s: contractcheck\n", failed ? "FAIL" : "PASS");
        return failed ? 1 : 0;
    }
}
