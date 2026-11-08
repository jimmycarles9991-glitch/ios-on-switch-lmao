// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca r12): Core Animation's mipmapped textures through the facade (raw/fx chains), over the mock GPU whose
// copy engine nvdec runs and whose 2D engine raw/fx/test/twod_sim runs (NVDEC_WRITETHROUGH, NVDEC_TWOD): level 0 copied in from a
// buffer, generateMipmapsForTexture:, every level copied out (level 0 by the copy engine, the others from the chain by the 2D
// engine) into textures of its size and read back, checked byte for byte against the chain computed on the CPU with the 2D engine's
// arithmetic (fx::referenceDownsample, level from level), then into buffers (the copy engine) and checked alike; the facade must
// take the raw path for every command buffer. Then the replay's rules: the buffer copies again in a command buffer with a compute
// encoder (m2v's), which must replay with the texture of every level made current first (its log line counts).
// usage: mipframe <pack> <cache> <nvk dylib>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <vector>
#include "../fx/fx_mip.h"
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: mipframe <pack> <cache> <nvk dylib>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        setenv("NVDEC_WRITETHROUGH", "1", 0);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        if (!m2v) {
            fprintf(stderr, "NO M2V DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        NSString *why = nil;
        id<MTLDevice> dev = raw_metal_device_open(argv[1], 0, m2v, &why);
        if (!dev) {
            fprintf(stderr, "NO RAW DEVICE: %s\n", why.UTF8String);
            return 1;
        }
        int failed = 0;
        id<MTLCommandQueue> q = [dev newCommandQueue];

        struct Size {
            unsigned w, h, levels;
        };

        for (Size z : {Size{776, 24, 10}, Size{180, 180, 8}, Size{97, 3, 7}, Size{60, 60, 3}, Size{1, 1, 1}}) {
            if (z.levels < 2)
                continue;
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:z.w
                                                                                        height:z.h
                                                                                     mipmapped:YES];
            d.mipmapLevelCount = z.levels;
            d.storageMode = MTLStorageModePrivate;
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            id<MTLTexture> mip = [dev newTextureWithDescriptor:d];
            // level 0's pattern, and the chain the 2D engine makes of it (the CPU's model of its arithmetic)
            std::vector<std::vector<uint8_t>> want(z.levels);
            std::vector<unsigned> lw(z.levels), lh(z.levels);
            for (unsigned l = 0; l < z.levels; l++) {
                lw[l] = std::max(1u, z.w >> l);
                lh[l] = std::max(1u, z.h >> l);
                want[l].resize(size_t(lw[l]) * lh[l] * 4);
            }
            for (size_t i = 0; i < want[0].size(); i++)
                want[0][i] = uint8_t(i * 131u + (i >> 7) * 17u + 5u);
            for (unsigned l = 1; l < z.levels; l++)
                raw::fx::referenceDownsample(want[l - 1].data(), lw[l - 1] * 4, lw[l - 1], lh[l - 1], want[l].data(),
                                             lw[l] * 4, lw[l], lh[l], 4);
            id<MTLBuffer> src = [dev newBufferWithBytes:want[0].data()
                                                 length:want[0].size()
                                                options:MTLResourceStorageModeShared];
            std::vector<id<MTLTexture>> out(z.levels);
            for (unsigned l = 0; l < z.levels; l++) {
                MTLTextureDescriptor *o =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                       width:lw[l]
                                                                      height:lh[l]
                                                                   mipmapped:NO];
                o.storageMode = MTLStorageModeShared;
                o.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
                out[l] = [dev newTextureWithDescriptor:o];
            }
            for (int pass = 0; pass < 3; pass++) {
                // pass 0: into textures; 1: into buffers; 2: into buffers in a command buffer the raw path refuses (the
                // replay's texture of every level, made current)
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                [b copyFromBuffer:src
                           sourceOffset:0
                      sourceBytesPerRow:z.w * 4
                    sourceBytesPerImage:0
                             sourceSize:MTLSizeMake(z.w, z.h, 1)
                              toTexture:mip
                       destinationSlice:0
                       destinationLevel:0
                      destinationOrigin:MTLOriginMake(0, 0, 0)];
                [b generateMipmapsForTexture:mip];
                [b endEncoding];
                std::vector<id<MTLBuffer>> back(z.levels);
                b = [cb blitCommandEncoder];
                for (unsigned l = 0; l < z.levels; l++) {
                    if (pass == 0)
                        [b copyFromTexture:mip
                                  sourceSlice:0
                                  sourceLevel:l
                                 sourceOrigin:MTLOriginMake(0, 0, 0)
                                   sourceSize:MTLSizeMake(lw[l], lh[l], 1)
                                    toTexture:out[l]
                             destinationSlice:0
                             destinationLevel:0
                            destinationOrigin:MTLOriginMake(0, 0, 0)];
                    else {
                        back[l] = [dev newBufferWithLength:size_t(lw[l]) * lh[l] * 4
                                                   options:MTLResourceStorageModeShared];
                        [b copyFromTexture:mip
                                         sourceSlice:0
                                         sourceLevel:l
                                        sourceOrigin:MTLOriginMake(0, 0, 0)
                                          sourceSize:MTLSizeMake(lw[l], lh[l], 1)
                                            toBuffer:back[l]
                                   destinationOffset:0
                              destinationBytesPerRow:lw[l] * 4
                            destinationBytesPerImage:0];
                    }
                }
                [b endEncoding];
                if (pass == 2)
                    [[cb computeCommandEncoder] endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                if (cb.error) {
                    printf("FAIL %ux%u pass %d: command buffer error %s\n", z.w, z.h, pass,
                           cb.error.localizedDescription.UTF8String);
                    failed++;
                    continue;
                }
                if (pass == 2) {  // m2v's levels: generated by m2v's own filter on the mock (not run): only that it ran
                    printf("OK   %ux%u replay: %u levels copied out from m2v's texture of every level\n", z.w, z.h,
                           z.levels);
                    continue;
                }
                for (unsigned l = 0; l < z.levels; l++) {
                    std::vector<uint8_t> got(want[l].size());
                    if (pass == 1)
                        memcpy(got.data(), back[l].contents, got.size());
                    else
                        [out[l] getBytes:got.data()
                             bytesPerRow:lw[l] * 4
                              fromRegion:MTLRegionMake2D(0, 0, lw[l], lh[l])
                             mipmapLevel:0];
                    size_t bad = 0;
                    for (size_t i = 0; i < got.size(); i++)
                        bad += got[i] != want[l][i];
                    printf("%s %ux%u level %u (%ux%u) %s: %zu bytes, %zu differ\n", bad ? "FAIL" : "OK  ", z.w, z.h, l,
                           lw[l], lh[l], pass ? "to a buffer" : "to a texture", got.size(), bad);
                    failed += bad != 0;
                }
            }
        }
        char rates[1024];
        if (raw_metal_rates(rates, sizeof rates))
            printf("RATES %s\n", rates);
        printf("mipframe: %d case(s) failed\n", failed);
        return failed ? 1 : 0;
    }
}
