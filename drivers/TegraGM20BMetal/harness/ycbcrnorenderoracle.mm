// SPDX-License-Identifier: MIT
// A pass into a two-plane YCbCr surface texture made with ShaderRead usage alone (Metal's validation error), validation off:
// what an M4 Max writes into the planes, against the same pass into a texture of RenderTarget usage over a twin surface.
// usage: ycbcrnort
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <vector>

static id<MTLDevice> dev;
static id<MTLCommandQueue> q;

static IOSurfaceRef surface(OSType fourcc, CVPixelBufferRef &pb)
{
    NSDictionary *attrs =
        @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
          (id)kCVPixelBufferMetalCompatibilityKey : @YES};
    return CVPixelBufferCreate(nullptr, 32, 32, fourcc, (__bridge CFDictionaryRef)attrs, &pb)
               ? nullptr
               : CVPixelBufferGetIOSurface(pb);
}

static std::vector<uint8_t> planes(IOSurfaceRef s)
{
    std::vector<uint8_t> out;
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
    for (size_t p = 0; p < 2; p++) {
        const uint8_t *b = static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, p));
        out.insert(out.end(), b, b + IOSurfaceGetBytesPerRowOfPlane(s, p) * IOSurfaceGetHeightOfPlane(s, p));
    }
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
    return out;
}

static void scribble(IOSurfaceRef s)
{
    IOSurfaceLock(s, 0, nullptr);
    for (size_t p = 0; p < 2; p++) {
        uint8_t *b = static_cast<uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, p));
        for (size_t i = 0; i < IOSurfaceGetBytesPerRowOfPlane(s, p) * IOSurfaceGetHeightOfPlane(s, p); i++)
            b[i] = uint8_t(i * 31 + 7);
    }
    IOSurfaceUnlock(s, 0, nullptr);
}

static std::vector<uint8_t> passInto(MTLPixelFormat f, OSType fourcc, MTLTextureUsage usage, long &status, bool &made)
{
    CVPixelBufferRef pb = nullptr;
    IOSurfaceRef s = surface(fourcc, pb);
    scribble(s);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                 width:32
                                                                                height:32
                                                                             mipmapped:NO];
    d.usage = usage;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:s plane:0];
    made = t != nil;
    if (!t)
        return {};
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0.25, 0.75, 1);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    [[cb renderCommandEncoderWithDescriptor:p] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    status = cb.status;
    std::vector<uint8_t> out = planes(s);
    CVPixelBufferRelease(pb);
    return out;
}

int main()
{
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice();
        q = [dev newCommandQueue];

        const struct {
            uint32_t format;
            OSType fourcc;
        } cases[] = {{540, '420v'}, {500, '420v'}, {520, '420v'}, {502, '422v'}, {503, '444v'}, {505, 'x420'}};

        for (const auto &c : cases) {
            for (MTLTextureUsage usage : {MTLTextureUsage(MTLTextureUsageShaderRead),
                                          MTLTextureUsage(MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)}) {
                long sa = 0, sb = 0;
                bool ma = false, mb = false;
                const std::vector<uint8_t> a = passInto(MTLPixelFormat(c.format), c.fourcc, usage, sa, ma),
                                           b = passInto(MTLPixelFormat(c.format), c.fourcc,
                                                        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, sb,
                                                        mb);
                size_t changed = 0, differ = 0;
                CVPixelBufferRef pb = nullptr;
                IOSurfaceRef s = surface(c.fourcc, pb);
                scribble(s);
                const std::vector<uint8_t> before = planes(s);
                CVPixelBufferRelease(pb);
                for (size_t i = 0; i < a.size() && i < b.size() && i < before.size(); i++) {
                    changed += a[i] != before[i];
                    differ += a[i] != b[i];
                }
                const char fc[5] = {char(c.fourcc >> 24), char(c.fourcc >> 16), char(c.fourcc >> 8), char(c.fourcc), 0};
                printf("format %u over %s usage 0x%lx: made %d (target-usage twin made %d), status %ld/%ld, "
                       "%zu of %zu bytes changed, %zu differ from the target-usage pass\n",
                       c.format, fc, (unsigned long)usage, ma, mb, sa, sb, changed, a.size(), differ);
            }
        }
    }
}
