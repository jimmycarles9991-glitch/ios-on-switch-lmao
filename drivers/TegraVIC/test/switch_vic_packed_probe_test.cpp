// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "../SwitchVICPackedProbe.h"

namespace {
using namespace switch_vic_packed_probe;

// How a model engine treats packed rows: the bytes of LumaWidth + 1 elements a row, and whether it reads and writes
// each packed format in the other's byte order; or, with `code`, the byte roles of the VIC format the job programs.
struct Engine {
    uint32_t elementBytes;
    bool otherOrder;
    uint32_t code;
};

// The packed codes' bytes as boot 21's L1000 read them: U8_Y8__V8_Y8 (59) takes byte 0 as Cr and byte 2 as Cb
// ('2vuy' roles 1203), Y8_U8__Y8_V8 (57) byte 1 as Cr and byte 3 as Cb ('yuvs' 2130); 60 and 58 are their names with U
// and V exchanged.
Role codeRole(uint32_t code, uint32_t p)
{
    static constexpr Role k57[4] = {kLumaEven, kCr, kLumaOdd, kCb}, k58[4] = {kLumaEven, kCb, kLumaOdd, kCr};
    static constexpr Role k59[4] = {kCr, kLumaEven, kCb, kLumaOdd}, k60[4] = {kCb, kLumaEven, kCr, kLumaOdd};
    switch (code) {
    case TEGRA_VIC_T_Y8_U8__Y8_V8:
        return k57[p];
    case TEGRA_VIC_T_Y8_V8__Y8_U8:
        return k58[p];
    case TEGRA_VIC_T_U8_Y8__V8_Y8:
        return k59[p];
    default:
        return k60[p];
    }
}

Role engineRole(uint32_t fourcc, uint32_t p, const Engine &e)
{
    if (e.code)
        return codeRole(e.code, p);
    return role(e.otherOrder ? (fourcc == k2vuy ? kYuvs : k2vuy) : fourcc, p);
}

// The VIC format the built job programs on the case's packed side.
uint32_t packedCode(const Case &c)
{
    TegraVICJob job;
    EXPECT_EQ(build(c, 0x40000000, 0x40100000, 0x1000, 0x2000, &job), nullptr);
    const uint32_t surface = packed(c.source) ? TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE : TEGRA_VIC_OUTPUT_SURFACE;
    return uint32_t(TEGRA_VIC_GET(job.config, surface, SURF_FORMAT));
}

// A copy through the pipeline: each pixel's luma and its pair's chroma read, then written as the destination lays
// them out (4:2:0 chroma from the top-left pixel of each 2x2), or converted to BGRA.
std::vector<uint8_t> run(const Case &c, const Engine &e)
{
    std::vector<uint8_t> source(kBufferBytes), destination(kBufferBytes, kPoison);
    fillSource(source.data(), c);
    const Layout s = layout(c.source, false), d = layout(c.destination, true);
    const auto stride = [&](const Plane &plane, uint32_t fourcc) {
        return packed(fourcc) ? uint64_t(plane.pitch) / c.elementBytes * e.elementBytes : uint64_t(plane.pitch);
    };
    const auto put = [&](uint64_t at, uint32_t value) {
        if (at < kBufferBytes)
            destination[at] = uint8_t(value);
    };
    for (uint32_t y = 0; y < kHeight; y++) {
        for (uint32_t x = 0; x < kWidth; x++) {
            uint32_t luma = 0, cb = 0, cr = 0;
            if (packed(c.source)) {
                const uint64_t pair = s.plane[0].offset + y * stride(s.plane[0], c.source) + 4 * (x / 2);
                for (uint32_t p = 0; p < 4; p++) {
                    const Role r = engineRole(c.source, p, e);
                    const uint32_t v = pair + p < kBufferBytes ? source[pair + p] : 0;
                    if (r == Role(x % 2))
                        luma = v;
                    cb = r == kCb ? v : cb;
                    cr = r == kCr ? v : cr;
                }
            } else {
                luma = source[s.plane[0].offset + uint64_t(y) * s.plane[0].pitch + x];
                const uint64_t chroma = s.plane[1].offset + uint64_t(y / 2) * s.plane[1].pitch + 2 * (x / 2);
                cb = source[chroma];
                cr = source[chroma + 1];
            }
            if (c.destination == kNV12) {
                put(d.plane[0].offset + uint64_t(y) * d.plane[0].pitch + x, luma);
                if (x % 2 == 0 && y % 2 == 0) {
                    const uint64_t chroma = d.plane[1].offset + uint64_t(y / 2) * d.plane[1].pitch + x;
                    put(chroma, cb);
                    put(chroma + 1, cr);
                }
            } else if (c.destination == kBGRA) {
                const uint32_t pixel = rgbOf(luma, cb, cr);
                for (uint32_t b = 0; b < 4; b++)
                    put(d.plane[0].offset + uint64_t(y) * d.plane[0].pitch + 4 * x + b, pixel >> (8 * b));
            } else {
                const uint64_t pair = d.plane[0].offset + y * stride(d.plane[0], c.destination) + 4 * (x / 2);
                for (uint32_t p = 0; p < 4; p++) {
                    const Role r = engineRole(c.destination, p, e);
                    if (r == Role(x % 2))
                        put(pair + p, luma);
                    else if (x % 2 == 0 && (r == kCb || r == kCr))
                        put(pair + p, r == kCb ? cb : cr);
                }
            }
        }
    }
    return destination;
}

SwitchVICPackedResult judge(const Case &c, const Engine &e)
{
    SwitchVICPackedResult r = {};
    compare(run(c, e).data(), c, &r);
    return r;
}

std::vector<uint32_t> cleanCases(const Engine &e)
{
    std::vector<uint32_t> found;
    for (uint32_t i = 0; i < SWITCH_VIC_PACKED_PROBE_CASES; i++) {
        if (clean(judge(kCases[i], e)))
            found.push_back(i);
    }
    return found;
}
}  // namespace

