// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
extern "C" {
#include "drivers/SwitchVideoDecoders/AVD/H264Syntax.h"
#include "drivers/SwitchVideoDecoders/AVD/HEVCSyntax.h"
}

namespace {
struct Bits {
    std::vector<uint8_t> bytes;
    unsigned count = 0;

    void put(uint32_t value, unsigned width)
    {
        for (unsigned i = width; i; --i) {
            if (!(count % 8))
                bytes.push_back(0);
            bytes.back() |= ((value >> (i - 1)) & 1) << (7 - count++ % 8);
        }
    }

    void ue(uint32_t value)
    {
        unsigned width = 0;
        for (uint32_t n = ++value; n; n >>= 1)
            ++width;
        put(0, width - 1);
        put(value, width);
    }

    void flag(bool value)
    {
        put(value, 1);
    }

    std::vector<uint8_t> nal(std::vector<uint8_t> header)
    {
        flag(true);
        while (count % 8)
            flag(false);
        unsigned zeros = 0;
        for (auto byte : bytes) {
            if (zeros == 2 && byte <= 3) {
                header.push_back(3);
                zeros = 0;
            }
            header.push_back(byte);
            zeros = byte ? 0 : zeros + 1;
        }
        return header;
    }
};

void vui(Bits &b, bool present, uint32_t top, uint32_t bottom)
{
    b.flag(false);
    b.flag(false);
    b.flag(true);
    b.put(5, 3);
    b.flag(false);
    b.flag(true);
    b.put(9, 8);
    b.put(16, 8);
    b.put(9, 8);
    b.flag(present);
    if (present) {
        b.ue(top);
        b.ue(bottom);
    }
}

std::vector<uint8_t> h264(bool present, uint32_t top, uint32_t bottom)
{
    Bits b;
    b.put(66, 8);
    b.put(0, 8);
    b.put(30, 8);
    b.ue(0);
    b.ue(0);
    b.ue(0);
    b.ue(0);
    b.ue(1);
    b.flag(false);
    b.ue(3);
    b.ue(3);
    b.flag(true);
    b.flag(true);
    b.flag(false);
    b.flag(true);
    vui(b, present, top, bottom);
    for (unsigned i = 0; i < 5; ++i)
        b.flag(false);
    return b.nal({0x67});
}

std::vector<uint8_t> hevc(bool present, uint32_t top, uint32_t bottom)
{
    Bits b;
    b.put(0, 4);
    b.put(0, 3);
    b.flag(true);
    b.put(0, 2);
    b.flag(false);
    b.put(1, 5);
    b.put(0, 32);
    b.put(0, 4);
    b.put(0, 32);
    b.put(0, 12);
    b.put(90, 8);
    b.ue(0);
    b.ue(1);
    b.ue(64);
    b.ue(64);
    b.flag(false);
    b.ue(0);
    b.ue(0);
    b.ue(0);
    b.flag(false);
    b.ue(0);
    b.ue(0);
    b.ue(0);
    b.ue(1);
    b.ue(0);
    b.ue(0);
    b.ue(0);
    b.ue(0);
    b.ue(0);
    b.flag(false);
    b.flag(false);
    b.flag(false);
    b.flag(false);
    b.ue(0);
    b.flag(false);
    b.flag(false);
    b.flag(false);
    b.flag(true);
    vui(b, present, top, bottom);
    return b.nal({0x42, 0x01});
}

TEST(VideoVui, H264PreservesBothFieldSitesAndColorCodes)
{
    for (uint32_t top = 0; top < 6; ++top) {
        const auto nal = h264(true, top, 5 - top);
        H264ParamSets sets = {};
        ASSERT_EQ(h264_parse_sps(&sets, nal.data(), nal.size(), nullptr), 0);
        const auto &s = sets.sps[0];
        ASSERT_TRUE(s.chroma_location);
        EXPECT_EQ(s.chroma_top, top);
        EXPECT_EQ(s.chroma_bottom, 5 - top);
        EXPECT_EQ(s.colour_primaries, 9);
        EXPECT_EQ(s.transfer, 16);
        EXPECT_EQ(s.matrix, 9);
    }
}

TEST(VideoVui, HevcPreservesBothFieldSitesAndColorCodes)
{
    for (uint32_t top = 0; top < 6; ++top) {
        const auto nal = hevc(true, top, 5 - top);
        HEVCParams params = {};
        ASSERT_EQ(hevc_parse_sps(&params, nal.data(), nal.size()), 0);
        const auto &s = params.sps[0];
        ASSERT_TRUE(s.chroma_location);
        EXPECT_EQ(s.chroma_top, top);
        EXPECT_EQ(s.chroma_bottom, 5 - top);
        EXPECT_EQ(s.colour_primaries, 9);
        EXPECT_EQ(s.transfer, 16);
        EXPECT_EQ(s.matrix, 9);
    }
}

TEST(VideoVui, AbsentSitesDoNotBecomeExplicitLeftChroma)
{
    const auto a = h264(false, 0, 0), b = hevc(false, 0, 0);
    H264ParamSets sets = {};
    HEVCParams params = {};
    ASSERT_EQ(h264_parse_sps(&sets, a.data(), a.size(), nullptr), 0);
    ASSERT_EQ(hevc_parse_sps(&params, b.data(), b.size()), 0);
    EXPECT_FALSE(sets.sps[0].chroma_location);
    EXPECT_FALSE(params.sps[0].chroma_location);
}
}
