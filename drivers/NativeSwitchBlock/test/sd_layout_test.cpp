// SPDX-License-Identifier: BSD-2-Clause-Patent
// The owner's 1 TB card (install/testdata/switch-card-1tb.json: its tables before the first iOS install) in the layouts
// the driver boots: iOS in Android's old userdata (entry 8), and iOS appended as entry 10 in the FAT's freed tail with
// hos_data and MBR slot 0 shrunk to match. setup()'s root rule (SwitchSdRoot.h) runs on the primary GPT as setup()
// reads it; the shared volume's selection then gets the root it found.
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "../SwitchSdRoot.h"
#include "bazel/testing/require.h"
#include "bazel/testing/source_text.h"
#include "sd_card_model.h"

using namespace switch_sd_test;

namespace {
constexpr uint64_t kCardSectors = UINT64_C(1023871549440) / 512;
constexpr uint64_t kBackupEntries = kCardSectors - 33;
constexpr uint64_t kAlign = 0x8000, kIosSectors = UINT64_C(64) << 21;
constexpr uint64_t kFatFirst = 32768, kOldFatEnd = 1730705408 + kFatFirst;
constexpr uint64_t kNewFatEnd = (kOldFatEnd - kIosSectors) / kAlign * kAlign;
constexpr uint64_t kOldFatSectors = kOldFatEnd - kFatFirst, kNewFatSectors = kNewFatEnd - kFatFirst;
constexpr uint64_t kUserdataFirst = 1810368512, kUserdataLast = 1877477375;
static_assert(kNewFatEnd == 1596489728 && kNewFatEnd % kAlign == 0 && kOldFatEnd - kNewFatEnd >= kIosSectors);
static_assert(kNewFatSectors == 1596456960 && kOldFatEnd - 1 == 1730738175);
// Ten 128-byte entries end inside LBA 4, so a count of 10 still reads entry 10's bytes.
static_assert((10 * 128 + 511) / 512 == (11 * 128 + 511) / 512);

constexpr std::array<uint8_t, 16> linuxType = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                               0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
constexpr std::array<uint8_t, 16> iosGuid = {0x5a, 0x1e, 0x00, 0x10, 0xc0, 0xde, 0x4a, 0x11,
                                             0x9f, 0x00, 0x53, 0x77, 0x69, 0x74, 0x63, 0x68};
constexpr uint8_t kHosDataGuid0 = 0x6a;

uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}

std::string base64(std::string_view text)
{
    static constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    uint32_t bits = 0;
    unsigned have = 0;
    for (const char c : text) {
        if (c == '=')
            break;
        const size_t value = digits.find(c);
        REQUIRE(value != std::string_view::npos);
        bits = bits << 6 | static_cast<uint32_t>(value);
        have += 6;
        if (have >= 8) {
            have -= 8;
            out.push_back(static_cast<char>(bits >> have & 0xff));
        }
    }
    return out;
}

uint64_t number(const std::string &json, size_t at, std::string_view key)
{
    const size_t found = json.find(key, at);
    REQUIRE(found != std::string::npos);
    return std::stoull(json.substr(found + key.size(), 24));
}

class OwnerCard : public Card {
public:
    OwnerCard()
    {
        sectors.clear();
        setCapacity(kCardSectors);
        const std::string json = switch_ios::test::SourceText("../../../install/testdata/switch-card-1tb.json");
        REQUIRE(number(json, 0, "\"disk_bytes\": ") == kCardSectors * 512);
        unsigned runs = 0;
        for (size_t at = json.find("\"lba\": "); at != std::string::npos; at = json.find("\"lba\": ", at + 1)) {
            const uint64_t lba = number(json, at, "\"lba\": "), count = number(json, at, "\"count\": ");
            const size_t start = json.find("\"base64\": \"", at) + 11;
            const std::string bytes = base64(std::string_view(json).substr(start, json.find('"', start) - start));
            REQUIRE(bytes.size() == count * 512);
            for (uint64_t i = 0; i < count; ++i)
                memcpy(sectors[lba + i].data(), bytes.data() + i * 512, 512);
            ++runs;
        }
        REQUIRE(runs == 5 && sectors.size() == 9);
        handoffMbr();
    }

    uint8_t *entry(uint32_t index)
    {
        return sectors[2 + index / 4].data() + (index % 4) * 128;
    }

    void name(uint32_t index, const std::array<uint8_t, 16> &type, const char *text)
    {
        uint8_t *e = entry(index);
        memcpy(e, type.data(), 16);
        memset(e + 56, 0, 72);
        for (unsigned i = 0; text[i]; ++i)
            e[56 + 2 * i] = static_cast<uint8_t>(text[i]);
    }

    void extent(uint32_t index, uint64_t first, uint64_t last)
    {
        put(entry(index) + 32, first, 8);
        put(entry(index) + 40, last, 8);
    }