TEST(SwitchVICPackedProbe, EveryCaseBuildsWithPackedAllowedAndItsRowReading)
{
    for (const Case &c : kCases) {
        TegraVICJob job;
        ASSERT_EQ(build(c, 0x40000000, 0x40100000, 0x1000, 0x2000, &job), nullptr);
        const uint32_t pitch = layout(packed(c.source) ? c.source : c.destination, !packed(c.source)).plane[0].pitch;
        const uint32_t base = packed(c.source) ? TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE : TEGRA_VIC_OUTPUT_SURFACE;
        if (packed(c.source) || packed(c.destination)) {
            EXPECT_EQ(TEGRA_VIC_GET(job.config, base, SURF_LUMA_WIDTH) + 1, pitch / c.elementBytes);
        }
    }
    EXPECT_EQ(packedCode(kCases[1]), uint32_t(TEGRA_VIC_T_V8_Y8__U8_Y8));
    EXPECT_EQ(packedCode(kCases[3]), uint32_t(TEGRA_VIC_T_Y8_V8__Y8_U8));
    EXPECT_EQ(packedCode(kCases[5]), uint32_t(TEGRA_VIC_T_V8_Y8__U8_Y8));
    EXPECT_EQ(packedCode(kCases[7]), uint32_t(TEGRA_VIC_T_Y8_V8__Y8_U8));
}

// Boot 21's per-case readings come back on the engine under the codes it ran: Cb and Cr exchanged against NV12 and
// BGRA (512 and 1024 bytes, every BGRA pixel, max 250 and 228), each format exact onto itself.
TEST(SwitchVICPackedProbe, TheCodeEngineReadsAsBoot21UnderTheCodesItRan)
{
    const auto under = [](uint32_t i) {
        const Case &c = kCases[i];
        const uint32_t fourcc = packed(c.source) ? c.source : c.destination;
        return judge(c, {2, false, fourcc == k2vuy ? uint32_t(TEGRA_VIC_T_U8_Y8__V8_Y8) : TEGRA_VIC_T_Y8_U8__Y8_V8});
    };

    const struct {
        uint32_t index, roles, wrong;
    } boot21[] = {{1, 0x1203, 512},  {3, 0x2130, 512}, {5, 0x1203, 1024},
                  {7, 0x2130, 1024}, {9, 0x3210, 0},   {11, 0x3210, 0}};

    for (const auto &b : boot21) {
        const SwitchVICPackedResult r = under(b.index);
        EXPECT_EQ(r.roleMap, b.roles) << b.index;
        EXPECT_EQ(r.mismatches, b.wrong) << b.index;
    }
    EXPECT_EQ(under(13).mismatches, 1024u);
    EXPECT_EQ(under(13).maxError, 250u);
    EXPECT_EQ(under(14).mismatches, 1024u);
    EXPECT_EQ(under(14).maxError, 228u);
}

