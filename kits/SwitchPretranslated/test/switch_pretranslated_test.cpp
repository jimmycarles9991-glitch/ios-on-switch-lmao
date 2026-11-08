// The pretranslated tree's index and tables (kits/SwitchPretranslated): what the 12377 kernel accepts, refuses and
// finds.
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "bazel/testing/require.h"
#include "kits/SwitchPretranslated/switch_pretranslated.h"
#include "kits/SwitchPretranslated/test/index_builder.h"

namespace {

using switch_pt_test::encode;
using switch_pt_test::File;
using switch_pt_test::set32;
using switch_pt_test::set64;

uint32_t fnv(const char *name, uint32_t length)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < length; ++i)
        h = (h ^ static_cast<uint8_t>(name[i])) * 16777619u;
    return h ? h : 1;
}

uint32_t collide(const char *, uint32_t)
{
    return 7;
}

const std::vector<File> kFiles = {
    {"/System/Library/a", 10, 100},
    {"/usr/libexec/a", 20, 200},
    {"/usr/libexec/b", 30, 300, {{7, 7, 7}}},
    {"/usr/sbin/b", 40, 400},
};
const std::vector<uint64_t> kDirs = {1000, 1001, 1002, 1003, 1004, 1005};

enum switch_pt_error parse(const std::vector<uint8_t> &raw, struct switch_pt_index *index = nullptr)
{
    struct switch_pt_index scratch;
    return switch_pt_index_parse(raw.data(), raw.size(), index ? index : &scratch);
}

struct Built {
    std::vector<uint8_t> raw;
    std::vector<uint64_t> storage;
    struct switch_pt_tables tables;
    enum switch_pt_error error;
};

Built build(const std::vector<File> &files, const std::vector<uint64_t> &dirs, switch_pt_hash_fn hash = fnv)
{
    Built b;
    b.raw = encode(files, dirs);
    struct switch_pt_index index;
    b.error = switch_pt_index_parse(b.raw.data(), b.raw.size(), &index);
    if (b.error != SWITCH_PT_OK)
        return b;
    const size_t bytes = switch_pt_tables_bytes(&index, 1);
    b.storage.assign((bytes + 7) / 8, 0);
    b.error = switch_pt_tables_build(&b.tables, &index, 1, b.storage.data(), bytes, hash);
    return b;
}

TEST(Index, ParsesWhatTheInstallerWrites)
{
    const std::vector<uint8_t> raw = encode(kFiles, kDirs);
    struct switch_pt_index index;
    REQUIRE(parse(raw, &index) == SWITCH_PT_OK);
    EXPECT_EQ(index.count, 4u);
    EXPECT_EQ(index.dirs, 6u);
    const struct switch_pt_file file = switch_pt_file_at(&index, 2);
    EXPECT_STREQ(file.path, "/usr/libexec/b");
    EXPECT_EQ(file.length, 14u);
    EXPECT_EQ(file.system_id, 30u);
    EXPECT_EQ(file.data_id, 300u);
    EXPECT_EQ(file.cdhash[0], 7);
    EXPECT_EQ(file.cdhash[1], 7);
    EXPECT_EQ(file.cdhash[3], 0);
    EXPECT_EQ(switch_pt_directory_at(&index, 5), 1005u);
}

TEST(Index, EveryTruncationIsRefused)
{
    const std::vector<uint8_t> raw = encode(kFiles, kDirs);
    for (size_t size = 0; size < raw.size(); ++size) {
        std::vector<uint8_t> cut(raw.begin(), raw.begin() + static_cast<long>(size));
        EXPECT_NE(parse(cut), SWITCH_PT_OK) << size;
    }
    std::vector<uint8_t> longer = raw;
    longer.push_back(0);
    EXPECT_EQ(parse(longer), SWITCH_PT_SIZE_BAD);
}

