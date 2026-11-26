// SPDX-License-Identifier: GPL-2.0-only
// EDID source limits and the native IOAV wire layout.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <random>
#include <vector>

#include "../SwitchDPLink.h"
#include "../SwitchDPModes.h"
#include "../SwitchDPAppleTiming.h"
#include <cstring>

namespace {

using Block = std::array<uint8_t, 128>;

void checksum(Block &block)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < 127; ++i) {
        sum = static_cast<uint8_t>(sum + block[i]);
    }
    block[127] = static_cast<uint8_t>(0x100 - sum);
}

// An 18-byte detailed timing descriptor, digital separate sync with both polarities positive.
std::array<uint8_t, 18> dtd(uint32_t pixel_khz, unsigned ha, unsigned hf, unsigned hs, unsigned hb, unsigned va,
                            unsigned vf, unsigned vs, unsigned vb, bool interlaced = false)
{
    const unsigned hblank = hf + hs + hb, vblank = vf + vs + vb, clock = pixel_khz / 10;
    std::array<uint8_t, 18> d{};
    d[0] = clock & 0xFF;
    d[1] = clock >> 8;
    d[2] = ha & 0xFF;
    d[3] = hblank & 0xFF;
    d[4] = static_cast<uint8_t>(((ha >> 8) << 4) | (hblank >> 8));
    d[5] = va & 0xFF;
    d[6] = vblank & 0xFF;
    d[7] = static_cast<uint8_t>(((va >> 8) << 4) | (vblank >> 8));
    d[8] = hf & 0xFF;
    d[9] = hs & 0xFF;
    d[10] = static_cast<uint8_t>(((vf & 0xF) << 4) | (vs & 0xF));
    d[11] = static_cast<uint8_t>(((hf >> 8) << 6) | ((hs >> 8) << 4) | ((vf >> 4) << 2) | (vs >> 4));
    d[17] = static_cast<uint8_t>(0x1E | (interlaced ? 0x80 : 0));
    return d;
}

Block base(const std::vector<std::array<uint8_t, 18>> &dtds, uint8_t extensions, bool vga = true)
{
    Block b{};
    const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::copy(header, header + 8, b.begin());
    b[18] = 1;
    b[19] = 4;
    b[35] = vga ? 0x20 : 0;
    for (size_t i = 0; i < dtds.size() && i < 4; ++i) {
        std::copy(dtds[i].begin(), dtds[i].end(), b.begin() + 54 + 18 * i);
    }
    b[126] = extensions;
    checksum(b);
    return b;
}

Block cta(const std::vector<uint8_t> &vics, const std::vector<std::array<uint8_t, 18>> &dtds = {})
{
    Block b{};
    b[0] = 0x02;
    b[1] = 0x03;
    b[4] = static_cast<uint8_t>((2 << 5) | vics.size());
    std::copy(vics.begin(), vics.end(), b.begin() + 5);
    b[2] = static_cast<uint8_t>(5 + vics.size());
    for (size_t i = 0; i < dtds.size(); ++i) {
        std::copy(dtds[i].begin(), dtds[i].end(), b.begin() + b[2] + 18 * i);
    }
    checksum(b);
    return b;
}

switch_dp_mode_list parse(const std::vector<Block> &blocks)
{
    switch_dp_mode_list list{};
    if (!blocks.empty() && switch_dp_edid_block_modes(blocks[0].data(), true, &list)) {
        for (size_t i = 1; i < blocks.size() && i <= blocks[0][126]; ++i)
            switch_dp_edid_block_modes(blocks[i].data(), false, &list);
    }
    return list;
}

Block displayId(uint8_t tag, uint32_t clock_units, unsigned ha, unsigned hf, unsigned hs, unsigned hb, unsigned va,
                unsigned vf, unsigned vs, unsigned vb, bool preferred = true, bool interlaced = false)
{
    Block b{};
    b[0] = 0x70;
    b[1] = tag == 0x22 ? 0x20 : 0x13;
    b[2] = 23;
    b[3] = 0x03;
    b[4] = 0;
    b[5] = tag;
    b[6] = 0;
    b[7] = 20;
    uint8_t *t = b.data() + 8;
    const uint32_t clk = clock_units - 1u;
    t[0] = static_cast<uint8_t>(clk & 0xFFu);
    t[1] = static_cast<uint8_t>((clk >> 8) & 0xFFu);
    t[2] = static_cast<uint8_t>((clk >> 16) & 0xFFu);
    t[3] = static_cast<uint8_t>((preferred ? 0x80u : 0u) | (interlaced ? 0x10u : 0u));
    const auto put16 = [](uint8_t *p, unsigned v) {
        const unsigned enc = v - 1u;
        p[0] = static_cast<uint8_t>(enc & 0xFFu);
        p[1] = static_cast<uint8_t>((enc >> 8) & 0xFFu);
    };
    const unsigned hblank = hf + hs + hb, vblank = vf + vs + vb;
    put16(t + 4, ha);
    put16(t + 6, hblank);
    put16(t + 8, hf);
    t[9] |= 0x80u;
    put16(t + 10, hs);
    put16(t + 12, va);
    put16(t + 14, vblank);
    put16(t + 16, vf);
    t[17] |= 0x80u;
    put16(t + 18, vs);
    uint8_t sum = 0;
    for (unsigned i = 1; i < 5u + b[2]; ++i)
        sum = static_cast<uint8_t>(sum + b[i]);
    b[5u + b[2]] = static_cast<uint8_t>(0x100u - sum);
    checksum(b);
    return b;
}

