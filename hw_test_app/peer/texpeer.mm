// SPDX-License-Identifier: MIT
// The second process of HW Test metal.native.shared_texture_peer (tests/NativeSharedTexturePeerTest.swift): a task of its
// own, with its own Metal device and raw channel, that opens each MTLSharedTextureHandle the app sends with
// newSharedTextureWithHandle:, writes every texel of it on the GPU in the pattern peer/texpeer_pattern.h gives and answers
// once that work completed, so the app reads in its own pages what this process wrote in them.
//
// The app is sandboxed and can start no process, so it hands its anonymous NSXPCListener's endpoint to whoever starts this
// one. 21H461: launchd starts nothing from the overlay, so the BlueTool stand-in starts it and holds the endpoint until it
// asks (drivers/SwitchBluetooth/BlueTool/BlueTool.c). 24B119: launchd starts it for its Mach service
// (com.brunostjohn.switch.texpeer.plist), which the app sends the endpoint to. Either way it then connects to the app and
// serves that one connection.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <os/log.h>
#include <algorithm>
#include <unistd.h>
#include <xpc/xpc.h>
#include "texpeer_pattern.h"

extern "C" xpc_connection_t texpeer_mach_service(const char *name, dispatch_queue_t queue,
                                                 uint64_t flags) __asm__("_xpc_connection_create_mach_service");

@interface NSXPCListenerEndpoint (Endpoint)
- (void)_setEndpoint:(xpc_object_t)endpoint;
@end

@protocol SharedTexturePeer
- (void)write:(MTLSharedTextureHandle *)handle reply:(void (^)(NSString *))reply;
@end

// The app's side: an NSXPC connection reaches the listener only with its first message
@protocol SharedTexturePeerHost
- (void)ready:(int)pid;
@end

namespace {

constexpr int64_t kIdleSeconds = 60;
constexpr MTLPixelFormat kBlocks = MTLPixelFormatASTC_4x4_LDR;

NSUInteger slicesOf(id<MTLTexture> t)
{
    switch (t.textureType) {
    case MTLTextureTypeCube:
        return 6;
    case MTLTextureTypeCubeArray:
        return 6 * t.arrayLength;
    case MTLTextureType1DArray:
    case MTLTextureType2DArray:
        return t.arrayLength;
    default:
        return 1;
    }
}

MTLClearColor clearOf(uint32_t level, uint32_t slice)
{
    return MTLClearColorMake(
        switch_texpeer_texel(level, slice, 0, 0, 0) / 255.0, switch_texpeer_texel(level, slice, 0, 0, 1) / 255.0,
        switch_texpeer_texel(level, slice, 0, 0, 2) / 255.0, switch_texpeer_texel(level, slice, 0, 0, 3) / 255.0);
}

void clear(id<MTLCommandBuffer> cb, id<MTLTexture> t, uint32_t level)
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = t;
    pass.colorAttachments[0].level = level;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = clearOf(level, 0);
    [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
}

// Every level and slice not rendered, from buffers of the pattern (texels; a block-compressed texture's blocks)
bool fill(id<MTLDevice> device, id<MTLCommandBuffer> cb, id<MTLTexture> t)
{
    const bool blocks = t.pixelFormat == kBlocks, volume = t.textureType == MTLTextureType3D;
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    for (uint32_t level = 0; level < t.mipmapLevelCount; level++) {
        if (switch_texpeer_rendered(uint32_t(t.textureType), uint32_t(t.mipmapLevelCount), level))
            continue;
        const NSUInteger w = std::max<NSUInteger>(t.width >> level, 1), h = std::max<NSUInteger>(t.height >> level, 1);
        const NSUInteger d = volume ? std::max<NSUInteger>(t.depth >> level, 1) : 1;
        const NSUInteger across = blocks ? (w + 3) / 4 : w, down = blocks ? (h + 3) / 4 : h, unit = blocks ? 16 : 4;
        for (uint32_t slice = 0; slice < slicesOf(t); slice++) {
            NSMutableData *bytes = [NSMutableData dataWithLength:across * down * d * unit];
            uint8_t *at = static_cast<uint8_t *>(bytes.mutableBytes);
            for (NSUInteger z = 0; z < d; z++)
                for (NSUInteger y = 0; y < down; y++)
                    for (NSUInteger x = 0; x < across; x++)
                        for (uint32_t c = 0; c < unit; c++)
                            *at++ = blocks ? switch_texpeer_block(level, slice, uint32_t(x), uint32_t(y), c)
                                           : switch_texpeer_texel(level, uint32_t(volume ? z : slice), uint32_t(x),
                                                                  uint32_t(y), c);
            id<MTLBuffer> from = [device newBufferWithBytes:bytes.bytes
                                                     length:bytes.length
                                                    options:MTLResourceStorageModeShared];
            if (!from) {
                [blit endEncoding];
                return false;
            }
            [blit copyFromBuffer:from
                       sourceOffset:0
                  sourceBytesPerRow:across * unit
                sourceBytesPerImage:volume ? across * down * unit : 0
                         sourceSize:MTLSizeMake(w, h, d)
                          toTexture:t
                   destinationSlice:volume ? 0 : slice
                   destinationLevel:level
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
    }
    [blit endEncoding];
    return true;
}

// What jetsam holds against this daemon's limits (24B119: the Supplemental jetsam overlay's), now and at its peak
NSString *footprint()
{
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) != KERN_SUCCESS)
        return @"footprint_kb=- peak_kb=-";
    return [NSString stringWithFormat:@"footprint_kb=%llu peak_kb=%llu", vm.phys_footprint >> 10,
                                      vm.ledger_phys_footprint_peak >> 10];
}

}  // namespace

