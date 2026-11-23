// SPDX-License-Identifier: MIT
// The pack's mip states of words (pack/fmt.metal: format_standin.h formatMipState, mip.h wordMip3dState) run on
// this Mac's GPU over a level 0 of splitmix64 words, against level 1 as an M4 Max's generateMipmapsForTexture: wrote it
// from the same words (macOS 27.2.1). The states are integer programs, so every GPU runs them to the same words.
#import <Metal/Metal.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

#include "../format_standin.h"
#include "../mip.h"
#include "bazel/testing/source_text.h"

namespace {

using switch_ios::test::SourceText;

uint64_t splitmix(uint64_t &s)
{
    s += 0x9e3779b97f4a7c15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// fmt.metal with the tables it includes inlined: newLibraryWithSource: has no include path
std::string packSource()
{
    const std::string src = SourceText("../pack/fmt.metal");
    std::string out;
    const std::regex include("#include \"([\\w.]+)\"");
    size_t at = 0;
    for (std::sregex_iterator it(src.begin(), src.end(), include), end; it != end; ++it) {
        out += src.substr(at, size_t(it->position()) - at) + SourceText("../pack/" + (*it)[1].str());
        at = size_t(it->position() + it->length());
    }
    return out + src.substr(at);
}

struct Step {
    const char *state;
    uint32_t view, width, height, depth;
    uint64_t seed;
};

// Level 1 of a level 0 of `s`'s size and seed (each word the low bytes of a splitmix64 output), drawn by its state a
// plane a pass as the runtime draws it (wordMipPasses, formatMipPasses): the level above read through its integer view,
// the sizes and the plane in buffer 0
std::vector<uint32_t> drawn(id<MTLDevice> dev, id<MTLLibrary> pack, const Step &s)
{
    const bool volume = s.depth > 1;
    const uint32_t lw = s.width / 2, lh = s.height / 2, ld = volume ? s.depth / 2 : 1;
    const uint32_t bytes = s.view == 13 ? 1 : s.view == 23 ? 2 : 4;
    std::vector<uint8_t> top(size_t(s.width) * s.height * s.depth * bytes);
    uint64_t state = s.seed;
    for (size_t i = 0; i < top.size(); i += bytes) {
        const uint32_t w = uint32_t(splitmix(state));
        memcpy(&top[i], &w, bytes);
    }
    MTLTextureDescriptor *in = [MTLTextureDescriptor new];
    in.textureType = volume ? MTLTextureType3D : MTLTextureType2D;
    in.pixelFormat = MTLPixelFormat(s.view);
    in.width = s.width, in.height = s.height, in.depth = s.depth;
    in.storageMode = MTLStorageModeShared;
    in.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> above = [dev newTextureWithDescriptor:in];
    [above replaceRegion:MTLRegionMake3D(0, 0, 0, s.width, s.height, s.depth)
             mipmapLevel:0
                   slice:0
               withBytes:top.data()
             bytesPerRow:s.width * bytes
           bytesPerImage:s.width * s.height * bytes];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [pack newFunctionWithName:@"fmt_vert"];
    pd.fragmentFunction = [pack newFunctionWithName:@(strrchr(s.state, '.') + 1)];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormat(s.view);
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
    EXPECT_NE(pipeline, nil) << s.state << ": " << error.localizedDescription.UTF8String;
    if (!pipeline)
        return {};
    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(s.view)
                                                                                  width:lw
                                                                                 height:lh
                                                                              mipmapped:NO];
    od.storageMode = MTLStorageModeShared;
    od.usage = MTLTextureUsageRenderTarget;
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    std::vector<uint32_t> level;
    for (uint32_t z = 0; z < ld; z++) {
        id<MTLTexture> plane = [dev newTextureWithDescriptor:od];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = plane;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:pipeline];
        [e setFragmentTexture:above atIndex:0];
        const uint32_t step[8] = {s.width, s.height, lw, lh, s.depth, ld, z, 0};
        [e setFragmentBytes:step length:sizeof step atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        std::vector<uint8_t> words(size_t(lw) * lh * bytes);
        [plane getBytes:words.data() bytesPerRow:lw * bytes fromRegion:MTLRegionMake2D(0, 0, lw, lh) mipmapLevel:0];
        for (size_t i = 0; i < words.size(); i += bytes) {
            uint32_t w = 0;
            memcpy(&w, &words[i], bytes);
            level.push_back(w);
        }
    }
    return level;
}

// `apple`: level 1 as an M4 Max wrote it, planes then rows
void expectApples(const Step &s, const std::vector<uint32_t> &apple)
{
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    ASSERT_NE(dev, nil);
    MTLCompileOptions *exact = [MTLCompileOptions new];
    exact.mathMode = MTLMathModeSafe;
    NSError *error = nil;
    id<MTLLibrary> pack = [dev newLibraryWithSource:@(packSource().c_str()) options:exact error:&error];
    ASSERT_NE(pack, nil) << error.localizedDescription.UTF8String;
    const std::vector<uint32_t> ours = drawn(dev, pack, s);
    ASSERT_EQ(ours.size(), apple.size());
    for (size_t i = 0; i < ours.size(); i++)
        EXPECT_EQ(ours[i], apple[i]) << s.state << " texel " << i << std::hex << ": ours 0x" << ours[i]
                                     << ", Apple's 0x" << apple[i];
}

const std::vector<uint32_t> kRgb9e5Flat1006 = {
    0xe1d68c5b, 0x95be9e27, 0x7c1ec196, 0xe6c17b0e, 0xec39ecc7, 0xbf5e12a8, 0xe5060438, 0x6c4c771e,
    0x9fc8b099, 0xbc7bcf2c, 0xb469da16, 0xb2be1725, 0x295f283b, 0xe5fc0cbe, 0x78371f1a, 0xd6c5cb63,
    0x9eaad28a, 0x71118348, 0xf3635fd5, 0xc386702d, 0xcc0a1616, 0xc5dfee7f, 0xb93ea6f9, 0xbda4d297,
    0xadbf4bbc, 0xcabb1193, 0x5d284299, 0xc5bbdaea, 0x6ce5f28e, 0xd5508863,
};
const std::vector<uint32_t> kRgb9e5Volume1224 = {
    0xbd8b03da, 0xc54ed460, 0xbaf38442, 0xddb7df3a, 0xe3947305, 0xda4cb15d, 0xbec59ff0, 0x408a206a, 0x8d75e752,
    0xbad62718, 0xae0d7b51, 0xd2637ec9, 0xd4de1cc7, 0xdaeece98, 0xadba2ef7, 0xe1ecf517, 0xd1025ca7, 0xdc3ef768,
    0xe5a9e8fe, 0xd652a362, 0xec8882aa, 0xbb5e2b83, 0xac41b398, 0xd54ab9f2, 0xc111b900, 0xcdad0e6f, 0xd08335cd,
    0xe0bb9884, 0xdc8084fa, 0xe500b113, 0xe563041f, 0x73522c5e, 0xcc698ad9, 0xa4d94432, 0xdff5b779, 0xe1e6d900,
};
const std::vector<uint32_t> kRgb9e5Volume2880 = {
    0xa53656e3, 0xc9264597, 0xc4ac4e77, 0xba373ef4, 0xbce2bc16, 0xd777d973, 0xe2ed9d2c, 0xf52cf341, 0xc6170795,
    0x91a96592, 0xc06591fd, 0xb2fa02fd, 0xe5f6aa9f, 0xc4233dae, 0xd4822565, 0xdc590d6e, 0xd5777f14, 0xdca63086,
    0xad425420, 0xe666acfd, 0xa1e43563, 0xd1bc3787, 0xe64cb687, 0xc538f090, 0xdbf55d0a, 0xe876da29, 0x33ea772f,
    0xef0f80d4, 0xe4856754, 0xa73f47ac, 0xa4c19eb3, 0xac404898, 0xcd76fa74, 0xbb335041, 0xe45b316c, 0xd3aa023b,
};
const std::vector<uint32_t> kBgr10XrVolume106 = {
    0xda366a39, 0xd7d5fded, 0xdd685d5c, 0xe0321deb, 0xd1f72206, 0xe298b1e2, 0xd0da8a5c, 0xd187b606, 0xd9498f2f,
    0xe5fb0729, 0xd4e67a80, 0xf325d9c8, 0xdbc6c602, 0xe5e97a7c, 0xea763e60, 0xe52b0e73, 0xd089f6db, 0xd2099533,
    0xdba5ad4b, 0xd019e324, 0xe3691158, 0xdc4c4dc7, 0xdf34c232, 0xdc545651, 0xed3665c3, 0xd91d4570, 0xd6d6ca7d,
    0xe25b1b02, 0xeb5771ab, 0xe107fdb1, 0xe3c71da1, 0xe50a9ee3, 0xe7361ea1, 0xf40afd02, 0xeb276df0, 0xd872f282,
};

const std::vector<uint32_t> kSrgb8Flat1570 = {
    0x93, 0x6b, 0x98, 0xc2, 0x96, 0x8e, 0xbd, 0x63, 0xaf, 0x9f, 0xaa, 0x59, 0x27, 0x0d, 0x63,
    0x9b, 0x9f, 0x6b, 0x39, 0xcf, 0xb4, 0x28, 0x7d, 0x96, 0x84, 0x73, 0x70, 0x65, 0xb1, 0x59,
};
const std::vector<uint32_t> kA1bgr5Flat9458 = {
    0x6d9b, 0x9adf, 0x4ba0, 0x8ab1, 0xa354, 0x8ad4, 0x5f0e, 0x7427, 0xd471, 0x818e,
    0x92d6, 0x8368, 0x7594, 0x8515, 0x2cb9, 0xce74, 0x5e46, 0xbb1a, 0x9e65, 0x41d9,
    0xba14, 0xa08e, 0x7d97, 0x841d, 0x8b61, 0xbd5d, 0xa89d, 0x6a6c, 0xb461, 0xa458,
};
const std::vector<uint32_t> kAbgr4Flat2286 = {
    0xab84, 0x6829, 0x344a, 0x1756, 0x3392, 0x83a7, 0xca96, 0xa2bb, 0xbd56, 0x59ab,
    0x76ca, 0x7b54, 0xc489, 0x4895, 0xc48a, 0x3562, 0x4db8, 0x712a, 0x6be8, 0xb677,
    0x5076, 0x9859, 0xc764, 0x7773, 0x8a97, 0x6a89, 0xa829, 0xccb9, 0xb847, 0xa646,
};

// RGB9E5Float's 2D step: each channel's four taps summed at 10 fraction bits and 7 guard bits under the largest term's
// top bit, each term truncated, rounded to a half, then the shared exponent and truncated mantissas
TEST(GM20BWordMips, Rgb9e5StepsAsAnM4)
{
    const uint32_t f = gm20b::kRgb9e5Float;
    expectApples({gm20b::formatMipState(f), gm20b::formatMipView(f), 13, 11, 1, 1006}, kRgb9e5Flat1006);
}

// Its 3D step from depth 5 to 2: the trilinear weights on 2^-16 (an x and z weight's product exact, its share on y's
// second row rounded half down), each plane's four taps summed apart, the two planes added
TEST(GM20BWordMips, Rgb9e5VolumeStepsAsAnM4)
{
    const uint32_t f = gm20b::kRgb9e5Float;
    expectApples({gm20b::wordMip3dState(f), gm20b::wordMipView(f), 13, 7, 5, 1224}, kRgb9e5Volume1224);
    expectApples({gm20b::wordMip3dState(f), gm20b::wordMipView(f), 13, 7, 5, 2880}, kRgb9e5Volume2880);
}

// BGR10_XR's 3D step: the same weights, each code's term floored before the sum
TEST(GM20BWordMips, ExtendedRangeVolumeStepsAsAnM4)
{
    expectApples({gm20b::wordMip3dState(554), gm20b::wordMipView(554), 13, 7, 5, 106}, kBgr10XrVolume106);
}

// The 2D steps of the 8-bit sRGB and 16-bit packed formats: R8Unorm_sRGB's twelve-bit linear steps, each term floored
// to 2^-12 step; A1BGR5Unorm's and ABGR4Unorm's codes widened to ten and eight bits, each term floored; each on Metal's
// grid for the format
TEST(GM20BWordMips, SrgbAndPackedStepsAsAnM4)
{
    using gm20b::formatMipState, gm20b::formatMipView;
    expectApples({formatMipState(gm20b::kR8UnormSrgb), formatMipView(gm20b::kR8UnormSrgb), 13, 11, 1, 1570},
                 kSrgb8Flat1570);
    expectApples({formatMipState(gm20b::kA1Bgr5Unorm), formatMipView(gm20b::kA1Bgr5Unorm), 13, 11, 1, 9458},
                 kA1bgr5Flat9458);
    expectApples({formatMipState(gm20b::kAbgr4Unorm), formatMipView(gm20b::kAbgr4Unorm), 13, 11, 1, 2286},
                 kAbgr4Flat2286);
}

}  // namespace
