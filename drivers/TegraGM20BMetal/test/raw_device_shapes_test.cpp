// SPDX-License-Identifier: MIT
// raw/runtime/raw_texture_shape.h over every descriptor shape an M4 Max answered (texture_shapes_m4.txt): each shape
// Metal stops or answers nil for is nil, and each shape it makes is, for each identity, nil by that identity's answers
// or GM20B's facts, or laid out by a raw creator on the device. One case a group of shapes, so a failing case names its
// group.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_blend_kill.h"
#include "../raw/runtime/raw_clear.h"
#include "../raw/runtime/raw_private_comp.h"
#include "../raw/runtime/raw_resolve_rule.h"
#include "../raw/runtime/raw_samples.h"
#include "../raw/runtime/raw_texture_shape.h"
#include "../raw/runtime/raw_attachment_target.h"
#include "../raw/runtime/raw_xr_parts.h"
#include "../raw/runtime/raw_bound_texture.h"
#include "../raw/runtime/raw_srgb_pair.h"
#include "../raw/runtime/raw_zeta_cpu.h"
#include "../raw/runtime/raw_turn_copy.h"
#include "../raw/runtime/raw_zbc.h"
#include <fstream>
#include <unordered_map>
#include <sstream>

namespace {

using raw::shapes::TextureShape;

struct Probed {
    char answer;  // M made, V a validation stop, N nil
    TextureShape shape;
};

std::vector<Probed> probed()
{
    std::vector<Probed> out;
    std::ifstream file("drivers/TegraGM20BMetal/test/texture_shapes_m4.txt");
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in(line);
        Probed p;
        TextureShape &s = p.shape;
        in >> p.answer >> s.format >> s.type >> s.storage >> s.levels >> s.samples >> s.array >> s.usage;
        s.width = 16;
        s.height = s.type == raw::kType1D || s.type == raw::kType1DArray ? 1 : 16;
        s.depth = s.type == raw::kType3D ? 4 : 1;
        s.ownMemory = true;
        out.push_back(p);
    }
    return out;
}

const std::vector<Probed> &all()
{
    static const std::vector<Probed> shapes = probed();
    return shapes;
}

enum class Group {
    MultisampleArray,
    Snorm,
    Unrendered,
    FourTwoTwo,
    Extended,
    Blocks,
    AstcHdr,
    SrgbPair,
    Depth,
    SharedMultisample,
    Memoryless,
    Other
};

Group groupOf(const TextureShape &s)
{
    const uint32_t f = s.format;
    if (s.type == raw::kType2DMultisampleArray)
        return Group::MultisampleArray;
    if (f == 12 || f == 22 || f == 32 || f == 62 || f == 72 || f == 112)
        return Group::Snorm;
    if (f == 11 || f == 41 || f == 42 || f == 93)
        return Group::Unrendered;
    if (f == 240 || f == 241)
        return Group::FourTwoTwo;
    if (f == 550 || f == 552 || f == 554)
        return Group::Extended;
    if (f >= 222 && f <= 236)
        return Group::AstcHdr;
    if (f == 31)
        return Group::SrgbPair;
    if (f >= 130 && f <= 221)
        return Group::Blocks;
    if (f == 250 || f == 252 || f == 253 || f == 260)
        return Group::Depth;
    if (s.type == raw::kType2DMultisample && s.storage == raw::shapes::kShared)
        return Group::SharedMultisample;
    if (s.storage == raw::shapes::kMemoryless)
        return Group::Memoryless;
    return Group::Other;
}

std::string describe(const TextureShape &s)
{
    std::ostringstream o;
    o << "format " << s.format << " type " << s.type << " storage " << s.storage << " levels " << s.levels
      << " samples " << s.samples << " array " << s.array << " usage " << s.usage;
    return o.str();
}

