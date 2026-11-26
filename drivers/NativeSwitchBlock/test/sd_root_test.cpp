// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <set>
#include <string>

#include "../SwitchSdRoot.h"

namespace {
using Sector = std::array<uint8_t, 512>;

void put(uint8_t *p, uint64_t n, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = static_cast<uint8_t>(n >> (8 * i));
}

Sector header(uint64_t lba, uint32_t count, uint32_t size)
{
    Sector h{};
    memcpy(h.data(), "EFI PART", 8);
    put(h.data() + 72, lba, 8);
    put(h.data() + 80, count, 4);
    put(h.data() + 84, size, 4);
    return h;
}

bool reads(const Sector &h)
{
    SwitchSdGptArray array;
    return switchSdGptArray(h.data(), &array);
}

void named(uint8_t *entry, const char *name, size_t units)
{
    memset(entry + 56, 0, 72);
    for (size_t i = 0; i < units; ++i)
        entry[56 + 2 * i] = static_cast<uint8_t>(name[i]);
}
}  // namespace

TEST(SdRoot, ReadsHeadersWhoseArrayFitsSixtyFourKiBBelowLba2To32)
{
    SwitchSdGptArray array;
    ASSERT_TRUE(switchSdGptArray(header(2, 11, 128).data(), &array));
    EXPECT_EQ(array.lba, 2u);
    EXPECT_EQ(array.count, 11u);
    EXPECT_EQ(array.size, 128u);

    EXPECT_TRUE(reads(header(2, 512, 128)));
    EXPECT_TRUE(reads(header(2, 128, 512)));
    EXPECT_TRUE(reads(header(UINT32_MAX, 1, 136)));
    EXPECT_FALSE(reads(header(2, 513, 128)));
    EXPECT_FALSE(reads(header(2, 129, 512)));
    EXPECT_FALSE(reads(header(2, 0, 128)));
    for (const uint32_t size : {0u, 120u, 132u, 520u})
        EXPECT_FALSE(reads(header(2, 1, size))) << size;
    for (const uint64_t lba : {uint64_t{0}, uint64_t{1}, uint64_t{UINT32_MAX} + 1})
        EXPECT_FALSE(reads(header(lba, 1, 128))) << lba;
    Sector bad = header(2, 11, 128);
    bad[7] = 'S';
    EXPECT_FALSE(reads(bad));
}

TEST(SdRoot, ReadsEveryDeclaredEntrysSectorBelowLba2To32)
{
    const auto sectors = [](uint64_t lba, uint32_t count, uint32_t size) {
        return switchSdGptArraySectors(SwitchSdGptArray{lba, count, size});
    };
    EXPECT_EQ(sectors(2, 10, 128), 3u);
    EXPECT_EQ(sectors(2, 11, 128), 3u);
    EXPECT_EQ(sectors(2, 12, 128), 3u);
    EXPECT_EQ(sectors(2, 13, 128), 4u);
    EXPECT_EQ(sectors(2, 128, 128), 32u);
    EXPECT_EQ(sectors(2, 512, 128), 128u);
    EXPECT_EQ(sectors(2, 1, 512), 1u);
    EXPECT_EQ(sectors(2, 11, 136), 3u);
    EXPECT_EQ(sectors(UINT32_MAX, 4, 128), 1u);
    EXPECT_EQ(sectors(UINT32_MAX, 5, 128), 0u);
    EXPECT_EQ(sectors(UINT32_MAX - 2, 11, 128), 3u);
    EXPECT_EQ(sectors(UINT32_MAX - 1, 11, 128), 0u);
    EXPECT_EQ(sectors(UINT32_MAX - 127, 512, 128), 128u);
    EXPECT_EQ(sectors(UINT32_MAX - 126, 512, 128), 0u);
}

TEST(SdRoot, SharedVolumeOnAnotherCardIsRefusedAndAFirstCidIsTaken)
{
    std::array<uint8_t, 16> booted{}, same{}, other{};
    for (unsigned i = 0; i < 16; ++i)
        booted[i] = same[i] = other[i] = static_cast<uint8_t>(0x10 + i);
    same[15] ^= 0xff;
    other[14] ^= 1;
    EXPECT_EQ(switchSdFilesStatus(SWITCH_GPT_OK, true, booted.data(), same.data()), SWITCH_GPT_OK);
    EXPECT_EQ(switchSdFilesStatus(SWITCH_GPT_OK, true, booted.data(), other.data()), kSwitchSdFilesOtherCard);
    EXPECT_EQ(switchSdFilesStatus(SWITCH_GPT_OK, false, booted.data(), other.data()), SWITCH_GPT_OK);
    EXPECT_EQ(switchSdFilesStatus(SWITCH_GPT_OK, false, booted.data(), same.data()), SWITCH_GPT_OK);
    for (int lookup = SWITCH_GPT_ARGUMENT; lookup <= SWITCH_GPT_AMBIGUOUS; ++lookup) {
        for (const bool haveCid : {false, true}) {
            EXPECT_EQ(switchSdFilesStatus(lookup, haveCid, booted.data(), same.data()), lookup);
            EXPECT_EQ(switchSdFilesStatus(lookup, haveCid, booted.data(), other.data()), lookup);
        }
    }
}

TEST(SdRoot, MatchesTheWholeNameOnly)
{
    std::array<uint8_t, 128> entry{};
    named(entry.data(), "iOS", 3);
    EXPECT_TRUE(switchSdGptNamed(entry.data(), "iOS"));
    for (const char *other : {"iO", "iOS-setup", "ios", ""})
        EXPECT_FALSE(switchSdGptNamed(entry.data(), other)) << other;
    named(entry.data(), "iOS-setup", 9);
    EXPECT_FALSE(switchSdGptNamed(entry.data(), "iOS"));
    named(entry.data(), "iOS", 3);
    entry[56 + 1] = 1;
    EXPECT_FALSE(switchSdGptNamed(entry.data(), "iOS"));
    named(entry.data(), "iOS", 3);
    entry[56 + 2 * 3 + 1] = 0x20;
    EXPECT_FALSE(switchSdGptNamed(entry.data(), "iOS"));
    const std::string full(36, 'x');
    named(entry.data(), full.c_str(), 36);
    EXPECT_TRUE(switchSdGptNamed(entry.data(), full.c_str()));
    EXPECT_FALSE(switchSdGptNamed(entry.data(), (full + "x").c_str()));
}

TEST(SdRoot, EverySharedVolumeStatusHasItsOwnWord)
{
    std::set<std::string> words;
    for (int status = SWITCH_GPT_ARGUMENT; status <= SWITCH_GPT_AMBIGUOUS; ++status)
        words.insert(switchSdFilesReason(status));
    words.insert(switchSdFilesReason(kSwitchSdFilesOtherCard));
    EXPECT_EQ(words.size(), static_cast<size_t>(SWITCH_GPT_AMBIGUOUS) + 1);
    EXPECT_EQ(words.count("unknown"), 0u);
    for (const std::string &word : words)
        EXPECT_EQ(word.find(' '), std::string::npos) << word;
    EXPECT_STREQ(switchSdFilesReason(SWITCH_GPT_MBR), "mbr");
    EXPECT_STREQ(switchSdFilesReason(SWITCH_GPT_PROBE), "probe");
    EXPECT_STREQ(switchSdFilesReason(SWITCH_GPT_NOT_FOUND), "not-found");
    EXPECT_STREQ(switchSdFilesReason(kSwitchSdFilesOtherCard), "other-card");
    EXPECT_STREQ(switchSdFilesReason(SWITCH_GPT_AMBIGUOUS + 1), "unknown");
}