TEST(Index, OversizedCountsAreRefusedBeforeAnyRead)
{
    std::vector<uint8_t> raw = encode(kFiles, kDirs);
    for (uint32_t count : {5u, SWITCH_PT_MAX_FILES, SWITCH_PT_MAX_FILES + 1, 0xffffffffu}) {
        std::vector<uint8_t> bad = raw;
        set32(bad, 8, count);
        EXPECT_NE(parse(bad), SWITCH_PT_OK) << count;
        bad = raw;
        set32(bad, 12, count);
        EXPECT_NE(parse(bad), SWITCH_PT_OK) << count;
        bad = raw;
        set32(bad, 16, count);
        EXPECT_NE(parse(bad), SWITCH_PT_OK) << count;
    }
}

TEST(Index, HeaderFieldsAreChecked)
{
    const std::vector<uint8_t> raw = encode(kFiles, kDirs);
    for (size_t at : {0u, 4u, 20u}) {
        std::vector<uint8_t> bad = raw;
        bad[at] ^= 1;
        EXPECT_EQ(parse(bad), SWITCH_PT_HEADER_BAD) << at;
    }
}

TEST(Index, DuplicateOrUnsortedPathsAreRefused)
{
    EXPECT_EQ(parse(encode({{"/a", 1, 2}, {"/a", 3, 4}}, {5})), SWITCH_PT_ORDER_BAD);
    EXPECT_EQ(parse(encode({{"/b", 1, 2}, {"/a", 3, 4}}, {5})), SWITCH_PT_ORDER_BAD);
    EXPECT_EQ(parse(encode({{"/a/b", 1, 2}, {"/a", 3, 4}}, {5})), SWITCH_PT_ORDER_BAD);
    EXPECT_EQ(parse(encode({{"/a", 1, 2}, {"/a/b", 3, 4}}, {5})), SWITCH_PT_OK);
}

TEST(Index, PathsTheWriterRefusesAreRefused)
{
    for (const char *bad : {"relative", "/", "/trailing/", "/a//b", "/a/./b", "/a/../b", "/.", "/..", "/.index"}) {
        EXPECT_EQ(parse(encode({{bad, 1, 2}}, {5})), SWITCH_PT_PATH_BAD) << bad;
    }
    EXPECT_EQ(parse(encode({{"/" + std::string(SWITCH_PT_MAX_PATH - 2, 'x'), 1, 2}}, {5})), SWITCH_PT_OK);
    EXPECT_EQ(parse(encode({{"/" + std::string(SWITCH_PT_MAX_PATH - 1, 'x'), 1, 2}}, {5})), SWITCH_PT_PATH_BAD);
}

TEST(Index, RecordsMustPointAtTheirOwnPackedPath)
{
    std::vector<uint8_t> raw = encode(kFiles, kDirs);
    std::vector<uint8_t> bad = raw;
    set32(bad, SWITCH_PT_HEADER + SWITCH_PT_RECORD + 16, 0);
    EXPECT_EQ(parse(bad), SWITCH_PT_RECORD_BAD);
    bad = raw;
    set32(bad, SWITCH_PT_HEADER + 20, 1);
    EXPECT_EQ(parse(bad), SWITCH_PT_RECORD_BAD);
    bad = raw;
    set64(bad, SWITCH_PT_HEADER, 0);
    EXPECT_EQ(parse(bad), SWITCH_PT_RECORD_BAD);
    bad = raw;
    bad.back() = 'x';
    EXPECT_EQ(parse(bad), SWITCH_PT_PATH_BAD);
}

TEST(Index, DirectoriesAscendAndAreNotZero)
{
    EXPECT_EQ(parse(encode(kFiles, {1002, 1001})), SWITCH_PT_DIRECTORY_BAD);
    EXPECT_EQ(parse(encode(kFiles, {1001, 1001})), SWITCH_PT_DIRECTORY_BAD);
    EXPECT_EQ(parse(encode(kFiles, {0, 1001})), SWITCH_PT_DIRECTORY_BAD);
    EXPECT_EQ(parse(encode({}, {})), SWITCH_PT_OK);
}

