// SPDX-License-Identifier: MIT
// raw/runtime/raw_ycbcr_store_rule.h ycbcrLoaded against this Mac's Metal: what the tile holds after a Load of a two-plane
// YCbCr code target (8- and 10-bit, 4:2:0 and 4:2:2, both ranges, every texel over random planes, a 10-bit word's low six
// bits random too), read by framebuffer fetch into an RGBA32Float attachment beside it; and ycbcrStoreOverLoad against the
// planes Metal stores after a pass that loads the same planes and draws a float green through write mask green.
// usage: ycbcrloadoracle
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "../runtime/raw_ycbcr_store_rule.h"

namespace {

constexpr uint32_t W = 64, H = 32;

NSString *const kSrc = @"#include <metal_stdlib>\nusing namespace metal;\n"
                       @"struct V { float4 p [[position]]; };\n"
                       @"vertex V strip(uint i [[vertex_id]]) { V v; v.p = float4(float(i & 1) * 2 - 1, "
                       @"float(i >> 1) * 2 - 1, 0, 1); return v; }\n"
                       @"struct O { float4 seen [[color(1)]]; };\n"
                       @"fragment O fetch(V in [[stage_in]], half4 c [[color(0)]]) { O o; o.seen = float4(c); "
                       @"return o; }\n"
                       @"fragment float4 green(V in [[stage_in]]) { return float4(0, in.p.x * 0.0123f, 0, 1); }\n";

// The codes of a code target's planes in `s`: a byte, or a 16-bit word's high ten bits
void codesOf(IOSurfaceRef s, uint32_t bytes, uint32_t cw, uint32_t ch, std::vector<uint16_t> &luma,
             std::vector<uint16_t> &chroma)
{
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr);
    for (size_t plane = 0; plane < 2; plane++) {
        const uint8_t *b = static_cast<const uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, plane));
        const size_t stride = IOSurfaceGetBytesPerRowOfPlane(s, plane);
        const uint32_t codes = plane ? cw * 2 : W, rows = plane ? ch : H;
        std::vector<uint16_t> &out = plane ? chroma : luma;
        out.clear();
        for (uint32_t y = 0; y < rows; y++)
            for (uint32_t x = 0; x < codes; x++)
                out.push_back(bytes == 2 ? uint16_t((b[y * stride + 2 * x] | b[y * stride + 2 * x + 1] << 8) >> 6)
                                         : b[y * stride + x]);
    }
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
}

}  // namespace

