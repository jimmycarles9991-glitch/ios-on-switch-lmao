// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstring>
#include "../SwitchHDAFormat.h"
#include "../hal/SwitchAudioConvert.h"

namespace {
using Edid = std::array<uint8_t, 384>;

Edid sink()
{
    Edid edid{};
    edid[126] = 1;
    edid[128] = 2;
    edid[129] = 3;
    edid[130] = 8;
    edid[132] = 0x23;
    edid[133] = 0x09;
    edid[134] = 4;
    edid[135] = 5;
    return edid;
}

TEST(HdaSink, RequiresStereoLpcmAt44k1Or48kOrBasicAudio)
{
    auto edid = sink();
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 3), 5u | SWITCH_HDA_SINK_48K);
    edid[134] = 2;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 3), 5u | SWITCH_HDA_SINK_44K1);
    edid[134] = 6;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 3), 5u | SWITCH_HDA_SINK_44K1 | SWITCH_HDA_SINK_48K);
    edid[134] = 1;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 3), 0u);
    edid[131] = 0x40;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 3), 1u | SWITCH_HDA_SINK_44K1 | SWITCH_HDA_SINK_48K);
    edid[131] = 0;
    edid[134] = 4;
    edid[133] = 0x08;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 3), 0u);
    edid[133] = 0x11;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 3), 0u);
}

TEST(HdaSink, PlaysTheRatesSinkAndConverterShare48kFirst)
{
    uint32_t rates[3];
    EXPECT_EQ(switch_hda_rates(SWITCH_HDA_SINK_44K1 | SWITCH_HDA_SINK_48K, (1u << 5) | (1u << 6), rates), 2u);
    EXPECT_EQ(rates[0], 48000u);
    EXPECT_EQ(rates[1], 44100u);
    EXPECT_EQ(rates[2], 0u);
    EXPECT_EQ(switch_hda_rates(SWITCH_HDA_SINK_44K1 | SWITCH_HDA_SINK_48K, 1u << 6, rates), 1u);
    EXPECT_EQ(rates[0], 48000u);
    EXPECT_EQ(rates[1], 0u);
    EXPECT_EQ(switch_hda_rates(SWITCH_HDA_SINK_44K1, 1u << 6, rates), 0u);
    EXPECT_EQ(rates[0], 0u);
}

TEST(HdaSink, RespectsProvidedExtensionAndDataBlockBounds)
{
    auto edid = sink();
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 1), 0u);
    EXPECT_EQ(switch_hda_sink_formats(nullptr, 3), 0u);
    edid[130] = 7;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 3), 0u);
    edid[130] = 255;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 3), 0u);
    edid[130] = 8;
    EXPECT_EQ(switch_hda_sink_formats(edid.data(), 5), 0u);
}

TEST(HdaEld, CarriesMonitorIdentityAndActualSadsOnDisplayPort)
{
    auto edid = sink();
    edid[8] = 0x12;
    edid[9] = 0x34;
    edid[10] = 0x56;
    edid[11] = 0x78;
    edid[57] = 0xfc;
    std::memcpy(edid.data() + 59, "Monitor\n", 8);
    std::array<uint8_t, 130> buffer;
    buffer.fill(0xa5);
    uint8_t *eld = buffer.data() + 1;
    EXPECT_EQ(switch_hda_eld(edid.data(), 3, eld), 32u);
    EXPECT_EQ(buffer.front(), 0xa5);
    EXPECT_EQ(buffer.back(), 0xa5);
    EXPECT_EQ(eld[0], 16);
    EXPECT_EQ(eld[2], 7);
    EXPECT_EQ(eld[4], 0x67);
    EXPECT_EQ(eld[5], 0x14);
    EXPECT_EQ(eld[7], 1);
    EXPECT_EQ(std::memcmp(eld + 16, edid.data() + 8, 4), 0);
    EXPECT_EQ(std::memcmp(eld + 20, "Monitor", 7), 0);
    EXPECT_EQ(std::memcmp(eld + 27, edid.data() + 133, 3), 0);
    EXPECT_EQ(eld[31], 0);
}

