// SPDX-License-Identifier: MIT
// Who reads the spare bits of a packed 10-bit surface's words (bits 30-31 and a row's slots past its last code), which the
// raw path's packed store states write 0 (raw_ycbcr_store.h ycbcrPackRow, raw_packed_plane.h): each reader on this Mac
// reads 'p420', 'pf20', 'p422', 'p444' surfaces of random codes twice, the spare bits 0 and then all set, and must read the
// same both times:
//  - Metal: R10Unorm_PACKED, RG10Unorm_PACKED, YCBCR10_*_2P_PACKED and RGB10_*_2P_PACKED textures over the planes;
//  - VideoToolbox: VTPixelTransferSession into the surface's 16-bit twin ('x420', 'xf20', 'x422', 'x444');
//  - Core Image: a CIImage of the buffer rendered to RGBAh.
// It also prints CoreVideo's black blocks of the fourccs (kCVPixelFormatBlackBlock), which hold the spare bits 0.
// usage: packedreaders
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>
#include <cstring>
#include <random>
#include <vector>

namespace {

NSString *const kReader =
    @"#include <metal_stdlib>\nusing namespace metal;\n"
    @"kernel void read_texels(texture2d<float, access::read> t [[texture(0)]], device float4 *out "
    @"[[buffer(0)]], uint2 at [[thread_position_in_grid]]) {\n"
    @"  if (at.x < t.get_width() && at.y < t.get_height()) out[at.y * t.get_width() + at.x] = t.read(at);\n}\n";

struct Surface {
    OSType packed, twin;
    uint32_t r10, rgb10, ycbcr10;  // the RGB10_*_2P_PACKED and YCBCR10_*_2P_PACKED formats over it
};

const Surface kSurfaces[] = {{'p420', 'x420', 588, 546, 508},
                             {'pf20', 'xf20', 588, 546, 508},
                             {'p422', 'x422', 588, 547, 509},
                             {'p444', 'x444', 588, 548, 510}};

CVPixelBufferRef make(OSType fourcc, uint32_t w, uint32_t h)
{
    CVPixelBufferRef pb = nullptr;
    NSDictionary *attrs =
        @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{},
          (id)kCVPixelBufferMetalCompatibilityKey : @YES};
    CVPixelBufferCreate(nullptr, w, h, fourcc, (__bridge CFDictionaryRef)attrs, &pb);
    return pb;
}

// Every word of both planes: random codes, and the spare bits all set or all clear
void fill(CVPixelBufferRef pb, uint32_t seed, bool spare)
{
    std::mt19937 rng(seed);
    CVPixelBufferLockBaseAddress(pb, 0);
    for (size_t p = 0; p < 2; p++) {
        uint8_t *base = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pb, p));
        const size_t codes = (p ? 2 : 1) * CVPixelBufferGetWidthOfPlane(pb, p), words = (codes + 2) / 3;
        for (size_t y = 0; y < CVPixelBufferGetHeightOfPlane(pb, p); y++)
            for (size_t x = 0; x < words; x++) {
                uint32_t w = 0;
                for (size_t i = 0; i < 3; i++) {
                    const uint32_t code = rng() & 1023;
                    if (3 * x + i < codes || spare)
                        w |= code << (10 * i);
                }
                w |= spare ? 3u << 30 : 0;
                memcpy(base + y * CVPixelBufferGetBytesPerRowOfPlane(pb, p) + 4 * x, &w, 4);
            }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

std::vector<float> metalRead(id<MTLDevice> dev, id<MTLComputePipelineState> reader, CVPixelBufferRef pb,
                             uint32_t format, size_t plane)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                           width:CVPixelBufferGetWidthOfPlane(pb, plane)
                                                          height:CVPixelBufferGetHeightOfPlane(pb, plane)
                                                       mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:CVPixelBufferGetIOSurface(pb) plane:plane];
    if (!t)
        return {};
    const size_t n = t.width * t.height;
    id<MTLBuffer> out = [dev newBufferWithLength:n * 16 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:reader];
    [e setTexture:t atIndex:0];
    [e setBuffer:out offset:0 atIndex:0];
    [e dispatchThreads:MTLSizeMake(t.width, t.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const float *f = static_cast<const float *>(out.contents);
    return std::vector<float>(f, f + 4 * n);
}

std::vector<uint8_t> transferred(VTPixelTransferSessionRef session, CVPixelBufferRef from, OSType twin)
{
    CVPixelBufferRef to = make(twin, uint32_t(CVPixelBufferGetWidth(from)), uint32_t(CVPixelBufferGetHeight(from)));
    std::vector<uint8_t> out;
    if (!VTPixelTransferSessionTransferImage(session, from, to)) {
        CVPixelBufferLockBaseAddress(to, kCVPixelBufferLock_ReadOnly);
        for (size_t p = 0; p < 2; p++) {
            const uint8_t *b = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(to, p));
            out.insert(out.end(), b,
                       b + CVPixelBufferGetBytesPerRowOfPlane(to, p) * CVPixelBufferGetHeightOfPlane(to, p));
        }
        CVPixelBufferUnlockBaseAddress(to, kCVPixelBufferLock_ReadOnly);
    }
    CVPixelBufferRelease(to);
    return out;
}

