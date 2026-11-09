// The tree's .originals (kits/SwitchPretranslated/switch_pretranslated_originals.h): what the 12377 kernel accepts,
// refuses and finds for an installed copy of a staged system app.
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <vector>

#include "bazel/testing/require.h"
#include "kits/SwitchPretranslated/switch_pretranslated_originals.h"
#include "kits/SwitchPretranslated/test/index_builder.h"

namespace {

using switch_pt_test::encode;
using switch_pt_test::File;
using switch_pt_test::put32;
using CDHash = std::array<uint8_t, SWITCH_PT_CDHASH>;

CDHash cdhash(uint8_t first, uint8_t last = 0)
{
    CDHash h{};
    h[0] = first;
    h[SWITCH_PT_CDHASH - 1] = last;
    return h;
}

struct Pair {
    CDHash original;
    CDHash translated;
};

std::vector<uint8_t> originals(const std::vector<Pair> &pairs, uint32_t version = SWITCH_PT_ORIGINALS_VERSION)
{
    std::vector<uint8_t> out = {'S', 'W', 'O', 'R'};
    put32(out, version);
    put32(out, static_cast<uint32_t>(pairs.size()));
    put32(out, 0);
    for (const Pair &p : pairs) {
        out.insert(out.end(), p.original.begin(), p.original.end());
        out.insert(out.end(), p.translated.begin(), p.translated.end());
    }
    return out;
}

enum switch_pt_error parse(const std::vector<uint8_t> &raw, struct switch_pt_originals *out = nullptr)
{
    struct switch_pt_originals scratch;
    return switch_pt_originals_parse(raw.data(), raw.size(), out ? out : &scratch);
}

const std::vector<File> kFiles = {
    {"/Applications/Preferences.app/Preferences", 10, 100, cdhash(1)},
    {"/private/var/staged_system_apps/FaceTime.app/FaceTime", 20, 200, cdhash(2)},
    {"/private/var/staged_system_apps/Music.app/Music", 30, 300, cdhash(3)},
};
const std::vector<Pair> kPairs = {{cdhash(0x40), cdhash(3)}, {cdhash(0x90, 1), cdhash(2)}};

struct Linked {
    std::vector<uint8_t> index_raw, originals_raw;
    struct switch_pt_index index;
    struct switch_pt_originals originals;
    std::vector<uint32_t> files;
};

Linked link(const std::vector<File> &files, const std::vector<Pair> &pairs, enum switch_pt_error want = SWITCH_PT_OK)
{
    Linked l;
    l.index_raw = encode(files, {1000});
    l.originals_raw = originals(pairs);
    REQUIRE(switch_pt_index_parse(l.index_raw.data(), l.index_raw.size(), &l.index) == SWITCH_PT_OK);
    REQUIRE(parse(l.originals_raw, &l.originals) == SWITCH_PT_OK);
    l.files.assign(l.originals.count, 0);
    EXPECT_EQ(switch_pt_originals_link(&l.index, &l.originals, l.files.data()), want);
    return l;
}

TEST(Originals, AnInstalledCopyFindsItsTranslationsRecord)
{
    const Linked l = link(kFiles, kPairs);
    const CDHash facetime = cdhash(0x90, 1), music = cdhash(0x40);
    EXPECT_EQ(switch_pt_original_file(&l.originals, l.files.data(), facetime.data()), 1);
    EXPECT_EQ(switch_pt_original_file(&l.originals, l.files.data(), music.data()), 2);
}

TEST(Originals, AnyOtherCdhashFindsNothing)
{
    const Linked l = link(kFiles, kPairs);
    for (const CDHash &other : {cdhash(0), cdhash(0x40, 1), cdhash(0x90), cdhash(2), cdhash(0xff, 0xff)}) {
        EXPECT_EQ(switch_pt_original_file(&l.originals, l.files.data(), other.data()), -1);
    }
    const Linked none = link(kFiles, {});
    EXPECT_EQ(switch_pt_original_file(&none.originals, none.files.data(), cdhash(0x40).data()), -1);
}

TEST(Originals, ATranslationWithoutARecordIsRefused)
{
    link(kFiles, {{cdhash(0x40), cdhash(9)}}, SWITCH_PT_RECORD_BAD);
}

TEST(Originals, HeaderFieldsAreChecked)
{
    EXPECT_EQ(parse(originals(kPairs)), SWITCH_PT_OK);
    EXPECT_EQ(parse(originals(kPairs, 2)), SWITCH_PT_HEADER_BAD);
    std::vector<uint8_t> raw = originals(kPairs);
    raw[0] = 'X';
    EXPECT_EQ(parse(raw), SWITCH_PT_HEADER_BAD);
    raw = originals(kPairs);
    raw[12] = 1;
    EXPECT_EQ(parse(raw), SWITCH_PT_HEADER_BAD);
    raw = originals({});
    raw[8] = 0;
    raw[10] = 2;
    EXPECT_EQ(parse(raw), SWITCH_PT_HEADER_BAD);
}

TEST(Originals, EveryTruncationAndExtensionIsRefused)
{
    const std::vector<uint8_t> raw = originals(kPairs);
    for (size_t size = 0; size < raw.size(); ++size) {
        const std::vector<uint8_t> cut(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(size));
        EXPECT_NE(parse(cut), SWITCH_PT_OK) << size;
    }
    std::vector<uint8_t> longer = raw;
    longer.push_back(0);
    EXPECT_EQ(parse(longer), SWITCH_PT_SIZE_BAD);
}

TEST(Originals, OriginalsAscendAndAreUnique)
{
    EXPECT_EQ(parse(originals({kPairs[1], kPairs[0]})), SWITCH_PT_ORDER_BAD);
    EXPECT_EQ(parse(originals({kPairs[0], kPairs[0]})), SWITCH_PT_ORDER_BAD);
    EXPECT_EQ(parse(originals({kPairs[0], {kPairs[0].original, cdhash(2)}})), SWITCH_PT_ORDER_BAD);
}

TEST(Originals, ByteFlipsNeverFindOutsideTheIndex)
{
    const std::vector<uint8_t> index_raw = encode(kFiles, {1000});
    struct switch_pt_index index;
    REQUIRE(switch_pt_index_parse(index_raw.data(), index_raw.size(), &index) == SWITCH_PT_OK);
    const std::vector<uint8_t> raw = originals(kPairs);
    srand(1);
    for (int round = 0; round < 20000; ++round) {
        std::vector<uint8_t> bad = raw;
        for (int flips = 1 + rand() % 3; flips; --flips) {
            bad[static_cast<size_t>(rand()) % bad.size()] = static_cast<uint8_t>(rand());
        }
        struct switch_pt_originals o;
        if (parse(bad, &o) != SWITCH_PT_OK) {
            continue;
        }
        std::vector<uint32_t> files(o.count, 0);
        if (switch_pt_originals_link(&index, &o, files.data()) != SWITCH_PT_OK) {
            continue;
        }
        for (uint32_t i = 0; i < o.count; ++i) {
            const int64_t file = switch_pt_original_file(&o, files.data(), switch_pt_original_at(&o, i));
            REQUIRE(file >= 0);
            EXPECT_LT(static_cast<uint32_t>(file), index.count);
        }
    }
}

}  // namespace
