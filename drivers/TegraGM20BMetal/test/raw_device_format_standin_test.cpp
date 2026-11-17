// SPDX-License-Identifier: MIT
// Colour targets of the formats the GM20B renders none of (raw/runtime/raw_format_standin.h): the store state's words and a
// clear's against an M4 Max's (draws and clears of edge values into 64x64 render targets Metal compresses, 0 of 4096 off
// each; a texture it does not compress clears as a draw, 512 of 512), and the stand-in
// passes raw::Device submits: R8Unorm_sRGB's first byte moved by the copy engine into an RGBA8Unorm_sRGB stand-in and back;
// A1BGR5Unorm, ABGR4Unorm and RGB9E5Float filled by the load state into an RGBA32Float stand-in and stored by their store
// state into the texture's words viewed as R16Uint or R32Uint; a level of a chain rendered through its own stand-in; and
// generateMipmapsForTexture: of each, a pass of the format's mip state a level into the level's words.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_attachment_target.h"
#include "../raw/runtime/raw_format_standin.h"
#include "../raw/runtime/raw_pack.h"
#include "../raw/runtime/raw_record.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include <cstring>
#include <map>
#include <vector>

namespace {

float asFloat(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// A texel an M4 Max drew (or cleared to) and the word it stored
struct Stored {
    uint32_t format;
    uint32_t rgba[4];
    uint32_t word;
    bool clear;
};

const Stored kStored[] = {
    {41, {0x3edef7be, 0x477fe000, 0x80000000, 0x3f3bd98f}, 0x000077c1, false},
    {41, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x0000001a, false},
    {41, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x000063cc, false},
    {41, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0x0000fe48, false},
    {41, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0x0000023f, false},
    {41, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x0000008b, false},
    {41, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x00000353, false},
    {41, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0x0000fabe, false},
    {41, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000000, false},
    {41, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0x00007ffe, false},
    {41, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0x000007c0, false},
    {41, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x00001838, false},
    {41, {0x3e6fce22, 0x3f000000, 0x3d45c8b0, 0x44e46365}, 0x00003c03, true},
    {41, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x0000001a, true},
    {41, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x000063cd, true},
    {41, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0x0000fe49, true},
    {41, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0x0000023f, true},
    {41, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x0000008b, true},
    {41, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x00000353, true},
    {41, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0x0000fabf, true},
    {41, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000000, true},
    {41, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0x00007fff, true},
    {41, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0x000007c0, true},
    {41, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x00001839, true},
    {42, {0x3edef7be, 0x477fe000, 0x80000000, 0x3f3bd98f}, 0x00007f0b, false},
    {42, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x00000066, false},
    {42, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x00006738, false},
    {42, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0x0000fc28, false},
    {42, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0x000004ff, false},
    {42, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x0000013c, false},
    {42, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x0000064f, false},
    {42, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0x0000f5f8, false},
    {42, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000003, false},
    {42, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0x00007ff8, false},
    {42, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0x00000f06, false},
    {42, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x000010d8, false},
    {42, {0x3e6fce22, 0x3f000000, 0x3d45c8b0, 0x44e46365}, 0x0000481f, true},
    {42, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x00000066, true},
    {42, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x00006738, true},
    {42, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0x0000fc28, true},
    {42, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0x000004ff, true},
    {42, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x0000013c, true},
    {42, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x0000064f, true},
    {42, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0x0000f5f8, true},
    {42, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000003, true},
    {42, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0x00007ff8, true},
    {42, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0x00000f06, true},
    {42, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x000010d8, true},
    {93, {0x3edef7be, 0x477fe000, 0x80000000, 0x3f3bd98f}, 0xf803fe00, false},
    {93, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x76700000, false},
    {93, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x72d7bf7c, false},
    {93, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0xf80001fe, false},
    {93, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0xfffc0000, false},
    {93, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x6d84c600, false},
    {93, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x74633a00, false},
    {93, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0xc800011a, false},
    {93, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000000, false},
    {93, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0xf803fe00, false},
    {93, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0xc8027e00, false},
    {93, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x7f200029, false},
    {93, {0x3e6fce22, 0x3f000000, 0x3d45c8b0, 0x44e46365}, 0x78660078, true},
    {93, {0x7fc00000, 0x7fc00000, 0x3ece739d, 0x3ec1f0a7}, 0x76740000, true},
    {93, {0x3ebe09b3, 0x3eef9616, 0x3e35e1d9, 0x3f000000}, 0x72dbbf7c, true},
    {93, {0x477f7f00, 0x3f51c31b, 0x3e00ec33, 0x3f000000}, 0xf80001ff, true},
    {93, {0x7fc00000, 0x3e86a7ab, 0x4788b800, 0x40000000}, 0xfffc0000, true},
    {93, {0x7fc00000, 0x3d46318c, 0x3e30c9f1, 0x3f5240af}, 0x6d88c600, true},
    {93, {0x7fc00000, 0x3ece8dd4, 0x3e8c7d50, 0x441b2282}, 0x74673a00, true},
    {93, {0x440d7a05, 0x3ead5124, 0x3f7f205e, 0x3f000000}, 0xc800011b, true},
    {93, {0x7fc00000, 0xc3076e39, 0xbd97b58c, 0x3e300527}, 0x00000000, true},
    {93, {0x3efa09a0, 0x4788b800, 0x3f88ddd5, 0x3f000000}, 0xf803fe00, true},
    {93, {0x7fc00000, 0x441fe74a, 0x32f91bad, 0x3ebdef7c}, 0xc8028000, true},
    {93, {0x3da55db8, 0x00000000, 0x3f644eff, 0x3f000000}, 0x7f240029, true},
};

TEST(FormatStandIn, TheStoreStateStoresMetalsWords)
{
    uint32_t checked = 0;
    for (const Stored &s : kStored) {
        float v[4];
        for (int k = 0; k < 4; k++)
            v[k] = asFloat(s.rgba[k]);
        if (s.clear)
            raw::formatStandInClear(s.format, v, v, true);
        EXPECT_EQ(raw::formatStoredWord(s.format, v), s.word)
            << s.format << (s.clear ? " clear " : " draw ") << std::hex << s.rgba[0] << " " << s.rgba[1] << " "
            << s.rgba[2] << " " << s.rgba[3];
        checked++;
    }
    EXPECT_EQ(checked, 72u);
}

// Of a texture Metal does not compress (ShaderWrite, PixelFormatView or Unknown usage, a side under 16 texels), a clear
// stores as a draw of its colour does
TEST(FormatStandIn, AnUncompressedTexturesClearIsADrawOfItsColour)
{
    for (const Stored &s : kStored) {
        float v[4], held[4];
        for (int k = 0; k < 4; k++)
            v[k] = asFloat(s.rgba[k]);
        raw::formatStandInClear(s.format, v, held, false);
        EXPECT_EQ(raw::formatStoredWord(s.format, held), raw::formatStoredWord(s.format, v));
    }
}

TEST(FormatStandIn, EachFormatsStandIn)
{
    EXPECT_EQ(raw::formatStandIn(11), 71u);
    for (uint32_t format : {41u, 42u, 93u})
        EXPECT_EQ(raw::formatStandIn(format), 125u);
    for (uint32_t format : {10u, 70u, 71u, 80u, 125u, 540u})
        EXPECT_EQ(raw::formatStandIn(format), 0u);
    EXPECT_TRUE(raw::formatHoldsAlpha(41) && raw::formatHoldsAlpha(42));
    EXPECT_FALSE(raw::formatHoldsAlpha(11) || raw::formatHoldsAlpha(93));
}

// The copy engine's launches the command buffer submitted: (source, destination, SET_REMAP_COMPONENTS) each
struct Launch {
    uint64_t from, to;
    uint32_t remap;
};

struct Method {
    uint32_t method, value;
};

// Every job's words from `first` decoded: the copy engine's launches, and the 3D subchannel's methods in order
void decode(size_t first, std::vector<Launch> &launches, std::vector<Method> &methods)
{
    constexpr uint32_t kLaunch = 0x0300, kInUpper = 0x0400, kInLower = 0x0404, kOutUpper = 0x0408, kOutLower = 0x040c,
                       kRemap = 0x0708;
    uint64_t in = 0, to = 0;
    uint32_t remap = 0;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++) {
        uint32_t op = 0, subc = 0, m = 0, left = 0;
        auto method = [&](uint32_t v) {
            if (subc == 0)
                methods.push_back({m, v});
            if (subc != raw::SUBC_COPY)
                return;
            if (m == kInUpper)
                in = (in & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kInLower)
                in = (in & ~0xffffffffull) | v;
            else if (m == kOutUpper)
                to = (to & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kOutLower)
                to = (to & ~0xffffffffull) | v;
            else if (m == kRemap)
                remap = v;
            else if (m == kLaunch)
                launches.push_back({in, to, remap});
        };
        for (const uint32_t w : jobs[j].words) {
            if (left) {
                method(w);
                left--;
                if (op == 1 || op == 5)
                    m += 4;
                if (op == 5)
                    op = 3;
                continue;
            }
            if (!w)
                continue;
            op = w >> 29;
            subc = w >> 13 & 7;
            m = (w & 0xfff) << 2;
            if (op == 4)
                method(w >> 16 & 0x1fff);
            else
                left = w >> 16 & 0x1fff;
        }
    }
}

// The colour target 0 each pass set: its address and format
std::vector<std::pair<uint64_t, uint32_t>> targets(const std::vector<Method> &methods)
{
    std::vector<std::pair<uint64_t, uint32_t>> out;
    uint64_t va = 0;
    for (const Method &m : methods) {
        if (m.method == NVB197_SET_COLOR_TARGET_A(0))
            va = uint64_t(m.value) << 32;
        else if (m.method == NVB197_SET_COLOR_TARGET_B(0))
            va |= m.value;
        else if (m.method == NVB197_SET_COLOR_TARGET_FORMAT(0) && m.value)
            out.push_back({va, m.value});
    }
    return out;
}

class DeviceFormatStandIn : public harness::DeviceTest {
protected:
    void addState(const char *name, uint32_t format)
    {
        raw::RuntimeRecord r;
        r.keyString = "samples=1 c0=" + std::to_string(format) + " ";
        for (raw::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.programs[0].id = std::string(name) + "-vs";
        r.programs[1].id = std::string(name) + "-fs";
        r.pipeline.keyHash = raw::Pack::fnv(name, strlen(name));
        r.blob = {0x1a00, 0x1111};
        std::string why;
        ASSERT_NE(dev.addRecord(r, false, why), nullptr) << why;
    }
};

// remapOf: DST_X..W each a source component or NO_WRITE (6), one-byte components, the source's and destination's counts
constexpr uint32_t remap(uint32_t sources, uint32_t destinations)
{
    return 0 | 6u << 4 | 6u << 8 | 6u << 12 | (sources - 1) << 20 | (destinations - 1) << 24;
}

TEST_F(DeviceFormatStandIn, AnSrgbR8TargetsFirstByteGoesIntoItsStandInAndBack)
{
    raw::Texture t;
    ASSERT_TRUE(dev.createTexture(40, 12, raw::kR8UnormSrgb, t));
    EXPECT_FALSE(t.renderable || t.shadowTarget) << "no render target format of the GM20B's";
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    raw::Texture *s = raw::formatStandInFor(dev, *cb, t, 3, true, &why);
    ASSERT_NE(s, nullptr) << why;
    EXPECT_EQ(s->format, 71u) << "RGBA8Unorm_sRGB";
    EXPECT_TRUE(s->renderable && s->blockLinear);
    EXPECT_EQ(s->width, 40u);
    EXPECT_EQ(s->height, 12u);
    const uint64_t standIn = s->va;
    ASSERT_TRUE(raw::storeFormatStandIn(dev, *cb, t, *s, &why)) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    ASSERT_EQ(launches.size(), 2u);
    EXPECT_EQ(launches[0].from, t.va);
    EXPECT_EQ(launches[0].to, standIn);
    EXPECT_EQ(launches[0].remap, remap(1, 4)) << "the byte into red, green, blue and alpha left";
    EXPECT_EQ(launches[1].from, standIn);
    EXPECT_EQ(launches[1].to, t.va);
    EXPECT_EQ(launches[1].remap, remap(4, 1)) << "red back into the byte";
}

TEST_F(DeviceFormatStandIn, AClearedTargetsStandInIsNotFilled)
{
    raw::Texture t;
    ASSERT_TRUE(dev.createTexture(16, 16, raw::kR8UnormSrgb, t));
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const size_t first = kernel_model::model.jobs.size();
    const char *why = nullptr;
    ASSERT_NE(raw::formatStandInFor(dev, *cb, t, 0, false, &why), nullptr) << why;
    uint32_t seq = 0;
    ASSERT_TRUE(cb->commit(seq));
    std::vector<Launch> launches;
    std::vector<Method> methods;
    decode(first, launches, methods);
    EXPECT_TRUE(launches.empty());
}

TEST_F(DeviceFormatStandIn, WithoutThePacksStatesAPackedFormatHasNoStandIn)
{
    raw::Texture t;
    ASSERT_TRUE(dev.createTexture(16, 16, raw::kAbgr4Unorm, t));
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    const char *why = nullptr;
    EXPECT_EQ(raw::formatStandInFor(dev, *cb, t, 0, true, &why), nullptr);
    ASSERT_NE(why, nullptr);
    EXPECT_NE(strstr(why, "no load or store state"), nullptr) << why;
    cb->abandon();
}

TEST_F(DeviceFormatStandIn, APackedTargetIsFilledByTheLoadStateAndStoredIntoItsWords)
{
    for (uint32_t format : {raw::kA1Bgr5Unorm, raw::kAbgr4Unorm, raw::kRgb9e5Float}) {
        addState(raw::formatLoadState(format), 125);
        addState(raw::formatStoreState(format), raw::formatStoreView(format));
    }
    const std::map<uint32_t, uint32_t> view = {
        {raw::kA1Bgr5Unorm, 0xf1}, {raw::kAbgr4Unorm, 0xf1}, {raw::kRgb9e5Float, 0xe4}};  // R16_UINT, R32_UINT
    for (const auto &[format, rt] : view) {
        raw::Texture t;
        ASSERT_TRUE(dev.createTexture(64, 16, format, t, false, true));
        raw::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const size_t first = kernel_model::model.jobs.size();
        const char *why = nullptr;
        raw::Texture *s = raw::formatStandInFor(dev, *cb, t, 1, true, &why);
        ASSERT_NE(s, nullptr) << why;
        EXPECT_EQ(s->format, 125u) << "RGBA32Float";
        const uint64_t standIn = s->va;
        ASSERT_TRUE(raw::storeFormatStandIn(dev, *cb, t, *s, &why)) << why;
        uint32_t seq = 0;
        ASSERT_TRUE(cb->commit(seq));
        std::vector<Launch> launches;
        std::vector<Method> methods;
        decode(first, launches, methods);
        const auto passes = targets(methods);
        ASSERT_EQ(passes.size(), 2u) << format;
        EXPECT_EQ(passes[0].first, standIn) << "the load state's pass into the stand-in";
        EXPECT_EQ(passes[0].second, 0xc0u) << "RGBA32Float's target";
        EXPECT_EQ(passes[1].first, t.va) << "the store state's pass into the texture's words";
        EXPECT_EQ(passes[1].second, rt) << format;
    }
}

TEST_F(DeviceFormatStandIn, ALevelOfAChainRendersThroughItsOwnStandIn)
{
    for (uint32_t format : {raw::kA1Bgr5Unorm, raw::kAbgr4Unorm, raw::kRgb9e5Float}) {
        addState(raw::formatLoadState(format), 125);
        addState(raw::formatStoreState(format), raw::formatStoreView(format));
    }
    for (uint32_t format : {raw::kR8UnormSrgb, raw::kA1Bgr5Unorm, raw::kAbgr4Unorm, raw::kRgb9e5Float}) {
        raw::Texture t, level;
        ASSERT_TRUE(dev.createMipTexture(64, 32, format, 4, t)) << format;
        ASSERT_TRUE(raw::attachmentTarget(t, 2, 0, level)) << format;
        EXPECT_FALSE(level.renderable) << "no render target format of the GM20B's";
        EXPECT_EQ(level.width, 16u);
        level.tic = dev.attachmentTic(t, level, 2, 0);
        raw::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const size_t first = kernel_model::model.jobs.size();
        const char *why = nullptr;
        raw::Texture *s = raw::formatStandInFor(dev, *cb, level, 0, true, &why);
        ASSERT_NE(s, nullptr) << why;
        EXPECT_EQ(s->width, 16u);
        EXPECT_EQ(s->height, 8u);
        ASSERT_TRUE(raw::storeFormatStandIn(dev, *cb, level, *s, &why)) << why;
        uint32_t seq = 0;
        ASSERT_TRUE(cb->commit(seq));
        std::vector<Launch> launches;
        std::vector<Method> methods;
        decode(first, launches, methods);
        if (format == raw::kR8UnormSrgb) {
            ASSERT_EQ(launches.size(), 2u);
            EXPECT_EQ(launches[1].to, level.va) << "the level's bytes";
        } else {
            const auto passes = targets(methods);
            ASSERT_EQ(passes.size(), 2u);
            EXPECT_EQ(passes[1].first, level.va) << "the store state's pass into the level's words";
        }
    }
}

// generateMipmapsForTexture: of a chain of each format: one pass a level and layer, of the format's mip state reading the
// level above's words, into the level's words viewed as the integers of its texel size
TEST_F(DeviceFormatStandIn, EachFormatsLevelsAreItsMipStatesPasses)
{
    const std::map<uint32_t, uint32_t> view = {{raw::kR8UnormSrgb, 0xf6},
                                               {raw::kA1Bgr5Unorm, 0xf1},
                                               {raw::kAbgr4Unorm, 0xf1},
                                               {raw::kRgb9e5Float, 0xe4}};  // R8_UINT, R16_UINT, R32_UINT
    for (const auto &[format, rt] : view) {
        raw::Texture t;
        ASSERT_TRUE(dev.createMipTexture(64, 32, format, 4, t)) << format;
        raw::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const char *why = nullptr;
        EXPECT_FALSE(raw::formatMipPasses(dev, *cb, t, 0, 3, 0, 1, format, &why)) << "no mip state in the pack yet";
        cb->abandon();
        addState(raw::formatMipState(format), raw::formatMipView(format));
        cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        const size_t first = kernel_model::model.jobs.size();
        ASSERT_TRUE(raw::formatMipPasses(dev, *cb, t, 0, 3, 0, 1, format, &why)) << why;
        uint32_t seq = 0;
        ASSERT_TRUE(cb->commit(seq));
        std::vector<Launch> launches;
        std::vector<Method> methods;
        decode(first, launches, methods);
        const auto passes = targets(methods);
        ASSERT_EQ(passes.size(), 3u) << format;
        for (uint32_t l = 1; l <= 3; l++) {
            raw::Texture level;
            ASSERT_TRUE(raw::attachmentTarget(t, l, 0, level));
            EXPECT_EQ(passes[l - 1].first, level.va) << format << " level " << l;
            EXPECT_EQ(passes[l - 1].second, rt) << format;
        }
    }
}

// Metal's mip steps of A1BGR5Unorm and ABGR4Unorm (probe/fmt/mipq.mm): a texel's codes weighted exactly by its taps, the
// mean held as the nearest float and then rounded to its code, ties even, as an M4 Max stores them on 2:1 steps (0 of
// 1365 texels off each); RGB9E5Float's the taps' blend held as a half, mantissas truncated
TEST(FormatStandIn, AMipStepsCodeIsTheExactMeansNearestFloatRounded)
{
    // four codes of one channel at weights of one half: their mean's code, a tie as the float the mean is held as
    const struct {
        uint32_t max, sum, code;
    } kSteps[] = {{15, 6, 2},  {15, 30, 8},  {15, 34, 8},  {15, 38, 9},  {15, 54, 13}, {31, 18, 4},   {31, 30, 7},
                  {31, 34, 9}, {31, 66, 17}, {31, 70, 17}, {31, 78, 19}, {31, 82, 21}, {31, 114, 29}, {31, 61, 15}};

    for (const auto &s : kSteps) {
        const uint32_t codes[4] = {s.sum / 4 + (s.sum % 4 > 0), s.sum / 4 + (s.sum % 4 > 1),
                                   s.sum / 4 + (s.sum % 4 > 2), s.sum / 4};
        EXPECT_EQ(raw::formatMipCode(codes, 128, 128, s.max), s.code) << s.max << " " << s.sum;
    }
}

}  // namespace