int main()
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
        id<MTLCommandQueue> q = [dev newCommandQueue];

        struct Config {
            uint32_t format;
            OSType fourcc;
            bool down;
            uint32_t bytes;
        };

        const Config configs[] = {{540, '420v', true, 1},  {540, '420f', true, 1}, {541, '422v', false, 1},
                                  {541, '422f', false, 1}, {543, 'x420', true, 2}, {543, 'xf20', true, 2},
                                  {544, 'x422', false, 2}, {544, 'xf22', false, 2}};
        std::mt19937 rng(7);
        uint32_t total = 0, off = 0, stored = 0, storedOff = 0;
        for (const Config &c : configs)
            for (int seed = 0; seed < 4; seed++) {
                CVPixelBufferRef pb = nullptr;
                NSDictionary *attrs =
                    @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
                      (id)kCVPixelBufferMetalCompatibilityKey : @YES};
                if (CVPixelBufferCreate(nullptr, W, H, c.fourcc, (__bridge CFDictionaryRef)attrs, &pb)) {
                    printf("no surface %u\n", c.format);
                    continue;
                }
                IOSurfaceRef s = CVPixelBufferGetIOSurface(pb);
                const raw::YcbcrCodeTarget target = {c.bytes == 2 ? 1023u : 255u, c.down};
                std::vector<uint16_t> luma, chroma;
                const uint32_t cw = W / 2, ch = c.down ? H / 2 : H;
                IOSurfaceLock(s, 0, nullptr);
                for (size_t plane = 0; plane < 2; plane++) {
                    uint8_t *b = static_cast<uint8_t *>(IOSurfaceGetBaseAddressOfPlane(s, plane));
                    const size_t stride = IOSurfaceGetBytesPerRowOfPlane(s, plane);
                    const uint32_t codes = plane ? cw * 2 : W, rows = plane ? ch : H;
                    for (uint32_t y = 0; y < rows; y++)
                        for (uint32_t x = 0; x < codes; x++) {
                            const uint32_t word = rng();
                            const uint16_t code = uint16_t(c.bytes == 2 ? (word & 0xffff) >> 6 : word & 0xff);
                            if (c.bytes == 2) {
                                b[y * stride + 2 * x] = uint8_t(word);
                                b[y * stride + 2 * x + 1] = uint8_t(word >> 8);
                            } else {
                                b[y * stride + x] = uint8_t(word);
                            }
                            (plane ? chroma : luma).push_back(code);
                        }
                }
                IOSurfaceUnlock(s, 0, nullptr);
                MTLTextureDescriptor *d =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(c.format)
                                                                       width:W
                                                                      height:H
                                                                   mipmapped:NO];
                d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:s plane:0];
                MTLTextureDescriptor *fd =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                       width:W
                                                                      height:H
                                                                   mipmapped:NO];
                fd.usage = MTLTextureUsageRenderTarget;
                fd.storageMode = MTLStorageModeShared;
                id<MTLTexture> seen = [dev newTextureWithDescriptor:fd];
                MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
                pd.vertexFunction = [lib newFunctionWithName:@"strip"];
                pd.fragmentFunction = [lib newFunctionWithName:@"fetch"];
                pd.colorAttachments[0].pixelFormat = MTLPixelFormat(c.format);
                pd.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
                pd.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA32Float;
                id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
                if (!t || !ps) {
                    printf("format %u: not made\n", c.format);
                    continue;
                }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = t;
                rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                rp.colorAttachments[0].storeAction = MTLStoreActionDontCare;
                rp.colorAttachments[1].texture = seen;
                rp.colorAttachments[1].loadAction = MTLLoadActionClear;
                rp.colorAttachments[1].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
                [enc setRenderPipelineState:ps];
                [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [enc endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                if (cb.error) {
                    printf("format %u: %s %ld\n", c.format, cb.error.domain.UTF8String, long(cb.error.code));
                    continue;
                }
                std::vector<float> f(W * H * 4);
                [seen getBytes:f.data() bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
                std::vector<float> want(W * H * 4);
                raw::ycbcrLoaded(target, luma.data(), chroma.data(), W, H, want.data());
                for (uint32_t i = 0; i < W * H; i++) {
                    total++;
                    off += memcmp(&f[4 * i], &want[4 * i], 16) != 0;
                }

                std::vector<uint16_t> lumaIn, chromaIn;
                codesOf(s, c.bytes, cw, ch, lumaIn, chromaIn);
                MTLRenderPipelineDescriptor *gd = [MTLRenderPipelineDescriptor new];
                gd.vertexFunction = pd.vertexFunction;
                gd.fragmentFunction = [lib newFunctionWithName:@"green"];
                gd.colorAttachments[0].pixelFormat = MTLPixelFormat(c.format);
                gd.colorAttachments[0].writeMask = MTLColorWriteMaskGreen;
                id<MTLRenderPipelineState> gs = [dev newRenderPipelineStateWithDescriptor:gd error:&e];
                id<MTLCommandBuffer> gcb = [q commandBuffer];
                MTLRenderPassDescriptor *gp = [MTLRenderPassDescriptor renderPassDescriptor];
                gp.colorAttachments[0].texture = t;
                gp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                gp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> genc = [gcb renderCommandEncoderWithDescriptor:gp];
                [genc setRenderPipelineState:gs];
                [genc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [genc endEncoding];
                [gcb commit];
                [gcb waitUntilCompleted];
                if (!gs || gcb.error) {
                    printf("format %u: the draw over a load failed\n", c.format);
                    storedOff++;
                    CVPixelBufferRelease(pb);
                    continue;
                }
                std::vector<float> green(W * H);
                for (uint32_t y = 0; y < H; y++)
                    for (uint32_t x = 0; x < W; x++)
                        green[y * W + x] = (float(x) + 0.5f) * 0.0123f;
                std::vector<uint16_t> lumaWant(W * H), chromaWant(cw * ch * 2), lumaGot, chromaGot;
                raw::ycbcrStoreOverLoad(target, lumaIn.data(), chromaIn.data(), green.data(), W, H, lumaWant.data(),
                                        chromaWant.data());
                codesOf(s, c.bytes, cw, ch, lumaGot, chromaGot);
                for (size_t i = 0; i < lumaWant.size(); i++)
                    storedOff += lumaGot[i] != lumaWant[i];
                for (size_t i = 0; i < chromaWant.size(); i++)
                    storedOff += chromaGot[i] != chromaWant[i];
                stored += uint32_t(lumaWant.size() + chromaWant.size());
                CVPixelBufferRelease(pb);
            }
        printf("%s: %u texels loaded from two-plane YCbCr code targets, %u off the rule; %u codes stored over a load "
               "through write mask green, %u off\n",
               dev.name.UTF8String, total, off, stored, storedOff);
        return off || storedOff || !total || !stored ? 1 : 0;
    }
}
