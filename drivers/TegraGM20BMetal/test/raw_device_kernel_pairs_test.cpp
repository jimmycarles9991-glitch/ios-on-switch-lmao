// SPDX-License-Identifier: MIT
// A kernel's sampler entries that hold the texture slots they sample (raw_pack.h kernelSamplerPairs) on raw::Device: each
// binds its own header where the address maps of its own textures leave its modes, the kernel's code and header then what
// a compile under those maps makes; one whose modes a map changes is no dispatch (raw_sampler_map.h). A kernel sampling a
// 4:2:0 YCbCr texture through a constexpr sampler beside an RGBA texture went to m2v (compute.image, TermConvertedSampler).
#include "raw_device_harness.h"
#include "../raw/runtime/raw_compute.h"
#include "../raw/runtime/raw_compute_record.h"

#include <array>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70;
constexpr uint32_t kQmdCbufValid = SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CONSTANT_BUFFER_VALID(0));
constexpr uint8_t kConstexpr = 0x80;
constexpr uint32_t kKnown = 1u << 31;
// TEXSAMP0 ADDRESS_U, _V and _P (clb197tex.h): all WRAP, all CLAMP_TO_EDGE, all BORDER
constexpr uint32_t kAddress = 0x1ff, kWrap = 0, kEdge = 2 | 2 << 3 | 2 << 6, kBorder = 3 | 3 << 3 | 3 << 6;

std::array<uint32_t, 8> header(uint32_t address)
{
    return {0x26000 | address, 0x291, 0xfff000, 0, 0, 0, 0, 0};
}

// Textures 0 and 1 each sampled through a combined descriptor of its own (set-0 words 0 and 1), both samplers constexpr
// of one header; `pairs0` and `pairs1` their windows
raw::ComputeRecord kernel(uint32_t address, uint32_t pairs0, uint32_t pairs1)
{
    raw::ComputeRecord r;
    r.code.assign(64, 0);
    r.cbufType[0] = raw::CbufRoot;
    r.cbufs = 1;
    r.kernel.regionWord = -1;
    r.kernel.qmd[kQmdCbufValid] |= 1;
    r.kernel.setBytes = 8;
    r.kernel.entries = 4;
    r.kernel.entry[0] = {0, raw::KernelTexture, 0, 0};
    r.kernel.entry[1] = {0, raw::KernelSampler, kConstexpr, pairs0};
    r.kernel.entry[2] = {4, raw::KernelTexture, 1, 0};
    r.kernel.entry[3] = {4, raw::KernelSampler, kConstexpr, pairs1};
    r.kernel.readOnlyEntries = 0xf;
    r.samplers = {header(address)};
    return r;
}

class DeviceKernelPairs : public harness::DeviceTest {
protected:
    raw::Texture converted, rgba;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(8, 8, kRGBA8, converted));
        ASSERT_TRUE(dev.createTexture(8, 8, kRGBA8, rgba));
        converted.addressMap = 3;  // a 4:2:0 texture read as codes (raw_ycbcr.h YcbcrPlan::addressMap)
    }

    // One dispatch of `r`, the converted texture at slot 0 and the RGBA one at 1: its two set-0 words, none where it was
    // refused (`why`)
    std::vector<uint32_t> words(const raw::ComputeRecord &r, std::string &why)
    {
        const raw::ComputeState *k = raw::placeCompute(dev, r);
        EXPECT_NE(k, nullptr);
        raw::CommandBuffer *cb = dev.begin();
        EXPECT_NE(cb, nullptr);
        if (!k || !cb)
            return {};
        std::vector<uint32_t> out;
        {
            raw::ComputeEncoder e(*cb, dev);
            e.setState(k);
            e.setTexture(0, &converted);
            e.setTexture(1, &rgba);
            const uint32_t groups[3] = {1, 1, 1}, threads[3] = {1, 1, 1};
            const char *refused = nullptr;
            if (e.dispatch(groups, threads, &refused)) {
                const uint32_t *set = raw::DeviceHarness::kernelSet(e);
                if (set)
                    out.assign(set, set + 2);
            } else {
                why = refused ? refused : "?";
            }
            e.end();
        }
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
        return out;
    }

    uint32_t address(uint32_t word)
    {
        return raw::DeviceHarness::samplerHeader(dev, word >> 20)[0] & kAddress;
    }
};