class DeviceShapes : public harness::DeviceTest {
protected:
    // Every shape of `group` the M4 made, for each identity: nil by its answers or GM20B's facts, or made by its route
    // on the device. Prints the counts a CLOSED.txt row cites.
    void walk(Group group)
    {
        for (const raw::answers::Kind &kind : raw::answers::kKinds) {
            uint32_t made = 0, nilIdentity = 0, nilHardware = 0, shapes = 0;
            std::vector<std::string> missing;
            for (const Probed &p : all()) {
                if (p.answer != 'M' || groupOf(p.shape) != group)
                    continue;
                shapes++;
                const TextureShape &s = p.shape;
                if (raw::shapes::identityRefusal(s, kind)) {
                    nilIdentity++;
                    continue;
                }
                if (raw::shapes::hardwareRefusal(s)) {
                    nilHardware++;
                    continue;
                }
                const raw::shapes::Route route = raw::shapes::route(s);
                raw::Texture t;
                if (route != raw::shapes::Route::None &&
                    raw::shapes::create(dev, s, route, s.storage == raw::shapes::kPrivate, 0, t)) {
                    made++;
                    dev.destroyTexture(t);
                } else if (missing.size() < 12) {
                    missing.push_back((route == raw::shapes::Route::None ? "no route: " : "not laid out: ") +
                                      describe(s));
                } else {
                    missing.push_back("");
                }
            }
            printf("  %.*s: %u shapes, %u made, %u nil by identity, %u nil by GM20B, %zu left\n", int(kind.kind.size()),
                   kind.kind.data(), shapes, made, nilIdentity, nilHardware, missing.size());
            for (const std::string &m : missing)
                if (!m.empty())
                    ADD_FAILURE() << std::string(kind.kind) << " " << m;
            EXPECT_TRUE(missing.empty()) << std::string(kind.kind) << ": " << missing.size()
                                         << " shapes the identity makes and raw does not";
        }
    }
};

TEST_F(DeviceShapes, MetalsStopsAndNilsAreNilAndItsTexturesAreNot)
{
    uint32_t stops = 0, made = 0;
    for (const Probed &p : all()) {
        const char *why = raw::shapes::metalRefusal(p.shape);
        if (p.answer == 'M') {
            made++;
            EXPECT_EQ(why, nullptr) << describe(p.shape);
        } else {
            stops += why != nullptr;
            EXPECT_NE(why, nullptr) << p.answer << " " << describe(p.shape);
        }
    }
    printf("  %zu shapes: %u made, %u stopped or nil\n", all().size(), made, stops);
    EXPECT_EQ(all().size(), 22932u);
}

TEST_F(DeviceShapes, EveryMadeShapeHasAGroup)
{
    for (const Probed &p : all())
        if (p.answer == 'M')
            EXPECT_NE(groupOf(p.shape), Group::Other) << describe(p.shape);
}

TEST_F(DeviceShapes, MultisampleArrays)
{
    walk(Group::MultisampleArray);
}

TEST_F(DeviceShapes, SignedNormalizedFormats)
{
    walk(Group::Snorm);
}

// What each path a signed normalized render target reaches does with it, now that its textures are raw's: the 3D engine
// renders and blends it (its row's target is NIL's: raw_tic_words_test; the blend kill takes its texels as finite); a
// private one binds the compressed kind of its texel size as the unorm formats of that size do (NIL's
// nvc0_choose_pte_kind picks colour kinds by element size alone), and only an all-zero clear writes ZBC tags; a turn
// copy moves its texels as unsigned integers of their size; a resolve has no pack state, so a pass that resolves one is
// refused (pass.level), never averaged as unorm.
TEST_F(DeviceShapes, SignedNormalizedTargetsReachTheirConsumers)
{
    const std::pair<uint32_t, uint32_t> snormAndUnorm[] = {{12, 10}, {22, 20}, {32, 30},
                                                           {62, 60}, {72, 70}, {112, 110}};
    for (const auto &[snorm, unorm] : snormAndUnorm) {
        const raw::FormatInfo *f = raw::formatInfo(snorm), *u = raw::formatInfo(unorm);
        ASSERT_TRUE(f && u) << snorm;
        EXPECT_NE(f->rt, 0u) << snorm;
        EXPECT_EQ(f->bpp, u->bpp) << snorm;
        EXPECT_TRUE(raw::normalizedFormat(snorm)) << snorm;
        raw::PrivateTextureFacts facts;
        facts.privateStorage = facts.compressibleUsage = true;
        facts.shape = raw::PrivateShape2D::Plain;
        facts.compression = true;
        facts.plainColour = true;
        facts.bpp = f->bpp;
        const uint32_t kind = raw::privateCompressedKind(facts);
        facts.bpp = u->bpp;
        EXPECT_EQ(kind, raw::privateCompressedKind(facts)) << snorm;
        EXPECT_EQ(kind, f->bpp == 4 ? raw::kCompKindC32 : f->bpp == 8 ? raw::kCompKindC64 : 0u) << snorm;
        const uint32_t zero[4] = {}, half[4] = {0x3f000000, 0xbf000000, 0, 0x3f800000};
        EXPECT_TRUE(raw::zbcBootColor(snorm, zero)) << snorm;
        EXPECT_FALSE(raw::zbcBootColor(snorm, half)) << snorm;
        TegraGM20BAbiZbc entry;
        EXPECT_FALSE(raw::zbcColorEntry(snorm, half, entry)) << snorm;
        EXPECT_NE(raw::turnCopyState(f->bpp), nullptr) << snorm;
        for (uint32_t samples : {2u, 4u})
            EXPECT_EQ(raw::resolveState(snorm, samples), nullptr) << snorm << " " << samples;
        raw::ResolveAsk ask;
        ask.format = uint16_t(snorm);
        ask.samples = 4;
        ask.wholeTarget = true;
        EXPECT_FALSE(raw::twodResolveShape(ask)) << snorm;
    }
}

