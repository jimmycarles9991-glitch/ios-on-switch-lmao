// SPDX-License-Identifier: MIT
// raw/runtime/raw_archive.h: the binary archive files the device opens where m2v's reader does not, iOS 27's thin Apple GPU
// binaries (RenderBox's and IconRendering's, which boot 13's RenderBox logged precondition failures over), and no other.
#include "../raw/runtime/raw_archive.h"

#include <gtest/gtest.h>

namespace {

// 24B119's IconRendering.framework/binary.metallib, its first 32 bytes (RenderBox's archive.metallib differs from byte 20 on)
constexpr uint8_t kIconRendering[32] = {0xcf, 0xfa, 0xed, 0xfe, 0x13, 0x00, 0x00, 0x01, 0xd2, 0x00, 0x00,
                                        0x00, 0x0d, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x60, 0x03,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

TEST(RawArchive, IosTwentySevensFrameworkArchivesAreAppleGpuBinaries)
{
    EXPECT_TRUE(raw::appleGpuBinary(kIconRendering, sizeof kIconRendering));
    EXPECT_TRUE(raw::appleGpuBinary(kIconRendering, 8));
}

TEST(RawArchive, EverythingElseIsM2vsToRead)
{
    const uint8_t metallib[8] = {'M', 'T', 'L', 'B', 1, 0x80, 2, 0};
    const uint8_t m2v[8] = {'M', '2', 'V', 'B', 'A', 'R', 'C', 'H'};
    const uint8_t arm64[8] = {0xcf, 0xfa, 0xed, 0xfe, 0x0c, 0x00, 0x00, 0x01};
    const uint8_t air64[8] = {0xcf, 0xfa, 0xed, 0xfe, 0x17, 0x00, 0x00, 0x01};
    const uint8_t text[8] = {'h', 'e', 'l', 'l', 'o', '\n', 0, 0};
    for (const uint8_t *head : {metallib, m2v, arm64, air64, text})
        EXPECT_FALSE(raw::appleGpuBinary(head, 8));
    EXPECT_FALSE(raw::appleGpuBinary(kIconRendering, 7)) << "a file shorter than the header";
}

}  // namespace
