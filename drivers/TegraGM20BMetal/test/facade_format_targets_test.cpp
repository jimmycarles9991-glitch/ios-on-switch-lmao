// SPDX-License-Identifier: MIT
// Passes into a texture of a format the GM20B renders none of (format_standin.h), as the range encode draws them
// (TegraGM20BMetalRangeForms.mm formatStandIns): a BGRA10_XR_sRGB target in a pass that draws nothing and neither clears nor
// resolves it keeps its codes (an M4 Max keeps an alpha code past 894 there: m2v-textures4 probes/xr553.mm), so nothing fills
// its stand-in or stores it back.
#include "facade_pass_scene.h"
#include "kits/SwitchGM20B/SwitchGM20B3D.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "../format_standin.h"
#include "../push.h"

#include <algorithm>

namespace {

using namespace pass_scene;

// The colour targets (address, format) the 3D engine's passes from job `first` on bound
std::vector<std::pair<uint64_t, uint32_t>> targets(size_t first)
{
    std::vector<std::pair<uint64_t, uint32_t>> out;
    uint64_t address = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const auto &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        for (long k = 0; k < count; ++k) {
            const auto &m = methods[k];
            if (m.subchannel != gm20b::SUBC_3D)
                continue;
            if (m.method == NVB197_SET_COLOR_TARGET_A(0))
                address = uint64_t(m.value) << 32;
            else if (m.method == NVB197_SET_COLOR_TARGET_B(0))
                address |= m.value;
            else if (m.method == NVB197_SET_COLOR_TARGET_FORMAT(0) && m.value)
                out.emplace_back(address, m.value);
        }
    }
    return out;
}

id<MTLDevice> formatDevice()
{
    return open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                          {{gm20b::formatLoadState(gm20b::kBgra10XrSrgb), MTLPixelFormatRGBA16Float},
                           {gm20b::formatStoreState(gm20b::kBgra10XrSrgb), MTLPixelFormatRGBA16Uint}}));
}

id<MTLTexture> bgra10XrSrgb(id<MTLDevice> device)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(gm20b::kBgra10XrSrgb)
                                                           width:64
                                                          height:64
                                                       mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:d];
}

void aPassThatDrawsNothingKeepsTheCodes(MTLLoadAction load)
{
    id<MTLDevice> device = formatDevice();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> t = bgra10XrSrgb(device);
    ASSERT_TRUE(t != nil && ((SwitchMetalTexture *)t)->_gm20bOk);
    const uint64_t codes = ((SwitchMetalTexture *)t)->_raw.va;
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = load;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(device, run(device, p));
    for (const auto &[address, format] : targets(first))
        EXPECT_NE(address, codes) << "a store into the codes of a pass that drew nothing (format " << format << ")";
}

// A cleared one is stored: Metal writes the clear into every texel
void aClearedTargetIsStored()
{
    id<MTLDevice> device = formatDevice();
    ASSERT_TRUE(device != nil);
    id<MTLTexture> t = bgra10XrSrgb(device);
    ASSERT_TRUE(t != nil && ((SwitchMetalTexture *)t)->_gm20bOk);
    const uint64_t codes = ((SwitchMetalTexture *)t)->_raw.va;
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    const size_t first = kernel_model::model.jobs.size();
    expectRaw(device, run(device, p));
    const auto passes = targets(first);
    EXPECT_TRUE(std::any_of(passes.begin(), passes.end(), [&](const auto &t) {
        return t.first == codes;
    })) << "the clear stored into the codes";
}

}  // namespace

TEST(GM20BFacadeFormatTargets, ALoadedPassThatDrawsNothingKeepsTheCodes)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        aPassThatDrawsNothingKeepsTheCodes(MTLLoadActionLoad);
    });
}

TEST(GM20BFacadeFormatTargets, ADontCarePassThatDrawsNothingKeepsTheCodes)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        aPassThatDrawsNothingKeepsTheCodes(MTLLoadActionDontCare);
    });
}

TEST(GM20BFacadeFormatTargets, AClearedTargetIsStored)
{
    switch_ios::test::ExpectPassesInFreshProcess(aClearedTargetIsStored);
}