TEST(HdaEld, BasicAudioHasItsMandatory16BitLpcmDescriptor)
{
    auto edid = sink();
    edid[130] = 0;
    edid[131] = 0x40;
    uint8_t eld[128];
    EXPECT_EQ(switch_hda_eld(edid.data(), 3, eld), 24u);
    EXPECT_EQ(eld[5], 0x14);
    EXPECT_EQ(eld[20], 9);
    EXPECT_EQ(eld[21], 7);
    EXPECT_EQ(eld[22], 1);
    EXPECT_EQ(eld[23], 0);
}

TEST(HdaEld, LimitsDescriptorsAcrossExtensions)
{
    auto edid = sink();
    edid[126] = 2;
    for (unsigned block = 1; block <= 2; ++block) {
        const unsigned base = block * 128;
        edid[base] = 2;
        edid[base + 1] = 3;
        edid[base + 2] = 35;
        edid[base + 4] = 0x3e;
        for (unsigned sad = 0; sad < 10; ++sad) {
            edid[base + 5 + sad * 3] = 9;
            edid[base + 6 + sad * 3] = 4;
            edid[base + 7 + sad * 3] = 5;
        }
    }
    uint8_t eld[128];
    EXPECT_EQ(switch_hda_eld(edid.data(), 7, eld), 68u);
    EXPECT_EQ(eld[5], 0xf4);
    EXPECT_EQ(eld[64], 5);
    EXPECT_EQ(eld[65], 0);
}

TEST(HdaProtocol, EncodesStereoFormatsAndVerbPayloads)
{
    EXPECT_EQ(switch_hda_stream_format(16, 48000), 0x11);
    EXPECT_EQ(switch_hda_stream_format(24, 48000), 0x31);
    EXPECT_EQ(switch_hda_stream_format(16, 44100), 0x4011);
    EXPECT_EQ(switch_hda_stream_format(24, 44100), 0x4031);
    EXPECT_EQ(switch_hda_verb(2, 3, 0x200, 0x31), 0x20320031u);
    EXPECT_EQ(switch_hda_verb(0, 7, 0xf00, 9), 0x007f0009u);
    EXPECT_EQ(switch_hda_verb(1, 4, 0x707, 0x40), 0x10470740u);
    EXPECT_EQ(switch_hda_verb(1, 4, 0xf09, 0), 0x104f0900u);
}

TEST(HdaProtocol, StripesStereoOnlyWhileEachLineCarriesSixteenBits)
{
    EXPECT_EQ(switch_hda_stripe(0x4401, 24), 0u);
    EXPECT_EQ(switch_hda_stripe(0x4403, 16), 1u);
    EXPECT_EQ(switch_hda_stripe(0x4403, 24), 1u);
    EXPECT_EQ(switch_hda_stripe(0x4405, 24), 2u);
    EXPECT_EQ(switch_hda_stripe(0x4405, 16), 1u);
}

TEST(HdaProtocol, RequiresMatchingSinkAndCodecPcmWidth)
{
    EXPECT_EQ(switch_hda_sample_bits(SWITCH_HDA_S16, 1u << 16), 0u);
    EXPECT_EQ(switch_hda_sample_bits(SWITCH_HDA_S16, 1u << 17), 16u);
    EXPECT_EQ(switch_hda_sample_bits(SWITCH_HDA_S16 | SWITCH_HDA_S24, 1u << 19), 24u);
    EXPECT_EQ(switch_hda_sample_bits(SWITCH_HDA_S24, 1u << 17), 0u);
}

TEST(HdaMix, FinalStereoMixOverwritesExistingSamplesInBothFormats)
{
    int16_t ring[] = {100, -100, 0, 0, 0, 10, -10, 32760, -32760};
    const float samples[] = {0.5f, -0.5f, INFINITY, -INFINITY, NAN, 0, 0, 0.5f, -0.5f};
    switch_audio_write_s16(ring, samples, 9);
    const int16_t expected[] = {16384, -16384, 32767, -32768, 0, 0, 0, 16384, -16384};
    EXPECT_EQ(std::memcmp(ring, expected, sizeof(ring)), 0);
    int32_t wide[9] = {10, 20, 30, 40, 50, 60, 70, 80, 90};
    switch_audio_write_s32(wide, samples, 9);
    const int32_t converted[] = {1 << 30, -(1 << 30), INT32_MAX, INT32_MIN, 0, 0, 0, 1 << 30, -(1 << 30)};
    EXPECT_EQ(std::memcmp(wide, converted, sizeof(wide)), 0);
}
}  // namespace