TEST(Index, ByteFlipsNeverReadOutOfBounds)
{
    const std::vector<uint8_t> raw = encode(kFiles, kDirs);
    srand(1);
    for (int round = 0; round < 20000; ++round) {
        std::vector<uint8_t> bad = raw;
        for (int flips = 1 + rand() % 3; flips; --flips) {
            bad[static_cast<size_t>(rand()) % bad.size()] = static_cast<uint8_t>(rand());
        }
        struct switch_pt_index index;
        if (switch_pt_index_parse(bad.data(), bad.size(), &index) == SWITCH_PT_OK) {
            for (uint32_t i = 0; i < index.count; ++i) {
                EXPECT_EQ(std::strlen(switch_pt_file_at(&index, i).path), switch_pt_file_at(&index, i).length);
            }
        }
    }
}

TEST(Tables, DuplicateIdsAreRefused)
{
    EXPECT_EQ(build({{"/a", 1, 2}, {"/b", 1, 4}}, {5}).error, SWITCH_PT_DUPLICATE_ID);
    EXPECT_EQ(build({{"/a", 1, 2}, {"/b", 1, 2, {{7}}}}, {5}).error, SWITCH_PT_DUPLICATE_ID);
    EXPECT_EQ(build({{"/a", 1, 2}, {"/b", 3, 2}}, {5}).error, SWITCH_PT_DUPLICATE_ID);
    EXPECT_EQ(build({{"/a", 1, 2}}, {2}).error, SWITCH_PT_DUPLICATE_ID);
    EXPECT_EQ(build(kFiles, kDirs).error, SWITCH_PT_OK);
}

TEST(Tables, StorageMustFit)
{
    const std::vector<uint8_t> raw = encode(kFiles, kDirs);
    struct switch_pt_index index;
    REQUIRE(parse(raw, &index) == SWITCH_PT_OK);
    std::vector<uint64_t> storage(switch_pt_tables_bytes(&index, 0) / 8, 0);
    struct switch_pt_tables tables;
    EXPECT_EQ(switch_pt_tables_build(&tables, &index, 0, storage.data(), storage.size() * 8 - 1, fnv),
              SWITCH_PT_STORAGE_SHORT);
}

TEST(Tables, FindsSystemFilesByIdAndName)
{
    Built b = build(kFiles, kDirs);
    REQUIRE(b.error == SWITCH_PT_OK);
    EXPECT_TRUE(switch_pt_name_listed(&b.tables, fnv("b", 1), "b", 1));
    EXPECT_FALSE(switch_pt_name_listed(&b.tables, fnv("c", 1), "c", 1));
    EXPECT_FALSE(switch_pt_name_listed(&b.tables, fnv("libexec", 7), "libexec", 7));
    EXPECT_EQ(switch_pt_system_file(&b.tables, 30, fnv("b", 1), "b", 1), 2);
    EXPECT_EQ(switch_pt_system_file(&b.tables, 40, fnv("b", 1), "b", 1), 3);
    EXPECT_EQ(switch_pt_system_file(&b.tables, 40, fnv("a", 1), "a", 1), -1);
    EXPECT_EQ(switch_pt_system_file(&b.tables, 50, fnv("b", 1), "b", 1), -1);
    EXPECT_EQ(switch_pt_system_file(&b.tables, 0, fnv("b", 1), "b", 1), -1);
}