TEST(SwitchVICPackedProbe, TheShippedRowsAreCleanOnTheCodeEngine)
{
    for (uint32_t i = 0; i < SWITCH_VIC_PACKED_PROBE_CASES; i++) {
        const Case &c = kCases[i];
        if (!packed(c.source) && !packed(c.destination))
            continue;
        const SwitchVICPackedResult r = judge(c, {2, false, packedCode(c)});
        EXPECT_EQ(clean(r), c.elementBytes == 2)
            << i << ": " << r.mismatches << " wrong, roles " << std::hex << r.roleMap;
        if (c.elementBytes == 2)
            EXPECT_EQ(r.roleMap, expectedRoles(c)) << i;
    }
}

TEST(SwitchVICPackedProbe, SourceBytesNameTheirPlace)
{
    for (const uint32_t fourcc : {k2vuy, kYuvs}) {
        for (uint32_t x = 0; x < 2 * kWidth; x++)
            EXPECT_EQ(sourceByte(fourcc, 0, x, 3) >> 6, x % 4);
    }
    for (uint32_t x = 0; x < kWidth; x++) {
        EXPECT_EQ(sourceByte(kNV12, 0, x, 5) >> 6, x % 2);
        EXPECT_EQ(sourceByte(kNV12, 1, x, 5) >> 6, kCb + x % 2);
    }
}

TEST(SwitchVICPackedProbe, AnEngineAsTheFormatTableSaysPassesEveryPixelAnElementCase)
{
    const Engine e = {2, false, 0};
    for (const Case &c : kCases) {
        const SwitchVICPackedResult r = judge(c, e);
        EXPECT_EQ(clean(r), c.elementBytes == 2 || !(packed(c.source) || packed(c.destination)))
            << c.source << " -> " << c.destination << " / " << c.elementBytes << ": " << r.mismatches << " wrong, "
            << r.outsideWritten << " outside";
        if (c.elementBytes == 2) {
            EXPECT_EQ(r.roleMap, expectedRoles(c)) << c.source << " -> " << c.destination;
        }
    }
    EXPECT_EQ(judge(kCases[13], e).maxError, 0u);
}

TEST(SwitchVICPackedProbe, AnEngineCountingPairsPassesThePairCasesInstead)
{
    const std::vector<uint32_t> found = cleanCases({4, false, 0});
    EXPECT_EQ(found, (std::vector<uint32_t>{0, 2, 4, 6, 8, 10, 12}));
    EXPECT_EQ(judge(kCases[2], {4, false, 0}).roleMap, 0x1302u);
    EXPECT_GT(judge(kCases[1], {4, false, 0}).outsideWritten + judge(kCases[5], {4, false, 0}).outsideWritten, 0u);
}

TEST(SwitchVICPackedProbe, AnEngineWithTheOtherByteOrderIsNamed)
{
    const Engine e = {2, true, 0};
    SwitchVICPackedResult r = judge(kCases[1], e);  // '2vuy' in, read as Y0 Cb Y1 Cr
    EXPECT_FALSE(clean(r));
    EXPECT_EQ(r.roleMap, 0x3120u);
    r = judge(kCases[7], e);  // 'yuvs' out, written as Cb Y0 Cr Y1
    EXPECT_FALSE(clean(r));
    EXPECT_EQ(r.roleMap, 0x1302u);
    EXPECT_TRUE(clean(judge(kCases[9], e)));  // a same-format copy cannot tell
    EXPECT_GT(judge(kCases[13], e).maxError, kTolerance);
}

TEST(SwitchVICPackedProbe, TheControlDecidesTheProbe)
{
    SwitchVICPackedProbeReply reply = {};
    reply.count = SWITCH_VIC_PACKED_PROBE_CASES;
    reply.results[3].mismatches = 2;
    summarise(&reply);
    EXPECT_EQ(reply.controlsClean, 1u);
    EXPECT_EQ(reply.cleanCases, 0x7ff7u);
    reply.results[0].outsideWritten = 1;
    summarise(&reply);
    EXPECT_EQ(reply.controlsClean, 0u);
}
