// The pack's decode states (pack/decode.metal) on this Mac's GPU against its own decode of the same blocks: random
// blocks (ASTC: of every block mode the footprint takes, one in 16 a void extent; PVRTC: any words) decoded by Apple's
// texture unit (a texture of the format filled by a blit from a buffer, PVRTC's rows by MTLBlitOptionRowLinearPVRTC) and
// by the decode state from that buffer into the stored format's integer view, each texel compared as the stored format
// samples it (an sRGB code through this Mac's own sRGB table). Exact for every format but the linear 4x2 and 8x4 ones,
// whose 16-bit codes are held to the driver's rule (the nearest code of Apple's k / 4095), their distance from Apple
// reported: the largest in codes, and the texels an 8-bit target would hold otherwise.
// usage: decodeoracle <decode metallib> <format> <blocks across> <blocks down> <seed>
//   the metallib: xcrun -sdk macosx metal -c pack/decode.metal -I pack -fno-fast-math, then xcrun metallib
//   build: clang++ -std=c++17 -fobjc-arc -I pack decodeoracle.mm -framework Foundation -framework Metal
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include "astc_decode.h"

using namespace gm20b::decode;

struct Step {
    uint32_t x, y, blockW, blockH, blocksX, blocksY, rowBytes, form, first;
};