TEST(Tables, HardLinksAreFoundByEveryName)
{
    for (switch_pt_hash_fn hash : {fnv, collide}) {
        Built b = build({{"/usr/libexec/d", 1, 2}, {"/usr/sbin/d", 1, 2}, {"/usr/sbin/e", 1, 2}, {"/usr/sbin/f", 3, 4}},
                        {5}, hash);
        REQUIRE(b.error == SWITCH_PT_OK);
        EXPECT_EQ(switch_pt_system_file(&b.tables, 1, hash("d", 1), "d", 1), 0);
        EXPECT_EQ(switch_pt_system_file(&b.tables, 1, hash("e", 1), "e", 1), 2);
        EXPECT_EQ(switch_pt_system_file(&b.tables, 1, hash("f", 1), "f", 1), -1);
        EXPECT_EQ(switch_pt_system_file(&b.tables, 3, hash("e", 1), "e", 1), -1);
        uint32_t listed = 0;
        EXPECT_TRUE(switch_pt_map_get(&b.tables.data, 2, &listed));
        EXPECT_EQ(listed, 0u);
    }
}

TEST(Tables, NamesThatShareAHashStayApart)
{
    Built b = build(kFiles, kDirs, collide);
    REQUIRE(b.error == SWITCH_PT_OK);
    EXPECT_TRUE(switch_pt_name_listed(&b.tables, 7, "a", 1));
    EXPECT_TRUE(switch_pt_name_listed(&b.tables, 7, "b", 1));
    EXPECT_FALSE(switch_pt_name_listed(&b.tables, 7, "c", 1));
}

TEST(Tables, DataIdsTellFilesFromDirectories)
{
    Built b = build(kFiles, kDirs);
    REQUIRE(b.error == SWITCH_PT_OK);
    uint32_t kind = 0;
    EXPECT_TRUE(switch_pt_map_get(&b.tables.data, 300, &kind));
    EXPECT_EQ(kind, 2u);
    EXPECT_TRUE(switch_pt_map_get(&b.tables.data, 1003, &kind));
    EXPECT_EQ(kind, SWITCH_PT_DIRECTORY);
    EXPECT_FALSE(switch_pt_map_get(&b.tables.data, 30, &kind));
    EXPECT_TRUE(switch_pt_map_put(&b.tables.data, 77, SWITCH_PT_PROTECTED));
    EXPECT_FALSE(switch_pt_map_put(&b.tables.data, 77, SWITCH_PT_PROTECTED));
}

}  // namespace