const auto k2160p60 = dtd(594000, 3840, 176, 88, 296, 2160, 8, 10, 72);
const auto k3440p60 = dtd(319750, 3440, 48, 32, 80, 1440, 3, 10, 28);
const auto k1440p144 = dtd(586590, 2560, 48, 32, 80, 1440, 3, 5, 62);
const auto k1080p60 = dtd(148500, 1920, 88, 44, 148, 1080, 4, 5, 36);
const uint64_t kHbr2Four = switch_dp_payload_kbps(SWITCH_DP_RATE_HBR2, 4);

TEST(Edid, StandardDetailedTimingParses)
{
    switch_dp_mode mode{};
    ASSERT_TRUE(switch_dp_edid_dtd(k1080p60.data(), &mode));
    EXPECT_EQ(mode.pixel_khz, 148500u);
    EXPECT_EQ(mode.hactive, 1920);
    EXPECT_EQ(mode.hfront, 88);
    EXPECT_EQ(mode.hsync, 44);
    EXPECT_EQ(mode.hback, 148);
    EXPECT_EQ(mode.vactive, 1080);
    EXPECT_EQ(mode.vfront, 4);
    EXPECT_EQ(mode.vsync, 5);
    EXPECT_EQ(mode.vback, 36);
    EXPECT_TRUE(mode.hsync_high);
    EXPECT_TRUE(mode.vsync_high);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&mode), 60000u);
    EXPECT_EQ(switch_dp_refresh_period(&mode), 0x04444444u);
}

TEST(Edid, InterlacedAndDescriptorsAreSkipped)
{
    switch_dp_mode mode{};
    const auto interlaced = dtd(74250, 1920, 88, 44, 148, 540, 2, 5, 15, true);
    EXPECT_FALSE(switch_dp_edid_dtd(interlaced.data(), &mode));
    const std::array<uint8_t, 18> name{0, 0, 0, 0xFC, 0, 'T', 'V'};
    EXPECT_FALSE(switch_dp_edid_dtd(name.data(), &mode));
}

TEST(SourceLimits, AdmitsUltrawideAndFourKWithinControllerHardwareCeiling)
{
    const auto list = parse({base({k3440p60, k2160p60, k1440p144, k1080p60}, 0)});
    ASSERT_EQ(list.count, 4u);
    EXPECT_EQ(list.modes[0].hactive, 3440);
    EXPECT_EQ(list.modes[0].vactive, 1440);
    EXPECT_TRUE(list.modes[0].preferred);
    EXPECT_EQ(list.modes[1].hactive, 3840);
    EXPECT_EQ(list.modes[1].vactive, 2160);
    EXPECT_EQ(list.modes[2].hactive, 1920);
    EXPECT_TRUE(switch_dp_prefer(&list.modes[0], &list.modes[1]));
    EXPECT_TRUE(switch_dp_prefer(&list.modes[1], &list.modes[2]));

    EXPECT_TRUE(switch_dp_mode_allowed(&list.modes[0], kHbr2Four));
    EXPECT_TRUE(switch_dp_mode_allowed(&list.modes[1], kHbr2Four));
    EXPECT_FALSE(switch_dp_mode_allowed(&list.modes[1], switch_dp_payload_kbps(SWITCH_DP_RATE_HBR, 4)));

    auto timing = list.modes[2];
    EXPECT_TRUE(switch_dp_mode_allowed(&timing, switch_dp_payload_kbps(SWITCH_DP_RATE_HBR, 2)));
    timing.pixel_khz = 149500;
    EXPECT_TRUE(switch_dp_mode_allowed(&timing, kHbr2Four));
    timing.pixel_khz = 150000;
    EXPECT_FALSE(switch_dp_mode_allowed(&timing, kHbr2Four));
    timing.pixel_khz = 148352;
    EXPECT_TRUE(switch_dp_mode_allowed(&timing, kHbr2Four));
    EXPECT_EQ(switch_dp_refresh_period(&timing), (uint64_t{2200} * 1125 << 32) / 148352000u);

    const switch_dp_mode dci4k{594000, 4096, 88, 88, 128, 2160, 8, 10, 72, true, true, false};
    const switch_dp_mode trmMax{672000, 4096, 48, 32, 80, 2560, 3, 10, 58, true, true, false};
    const switch_dp_mode portrait4k{594000, 2160, 88, 44, 208, 3840, 8, 10, 102, true, true, false};
    const switch_dp_mode superWide{472000, 5120, 48, 32, 80, 1440, 3, 10, 28, true, true, false};
    EXPECT_TRUE(switch_dp_mode_allowed(&dci4k, kHbr2Four));
    EXPECT_TRUE(switch_dp_mode_allowed(&trmMax, kHbr2Four));
    EXPECT_TRUE(switch_dp_mode_allowed(&portrait4k, kHbr2Four));
    EXPECT_FALSE(switch_dp_mode_allowed(&superWide, kHbr2Four));
}