std::vector<uint16_t> rendered(CIContext *context, CVPixelBufferRef from)
{
    const size_t w = CVPixelBufferGetWidth(from), h = CVPixelBufferGetHeight(from);
    std::vector<uint16_t> out(4 * w * h);
    [context render:[CIImage imageWithCVPixelBuffer:from]
           toBitmap:out.data()
           rowBytes:w * 8
             bounds:CGRectMake(0, 0, w, h)
             format:kCIFormatRGBAh
         colorSpace:nil];
    return out;
}

void blackBlocks(OSType fourcc)
{
    CFDictionaryRef d = CVPixelFormatDescriptionCreateWithPixelFormatType(nullptr, fourcc);
    NSArray *planes = d ? ((__bridge NSDictionary *)d)[(id)kCVPixelFormatPlanes] : nil;
    printf("%c%c%c%c black blocks:", char(fourcc >> 24), char(fourcc >> 16), char(fourcc >> 8), char(fourcc));
    for (NSDictionary *plane in planes) {
        NSData *block = plane[(id)kCVPixelFormatBlackBlock];
        const uint32_t *words = static_cast<const uint32_t *>(block.bytes);
        for (NSUInteger i = 0; i < block.length / 4; i++)
            printf(" %08x", words[i]);
        printf(" |");
    }
    printf("\n");
    if (d)
        CFRelease(d);
}

}  // namespace

int main()
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kReader options:nil error:&e];
        id<MTLComputePipelineState> reader =
            [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"read_texels"] error:&e];
        VTPixelTransferSessionRef session = nullptr;
        VTPixelTransferSessionCreate(nullptr, &session);
        CIContext *context = [CIContext contextWithOptions:@{kCIContextWorkingFormat : @(kCIFormatRGBAh)}];
        uint32_t reads = 0, moved = 0, missing = 0;
        for (const Surface &s : kSurfaces) {
            blackBlocks(s.packed);
            for (const uint32_t width : {96u, 97u, 98u}) {
                CVPixelBufferRef pb = make(s.packed, width, 8);
                auto all = [&](bool spare) {
                    fill(pb, width, spare);
                    std::vector<std::vector<float>> metal = {
                        metalRead(dev, reader, pb, s.r10, 0), metalRead(dev, reader, pb, s.r10 + 1, 1),
                        metalRead(dev, reader, pb, s.rgb10, 0), metalRead(dev, reader, pb, s.ycbcr10, 0)};
                    std::vector<uint8_t> vt = transferred(session, pb, s.twin);
                    std::vector<uint16_t> ci = rendered(context, pb);
                    return std::make_tuple(metal, vt, ci);
                };
                const auto [metal0, vt0, ci0] = all(false);
                const auto [metal1, vt1, ci1] = all(true);
                for (size_t i = 0; i < metal0.size(); i++) {
                    missing += metal0[i].empty();
                    moved += metal0[i] != metal1[i];
                }
                missing += vt0.empty() + ci0.empty();
                moved += (vt0 != vt1) + (ci0 != ci1);
                reads += 6;
                CVPixelBufferRelease(pb);
            }
        }
        VTPixelTransferSessionInvalidate(session);
        CFRelease(session);
        printf("%s: %u reads (Metal's four views, VTPixelTransferSession, Core Image) of each surface, %u moved by the "
               "spare "
               "bits, %u not made\n",
               dev.name.UTF8String, reads, moved, missing);
        return moved || missing ? 1 : 0;
    }
}