    void seal(uint32_t count)
    {
        std::array<uint8_t, 3 * 512> array{};
        for (unsigned s = 0; s < 3; ++s) {
            sectors[kBackupEntries + s] = sectors[2 + s];
            memcpy(array.data() + s * 512, sectors[2 + s].data(), 512);
        }
        const uint32_t crc = switch_sd_crc32(array.data(), count * 128);
        for (const uint64_t lba : {uint64_t{1}, kCardSectors - 1}) {
            uint8_t *h = sectors[lba].data();
            put(h + 80, count);
            put(h + 88, crc);
            put(h + 16, 0);
            put(h + 16, switch_sd_crc32(h, 92));
        }
    }

    void oldLayout()
    {
        name(8, apfsType, "iOS");
        seal(10);
        writeFat32Vbr(sectors[kFatFirst], static_cast<uint32_t>(kOldFatSectors), 64);
    }

    void newLayout(uint32_t count = 11)
    {
        oldLayout();
        name(8, linuxType, "userdata");
        put(entry(0) + 40, kNewFatEnd - 1, 8);
        name(10, apfsType, "iOS");
        memcpy(entry(10) + 16, iosGuid.data(), 16);
        extent(10, kNewFatEnd, kOldFatEnd - 1);
        seal(count);
        shrinkMbr(0x0c);
        writeFat32Vbr(sectors[kFatFirst], static_cast<uint32_t>(kOldFatSectors), 64);
        put(sectors[kFatFirst].data() + 32, kNewFatSectors);
    }

    void shrinkMbr(uint8_t type, uint64_t sectorCount = kNewFatSectors)
    {
        mbrEntry(sectors[0].data() + 446, type, kFatFirst, static_cast<uint32_t>(sectorCount));
        handoffMbr();
    }

    void cutExfat(uint64_t volumeSectors, bool cutClusters = true, uint8_t mbrType = 0x07)
    {
        Sector &boot = sectors[kFatFirst];
        writeExfatVbr(boot, static_cast<uint32_t>(kOldFatSectors), 9, 8);
        const uint64_t heap = get32(boot.data() + 88);
        put(boot.data() + 72, volumeSectors, 8);
        if (cutClusters)
            put(boot.data() + 92, (volumeSectors - heap) >> 8);
        shrinkMbr(mbrType);
    }

    SwitchSdRoot root()
    {
        SwitchSdGptArray array;
        REQUIRE(switchSdGptArray(sectors.at(1).data(), &array));
        const uint32_t entrySectors = switchSdGptArraySectors(array);
        REQUIRE(entrySectors != 0);
        std::vector<uint8_t> entries(kSwitchSdGptArrayBytes);
        for (uint32_t s = 0; s < entrySectors; ++s)
            REQUIRE(read(this, array.lba + s, entries.data() + static_cast<size_t>(s) * 512) == 0);
        return switchSdFindRoot(entries.data(), array, apfsType.data(), "iOS");
    }

    int files(const SwitchSdWindow &rootWindow)
    {
        return switch_sd_files_select(read, this, page.data(), rootWindow.first, rootWindow.count, &work, &slice);
    }
};

void expectWindow(const SwitchSdWindow &window, uint64_t first, uint64_t last)
{
    EXPECT_EQ(window.first, first);
    EXPECT_EQ(window.count, last - first + 1);
}

void expectHosData(const switch_sd_files_slice &slice, uint64_t sectors, bool exfat)
{
    EXPECT_EQ(slice.first_sector, kFatFirst);
    EXPECT_EQ(slice.sector_count, sectors);
    EXPECT_EQ(slice.exfat, exfat);
    EXPECT_EQ(slice.unique_guid[0], kHosDataGuid0);
}
}  // namespace

TEST(SdLayout, FixtureResealsToItsRecordedCrcsAndHasNoRootBeforeInstall)
{
    OwnerCard card;
    const auto recorded = card.sectors;
    card.seal(10);
    EXPECT_EQ(card.sectors, recorded);
    EXPECT_EQ(card.root().found, 0u);
}

TEST(SdLayout, OldLayoutBootsEntry8AndSharesTheWholeFat)
{
    OwnerCard card;
    card.oldLayout();
    const SwitchSdRoot root = card.root();
    ASSERT_TRUE(root.unique());
    expectWindow(root.window, kUserdataFirst, kUserdataLast);
    ASSERT_EQ(card.files(root.window), SWITCH_GPT_OK);
    expectHosData(card.slice, kOldFatSectors, false);
}

TEST(SdLayout, NewLayoutBootsAppendedEntry10)
{
    OwnerCard card;
    card.newLayout();
    const SwitchSdRoot root = card.root();
    ASSERT_TRUE(root.unique());
    expectWindow(root.window, kNewFatEnd, kOldFatEnd - 1);
    EXPECT_EQ(memcmp(card.entry(8), linuxType.data(), 16), 0);
    EXPECT_TRUE(switchSdGptNamed(card.entry(8), "userdata"));
}