TEST(SourceLimits, AcceptsVesa60HzToleranceAndRejectsHigherRates)
{
    const switch_dp_mode xga60{65000, 1024, 24, 136, 160, 768, 3, 6, 29, false, false, false};      // 60.004 Hz
    const switch_dp_mode sxga60{108000, 1280, 48, 112, 248, 1024, 1, 3, 38, true, true, false};     // 60.020 Hz
    const switch_dp_mode svga60{40000, 800, 40, 128, 88, 600, 1, 4, 23, true, true, false};         // 60.317 Hz
    EXPECT_TRUE(switch_dp_source_timing(&xga60));
    EXPECT_TRUE(switch_dp_source_timing(&sxga60));
    EXPECT_TRUE(switch_dp_source_timing(&svga60));

    const switch_dp_mode xga70{75000, 1024, 24, 136, 144, 768, 3, 6, 29, false, false, false};      // 70.069 Hz
    const switch_dp_mode sxga75{135000, 1280, 16, 144, 248, 1024, 1, 3, 38, true, true, false};     // 75.025 Hz
    const switch_dp_mode fhd120{297000, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true, false};      // 120.000 Hz
    const switch_dp_mode fhd144{356400, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true, false};      // 144.000 Hz
    EXPECT_FALSE(switch_dp_source_timing(&xga70));
    EXPECT_FALSE(switch_dp_source_timing(&sxga75));
    EXPECT_FALSE(switch_dp_source_timing(&fhd120));
    EXPECT_FALSE(switch_dp_source_timing(&fhd144));
}

TEST(Edid, ParsesStandardTimingsAt60Hz)
{
    Block b = base({}, 0, false);
    const uint8_t stdTimings[16] = {
        0xD1, 0xC0,  // 1920x1080 @ 60 Hz
        0xA9, 0xC0,  // 1600x900 @ 60 Hz
        0x81, 0xC0,  // 1280x720 @ 60 Hz
        0x81, 0x80,  // 1280x1024 @ 60 Hz
        0x61, 0x40,  // 1024x768 @ 60 Hz
        0x45, 0x40,  // 800x600 @ 60 Hz
        0x81, 0x8F,  // 1280x1024 @ 75 Hz (skipped)
        0x01, 0x01,  // unused padding
    };
    std::copy(std::begin(stdTimings), std::end(stdTimings), b.begin() + 38);
    checksum(b);
    const auto list = parse({b});
    ASSERT_EQ(list.count, 6u);
    const std::pair<uint16_t, uint16_t> expected[6] = {
        {1920, 1080}, {1600, 900}, {1280, 720}, {1280, 1024}, {1024, 768}, {800, 600},
    };
    for (unsigned i = 0; i < 6; ++i) {
        EXPECT_EQ(list.modes[i].hactive, expected[i].first);
        EXPECT_EQ(list.modes[i].vactive, expected[i].second);
        EXPECT_TRUE(switch_dp_mode_allowed(&list.modes[i], kHbr2Four));
        for (bool b01 : {false, true}) {
            switch_dp_plld2 pll{};
            EXPECT_TRUE(switch_dp_plld2_for(list.modes[i].pixel_khz, 38400, b01, &pll));
        }
    }
}