TEST_F(DeviceShapes, FormatsGM20BRendersNone)
{
    walk(Group::Unrendered);
}

TEST_F(DeviceShapes, FourTwoTwoFormats)
{
    walk(Group::FourTwoTwo);
}

TEST_F(DeviceShapes, ExtendedRangeFormats)
{
    walk(Group::Extended);
}

// Every texel's address in `t` (its level, layer, plane or sample, row and column in that order), by fx's own layouts
std::vector<uint64_t> texelAddresses(const raw::Texture &t)
{
    std::vector<uint64_t> out;
    raw::fx::MipChain c;
    if (t.samples > 1) {
        EXPECT_TRUE(raw::fx::msLayout(t.width, t.height, t.samples, t.bpp, c));
        const uint32_t cellW = 1u << c.sxLog2, cellH = 1u << c.syLog2;
        for (uint32_t y = 0; y < t.height * cellH; y++)
            for (uint32_t x = 0; x < t.width * cellW; x++)
                out.push_back(t.va + raw::fx::texelOffset(c, 0, x * t.bpp, y));
        return out;
    }
    uint64_t base = 0;
    EXPECT_TRUE(raw::attachmentImage(t, c, base));
    for (uint32_t layer = 0; layer < c.layers; layer++)
        for (uint32_t l = 0; l < c.levels; l++) {
            const raw::fx::MipLevel &lv = c.level[l];
            for (uint32_t z = 0; z < (t.type == raw::fx::kTexThreeD ? lv.depth : 1u); z++)
                for (uint32_t y = 0; y < lv.height; y++)
                    for (uint32_t x = 0; x < lv.width; x++)
                        out.push_back(base + layer * c.arrayStride +
                                      (t.type == raw::fx::kTexThreeD ? raw::fx::volumeOffset(c, l, x * t.bpp, y, z)
                                                                     : raw::fx::texelOffset(c, l, x * t.bpp, y)));
        }
    return out;
}