namespace {

constexpr uint32_t kV0 = SWITCH_PT_VNODE(0), kV1 = SWITCH_PT_VNODE(1), kV2 = SWITCH_PT_VNODE(2),
                   kV3 = SWITCH_PT_VNODE(3);

TEST(Deny, NothingOutsideTheTreeIsRefused)
{
    for (int op = 0; op < SWITCH_PT_OPS; ++op) {
        for (uint32_t intent : {0u, SWITCH_PT_WRITES}) {
            EXPECT_FALSE(switch_pt_denied(static_cast<enum switch_pt_op>(op), 0, intent)) << op;
        }
    }
}

TEST(Deny, OpensAndMappingsOnlyWhenTheyWrite)
{
    for (enum switch_pt_op op : {SWITCH_PT_OPEN, SWITCH_PT_MMAP}) {
        EXPECT_FALSE(switch_pt_denied(op, kV0, 0));
        EXPECT_TRUE(switch_pt_denied(op, kV0, SWITCH_PT_WRITES));
        EXPECT_FALSE(switch_pt_denied(op, kV1 | kV2 | kV3, SWITCH_PT_WRITES));
    }
}

TEST(Deny, EveryChangeOfATreeFileOrDirectory)
{
    for (enum switch_pt_op op : {SWITCH_PT_TRUNCATE, SWITCH_PT_SETATTR, SWITCH_PT_SETEXTATTR, SWITCH_PT_DELETEEXTATTR,
                                 SWITCH_PT_CREATE, SWITCH_PT_MOUNT, SWITCH_PT_GRAFT}) {
        EXPECT_TRUE(switch_pt_denied(op, kV0, 0)) << op;
        EXPECT_FALSE(switch_pt_denied(op, kV1 | kV2 | kV3, 0)) << op;
    }
}

TEST(Deny, UnlinkOfTheTreeOrInsideIt)
{
    EXPECT_TRUE(switch_pt_denied(SWITCH_PT_UNLINK, kV0, 0));
    EXPECT_TRUE(switch_pt_denied(SWITCH_PT_UNLINK, kV1, 0));
    EXPECT_TRUE(switch_pt_denied(SWITCH_PT_UNLINK, kV0 | kV1, 0));
}

TEST(Deny, RenameFromIntoOverOrOfTheTree)
{
    for (uint32_t bit : {kV0, kV1, kV2, kV3})
        EXPECT_TRUE(switch_pt_denied(SWITCH_PT_RENAME, bit, 0)) << bit;
}

TEST(Deny, LinksAndExchangesEitherWay)
{
    for (enum switch_pt_op op : {SWITCH_PT_LINK, SWITCH_PT_EXCHANGEDATA}) {
        EXPECT_TRUE(switch_pt_denied(op, kV0, 0));
        EXPECT_TRUE(switch_pt_denied(op, kV1, 0));
    }
}

TEST(Deny, ClonesAndCopiesEitherWay)
{
    for (enum switch_pt_op op : {SWITCH_PT_CLONE, SWITCH_PT_COPYFILE}) {
        EXPECT_TRUE(switch_pt_denied(op, kV0, 0));
        EXPECT_TRUE(switch_pt_denied(op, kV1, 0));
        EXPECT_FALSE(switch_pt_denied(op, kV2, 0));
    }
}

TEST(Deny, FsctlAndIoctlOnlyWhenTheyWrite)
{
    EXPECT_FALSE(switch_pt_denied(SWITCH_PT_FSCTL, kV0, 0));
    EXPECT_TRUE(switch_pt_denied(SWITCH_PT_FSCTL, kV0, SWITCH_PT_WRITES));
    EXPECT_FALSE(switch_pt_denied(SWITCH_PT_FSCTL, kV1, SWITCH_PT_WRITES));
}

TEST(Deny, AnUnknownOpIsRefused)
{
    EXPECT_TRUE(switch_pt_denied(SWITCH_PT_OPS, 0, 0));
}

}  // namespace

namespace {

struct switch_pt_trust_facts trusted()
{
    struct switch_pt_trust_facts facts = {};
    facts.listed = true;
    facts.regular = true;
    facts.mode = 0755;
    facts.cdhash_pinned = true;
    facts.cdhash_matches = true;
    return facts;
}

TEST(Trust, AListedRootFileWithAPinnedCdhash)
{
    const struct switch_pt_trust_facts facts = trusted();
    EXPECT_TRUE(switch_pt_trusted(&facts));
    struct switch_pt_trust_facts setuid = trusted();
    setuid.mode = 04755;
    EXPECT_TRUE(switch_pt_trusted(&setuid));
}

TEST(Trust, EachMissingFactRefuses)
{
    const std::vector<void (*)(struct switch_pt_trust_facts &)> breaks = {
        [](struct switch_pt_trust_facts &f) {
            f.listed = false;
        },
        [](struct switch_pt_trust_facts &f) {
            f.regular = false;
        },
        [](struct switch_pt_trust_facts &f) {
            f.uid = 501;
        },
        [](struct switch_pt_trust_facts &f) {
            f.gid = 20;
        },
        [](struct switch_pt_trust_facts &f) {
            f.mode = 0775;
        },
        [](struct switch_pt_trust_facts &f) {
            f.mode = 0757;
        },
        [](struct switch_pt_trust_facts &f) {
            f.cdhash_pinned = false;
        },
        [](struct switch_pt_trust_facts &f) {
            f.cdhash_matches = false;
        },
    };
    for (size_t i = 0; i < breaks.size(); ++i) {
        struct switch_pt_trust_facts facts = trusted();
        breaks[i](facts);
        EXPECT_FALSE(switch_pt_trusted(&facts)) << i;
    }
}

}  // namespace