TEST_F(DeviceKernelPairs, AnEdgeSamplerOfAConvertedTextureKeepsItsHeader)
{
    static const raw::ComputeRecord r = kernel(kEdge, kKnown | 1u << 0, kKnown | 1u << 1);
    std::string why;
    const std::vector<uint32_t> set = words(r, why);
    ASSERT_EQ(set.size(), 2u) << why;
    EXPECT_EQ(set[0] & 0xfffff, converted.tic);
    EXPECT_EQ(set[1] & 0xfffff, rgba.tic);
    EXPECT_EQ(address(set[0]), kEdge);
    EXPECT_EQ(set[0] >> 20, set[1] >> 20);
}

TEST_F(DeviceKernelPairs, ARecordCompiledUnderTheMapIsDispatchedAsMade)
{
    // the direct compile under the texture's map (NativeMetalDirectCompute.rs facts_of) makes a repeating sampler of the
    // 4:2:0 codes texture's header clamp to edge
    uint32_t compiled = kWrap;
    for (const uint32_t shift : {0u, 3u, 6u})
        compiled = (compiled & ~(7u << shift)) | raw::mappedHeaderAddress(kWrap >> shift & 7, 3) << shift;
    static const raw::ComputeRecord r = kernel(compiled, kKnown | 1u << 0, kKnown);
    std::string why;
    const std::vector<uint32_t> set = words(r, why);
    ASSERT_EQ(set.size(), 2u) << why;
    EXPECT_EQ(address(set[0]), kEdge);
}

TEST_F(DeviceKernelPairs, ASamplerWhoseModesTheMapChangesIsNoDispatch)
{
    static const raw::ComputeRecord r = kernel(kWrap, kKnown | 1u << 0, kKnown | 1u << 1);
    std::string why;
    EXPECT_TRUE(words(r, why).empty())
        << "its kernel wraps the coordinate itself: only a compile under the map is right";
    EXPECT_EQ(why, "a kernel sampler whose modes a converted texture's address map changes, compiled without that map");
}

TEST_F(DeviceKernelPairs, ARepeatingSamplerOfOnlyTheRgbaTextureKeepsItsHeader)
{
    static const raw::ComputeRecord r = kernel(kWrap, kKnown, kKnown | 1u << 1);
    std::string why;
    const std::vector<uint32_t> set = words(r, why);
    ASSERT_EQ(set.size(), 2u) << why;
    EXPECT_EQ(address(set[0]), kWrap) << "the null image's";
    EXPECT_EQ(address(set[1]), kWrap);
}

TEST_F(DeviceKernelPairs, AMapOfTheWrappingModesLeavesAClampToZero)
{
    converted.addressMap = 1;  // a converted 4:2:0 texture, not read as codes
    static const raw::ComputeRecord r = kernel(kBorder, kKnown | 1u << 0, kKnown | 1u << 1);
    std::string why;
    const std::vector<uint32_t> set = words(r, why);
    ASSERT_EQ(set.size(), 2u) << why;
    EXPECT_EQ(address(set[0]), kBorder);
}

TEST_F(DeviceKernelPairs, ARecordWithoutPairsKeepsTheOldRule)
{
    static const raw::ComputeRecord r = kernel(kWrap, 0, 0);
    std::string why;
    const std::vector<uint32_t> set = words(r, why);
    ASSERT_EQ(set.size(), 2u) << why;
    EXPECT_EQ(address(set[0]), kWrap) << "a constexpr sampler without pairs is bound as it is (admission refuses it)";
    EXPECT_EQ(address(set[1]), kWrap);
}

}  // namespace