// An extended-range texture of every layout but one-level 2D converts into its stand-in (raw_xr_parts.h) through pairs
// of 2D images that put each texel of the codes and the same texel of the stand-in at one place in both: every texel of
// the codes is in a part, and the part's texel there is the stand-in's texel of the same level, layer, plane or sample
TEST_F(DeviceShapes, ExtendedRangeStandInsConvertTexelForTexel)
{
    struct Case {
        uint32_t format, type, storage, width, height, depth, levels, samples, array;
        uint64_t usage;
    };

    const Case cases[] = {
        {554, raw::kType2D, raw::shapes::kPrivate, 37, 21, 1, 3, 1, 1, 1},
        {552, raw::kType2DArray, raw::shapes::kShared, 19, 11, 1, 1, 1, 3, 1},
        {550, raw::kType2DArray, raw::shapes::kPrivate, 19, 300, 1, 1, 1, 70, 1},
        {554, raw::kType2DArray, raw::shapes::kPrivate, 40, 24, 1, 3, 1, 2, 1},
        {552, raw::kTypeCube, raw::shapes::kPrivate, 16, 16, 1, 2, 1, 1, 5},
        {554, raw::kTypeCubeArray, raw::shapes::kPrivate, 8, 8, 1, 1, 1, 2, 1},
        {552, raw::kType3D, raw::shapes::kPrivate, 20, 12, 5, 3, 1, 1, 5},
        {554, raw::kType3D, raw::shapes::kShared, 20, 12, 13, 2, 1, 1, 1},
        {550, raw::kType3D, raw::shapes::kPrivate, 9, 70, 300, 1, 1, 1, 1},
        {554, raw::kType2DMultisample, raw::shapes::kPrivate, 13, 9, 1, 1, 4, 1, 5},
        {552, raw::kType2DMultisample, raw::shapes::kShared, 13, 9, 1, 1, 2, 1, 5},
        {550, raw::kType1D, raw::shapes::kShared, 70, 1, 1, 1, 1, 1, 1},
        {554, raw::kType1DArray, raw::shapes::kPrivate, 70, 1, 1, 1, 1, 4, 1},
    };
    for (const Case &k : cases) {
        TextureShape s;
        s.format = k.format;
        s.type = k.type;
        s.storage = k.storage;
        s.width = k.width;
        s.height = k.height;
        s.depth = k.depth;
        s.levels = k.levels;
        s.samples = k.samples;
        s.array = k.array;
        s.usage = k.usage;
        s.ownMemory = true;
        const std::string what = describe(s);
        const raw::shapes::Route route = raw::shapes::route(s);
        raw::Texture codes, standIn;
        ASSERT_TRUE(route != raw::shapes::Route::None && raw::shapes::create(dev, s, route, true, 0, codes)) << what;
        ASSERT_TRUE(raw::DeviceHarness::standIn(dev, codes, standIn)) << what;
        std::vector<raw::XrPart> parts;
        ASSERT_TRUE(raw::xrParts(codes, standIn, parts)) << what;
        std::unordered_map<uint64_t, uint64_t> pairs;
        for (const raw::XrPart &part : parts) {
            EXPECT_LE(part.codes.height, 16384u) << what;
            for (uint32_t y = 0; y < part.codes.height; y++)
                for (uint32_t x = 0; x < part.codes.width; x++)
                    pairs[part.codes.va +
                          tgm_layout_offset_of(part.codes.pitch, part.codes.blockHeight, x * part.codes.bpp, y)] =
                        part.standIn.va +
                        tgm_layout_offset_of(part.standIn.pitch, part.standIn.blockHeight, x * part.standIn.bpp, y);
        }
        const std::vector<uint64_t> from = texelAddresses(codes), to = texelAddresses(standIn);
        ASSERT_EQ(from.size(), to.size()) << what;
        uint32_t missed = 0, wrong = 0;
        for (size_t i = 0; i < from.size(); i++) {
            const auto at = pairs.find(from[i]);
            missed += at == pairs.end();
            wrong += at != pairs.end() && at->second != to[i];
        }
        EXPECT_EQ(missed, 0u) << what << ": " << from.size() << " texels";
        EXPECT_EQ(wrong, 0u) << what << ": " << from.size() << " texels";
        dev.destroyTexture(standIn);
        dev.destroyTexture(codes);
    }
}

// A pass into a level and slice of an extended-range array renders its codes there (attachmentTarget), as a pass into a
// whole extended-range texture does; the encoder draws the stand-in again before the next draw samples it
TEST_F(DeviceShapes, APassIntoAnExtendedRangeLevelAndSliceRendersItsCodes)
{
    TextureShape s;
    s.format = 554;
    s.type = raw::kType2DArray;
    s.storage = raw::shapes::kPrivate;
    s.width = s.height = 32;
    s.levels = 2;
    s.array = 3;
    s.usage = raw::shapes::kUsageRead | raw::shapes::kUsageTarget;
    raw::Texture codes, standIn, part;
    ASSERT_TRUE(raw::shapes::create(dev, s, raw::shapes::route(s), true, 0, codes));
    ASSERT_TRUE(raw::DeviceHarness::standIn(dev, codes, standIn));
    codes.standIn = &standIn;
    ASSERT_TRUE(raw::attachmentTarget(codes, 1, 2, part));
    const raw::fx::MipChain &c = codes.chain->layout;
    EXPECT_EQ(part.va, codes.va + 2 * c.arrayStride + c.level[1].offset);
    EXPECT_EQ(part.format, 554u);
    raw::PassDesc p;
    p.color[0].texture = &part;
    p.color[0].load = raw::ColorAttachment::Clear;
    p.count = 1;
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    raw::RenderEncoder *e = cb->render(p);
    ASSERT_NE(e, nullptr);
    e->end();
    uint32_t seqno = 0;
    EXPECT_TRUE(cb->commit(seqno));
    codes.standIn = nullptr;
    dev.destroyTexture(standIn);
    dev.destroyTexture(codes);
}