TEST(Edid, PublishesFourKAndFractionalRatesFromCta)
{
    const auto list = parse({base({}, 1, false), cta({97, 95, 0x80 | 16})});
    ASSERT_EQ(list.count, 6u);
    EXPECT_EQ(list.modes[0].hactive, 3840);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&list.modes[0]), 60000u);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&list.modes[1]), 59940u);
    EXPECT_EQ(list.modes[2].hactive, 3840);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&list.modes[2]), 30000u);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&list.modes[3]), 29970u);
    EXPECT_EQ(list.modes[4].hactive, 1920);
    EXPECT_TRUE(list.modes[4].preferred);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&list.modes[5]), 59940u);
}

TEST(Edid, ParsesDisplayIdType1AndType7TimingsAndRejectsBrokenSections)
{
    const auto type1 = displayId(0x03, 31975, 3440, 48, 32, 80, 1440, 3, 10, 28, true);
    const auto type7 = displayId(0x22, 672000, 4096, 48, 32, 80, 2560, 3, 10, 58, false);
    const auto list = parse({base({}, 2, false), type1, type7});
    ASSERT_EQ(list.count, 2u);
    EXPECT_EQ(list.modes[0].hactive, 3440);
    EXPECT_EQ(list.modes[0].vactive, 1440);
    EXPECT_EQ(list.modes[0].pixel_khz, 319750u);
    EXPECT_TRUE(list.modes[0].preferred);
    EXPECT_TRUE(list.modes[0].hsync_high);
    EXPECT_TRUE(list.modes[0].vsync_high);
    EXPECT_EQ(list.modes[1].hactive, 4096);
    EXPECT_EQ(list.modes[1].vactive, 2560);
    EXPECT_EQ(list.modes[1].pixel_khz, 672000u);
    EXPECT_FALSE(list.modes[1].preferred);

    auto badChecksum = type1;
    badChecksum[5 + badChecksum[2]] ^= 1;
    checksum(badChecksum);
    EXPECT_EQ(parse({base({}, 1, false), badChecksum}).count, 0u);

    const auto interlaced = displayId(0x03, 14850, 1920, 88, 44, 148, 540, 2, 5, 15, false, true);
    EXPECT_EQ(parse({base({}, 1, false), interlaced}).count, 0u);
}

TEST(Edid, RepeatedDetailedAndCtaTimingsShareOneModeId)
{
    const auto list = parse({base({k1080p60}, 1, false), cta({16, 0x80 | 16}, {k1080p60})});
    ASSERT_EQ(list.count, 2u);
    EXPECT_TRUE(list.modes[0].preferred);
    EXPECT_EQ(switch_dp_mode_refresh_mhz(&list.modes[1]), 59940u);
}

TEST(Edid, BrokenBaseAndExtensionAreIgnored)
{
    Block broken = base({k1080p60}, 0);
    broken[60] ^= 0xff;
    const auto fallback = parse({broken});
    EXPECT_EQ(fallback.count, 0u);
    auto extension = cta({97});
    extension[127] ^= 1;
    const auto list = parse({base({k1080p60}, 1, false), extension});
    ASSERT_EQ(list.count, 1u);
    EXPECT_EQ(list.modes[0].hactive, 1920);
}

TEST(SourceLimits, RejectsTimingsTheControllerCannotEncode)
{
    switch_dp_mode mode{};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    mode.hactive = 4097;
    EXPECT_FALSE(switch_dp_mode_allowed(&mode, kHbr2Four));
    mode.hactive = 1920;
    mode.vactive = 4097;
    EXPECT_FALSE(switch_dp_mode_allowed(&mode, kHbr2Four));
    mode.vactive = 1080;
    mode.vfront = 1;
    EXPECT_FALSE(switch_dp_mode_allowed(&mode, kHbr2Four));
    mode.vfront = 4;
    mode.pixel_khz = 750001;
    EXPECT_FALSE(switch_dp_mode_allowed(&mode, UINT64_MAX));
}

TEST(NativeTiming, MatchesBothPinnedNative1080p60Tables)
{
    switch_dp_mode mode{};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    const auto timing = switch_dp_apple_timing(&mode);
    const uint32_t native[] = {0, 2200, 1920, 44, 148, 88, 0, 1, 0,
                              1125, 1080, 5, 36, 4, 0x3c0000, 1, 0, 148500000, 0, 0};
    static_assert(sizeof(timing) == 80);
    EXPECT_EQ(std::memcmp(&timing, native, sizeof(native)), 0);
    const auto color = switch_dp_apple_rgb8();
    const uint32_t rgb8[] = {8, 0, 0, 10, 0, 0, 0, 0};
    static_assert(sizeof(color) == 32);
    EXPECT_EQ(std::memcmp(&color, rgb8, sizeof(rgb8)), 0);
}

}  // namespace
