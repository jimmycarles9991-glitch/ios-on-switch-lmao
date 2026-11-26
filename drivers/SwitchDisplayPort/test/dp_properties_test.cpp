// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>

#include <cstring>

#include "switch_dp_properties_stubs.h"
#include "../SwitchDPAppleTiming.h"
#include "../SwitchDPProperties.h"

namespace {

TEST(DisplayProperties, EdidIdentityMatchesNativeIoavAndSurvivesSerialAndTimingChanges)
{
    uint8_t edid[128] = {};
    for (unsigned i = 0; i < sizeof(edid); ++i)
        edid[i] = static_cast<uint8_t>(i);
    auto *uuid = switch_dp_edid_uuid(edid);
    ASSERT_NE(uuid, nullptr);
    EXPECT_STREQ(uuid->getCStringNoCopy(), "08090A0B-0000-0000-1011-121314151617");
    uuid->release();

    std::memset(edid + 12, 0xff, 4);
    std::memset(edid + 54, 0xff, 74);
    uuid = switch_dp_edid_uuid(edid);
    ASSERT_NE(uuid, nullptr);
    EXPECT_STREQ(uuid->getCStringNoCopy(), "08090A0B-0000-0000-1011-121314151617");
    uuid->release();

    edid[10] = 0xff;
    uuid = switch_dp_edid_uuid(edid);
    ASSERT_NE(uuid, nullptr);
    EXPECT_STREQ(uuid->getCStringNoCopy(), "0809FF0B-0000-0000-1011-121314151617");
    uuid->release();
}

TEST(DisplayProperties, ZeroEdidUsesNativeResetIdentity)
{
    const uint8_t edid[128] = {};
    auto *uuid = switch_dp_edid_uuid(edid);
    ASSERT_NE(uuid, nullptr);
    EXPECT_STREQ(uuid->getCStringNoCopy(), "00000000-0000-0000-0000-000000000000");
    uuid->release();
}

uint64_t number(OSDictionary *dictionary, const char *key, unsigned bits = 32)
{
    auto *value = OSDynamicCast(OSNumber, dictionary->getObject(key));
    EXPECT_NE(value, nullptr) << key;
    if (!value)
        return 0;
    EXPECT_EQ(value->numberOfBits(), bits) << key;
    return value->unsigned64BitValue();
}

void axis(OSDictionary *timing, const char *key, uint32_t total, uint32_t front, uint32_t sync, uint32_t back,
          uint32_t active, uint32_t rate, uint32_t precise, uint32_t polarity)
{
    auto *attributes = OSDynamicCast(OSDictionary, timing->getObject(key));
    ASSERT_NE(attributes, nullptr) << key;
    EXPECT_EQ(attributes->getCount(), 9u);
    EXPECT_EQ(number(attributes, "Total"), total);
    EXPECT_EQ(number(attributes, "FrontPorch"), front);
    EXPECT_EQ(number(attributes, "SyncWidth"), sync);
    EXPECT_EQ(number(attributes, "BackPorch"), back);
    EXPECT_EQ(number(attributes, "Active"), active);
    EXPECT_EQ(number(attributes, "SyncRate"), rate);
    EXPECT_EQ(number(attributes, "PreciseSyncRate"), precise);
    EXPECT_EQ(number(attributes, "SyncPolarity"), polarity);
    EXPECT_EQ(number(attributes, "PixelRepetition"), 0u);
}

TEST(DisplayProperties, TimingExportsNativeExpandedAxesAndOriginalPayload)
{
    switch_dp_mode mode{};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    mode.preferred = true;
    auto *colors = OSArray::withCapacity(1);
    auto *color = switch_dp_color_element();
    ASSERT_NE(color, nullptr);
    ASSERT_TRUE(colors->setObject(color));
    color->release();
    auto *timing = switch_dp_timing_element(&mode, 7, colors);
    ASSERT_NE(timing, nullptr);
    EXPECT_EQ(number(timing, "ID", 64), 7u);
    EXPECT_EQ(number(timing, "ElementType"), 0u);
    EXPECT_EQ(timing->getObject("IsVirtual"), kOSBooleanFalse);
    EXPECT_EQ(timing->getObject("IsPreferred"), kOSBooleanTrue);
    EXPECT_EQ(timing->getObject("IsInterlaced"), kOSBooleanFalse);
    EXPECT_EQ(timing->getObject("IsSplit"), kOSBooleanFalse);
    EXPECT_EQ(timing->getObject("IsOverscanned"), kOSBooleanFalse);
    EXPECT_EQ(timing->getObject("IsPromoted"), kOSBooleanFalse);
    EXPECT_EQ(number(timing, "ScanInformation"), 0u);
    EXPECT_EQ(number(timing, "ValidPixelEncodings"), 1u);
    EXPECT_EQ(number(timing, "StandardType"), 1u);
    EXPECT_EQ(number(timing, "TimingType"), 3u);
    EXPECT_EQ(number(timing, "AspectRatio"), 0u);
    EXPECT_EQ(number(timing, "PreciseAspectRatio"), 116508u);
    EXPECT_EQ(timing->getObject("ColorModes"), colors);
    for (const auto *key : {"UnsafeColorElementIDs", "DSCRequiredColorElementIDs"}) {
        auto *ids = OSDynamicCast(OSArray, timing->getObject(key));
        ASSERT_NE(ids, nullptr) << key;
        EXPECT_EQ(ids->getCount(), 0u);
    }
    axis(timing, "HorizontalAttributes", 2200, 88, 44, 148, 1920, 0, 0, 1);
    axis(timing, "VerticalAttributes", 1125, 4, 5, 36, 1080, 0x003c0000, 0x003c0000, 1);
    auto *data = OSDynamicCast(OSData, timing->getObject("ElementData"));
    ASSERT_NE(data, nullptr);
    const auto expected = switch_dp_apple_timing(&mode);
    ASSERT_EQ(data->getLength(), 80u);
    EXPECT_EQ(std::memcmp(data->getBytesNoCopy(), &expected, sizeof(expected)), 0);
    timing->release();
    EXPECT_EQ(colors->getCount(), 1u);
    colors->release();
}

TEST(DisplayProperties, FractionalRefreshKeepsFixedPointPrecision)
{
    switch_dp_mode mode{};
    ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
    mode.pixel_khz = 148352;
    mode.hsync_high = false;
    mode.vsync_high = false;
    auto *colors = OSArray::withCapacity(0);
    auto *timing = switch_dp_timing_element(&mode, 3, colors);
    ASSERT_NE(timing, nullptr);
    EXPECT_EQ(timing->getObject("IsPreferred"), kOSBooleanFalse);
    axis(timing, "HorizontalAttributes", 2200, 88, 44, 148, 1920, 0, 0, 0);
    axis(timing, "VerticalAttributes", 1125, 4, 5, 36, 1080, 0x003c0000, 0x003bf0b1, 0);
    timing->release();
    colors->release();
}

TEST(DisplayProperties, NativeScoresAdmitExternalTimingsAndPreserveEdidPreference)
{
    auto *colors = OSArray::withCapacity(0);

    const struct {
        uint32_t clock, ordinary, preferred;
    } samples[] = {
        {148500, 184, 10284},
        {148352, 184, 10284},
        {146273, 182, 10282},
    };

    for (const auto &sample : samples) {
        switch_dp_mode mode{};
        ASSERT_TRUE(switch_dp_cta_vic(16, &mode));
        mode.pixel_khz = sample.clock;
        uint64_t scores[2] = {};
        for (unsigned preferred = 0; preferred < 2; ++preferred) {
            mode.preferred = preferred != 0;
            auto *timing = switch_dp_timing_element(&mode, 1, colors);
            ASSERT_NE(timing, nullptr);
            scores[preferred] = number(timing, "Score");
            const bool isVirtual = timing->getObject("IsVirtual") == kOSBooleanTrue;
            EXPECT_FALSE(isVirtual);
            EXPECT_TRUE(scores[preferred] != 0 || isVirtual);
            timing->release();
        }
        EXPECT_EQ(scores[0], sample.ordinary);
        EXPECT_EQ(scores[1], sample.preferred);
        EXPECT_GT(scores[1], scores[0]);
    }
    colors->release();
}

TEST(DisplayProperties, ColorExportsNativeRgb8FieldsAndOriginalPayload)
{
    auto *color = switch_dp_color_element();
    ASSERT_NE(color, nullptr);
    EXPECT_EQ(number(color, "ID", 64), 1u);
    EXPECT_EQ(number(color, "ElementType"), 1u);
    EXPECT_EQ(color->getObject("IsVirtual"), kOSBooleanFalse);
    EXPECT_EQ(number(color, "Depth"), 8u);
    EXPECT_EQ(number(color, "PixelEncoding"), 0u);
    EXPECT_EQ(number(color, "DynamicRange"), 0u);
    EXPECT_EQ(number(color, "EOTF"), 0u);
    EXPECT_EQ(number(color, "Colorimetry"), 10u);
    EXPECT_EQ(number(color, "StandardType"), 1u);
    EXPECT_EQ(color->getObject("SupportsDSC"), kOSBooleanFalse);
    auto *data = OSDynamicCast(OSData, color->getObject("ElementData"));
    ASSERT_NE(data, nullptr);
    ASSERT_EQ(data->getLength(), 32u);
    const auto expected = switch_dp_apple_rgb8();
    EXPECT_EQ(std::memcmp(data->getBytesNoCopy(), &expected, sizeof(expected)), 0);
    color->release();
}

}