// A conversion's image of every face of a one-level cube and a pass's face 0 keep a header each: asked in either order,
// the face's header is one face tall and the conversion's six
TEST_F(DeviceShapes, ExtendedRangeConversionHeadersAreNotAPassPartsHeaders)
{
    TextureShape s;
    s.format = 554;
    s.type = raw::kTypeCube;
    s.storage = raw::shapes::kPrivate;
    s.width = s.height = 16;
    s.usage = raw::shapes::kUsageRead | raw::shapes::kUsageTarget;
    raw::Texture codes, standIn, face;
    ASSERT_TRUE(raw::shapes::create(dev, s, raw::shapes::route(s), true, 0, codes));
    ASSERT_TRUE(raw::DeviceHarness::standIn(dev, codes, standIn));
    ASSERT_TRUE(raw::attachmentTarget(codes, 0, 0, face));
    const uint32_t faceTic = dev.attachmentTic(codes, face, 0, 0);
    std::vector<raw::XrPart> parts;
    ASSERT_TRUE(raw::xrParts(codes, standIn, parts));
    ASSERT_EQ(parts.size(), 1u);
    const uint32_t partTic = dev.attachmentTic(codes, parts[0].codes, parts[0].level, parts[0].key);
    ASSERT_TRUE(faceTic && partTic);
    EXPECT_NE(faceTic, partTic);
    auto height = [&](uint32_t tic) {
        return (raw::DeviceHarness::header(dev, tic)[5] & 0xffff) + 1;  // HEIGHT_MINUS_ONE, bits 175:160
    };
    EXPECT_EQ(height(faceTic), 16u);
    EXPECT_EQ(height(partTic), 6 * 16u);
    dev.destroyTexture(standIn);
    dev.destroyTexture(codes);
}

// A shared RGB10A8_2P_XR10 texture is the raw path's in memory of its own alone: over a buffer's or a surface's bytes
// (Metal's colour words) the shape is refused, as every shared one was before
TEST_F(DeviceShapes, SharedTwoPlaneExtendedRangeIsOwnMemorysAlone)
{
    TextureShape s;
    s.format = 550;
    s.storage = raw::shapes::kShared;
    s.width = s.height = 8;
    EXPECT_FALSE(raw::shapes::rawShape(s));
    s.ownMemory = true;
    EXPECT_TRUE(raw::shapes::rawShape(s));
    s.storage = raw::shapes::kPrivate;
    s.ownMemory = false;
    EXPECT_TRUE(raw::shapes::rawShape(s));
}

// A mipmapped depth/stencil array's zeta memory is NIL's image of it: the level offsets, row pitches and array pitch
// NVK answers for a VK_IMAGE_TYPE_2D optimal image of 6 layers and 4 levels at 100x60 (m2vf-shapes2 probe/nil/nilzeta
// over x1-allon-key's NVK host twin, the depth aspect's vkGetImageSubresourceLayout), its header an array of every
// level in the format's zeta row, a pass's target its level 0 at that array pitch; and a cube array's header names its
// cubes
TEST_F(DeviceShapes, StoredDepthChainsAndCubeArraysAreNilsImages)
{
    struct Nil {
        uint32_t format;
        uint64_t arrayPitch, offset[4], rowPitch[4];
    };

    const Nil nils[] = {
        {250, 24576, {0, 16384, 20480, 21504}, {256, 128, 64, 64}},
        {252, 40960, {0, 28672, 36864, 38912}, {448, 256, 128, 64}},
        {253, 12288, {0, 8192, 10240, 11264}, {128, 64, 64, 64}},
        {260, 73728, {0, 53248, 67584, 71680}, {832, 448, 256, 128}},
    };
    auto field = [](const std::array<uint32_t, 8> &w, uint32_t hi, uint32_t lo) {
        return uint32_t((((uint64_t(w[hi / 32]) << 32) | w[lo / 32]) >> (lo % 32)) & ((1ull << (hi - lo + 1)) - 1));
    };
    for (const Nil &n : nils) {
        TextureShape s;
        s.format = n.format;
        s.type = raw::kType2DArray;
        s.storage = raw::shapes::kPrivate;
        s.width = 100;
        s.height = 60;
        s.levels = 4;
        s.array = 6;
        s.usage = raw::shapes::kUsageRead;
        s.ownMemory = true;
        ASSERT_EQ(raw::shapes::route(s), raw::shapes::Route::StoredZeta) << n.format;
        raw::Texture t;
        ASSERT_TRUE(raw::shapes::create(dev, s, raw::shapes::Route::StoredZeta, true, 0, t)) << n.format;
        ASSERT_NE(t.chain, nullptr) << n.format;
        const raw::fx::MipChain &c = t.chain->layout;
        EXPECT_EQ(c.arrayStride, n.arrayPitch) << n.format;
        EXPECT_EQ(t.layerBytes, n.arrayPitch) << n.format;
        for (uint32_t l = 0; l < 4; l++) {
            EXPECT_EQ(c.level[l].offset, n.offset[l]) << n.format << " level " << l;
            EXPECT_EQ(c.level[l].rowBytes, n.rowPitch[l]) << n.format << " level " << l;
        }
        const std::array<uint32_t, 8> w = raw::DeviceHarness::header(dev, t.tic);
        EXPECT_EQ(w[0], raw::kZetaFormatInfo[raw::zetaFormatIndex(n.format)].tic0) << n.format;
        EXPECT_EQ(field(w, 154, 151), raw::fx::kTexTwoDArray) << n.format;
        EXPECT_EQ(field(w, 127, 124), 3u) << n.format;
        EXPECT_EQ(field(w, 189, 176), 5u) << n.format;
        const raw::ZetaTargetValues v = raw::zetaTargetValues(t, 60);
        EXPECT_EQ(v.a[4], uint32_t(n.arrayPitch >> 2)) << n.format;
        EXPECT_EQ(v.size[0], n.rowPitch[0] / t.bpp) << n.format;
        dev.destroyTexture(t);
    }
    TextureShape s;
    s.format = 252;
    s.type = raw::kTypeCubeArray;
    s.storage = raw::shapes::kPrivate;
    s.width = s.height = 32;
    s.array = 2;
    s.usage = raw::shapes::kUsageRead | raw::shapes::kUsageTarget;
    s.ownMemory = true;
    raw::Texture cubes;
    ASSERT_TRUE(raw::shapes::create(dev, s, raw::shapes::route(s), true, 0, cubes));
    EXPECT_EQ(cubes.layers, 12u);
    const std::array<uint32_t, 8> w = raw::DeviceHarness::header(dev, cubes.tic);
    EXPECT_EQ(field(w, 154, 151), raw::fx::kTexCubeArray);
    EXPECT_EQ(field(w, 189, 176), 1u);
    dev.destroyTexture(cubes);
}

