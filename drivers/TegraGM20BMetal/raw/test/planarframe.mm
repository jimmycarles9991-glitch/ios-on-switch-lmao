// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca): a widget snapshot's texture as the render server samples it, RGB8_444_2P (542) over a two-plane
// 4:4:4 surface ('_RB4': green codes in plane 0, the blue and red pairs in plane 1), drawn through the facade over the mock GPU
// whose copy engine nvdec runs into host memory (NVDEC_WRITETHROUGH). The command buffer that samples it must stay on the raw
// path (the rates line: c=1, m2v=0), and the image the raw path samples must hold, texel for texel, what Metal reads of the
// format (m2v ycbcr.rs, "Codes"): red Cr, green Y, blue Cb, alpha 255, at odd sizes and padded rows. The raw path converts the
// image when a draw's state samples the texture, so the pass draws once with each of the corpus's first BGRA8 states in the
// pack, the texture at every fragment slot, until one samples it.
// usage: planarframe <pack> <cache> <nvk host dylib> <QuartzCore default.metallib> <corpus.log>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <cstring>
#include <vector>
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "state_desc.h"

static void missing(const char *, const char *)
{}

static IOSurfaceRef planarSurface(uint32_t w, uint32_t h, uint32_t lumaRow, uint32_t chromaRow)
{
    const uint32_t lumaBytes = lumaRow * h, chromaOffset = (lumaBytes + 4095) & ~4095u, chromaBytes = chromaRow * h;
    NSDictionary *luma = @{
        (id)kIOSurfacePlaneWidth : @(w),
        (id)kIOSurfacePlaneHeight : @(h),
        (id)kIOSurfacePlaneBytesPerElement : @1,
        (id)kIOSurfacePlaneBytesPerRow : @(lumaRow),
        (id)kIOSurfacePlaneOffset : @0,
        (id)kIOSurfacePlaneSize : @(lumaBytes)
    };
    NSDictionary *chroma = @{
        (id)kIOSurfacePlaneWidth : @(w),
        (id)kIOSurfacePlaneHeight : @(h),
        (id)kIOSurfacePlaneBytesPerElement : @2,
        (id)kIOSurfacePlaneBytesPerRow : @(chromaRow),
        (id)kIOSurfacePlaneOffset : @(chromaOffset),
        (id)kIOSurfacePlaneSize : @(chromaBytes)
    };
    NSDictionary *props = @{
        (id)kIOSurfaceWidth : @(w),
        (id)kIOSurfaceHeight : @(h),
        (id)kIOSurfacePixelFormat : @(uint32_t('_RB4')),
        (id)kIOSurfacePlaneInfo : @[ luma, chroma ],
        (id)kIOSurfaceAllocSize : @(((chromaOffset + chromaBytes) + 16383) & ~16383u)
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

// The image the raw path samples is converted again after writes its surface's seed does not show: through an R8 texture over
// the luma plane (a plane texture: the raw path's), on the raw path, then in a command buffer m2v replays, then the CPU's
// through Metal. Each write puts a new luma pattern; a draw that samples the 542 texture must see it in green.
static int staleWrites(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLRenderPipelineState> samples, IOSurfaceRef s,
                       id<MTLTexture> t)
{
    const uint32_t w = uint32_t(t.width), h = uint32_t(t.height);
    MTLTextureDescriptor *pd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                  width:w
                                                                                 height:h
                                                                              mipmapped:NO];
    pd.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> luma = [dev newTextureWithDescriptor:pd iosurface:s plane:0];
    int failed = 0;
    printf("%s R8 texture over the luma plane: %s\n", raw_metal_texture_kept(luma) ? "OK  " : "FAIL",
           raw_metal_texture_kept(luma) ? "raw" : "m2v");
    failed += !raw_metal_texture_kept(luma);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:64
                                                                                 height:64
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = [dev newTextureWithDescriptor:td];
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    td.storageMode = MTLStorageModeMemoryless;
    pass.colorAttachments[1].texture = [dev newTextureWithDescriptor:td];
    size_t pitch = 0;
    const uint8_t *img = (const uint8_t *)raw_metal_texture_image(t, &pitch);
    const uint8_t vertices[256] = {};
    for (const char *how : {"a raw copy into the plane texture", "an m2v copy into the plane texture",
                            "a CPU replaceRegion of the plane texture"}) {
        std::vector<uint8_t> next(size_t(w) * h);
        for (size_t i = 0; i < next.size(); i++)
            next[i] = uint8_t(i * 29 + strlen(how) * 7);
        if (how[2] == 'C') {
            [luma replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:next.data() bytesPerRow:w];
        } else {
            id<MTLBuffer> rows = [dev newBufferWithBytes:next.data()
                                                  length:next.size()
                                                 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromBuffer:rows
                       sourceOffset:0
                  sourceBytesPerRow:w
                sourceBytesPerImage:0
                         sourceSize:MTLSizeMake(w, h, 1)
                          toTexture:luma
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
            if (how[3] == 'm')
                [[cb computeCommandEncoder]
                    endEncoding];  // the raw path hands compute to m2v, the whole command buffer
            [cb commit];
            [cb waitUntilCompleted];
        }
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
        [e setRenderPipelineState:samples];
        for (NSUInteger slot = 0; slot < 31; slot++)
            [e setVertexBytes:vertices length:sizeof vertices atIndex:slot];
        for (NSUInteger slot = 0; slot < 16; slot++)
            [e setFragmentTexture:t atIndex:slot];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        size_t bad = 0;
        for (uint32_t r = 0; r < h; r++)
            for (uint32_t x = 0; x < w; x++)
                bad += img[r * pitch + 4 * x + 1] != next[size_t(r) * w + x];
        printf("%s after %s: %zu of %u luma codes stale\n", bad ? "FAIL" : "OK  ", how, bad, w * h);
        failed += bad != 0;
    }
    return failed;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 6) {
            fprintf(stderr, "usage: planarframe <pack> <cache> <nvk dylib> <default.metallib> <corpus.log>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        setenv("NVDEC_WRITETHROUGH", "1", 0);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        NSString *why = nil;
        id<MTLDevice> dev = m2v ? raw_metal_device_open(argv[1], 0, m2v, &why) : nil;
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", m2v ? why.UTF8String : nmb_last_error());
            return 1;
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *error = nil;
        id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[4])] error:&error];
        std::vector<id<MTLRenderPipelineState>> states;
        for (const frame::State &st : frame::states(argv[5]))
            if (states.size() < 32)
                if (id<MTLRenderPipelineState> ps =
                        [dev newRenderPipelineStateWithDescriptor:state_desc::descriptor(qc, st.line) error:&error])
                    states.push_back(ps);
        if (states.empty()) {
            fprintf(stderr, "NO STATES: %s\n", error.localizedDescription.UTF8String ?: "-");
            return 1;
        }
        int failed = 0;
        id<MTLRenderPipelineState> samples = nil;  // the first state that samples the texture
        IOSurfaceRef kept = nullptr;
        id<MTLTexture> keptTexture = nil;

        struct Case {
            uint32_t w, h, lumaRow, chromaRow;
        };

        for (Case c : {Case{100, 60, 128, 256}, Case{37, 23, 64, 128}, Case{256, 128, 256, 512}}) {
            IOSurfaceRef s = planarSurface(c.w, c.h, c.lumaRow, c.chromaRow);
            if (!s) {
                printf("FAIL no '_RB4' surface %ux%u\n", c.w, c.h);
                failed++;
                continue;
            }
            IOSurfaceLock(s, 0, nullptr);
            uint8_t *y = (uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 0),
                    *cbcr = (uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 1);
            for (uint32_t r = 0; r < c.h; r++)
                for (uint32_t x = 0; x < c.w; x++) {
                    y[r * c.lumaRow + x] = uint8_t(x * 7 + r * 13 + 1);
                    cbcr[r * c.chromaRow + 2 * x] = uint8_t(x * 3 + r * 5 + 40);       // blue (Cb)
                    cbcr[r * c.chromaRow + 2 * x + 1] = uint8_t(x * 11 + r * 2 + 90);  // red (Cr)
                }
            IOSurfaceUnlock(s, 0, nullptr);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(542)
                                                                                         width:c.w
                                                                                        height:c.h
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:s plane:0];
            size_t pitch = 0;
            if (!t || !raw_metal_texture_image(t, &pitch)) {
                printf("FAIL %ux%u: no raw planar texture\n", c.w, c.h);
                failed++;
                CFRelease(s);
                continue;
            }
            memset((void *)raw_metal_texture_image(t, nullptr), 0, pitch * c.h);
            // a pass that samples it (as Core Animation binds a snapshot): the conversion runs before it
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:64
                                                                                         height:64
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> target = [dev newTextureWithDescriptor:td];
            td.storageMode = MTLStorageModeMemoryless;
            td.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> second = [dev newTextureWithDescriptor:td];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[1].texture = second;
            pass.colorAttachments[1].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[1].storeAction = MTLStoreActionDontCare;
            const uint8_t *img = (const uint8_t *)raw_metal_texture_image(t, nullptr);
            const uint8_t vertices[256] = {};
            for (id<MTLRenderPipelineState> ps : states) {
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
                [e setRenderPipelineState:ps];
                for (NSUInteger slot = 0; slot < 31; slot++)
                    [e setVertexBytes:vertices length:sizeof vertices atIndex:slot];
                for (NSUInteger slot = 0; slot < 16; slot++)
                    [e setFragmentTexture:t atIndex:slot];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                if (img[0] | img[1] | img[2] | img[3]) {
                    samples = samples ?: ps;
                    break;
                }
            }
            size_t bad = 0;
            for (uint32_t r = 0; r < c.h; r++)
                for (uint32_t x = 0; x < c.w; x++) {
                    const uint8_t *p = img + r * pitch + 4 * x;
                    const uint8_t want[4] = {cbcr[r * c.chromaRow + 2 * x + 1], y[r * c.lumaRow + x],
                                             cbcr[r * c.chromaRow + 2 * x], 255};
                    if (memcmp(p, want, 4)) {
                        if (!bad && getenv("PLANAR_DEBUG"))
                            fprintf(stderr, "  (%u,%u) got %02x%02x%02x%02x want %02x%02x%02x%02x\n", x, r, p[0], p[1],
                                    p[2], p[3], want[0], want[1], want[2], want[3]);
                        bad++;
                    }
                }
            printf("%s 542 over '_RB4' %ux%u (rows %u/%u): %zu of %u texels differ\n", bad ? "FAIL" : "OK  ", c.w, c.h,
                   c.lumaRow, c.chromaRow, bad, c.w * c.h);
            failed += bad != 0;
            if (!kept) {
                kept = s;
                keptTexture = t;
            } else {
                CFRelease(s);
            }
        }
        char line[1024];
        if (raw_metal_rates(line, sizeof line)) {
            printf("rates: %s\n", line);
            if (!strstr(line, " m2v=0 ")) {
                printf("FAIL a command buffer went to m2v\n");
                failed++;
            }
        }
        if (kept && samples)
            failed += staleWrites(dev, q, samples, kept, keptTexture);
        if (kept)
            CFRelease(kept);
        printf("planarframe: %d case(s) failed\n", failed);
        return failed ? 1 : 0;
    }
}
