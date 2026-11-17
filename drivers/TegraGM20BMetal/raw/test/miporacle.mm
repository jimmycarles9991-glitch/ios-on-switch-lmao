// generateMipmapsForTexture: of R8Unorm_sRGB, A1BGR5Unorm, ABGR4Unorm and RGB9E5Float chains against this Mac's Metal: each
// level Metal made, then the pack's mip state (pack/fmt_store.metal fmt_mip_*) on this Mac's GPU from the level above as
// Metal stored it, into the level's words; every word compared.
// usage: miporacle <raw/pack directory>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "../runtime/raw_format_standin.h"

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: miporacle <raw/pack directory>\n");
            return 2;
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError *e = nil;
        NSString *src = [NSString stringWithContentsOfFile:[@(argv[1]) stringByAppendingPathComponent:@"fmt.metal"]
                                                  encoding:NSUTF8StringEncoding
                                                     error:nil];
        MTLCompileOptions *exact = [MTLCompileOptions new];
        exact.mathMode = MTLMathModeSafe;
        id<MTLLibrary> pack = src ? [dev newLibraryWithSource:src options:exact error:&e] : nil;
        if (!pack) {
            fprintf(stderr, "library: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        std::mt19937 rng(29);
        long steps = 0, off = 0, beyondOne = 0;
        for (uint32_t format : {raw::kR8UnormSrgb, raw::kA1Bgr5Unorm, raw::kAbgr4Unorm, raw::kRgb9e5Float}) {
            const uint32_t bytes = format == raw::kR8UnormSrgb ? 1 : format == raw::kRgb9e5Float ? 4 : 2;
            const MTLPixelFormat view = bytes == 1   ? MTLPixelFormatR8Uint
                                        : bytes == 2 ? MTLPixelFormatR16Uint
                                                     : MTLPixelFormatR32Uint;
            NSString *name = @(strrchr(raw::formatMipState(format), '.') + 1);
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [pack newFunctionWithName:@"fmt_vert"];
            pd.fragmentFunction = [pack newFunctionWithName:name];
            pd.colorAttachments[0].pixelFormat = view;
            id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
            if (!state) {
                fprintf(stderr, "%s: %s\n", name.UTF8String, e.localizedDescription.UTF8String);
                return 1;
            }
            long formatOff = 0, formatSteps = 0;
            for (const auto &size : {std::pair<uint32_t, uint32_t>{64, 64}, {37, 23}, {100, 7}, {256, 128}}) {
                const uint32_t w = size.first, h = size.second;
                MTLTextureDescriptor *d =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                       width:w
                                                                      height:h
                                                                   mipmapped:YES];
                d.usage = MTLTextureUsageShaderRead;
                d.storageMode = MTLStorageModeShared;
                id<MTLTexture> t = [dev newTextureWithDescriptor:d];
                std::vector<uint8_t> top(w * h * bytes);
                for (size_t i = 0; i < w * h; i++) {
                    uint32_t word = rng();
                    if (format == raw::kRgb9e5Float) {
                        const float c[3] = {float(rng() % 10000) / 997.0f, float(rng() % 10000) / 2311.0f,
                                            float(rng() % 1000) / 9973.0f};
                        word = raw::rgb9e5Word(c, true);
                    }
                    memcpy(&top[i * bytes], &word, bytes);
                }
                [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:top.data() bytesPerRow:w * bytes];
                id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
                [b generateMipmapsForTexture:t];
                [b endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                for (NSUInteger l = 0; l + 1 < t.mipmapLevelCount; l++) {
                    const uint32_t aw = std::max(1u, w >> l), ah = std::max(1u, h >> l), lw = std::max(1u, aw / 2),
                                   lh = std::max(1u, ah / 2);
                    std::vector<uint8_t> above(aw * ah * bytes), metal(lw * lh * bytes), ours(lw * lh * bytes);
                    [t getBytes:above.data()
                        bytesPerRow:aw * bytes
                         fromRegion:MTLRegionMake2D(0, 0, aw, ah)
                        mipmapLevel:l];
                    [t getBytes:metal.data()
                        bytesPerRow:lw * bytes
                         fromRegion:MTLRegionMake2D(0, 0, lw, lh)
                        mipmapLevel:l + 1];
                    MTLTextureDescriptor *vd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:view
                                                                                                  width:aw
                                                                                                 height:ah
                                                                                              mipmapped:NO];
                    vd.usage = MTLTextureUsageShaderRead;
                    vd.storageMode = MTLStorageModeShared;
                    id<MTLTexture> in = [dev newTextureWithDescriptor:vd];
                    [in replaceRegion:MTLRegionMake2D(0, 0, aw, ah)
                          mipmapLevel:0
                            withBytes:above.data()
                          bytesPerRow:aw * bytes];
                    vd.width = lw;
                    vd.height = lh;
                    vd.usage = MTLTextureUsageRenderTarget;
                    id<MTLTexture> out = [dev newTextureWithDescriptor:vd];
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = out;
                    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    id<MTLCommandBuffer> c2 = [q commandBuffer];
                    id<MTLRenderCommandEncoder> re = [c2 renderCommandEncoderWithDescriptor:rp];
                    [re setRenderPipelineState:state];
                    [re setFragmentTexture:in atIndex:0];
                    const raw::FormatMipStep step = {{aw, ah}, {lw, lh}};
                    [re setFragmentBytes:&step length:sizeof step atIndex:0];
                    [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                    [re endEncoding];
                    [c2 commit];
                    [c2 waitUntilCompleted];
                    [out getBytes:ours.data()
                        bytesPerRow:lw * bytes
                         fromRegion:MTLRegionMake2D(0, 0, lw, lh)
                        mipmapLevel:0];
                    for (uint32_t i = 0; i < lw * lh; i++) {
                        uint32_t a = 0, m = 0;
                        memcpy(&a, &ours[i * bytes], bytes);
                        memcpy(&m, &metal[i * bytes], bytes);
                        formatSteps++;
                        if (a == m)
                            continue;
                        formatOff++;
                        uint32_t most = 0;
                        if (format == raw::kRgb9e5Float) {
                            float x[3], y[3];
                            raw::rgb9e5Floats(a, x);
                            raw::rgb9e5Floats(m, y);
                            const float unit = std::ldexp(1.0f, int(std::min(a >> 27, m >> 27)) - 24);
                            for (int k = 0; k < 3; k++)
                                most = std::max(most, uint32_t(std::ceil(std::fabs(x[k] - y[k]) / unit)));
                        } else {
                            const uint32_t mask = format == raw::kAbgr4Unorm    ? 15
                                                  : format == raw::kA1Bgr5Unorm ? 31
                                                                                : 255;
                            const int step4 = format == raw::kAbgr4Unorm ? 4 : format == raw::kA1Bgr5Unorm ? 5 : 8;
                            for (int sh = format == raw::kA1Bgr5Unorm ? 1 : 0; sh < int(bytes * 8); sh += step4)
                                most = std::max(most, uint32_t(std::abs(int(a >> sh & mask) - int(m >> sh & mask))));
                            if (format == raw::kA1Bgr5Unorm)
                                most = std::max(most, (a ^ m) & 1);
                        }
                        beyondOne += most > 1;
                    }
                }
            }
            printf("  %u: %ld of %ld texels off Metal's\n", format, formatOff, formatSteps);
            steps += formatSteps;
            off += formatOff;
        }
        printf("%s: %ld mip texels of R8Unorm_sRGB, A1BGR5Unorm, ABGR4Unorm and RGB9E5Float chains, %ld off Metal's, "
               "%ld of them "
               "more than one code\n",
               dev.name.UTF8String, steps, off, beyondOne);
        return beyondOne ? 1 : 0;
    }
}