// The copy engine's SET_REMAP_COMPONENTS and source and destination of each launch the jobs from `first` on submitted
std::vector<std::array<uint64_t, 3>> copyLaunches(size_t first)
{
    constexpr uint32_t kLaunch = 0x0300, kInUpper = 0x0400, kInLower = 0x0404, kOutUpper = 0x0408, kOutLower = 0x040c,
                       kRemap = 0x0708;
    std::vector<std::array<uint64_t, 3>> out;
    uint64_t in = 0, to = 0, remap = 0;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++) {
        uint32_t op = 0, subc = 0, m = 0, left = 0;
        auto method = [&](uint32_t v) {
            if (subc != raw::SUBC_COPY)
                return;
            if (m == kInUpper || m == kInLower)
                in = m == kInUpper ? (in & 0xffffffffull) | uint64_t(v) << 32 : (in & ~0xffffffffull) | v;
            else if (m == kOutUpper || m == kOutLower)
                to = m == kOutUpper ? (to & 0xffffffffull) | uint64_t(v) << 32 : (to & ~0xffffffffull) | v;
            else if (m == kRemap)
                remap = v;
            else if (m == kLaunch)
                out.push_back({in, to, remap});
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
    return out;
}

// An RG8Unorm_sRGB texture is sampled through its RGBA8Unorm_sRGB stand-in of its shape, read red, green, zero, one
// (none: no header a draw reads), which the copy engine widens from its bytes a 2D image at a time: a one-level array's
// layers in one copy of its red and green bytes into the stand-in's first two (blue and alpha NO_WRITE), a mipmapped
// one's levels one copy each
TEST_F(DeviceShapes, SrgbRedGreenSamplesAWidenedStandIn)
{
    TextureShape s;
    s.format = raw::kRG8Srgb;
    s.type = raw::kType2DArray;
    s.storage = raw::shapes::kShared;
    s.width = 20;
    s.height = 12;
    s.array = 3;
    s.usage = raw::shapes::kUsageRead;
    s.ownMemory = true;
    raw::Texture t;
    ASSERT_TRUE(raw::shapes::create(dev, s, raw::shapes::route(s), false, 0, t));
    EXPECT_FALSE(raw::drawsBound(t));
    ASSERT_TRUE(dev.createStandIn(t));
    ASSERT_NE(t.standIn, nullptr);
    EXPECT_TRUE(raw::drawsBound(t));
    EXPECT_EQ(t.standIn->format, raw::kSrgbPairStandInFormat);
    EXPECT_EQ(t.standIn->layers, 3u);
    EXPECT_EQ(memcmp(t.standIn->swizzle, raw::kSrgbPairSwizzle, 4), 0);
    const size_t first = kernel_model::model.jobs.size();
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    raw::BlitEncoder *b = cb->blit();
    EXPECT_TRUE(b->widenSrgbPair(t));
    b->end();
    uint32_t seqno = 0;
    ASSERT_TRUE(cb->commit(seqno));
    const auto launched = copyLaunches(first);
    ASSERT_EQ(launched.size(), 1u);
    EXPECT_EQ(launched[0][0], t.va);
    EXPECT_EQ(launched[0][1], t.standIn->va);
    EXPECT_EQ(launched[0][2], 0x03106610u);  // DST_X SRC_X, DST_Y SRC_Y, Z and W NO_WRITE; 1-byte, 2 in, 4 out
    dev.destroyTexture(t);

    s.type = raw::kType2D;
    s.array = 1;
    s.levels = 3;
    s.storage = raw::shapes::kPrivate;
    ASSERT_TRUE(raw::shapes::create(dev, s, raw::shapes::route(s), true, 0, t));
    ASSERT_TRUE(dev.createStandIn(t));
    const size_t next = kernel_model::model.jobs.size();
    cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    b = cb->blit();
    EXPECT_TRUE(b->widenSrgbPair(t));
    b->end();
    ASSERT_TRUE(cb->commit(seqno));
    EXPECT_EQ(copyLaunches(next).size(), 3u);
    raw::Texture view;
    EXPECT_FALSE(dev.createView(t, raw::kRG8Srgb, view)) << "a view would read its bytes without the stand-in";
    EXPECT_FALSE(dev.createView(t, 30, view)) << "a view would read its bytes without the stand-in";
    dev.destroyTexture(t);

    // a shared 2D one's pitch bytes, widened in one copy; a descriptor's swizzle (alpha, red, green, blue) lands on its
    // stand-in's red, green, zero, one: one, red, green, zero
    s.levels = 1;
    s.storage = raw::shapes::kShared;
    ASSERT_TRUE(raw::shapes::create(dev, s, raw::shapes::route(s), false, 0, t));
    ASSERT_FALSE(t.blockLinear);
    ASSERT_TRUE(dev.createStandIn(t));
    const uint8_t swizzle[4] = {5, 2, 3, 4};
    dev.swizzle(t, swizzle);
    const auto sources = [&](uint32_t tic) {
        const uint32_t w = raw::DeviceHarness::header(dev, tic)[0];
        return std::array<uint32_t, 4>{w >> 19 & 7, w >> 22 & 7, w >> 25 & 7, w >> 28 & 7};
    };
    const std::array<uint32_t, 4> standIn = sources(t.standIn->tic);
    EXPECT_EQ(standIn[0], 7u);  // ONE_FLOAT
    EXPECT_EQ(standIn[1], 2u);  // R
    EXPECT_EQ(standIn[2], 3u);  // G
    EXPECT_EQ(standIn[3], 0u);  // ZERO
    const size_t pitched = kernel_model::model.jobs.size();
    cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    b = cb->blit();
    EXPECT_TRUE(b->widenSrgbPair(t));
    b->end();
    ASSERT_TRUE(cb->commit(seqno));
    const auto widened = copyLaunches(pitched);
    ASSERT_EQ(widened.size(), 1u);
    EXPECT_EQ(widened[0][0], t.va);
    EXPECT_EQ(widened[0][1], t.standIn->va);
    dev.destroyTexture(t);
}

// A shared depth/stencil texture's CPU copies are copy-engine copies between its zeta image and a pitch staging buffer
// (Device::transferTexture): each format's remap word (raw_zeta_cpu.h; Depth32Float_Stencil8's depth aspect for a
// chain's level too, which is no zeta header of its own), at the base of the layer or level the copy names, the rows of
// a multisampled one's sample grid
TEST_F(DeviceShapes, SharedZetaCopiesNameTheirAspectAndPlace)
{
    struct Case {
        uint32_t format, type, levels, samples, array, level, slice;
    };

    const Case cases[] = {
        {250, raw::kType2D, 1, 1, 1, 0, 0},      {253, raw::kType2D, 1, 1, 1, 0, 0},
        {252, raw::kType2DArray, 1, 1, 3, 0, 2}, {260, raw::kType2DArray, 3, 1, 2, 2, 1},
        {260, raw::kType2D, 1, 1, 1, 0, 0},      {252, raw::kType2DMultisample, 1, 4, 1, 0, 0},
    };
    for (const Case &k : cases) {
        TextureShape s;
        s.format = k.format;
        s.type = k.type;
        s.storage = raw::shapes::kShared;
        s.width = 16;
        s.height = 8;
        s.levels = k.levels;
        s.samples = k.samples;
        s.array = k.array;
        s.usage = raw::shapes::kUsageRead | raw::shapes::kUsageTarget;
        s.ownMemory = true;
        raw::Texture t;
        ASSERT_TRUE(raw::shapes::create(dev, s, raw::shapes::route(s), false, 0, t)) << describe(s);
        raw::ZetaCpuCopy copy;
        ASSERT_TRUE(raw::zetaCpuCopy(k.format, copy));
        const uint32_t w = std::max(1u, 16u >> k.level), h = std::max(1u, 8u >> k.level);
        std::vector<uint8_t> bytes(size_t(w) * h * k.samples * copy.bytes, 0x5c);
        const uint32_t origin[3] = {0, 0, 0}, size[3] = {w, h, 1};
        const size_t first = kernel_model::model.jobs.size();
        ASSERT_EQ(dev.transferTexture(t, k.level, k.slice, origin, size, bytes.data(), w * k.samples * copy.bytes, 0,
                                      true, nullptr),
                  raw::TextureTransferResult::Complete)
            << describe(s);
        const auto launched = copyLaunches(first);
        ASSERT_EQ(launched.size(), 1u) << describe(s);
        EXPECT_EQ(launched[0][2], copy.remapIn) << describe(s);
        const uint64_t base =
            t.chain ? t.chain->va + k.slice * t.chain->layout.arrayStride + t.chain->layout.level[k.level].offset
                    : t.va + k.slice * t.layerBytes;
        EXPECT_EQ(launched[0][1], base) << describe(s);
        dev.destroyTexture(t);
    }
}

TEST_F(DeviceShapes, BlockCompressedFormats)
{
    walk(Group::Blocks);
}

// A BC1 3D texture's header reads its planes of blocks as a 3D image: TEXTURE_TYPE THREE_D, its size in texels and
// planes, GOBS_PER_BLOCK_DEPTH and the levels of NIL's image (fx::volumeLayout of 4x4 blocks), which its memory spans
TEST_F(DeviceShapes, BlockVolumesHeadTheirPlanes)
{
    TextureShape s;
    s.type = raw::kType3D;
    s.format = 130;
    s.storage = raw::shapes::kShared;
    s.width = 100;
    s.height = 60;
    s.depth = 7;
    s.levels = 4;
    ASSERT_EQ(raw::shapes::route(s), raw::shapes::Route::Block);
    raw::Texture t;
    ASSERT_TRUE(raw::shapes::create(dev, s, raw::shapes::Route::Block, false, 0, t));
    raw::fx::MipChain c;
    ASSERT_TRUE(raw::fx::volumeLayout(100, 60, 7, 4, 8, false, c, 4, 4));
    const std::array<uint32_t, 8> w = raw::DeviceHarness::header(dev, t.tic);
    auto field = [&](uint32_t hi, uint32_t lo) {
        return uint32_t((((uint64_t(w[hi / 32]) << 32) | w[lo / 32]) >> (lo % 32)) & ((1ull << (hi - lo + 1)) - 1));
    };
    EXPECT_EQ(field(154, 151), raw::fx::kTexThreeD);
    EXPECT_EQ(field(143, 128), 99u);
    EXPECT_EQ(field(175, 160), 59u);
    EXPECT_EQ(field(189, 176), 6u);
    EXPECT_EQ(field(104, 102), c.zLog2);
    EXPECT_EQ(field(127, 124), 3u);
    EXPECT_EQ(t.depth, 7u);
    EXPECT_EQ(t.layerBytes, c.bytes);
    EXPECT_GE(t.span.bytes, c.bytes);
    dev.destroyTexture(t);
}

TEST_F(DeviceShapes, AstcHdrFormats)
{
    walk(Group::AstcHdr);
}

TEST_F(DeviceShapes, SrgbRedGreenFormat)
{
    walk(Group::SrgbPair);
}

TEST_F(DeviceShapes, DepthFormats)
{
    walk(Group::Depth);
}

TEST_F(DeviceShapes, SharedMultisampled)
{
    walk(Group::SharedMultisample);
}

TEST_F(DeviceShapes, Memoryless)
{
    walk(Group::Memoryless);
}

// generateMipmapsForTexture: draws a 3D texture's levels a plane a pass, so a mipmapped one only sampled is laid out
// one plane a block as a rendered one is
TEST(VolumeShapes, AMipmappedVolumeIsLaidOutAsARenderedOne)
{
    TextureShape s;
    s.type = raw::kType3D;
    s.format = 70;  // RGBA8Unorm
    s.storage = raw::shapes::kPrivate;
    s.usage = raw::shapes::kUsageRead;
    s.width = s.height = s.depth = 16;
    EXPECT_FALSE(raw::shapes::volumeRendered(s));
    s.levels = 5;
    EXPECT_TRUE(raw::shapes::volumeRendered(s));
    EXPECT_TRUE(raw::shapes::volumeView2d(s));
}

}  // namespace