@interface TexturePeer : NSObject <SharedTexturePeer>
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> queue;
@end

@implementation TexturePeer

- (void)write:(MTLSharedTextureHandle *)handle reply:(void (^)(NSString *))reply
{
    id<MTLTexture> t = handle ? [self.device newSharedTextureWithHandle:handle] : nil;
    if (!t) {
        reply([NSString stringWithFormat:@"fail=no_texture pid=%d", getpid()]);
        return;
    }
    id<MTLCommandBuffer> cb = [self.queue commandBuffer];
    if (t.sampleCount > 1) {
        clear(cb, t, 0);
    } else {
        for (uint32_t level = 0; level < t.mipmapLevelCount; level++)
            if (switch_texpeer_rendered(uint32_t(t.textureType), uint32_t(t.mipmapLevelCount), level))
                clear(cb, t, level);
        if (!fill(self.device, cb, t)) {
            reply([NSString stringWithFormat:@"fail=no_buffer pid=%d", getpid()]);
            return;
        }
    }
    [cb commit];
    [cb waitUntilCompleted];
    const SEL path = NSSelectorFromString(@"switchRawPath");
    NSString *taken = [(id)cb respondsToSelector:path] ? [(id)cb valueForKey:@"switchRawPath"] : @"device";
    reply([NSString stringWithFormat:@"%@ pid=%d registry=%llx path=%@ shareable=%d %@",
                                     cb.status == MTLCommandBufferStatusCompleted ? @"ok" : @"fail=not_completed",
                                     getpid(), (unsigned long long)self.device.registryID, taken, t.isShareable,
                                     footprint()]);
}

@end

static TexturePeer *gPeer;
static NSXPCConnection *gConnection;

// Connects to the app's listener and serves it until the app lets the connection go
static void serve(xpc_object_t endpoint)
{
    if (gConnection || !endpoint || xpc_get_type(endpoint) != XPC_TYPE_ENDPOINT)
        return;
    NSXPCListenerEndpoint *app = [NSXPCListenerEndpoint new];
    [app _setEndpoint:endpoint];
    gConnection = [[NSXPCConnection alloc] initWithListenerEndpoint:app];
    NSXPCInterface *interface = [NSXPCInterface interfaceWithProtocol:@protocol(SharedTexturePeer)];
    [interface setClasses:[NSSet setWithObject:[MTLSharedTextureHandle class]]
              forSelector:@selector(write:reply:)
            argumentIndex:0
                  ofReply:NO];
    gConnection.exportedInterface = interface;
    gConnection.exportedObject = gPeer;
    gConnection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(SharedTexturePeerHost)];
    gConnection.invalidationHandler = ^{
        exit(0);
    };
    gConnection.interruptionHandler = ^{
        exit(0);
    };
    [gConnection resume];
    [(id<SharedTexturePeerHost>)gConnection.remoteObjectProxy ready:getpid()];
    os_log(OS_LOG_DEFAULT, "texpeer: serving the app's endpoint (pid %d)", getpid());
}

int main(void)
{
    @autoreleasepool {
        gPeer = [TexturePeer new];
        gPeer.device = MTLCreateSystemDefaultDevice();
        gPeer.queue = [gPeer.device newCommandQueue];
        if (!gPeer.queue) {
            os_log_error(OS_LOG_DEFAULT, "texpeer: no Metal device");
            return 1;
        }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kIdleSeconds * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            if (!gConnection)
                exit(0);
        });
#if SWITCH_TEXPEER_LAUNCHD
        xpc_connection_t listener = texpeer_mach_service("com.brunostjohn.switch.texpeer", dispatch_get_main_queue(),
                                                         XPC_CONNECTION_MACH_SERVICE_LISTENER);
        xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
            if (xpc_get_type(peer) != XPC_TYPE_CONNECTION)
                return;
            xpc_connection_t connection = (xpc_connection_t)peer;
            xpc_connection_set_event_handler(connection, ^(xpc_object_t message) {
                if (xpc_get_type(message) != XPC_TYPE_DICTIONARY)
                    return;
                serve(xpc_dictionary_get_value(message, "endpoint"));
                xpc_object_t answer = xpc_dictionary_create_reply(message);
                if (!answer)
                    return;
                xpc_dictionary_set_int64(answer, "result", gConnection ? 0 : EINVAL);
                xpc_dictionary_set_int64(answer, "pid", getpid());
                xpc_connection_send_message(connection, answer);
            });
            xpc_connection_resume(connection);
        });
        xpc_connection_resume(listener);
#else
        xpc_connection_t bluetool = texpeer_mach_service("com.apple.BlueTool", dispatch_get_main_queue(), 0);
        xpc_connection_set_event_handler(bluetool, ^(xpc_object_t){
                                         });
        xpc_connection_resume(bluetool);
        xpc_object_t ask = xpc_dictionary_create(nullptr, nullptr, 0);
        xpc_dictionary_set_string(ask, "kMsgId", "SwitchTexturePeerEndpoint");
        xpc_object_t answer = xpc_connection_send_message_with_reply_sync(bluetool, ask);
        serve(xpc_get_type(answer) == XPC_TYPE_DICTIONARY ? xpc_dictionary_get_value(answer, "endpoint") : nullptr);
        if (!gConnection) {
            os_log_error(OS_LOG_DEFAULT, "texpeer: the BlueTool stand-in held no endpoint for this process");
            return 1;
        }
#endif
    }
    dispatch_main();
}