int main(int, char **argv)
{
    @autoreleasepool {
        const uint32_t f = uint32_t(atoi(argv[2])), bx = uint32_t(atoi(argv[3])), by = uint32_t(atoi(argv[4]));
        std::mt19937 rng(uint32_t(atoi(argv[5])));
        const bool pvrtc = f >= 160 && f <= 167, hdr = f >= 221, srgb = pvrtc ? (f & 1) : f <= 200;
        static const uint32_t dims[][2] = {{4, 2}, {4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 4},   {8, 5},
                                           {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
        const uint32_t i = f >= 221 ? f - 221 : f >= 203 ? f - 203 : f - 185;
        const uint32_t bw = pvrtc ? (((f - 160) / 2) % 2 == 0 ? 8 : 4) : dims[i][0], bh = pvrtc ? 4 : dims[i][1];
        const uint32_t bb = pvrtc ? 8 : 16, w = bx * bw, h = by * bh, row = bx * bb;
        std::vector<uint32_t> words(size_t(bx) * by * bb / 4);
        std::vector<uint32_t> modes;
        for (uint32_t m = 0; m < 2048 && !pvrtc; m++) {
            const Words4 b = {m, 0, 0, 0};
            const AstcMode a = astcMode(b, int(bw), int(bh));
            const Quant q = weightQuant(a.weightRange);
            const int n = a.gridW * a.gridH * (a.dual ? 2 : 1), bits = iseBits(q, n);
            if (!a.voidExtent && !a.error && !reservedQuant(q) && n <= 64 && bits >= 24 && bits <= 96)
                modes.push_back(m);
        }
        for (size_t k = 0; k < words.size(); k++)
            words[k] = rng();
        for (size_t k = 0; !pvrtc && k < words.size(); k += 4) {
            if (rng() % 16 == 0) {
                words[k] = (words[k] & ~0xfffu) | 0x1fc | (rng() & 0x200) | (rng() % 8 ? 0xc00 : 0);
                if (rng() % 2)
                    words[k] |= 0xfffff000u, words[k + 1] = 0xffffffffu;
            } else {
                words[k] = (words[k] & ~0x7ffu) | modes[rng() % modes.size()];
            }
        }
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> q = [d newCommandQueue];
        NSError *e = nil;
        id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
        id<MTLBuffer> blocks = [d newBufferWithBytes:words.data()
                                              length:words.size() * 4
                                             options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)f
                                                                                      width:w
                                                                                     height:h
                                                                                  mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> apple = [d newTextureWithDescriptor:td];
        NSString *get =
            @"#include <metal_stdlib>\nusing namespace metal;\nkernel void get(texture2d<float, access::read> t "
            @"[[texture(0)]], device float4 *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { if (p.x < "
            @"t.get_width() && p.y < t.get_height()) o[p.y * t.get_width() + p.x] = t.read(p); }\n";
        id<MTLComputePipelineState> read =
            [d newComputePipelineStateWithFunction:[[d newLibraryWithSource:get options:nil
                                                                      error:nil] newFunctionWithName:@"get"]
                                             error:nil];
        const char *frag = pvrtc  ? "decode_pvrtc_frag"
                           : hdr  ? "decode_astc_half_frag"
                           : srgb ? "decode_astc_unorm8_frag"
                                  : "decode_astc_unorm16_frag";
        const MTLPixelFormat view = pvrtc || (srgb && !hdr) ? MTLPixelFormatR32Uint : MTLPixelFormatRGBA16Uint;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"decode_vert"];
        pd.fragmentFunction = [lib newFunctionWithName:@(frag)];
        pd.colorAttachments[0].pixelFormat = view;
        id<MTLRenderPipelineState> state = [d newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!state) {
            printf("no state: %s\n", e.description.UTF8String);
            return 2;
        }
        MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:view
                                                                                      width:w
                                                                                     height:h
                                                                                  mipmapped:NO];
        od.storageMode = MTLStorageModeShared;
        od.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> ours = [d newTextureWithDescriptor:od];
        id<MTLBuffer> out = [d newBufferWithLength:size_t(w) * h * 16 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromBuffer:blocks
                   sourceOffset:0
              sourceBytesPerRow:row
            sourceBytesPerImage:row * by
                     sourceSize:MTLSizeMake(w, h, 1)
                      toTexture:apple
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)
                        options:pvrtc ? MTLBlitOptionRowLinearPVRTC : MTLBlitOptionNone];
        [b endEncoding];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
        rp.colorAttachments[0].texture = ours;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> r = [cb renderCommandEncoderWithDescriptor:rp];
        [r setRenderPipelineState:state];
        const Step s = {
            0,  0,  bw,  bh,
            bx, by, row, uint32_t(pvrtc ? (f < 164 ? 1 : 0) : hdr ? kAstcHalf : srgb ? kAstcUnorm8 : kAstcUnorm12),
            0};
        [r setFragmentBytes:&s length:sizeof s atIndex:0];
        [r setFragmentBuffer:blocks offset:0 atIndex:1];
        [r drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [r endEncoding];
        id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
        [c setComputePipelineState:read];
        [c setTexture:apple atIndex:0];
        [c setBuffer:out offset:0 atIndex:0];
        [c dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [c endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const size_t texel = view == MTLPixelFormatR32Uint ? 4 : view == MTLPixelFormatRGBA16Uint ? 8 : 16;
        std::vector<uint8_t> mine(size_t(w) * h * texel);
        [ours getBytes:mine.data() bytesPerRow:w * texel fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
        float srgbTable[256];
        {
            uint8_t px[1024];
            for (int k = 0; k < 256; k++)
                px[4 * k] = px[4 * k + 1] = px[4 * k + 2] = uint8_t(k), px[4 * k + 3] = 255;
            MTLTextureDescriptor *sd =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                                   width:256
                                                                  height:1
                                                               mipmapped:NO];
            sd.storageMode = MTLStorageModeShared;
            id<MTLTexture> st = [d newTextureWithDescriptor:sd];
            [st replaceRegion:MTLRegionMake2D(0, 0, 256, 1) mipmapLevel:0 withBytes:px bytesPerRow:1024];
            id<MTLBuffer> so = [d newBufferWithLength:4096 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> scb = [q commandBuffer];
            id<MTLComputeCommandEncoder> sc = [scb computeCommandEncoder];
            [sc setComputePipelineState:read];
            [sc setTexture:st atIndex:0];
            [sc setBuffer:so offset:0 atIndex:0];
            [sc dispatchThreads:MTLSizeMake(256, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [sc endEncoding];
            [scb commit];
            [scb waitUntilCompleted];
            for (int k = 0; k < 256; k++)
                srgbTable[k] = ((const float *)so.contents)[4 * k];
        }
        const float *a = (const float *)out.contents;
        long bad = 0, off8 = 0;
        double farthest = 0;
        for (size_t k = 0; k < size_t(w) * h; k++) {
            float m[4];
            for (int ch = 0; ch < 4; ch++) {
                if (texel == 4) {
                    const uint32_t code = mine[k * 4 + ch];
                    m[ch] = srgb && ch < 3 ? srgbTable[code] : float(code) / 255.0f;
                } else if (hdr) {
                    _Float16 hv;
                    memcpy(&hv, &mine[k * 8 + ch * 2], 2);
                    m[ch] = float(hv);
                } else {
                    uint16_t code;
                    memcpy(&code, &mine[k * 8 + ch * 2], 2);
                    const float apple = a[k * 4 + ch];
                    const uint32_t twelve = uint32_t(std::lround(apple * 4095.0f));
                    farthest = std::max(farthest, std::fabs(double(code) - double(apple) * 65535.0));
                    off8 += std::lround(code / 65535.0 * 255.0) != std::lround(double(apple) * 255.0);
                    m[ch] = float(twelve) / 4095.0f == apple && code == unorm12To16(twelve) ? apple : -1.0f;
                }
                const bool same = m[ch] == a[k * 4 + ch] || (std::isnan(m[ch]) && std::isnan(a[k * 4 + ch]));
                if (!same && bad++ < 4)
                    printf("texel %zu,%zu ch %d: ours %.8f apple %.8f\n", k % w, k / w, ch, m[ch], a[k * 4 + ch]);
            }
        }
        printf("format %u %ux%u: %ld of %zu channels differ (the pack's %s on this GPU vs Apple's decode)\n", f, w, h,
               bad, size_t(w) * h * 4, frag);
        if (!pvrtc && !srgb && !hdr)
            printf("  16-bit codes: at most %.4f of a code from Apple's value; %ld channels an 8-bit target holds "
                   "otherwise\n",
                   farthest, off8);
        return bad != 0;
    }
}