TEST(SdLayout, RootRuleWalksOnlyTheDeclaredEntries)
{
    OwnerCard card;
    card.newLayout(10);
    ASSERT_EQ(memcmp(card.entry(10), apfsType.data(), 16), 0);
    EXPECT_EQ(card.root().found, 0u);
    EXPECT_FALSE(card.root().unique());
}

TEST(SdLayout, RefusesTwoRoots)
{
    for (const auto &[type, name] : {std::pair{apfsType, "iOS"}, {linuxType, "iOS"}, {apfsType, "userdata"}}) {
        OwnerCard card;
        card.newLayout();
        card.name(8, type, name);
        card.seal(11);
        EXPECT_EQ(card.root().found, 2u) << name;
        EXPECT_FALSE(card.root().unique()) << name;
    }
}

TEST(SdLayout, LinuxIosSetupIsNotARoot)
{
    OwnerCard card;
    card.newLayout();
    card.name(10, linuxType, "iOS-setup");
    card.seal(11);
    EXPECT_EQ(card.root().found, 0u);
    card.name(8, apfsType, "iOS");
    card.seal(11);
    const SwitchSdRoot root = card.root();
    ASSERT_TRUE(root.unique());
    expectWindow(root.window, kUserdataFirst, kUserdataLast);
}

TEST(SdLayout, SkipsRootsOneCardCommandCannotAddress)
{
    constexpr uint64_t high = UINT64_C(0xffff0000);
    constexpr uint64_t beyond = uint64_t{UINT32_MAX} + 1;
    for (const auto &[first, last] : {std::pair{high, beyond}, {0, kOldFatEnd - 1}, {high, high - 1}}) {
        OwnerCard card;
        card.newLayout();
        card.extent(10, first, last);
        EXPECT_EQ(card.root().found, 0u) << first << ".." << last;
        card.name(8, apfsType, "iOS");
        const SwitchSdRoot beside = card.root();
        ASSERT_TRUE(beside.unique()) << first << ".." << last;
        expectWindow(beside.window, kUserdataFirst, kUserdataLast);
    }
    OwnerCard card;
    card.newLayout();
    card.extent(10, high, UINT32_MAX);
    const SwitchSdRoot root = card.root();
    ASSERT_TRUE(root.unique());
    expectWindow(root.window, high, UINT32_MAX);
}

TEST(SdLayout, FilesSelectsTheCutFat32)
{
    OwnerCard card;
    card.newLayout();
    ASSERT_EQ(card.files(card.root().window), SWITCH_GPT_OK);
    expectHosData(card.slice, kNewFatSectors, false);

    put(card.sectors[kFatFirst].data() + 32, kNewFatSectors - kAlign);
    ASSERT_EQ(card.files(card.root().window), SWITCH_GPT_OK);
    expectHosData(card.slice, kNewFatSectors, false);
}

TEST(SdLayout, FilesSelectsTheCutExfat)
{
    for (const uint8_t mbrType : {0x07, 0x0c}) {
        OwnerCard card;
        card.newLayout();
        card.cutExfat(kNewFatSectors, true, mbrType);
        ASSERT_EQ(card.files(card.root().window), SWITCH_GPT_OK) << int{mbrType};
        expectHosData(card.slice, kNewFatSectors, true);
    }
}

TEST(SdLayout, FilesSelectsUnderAPureProtectiveMbr)
{
    OwnerCard card;
    card.newLayout();
    card.setProtectiveMbr(static_cast<uint32_t>(kCardSectors - 1));
    ASSERT_EQ(card.files(card.root().window), SWITCH_GPT_OK);
    expectHosData(card.slice, kNewFatSectors, false);
}

TEST(SdLayout, FilesRefusesAnMbrSlotLeftAtTheOldLength)
{
    OwnerCard card;
    card.newLayout();
    card.shrinkMbr(0x0c, kOldFatSectors);
    EXPECT_EQ(card.files(card.root().window), SWITCH_GPT_MBR);
    EXPECT_EQ(card.slice.sector_count, 0u);
}

TEST(SdLayout, FilesRefusesAFilesystemStillSizedForTheOldExtent)
{
    OwnerCard card;
    card.newLayout();
    put(card.sectors[kFatFirst].data() + 32, kOldFatSectors);
    EXPECT_EQ(card.files(card.root().window), SWITCH_GPT_PROBE);

    card.cutExfat(kOldFatSectors);
    EXPECT_EQ(card.files(card.root().window), SWITCH_GPT_PROBE);

    card.cutExfat(kNewFatSectors, false);
    EXPECT_EQ(card.files(card.root().window), SWITCH_GPT_PROBE);
}
