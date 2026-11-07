// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca): pack/ycbcr.metal's conversion, run on this Mac's own GPU, against what Metal reads of two-plane 4:2:0
// textures: an Apple M4's measured reads that m2v's tests hold (metal2vulkan bind/ycbcr_convert/tests.rs: METAL_READ_DIGEST and
// the 8-bit entries of METAL_PROBE_READS, each the FNV-1a digest of every texel's red, green and blue as little-endian 16-bit
// words, and the first four texels), over the same planes (the probes' generators at the same seeds). The shader's arithmetic is
// integer, so any GPU that runs it gives the same texels; the raw path runs it on the GM20B (pack.bin's ycbcr states).
// usage: ycbcrcheck <ycbcr.metallib built for macOS>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <vector>

namespace {
struct Random {
    uint64_t s;

    uint8_t next()
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return uint8_t(s >> 33);
    }

    std::vector<uint8_t> bytes(uint32_t n)
    {
        std::vector<uint8_t> b(n);
        for (auto &x : b)
            x = next();
        return b;
    }
};

Random ycf3(uint64_t seed)
{
    return {seed * 0x9E3779B97F4A7C15ull | 1};
}

enum Out {
    Converted,
    Srgb,
    Codes
};

enum Matrix {
    Bt601,
    Bt709,
    Bt2020
};

// m2v ycbcr_convert.rs `coefficients`: each channel's (kb, kr), red, green, blue
void coefficients(bool full, Matrix m, int32_t k[6])
{
    static const int32_t t[2][3][6] = {
        {{-9, 13123, -3218, -6686, 16591, 9}, {-1, 14743, -1753, -4383, 17373, 1}, {0, 13806, -1540, -5349, 17614, -1}},
        {{-8, 11483, -2816, -5850, 14518, 8},
         {-1, 12900, -1534, -3835, 15201, 1},
         {0, 12080, -1348, -4680, 15412, -1}}};
    memcpy(k, t[full][m], sizeof t[0][0]);
}

struct Case {
    const char *what;
    Out out;
    uint32_t w, h;
    uint32_t rows[4];  // luma row bytes and rows, chroma row bytes and rows
    bool full;
    Matrix matrix;
    int seedKind;  // 0: Random(seed) packed planes (METAL_READ_DIGEST), 1: ycf3(seed) over the rows
    uint64_t seed;
    uint32_t digest;
    uint16_t first[4][3];
};

const Case kCases[] = {
    {"YCBCR8_420_2P over 420v, no matrix, 40x24",
     Converted,
     40,
     24,
     {40, 24, 40, 12},
     false,
     Bt601,
     0,
     5,
     0x26e2da15,
     {{0, 869, 493}, {114, 1948, 1237}, {373, 2048, 957}, {239, 2048, 333}}},
    {"YCBCR8_420_2P over 420v, BT.2020",
     Converted,
     64,
     64,
     {64, 64, 64, 32},
     false,
     Bt2020,
     1,
     301,
     0xb78b8d92,
     {{0, 1172, 2048}, {604, 1364, 1919}, {1688, 1549, 1460}, {2048, 1464, 1269}}},
    {"YCBCR8_420_2P_sRGB over 420f, BT.2020, 33x17",
     Srgb,
     33,
     17,
     {64, 17, 64, 9},
     true,
     Bt2020,
     1,
     308,
     0xbb351938,
     {{262, 7677, 5655}, {0, 1272, 78}, {0, 604, 0}, {0, 3055, 0}}},
    {"YCBCR8_420_2P_sRGB over 420f, BT.709",
     Srgb,
     64,
     64,
     {64, 64, 64, 32},
     true,
     Bt709,
     1,
     109,
     0x5078acf7,
     {{142, 1245, 7502}, {0, 453, 5351}, {137, 3816, 8192}, {91, 1176, 8192}}},
    {"YCBCR8_420_2P_sRGB over 420v, no matrix",
     Srgb,
     64,
     64,
     {64, 64, 64, 32},
     false,
     Bt601,
     1,
     74,
     0xbf2e3b63,
     {{0, 2274, 2082}, {3382, 8192, 8192}, {1659, 3748, 4038}, {2856, 7235, 2754}}},
    {"RGB8_420_2P over 420v, no matrix",
     Codes,
     64,
     64,
     {64, 64, 64, 32},
     false,
     Bt601,
     1,
     541,
     0x9a1c4dae,
     {{152, 191, 246}, {115, 126, 214}, {40, 151, 150}, {64, 118, 105}}},
    {"RGB8_420_2P over 420f, BT.709, 33x17",
     Codes,
     33,
     17,
     {64, 17, 64, 9},
     true,
     Bt709,
     1,
     544,
     0xd068c106,
     {{113, 202, 72}, {96, 168, 69}, {61, 143, 62}, {57, 109, 44}}},
    {"RGB8_420_2P over 420v, BT.601, 7x5",
     Codes,
     7,
     5,
     {64, 5, 64, 3},
     false,
     Bt601,
     1,
     545,
     0x348cb55c,
     {{101, 5, 31}, {136, 245, 86}, {205, 231, 195}, {191, 40, 245}}},
};

uint32_t fnv(const std::vector<uint16_t> &v)
{
    uint32_t h = 2166136261u;
    for (uint16_t x : v)
        for (int b = 0; b < 2; b++)
            h = (h ^ uint8_t(x >> (8 * b))) * 16777619u;
    return h;
}

uint16_t halfBits(uint16_t h)  // half to the v of v / 2048 it holds
{
    __fp16 f;
    memcpy(&f, &h, 2);
    return uint16_t(lrintf(float(f) * 2048.0f));
}
}  // namespace

struct Shape {  // pack/ycbcr.metal's, of 8-bit 4:2:0 codes
    uint32_t width, height;
    int32_t full;
    int32_t k[6];
    uint32_t unit = 255, drop = 0, up = 2, max = 255, halved = 3, chroma = 1;
};

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: ycbcrcheck <ycbcr.metallib>\n");
            return 2;
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
        if (!lib) {
            fprintf(stderr, "no library: %s\n", e.localizedDescription.UTF8String);
            return 1;
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int failed = 0;
        for (const Case &c : kCases) {
            const uint32_t cw = (c.w + 1) / 2, ch = (c.h + 1) / 2;
            std::vector<uint8_t> luma(size_t(c.w) * c.h), chroma(size_t(cw) * ch * 2);
            if (c.seedKind == 0) {
                Random r{c.seed};
                luma = r.bytes(c.w * c.h);
                chroma = r.bytes(2 * cw * ch);
            } else {
                Random r = ycf3(c.seed);
                std::vector<uint8_t> l = r.bytes(c.rows[0] * c.rows[1]), k = r.bytes(c.rows[2] * c.rows[3]);
                for (uint32_t y = 0; y < c.h; y++)
                    for (uint32_t x = 0; x < c.w; x++)
                        luma[y * c.w + x] = l[y * c.rows[0] + x];
                for (uint32_t y = 0; y < ch; y++)
                    for (uint32_t x = 0; x < 2 * cw; x++)
                        chroma[y * 2 * cw + x] = k[y * c.rows[2] + x];
            }
            MTLTextureDescriptor *ld = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                          width:c.w
                                                                                         height:c.h
                                                                                      mipmapped:NO];
            MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm
                                                                                          width:cw
                                                                                         height:ch
                                                                                      mipmapped:NO];
            id<MTLTexture> lt = [dev newTextureWithDescriptor:ld], ct = [dev newTextureWithDescriptor:cd];
            [lt replaceRegion:MTLRegionMake2D(0, 0, c.w, c.h) mipmapLevel:0 withBytes:luma.data() bytesPerRow:c.w];
            [ct replaceRegion:MTLRegionMake2D(0, 0, cw, ch) mipmapLevel:0 withBytes:chroma.data() bytesPerRow:2 * cw];
            const MTLPixelFormat pf = c.out == Converted ? MTLPixelFormatRGBA16Float
                                      : c.out == Srgb    ? MTLPixelFormatRGBA32Float
                                                         : MTLPixelFormatRGBA8Unorm;
            const char *frag = c.out == Converted ? "ycbcr_converted_frag"
                               : c.out == Srgb    ? "ycbcr_srgb_frag"
                                                  : "ycbcr_codes_frag";
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [lib newFunctionWithName:@"ycbcr_vert"];
            pd.fragmentFunction = [lib newFunctionWithName:@(frag)];
            pd.colorAttachments[0].pixelFormat = pf;
            id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
            if (!ps) {
                fprintf(stderr, "no pipeline: %s\n", e.localizedDescription.UTF8String);
                return 1;
            }
            MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                                                                                          width:c.w
                                                                                         height:c.h
                                                                                      mipmapped:NO];
            od.usage = MTLTextureUsageRenderTarget;
            od.storageMode = MTLStorageModeShared;
            id<MTLTexture> ot = [dev newTextureWithDescriptor:od];
            Shape s = {c.w, c.h, c.full, {}};
            static_assert(sizeof s == 15 * 4, "raw_ycbcr.h kYcbcrShapeWords");
            coefficients(c.full, c.matrix, s.k);
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = ot;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
            [en setRenderPipelineState:ps];
            [en setFragmentTexture:lt atIndex:0];
            [en setFragmentTexture:ct atIndex:1];
            [en setFragmentBytes:&s length:sizeof s atIndex:0];
            [en drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [en endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            std::vector<uint16_t> texels;
            const size_t bpp = c.out == Converted ? 8 : c.out == Srgb ? 16 : 4;
            std::vector<uint8_t> px(size_t(c.w) * c.h * bpp);
            [ot getBytes:px.data() bytesPerRow:c.w * bpp fromRegion:MTLRegionMake2D(0, 0, c.w, c.h) mipmapLevel:0];
            for (size_t i = 0; i < size_t(c.w) * c.h; i++)
                for (int k = 0; k < 3; k++) {
                    uint16_t v;
                    if (c.out == Converted) {
                        uint16_t h;
                        memcpy(&h, &px[i * 8 + 2 * k], 2);
                        v = halfBits(h);
                    } else if (c.out == Srgb) {
                        float f;
                        memcpy(&f, &px[i * 16 + 4 * k], 4);
                        v = uint16_t(lrintf(f * 8192.0f));
                    } else {
                        v = px[i * 4 + k];  // red Cr, green Y, blue Cb: the [cr, y, cb] Metal reads
                    }
                    texels.push_back(v);
                }
            const uint32_t d = fnv(texels);
            bool firstOk = true;
            for (int i = 0; i < 4; i++)
                for (int k = 0; k < 3; k++)
                    firstOk = firstOk && texels[i * 3 + k] == c.first[i][k];
            const bool ok = d == c.digest && firstOk;
            printf("%s %s: digest %08x (Metal %08x)%s\n", ok ? "OK  " : "FAIL", c.what, d, c.digest,
                   firstOk ? "" : " first texels differ");
            failed += !ok;
        }
        printf("ycbcrcheck: %d of %zu differ from Metal's reads\n", failed, sizeof kCases / sizeof kCases[0]);
        return failed ? 1 : 0;
    }
}
