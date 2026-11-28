// A deploy's translated System files (kits/SwitchPretranslated/switch_pretranslated_updates.h): the boot package's
// /chosen list the 12377 kernel accepts and refuses, an update taking over its file's index entries, a turned-down
// update's file leaving the redirect, and the signature check.
#include <CommonCrypto/CommonDigest.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "kits/SwitchPretranslated/switch_pretranslated_updates.h"
#include "kits/SwitchPretranslated/test/index_builder.h"

namespace {

using switch_pt_test::encode;
using switch_pt_test::File;
using switch_pt_test::put32;
using switch_pt_test::set32;

struct Update {
    std::string path;
    std::array<uint8_t, 20> cdhash{};
};

// The list tools/package/adt_edit.py writes (same layout, no checks).
std::vector<uint8_t> updates(const std::vector<Update> &entries)
{
    std::string strings;
    std::vector<uint8_t> out;
    out.insert(out.end(), {'S', 'W', 'U', 'P'});
    put32(out, 1);
    put32(out, static_cast<uint32_t>(entries.size()));
    size_t total = 0;
    for (const Update &u : entries)
        total += u.path.size() + 1;
    put32(out, static_cast<uint32_t>(total));
    for (const Update &u : entries) {
        put32(out, static_cast<uint32_t>(strings.size()));
        out.insert(out.end(), u.cdhash.begin(), u.cdhash.end());
        strings += u.path;
        strings.push_back('\0');
    }
    out.insert(out.end(), strings.begin(), strings.end());
    return out;
}

enum switch_pt_error parse(const std::vector<uint8_t> &raw, struct switch_pt_updates *out = nullptr)
{
    struct switch_pt_updates scratch;
    return switch_pt_updates_parse(raw.data(), raw.size(), out ? out : &scratch);
}

uint32_t fnv(const char *name, uint32_t length)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < length; ++i)
        h = (h ^ static_cast<uint8_t>(name[i])) * 16777619u;
    return h ? h : 1;
}

const std::vector<Update> kUpdates = {
    {"/usr/libexec/locationd", {{0x02, 0x0e}}},
    {"/usr/sbin/b", {{0x09}}},
};

}  // namespace

TEST(Updates, ParsesWhatTheBootPackageWrites)
{
    const std::vector<uint8_t> raw = updates(kUpdates);
    struct switch_pt_updates list;
    ASSERT_EQ(parse(raw, &list), SWITCH_PT_OK);
    ASSERT_EQ(list.count, 2u);
    const struct switch_pt_update first = switch_pt_update_at(&list, 0);
    EXPECT_STREQ(first.path, "/usr/libexec/locationd");
    EXPECT_EQ(first.length, 22u);
    EXPECT_EQ(first.cdhash[0], 0x02);
    EXPECT_STREQ(switch_pt_update_relative(&first), "usr/libexec/locationd");
    EXPECT_STREQ(switch_pt_update_at(&list, 1).path, "/usr/sbin/b");
}

TEST(Updates, NoPropertyIsNoUpdates)
{
    struct switch_pt_updates list;
    EXPECT_EQ(switch_pt_updates_parse(nullptr, 0, &list), SWITCH_PT_OK);
    EXPECT_EQ(list.count, 0u);
}

TEST(Updates, EveryTruncationIsRefused)
{
    const std::vector<uint8_t> raw = updates(kUpdates);
    for (size_t size = 1; size < raw.size(); ++size) {
        struct switch_pt_updates list;
        EXPECT_NE(switch_pt_updates_parse(raw.data(), size, &list), SWITCH_PT_OK) << size;
    }
}

TEST(Updates, HeaderFieldsAreChecked)
{
    for (const size_t at : {0u, 4u}) {
        std::vector<uint8_t> raw = updates(kUpdates);
        raw[at] ^= 1;
        EXPECT_EQ(parse(raw), SWITCH_PT_HEADER_BAD) << at;
    }
    std::vector<uint8_t> none = updates({});
    EXPECT_EQ(parse(none), SWITCH_PT_HEADER_BAD) << "a property with no entries";
    std::vector<Update> many;
    for (unsigned i = 0; i <= SWITCH_PT_MAX_UPDATES; ++i)
        many.push_back({"/u/" + std::to_string(1000 + i)});
    EXPECT_EQ(parse(updates(many)), SWITCH_PT_HEADER_BAD);
}

TEST(Updates, UnsortedDuplicateOrBadPathsAreRefused)
{
    EXPECT_EQ(parse(updates({kUpdates[1], kUpdates[0]})), SWITCH_PT_ORDER_BAD);
    EXPECT_EQ(parse(updates({kUpdates[0], kUpdates[0]})), SWITCH_PT_ORDER_BAD);
    for (const char *bad : {"usr/libexec/locationd", "/usr/../etc", "/usr//a", "/usr/a/"})
        EXPECT_EQ(parse(updates({{bad}})), SWITCH_PT_PATH_BAD) << bad;
}

TEST(Updates, RecordsMustPointAtTheirOwnPackedPath)
{
    std::vector<uint8_t> raw = updates(kUpdates);
    set32(raw, SWITCH_PT_UPDATES_HEADER + SWITCH_PT_UPDATES_RECORD, 0);
    EXPECT_EQ(parse(raw), SWITCH_PT_RECORD_BAD);
}

TEST(Updates, ByteFlipsNeverReadOutOfBounds)
{
    const std::vector<uint8_t> clean = updates(kUpdates);
    for (size_t at = 0; at < clean.size(); ++at) {
        for (const uint8_t flip : {0x01, 0x80, 0xff}) {
            std::vector<uint8_t> raw = clean;
            raw[at] ^= flip;
            struct switch_pt_updates list;
            if (switch_pt_updates_parse(raw.data(), raw.size(), &list) != SWITCH_PT_OK)
                continue;
            for (uint32_t i = 0; i < list.count; ++i) {
                const struct switch_pt_update u = switch_pt_update_at(&list, i);
                EXPECT_LT(static_cast<size_t>(u.path - list.strings), list.string_bytes);
            }
        }
    }
}

// The update's Data file and CDHash replace the tree's in every record of its System file (here two hard links), so
// the tables built afterwards send a lookup to the update and the tree's copy is no longer listed.
TEST(Updates, AnUpdateTakesOverEveryRecordOfItsFile)
{
    const std::vector<File> files = {
        {"/System/Library/a", 10, 100},
        {"/usr/libexec/locationd", 20, 200, {{0xac, 0x43}}},
        {"/usr/libexec/locationd-link", 20, 200, {{0xac, 0x43}}},
        {"/usr/sbin/b", 40, 400},
    };
    std::vector<uint8_t> raw = encode(files, {1000, 1001});
    struct switch_pt_index index;
    ASSERT_EQ(switch_pt_index_parse(raw.data(), raw.size(), &index), SWITCH_PT_OK);
    const std::array<uint8_t, 20> cdhash{{0x02, 0x0e}};
    uint64_t replaced = 0;
    EXPECT_EQ(switch_pt_index_point(raw.data(), &index, 20, 900, cdhash.data(), &replaced), 2u);
    EXPECT_EQ(replaced, 200u);
    EXPECT_EQ(switch_pt_index_point(raw.data(), &index, 77, 901, cdhash.data(), &replaced), 0u) << "not in the tree";
    ASSERT_EQ(switch_pt_index_parse(raw.data(), raw.size(), &index), SWITCH_PT_OK) << "still a valid index";
    const size_t bytes = switch_pt_tables_bytes(&index, 2);
    std::vector<uint64_t> storage((bytes + 7) / 8, 0);
    struct switch_pt_tables tables;
    ASSERT_EQ(switch_pt_tables_build(&tables, &index, 2, storage.data(), bytes, fnv), SWITCH_PT_OK);
    const int64_t file = switch_pt_system_file(&tables, 20, fnv("locationd", 9), "locationd", 9);
    ASSERT_GE(file, 0);
    const struct switch_pt_file listed = switch_pt_file_at(&index, static_cast<uint32_t>(file));
    EXPECT_EQ(listed.data_id, 900u);
    EXPECT_EQ(std::memcmp(listed.cdhash, cdhash.data(), 20), 0);
    EXPECT_EQ(switch_pt_system_id(&tables, 900), 20u);
    EXPECT_EQ(switch_pt_system_id(&tables, 200), 0u) << "the tree's copy is the caller's to protect";
    EXPECT_EQ(switch_pt_system_id(&tables, 100), 10u) << "other files untouched";
}

// A turned-down update's file leaves the redirect: no lookup finds it, so its System file runs translated at load time,
// and its tree copy stays listed, so the tables keep protecting it.
TEST(Updates, ATurnedDownUpdatesFileLeavesTheRedirect)
{
    const std::vector<File> files = {
        {"/System/Library/a", 10, 100},
        {"/usr/libexec/locationd", 20, 200, {{0xac, 0x43}}},
        {"/usr/libexec/locationd-link", 20, 200, {{0xac, 0x43}}},
        {"/usr/sbin/b", 40, 400},
    };
    std::vector<uint8_t> raw = encode(files, {1000, 1001});
    struct switch_pt_index index;
    ASSERT_EQ(switch_pt_index_parse(raw.data(), raw.size(), &index), SWITCH_PT_OK);
    EXPECT_EQ(switch_pt_index_find(&index, "/usr/libexec/locationd", 22), 1);
    EXPECT_EQ(switch_pt_index_find(&index, "/usr/libexec/locationd-link", 27), 2);
    EXPECT_EQ(switch_pt_index_find(&index, "/System/Library/a", 17), 0);
    EXPECT_EQ(switch_pt_index_find(&index, "/usr/sbin/b", 11), 3);
    EXPECT_EQ(switch_pt_index_find(&index, "/usr/libexec/locationd2", 23), -1);
    EXPECT_EQ(switch_pt_index_find(&index, "/usr/libexec", 12), -1);
    EXPECT_EQ(switch_pt_index_unlist(raw.data(), &index, 20, 0), 2u);
    ASSERT_EQ(switch_pt_index_parse(raw.data(), raw.size(), &index), SWITCH_PT_OK) << "still a valid index";
    const size_t bytes = switch_pt_tables_bytes(&index, 2);
    std::vector<uint64_t> storage((bytes + 7) / 8, 0);
    struct switch_pt_tables tables;
    ASSERT_EQ(switch_pt_tables_build(&tables, &index, 2, storage.data(), bytes, fnv), SWITCH_PT_OK);
    EXPECT_EQ(switch_pt_system_file(&tables, 20, fnv("locationd", 9), "locationd", 9), -1);
    EXPECT_EQ(switch_pt_system_file(&tables, 20, fnv("locationd-link", 14), "locationd-link", 14), -1);
    EXPECT_NE(switch_pt_system_id(&tables, 200), 0u) << "the tree's copy stays listed";
    EXPECT_EQ(switch_pt_system_id(&tables, 200), SWITCH_PT_UNLISTED_ID(0));
    EXPECT_GE(switch_pt_system_file(&tables, 40, fnv("b", 1), "b", 1), 0) << "other files untouched";
}

// A hard link the index spells otherwise is found by its System file's id.
TEST(Updates, AFileIsFoundByItsIdWhereThePathMisses)
{
    const std::vector<File> files = {
        {"/usr/a", 20, 200}, {"/usr/libexec/locationd-link", 30, 300}, {"/usr/z", 40, 400}};
    std::vector<uint8_t> raw = encode(files, {1000});
    struct switch_pt_index index;
    ASSERT_EQ(switch_pt_index_parse(raw.data(), raw.size(), &index), SWITCH_PT_OK);
    EXPECT_EQ(switch_pt_index_find(&index, "/usr/libexec/locationd", 22), -1);
    EXPECT_EQ(switch_pt_index_find_id(&index, 30), 1);
    EXPECT_EQ(switch_pt_index_find_id(&index, 77), -1);
}

// Two turned-down files keep ids of their own, so the tables still take every record.
TEST(Updates, TurnedDownFilesKeepDistinctIds)
{
    const std::vector<File> files = {{"/usr/a", 20, 200}, {"/usr/b", 30, 300}};
    std::vector<uint8_t> raw = encode(files, {1000});
    struct switch_pt_index index;
    ASSERT_EQ(switch_pt_index_parse(raw.data(), raw.size(), &index), SWITCH_PT_OK);
    EXPECT_EQ(switch_pt_index_unlist(raw.data(), &index, 20, 0), 1u);
    EXPECT_EQ(switch_pt_index_unlist(raw.data(), &index, 30, 1), 1u);
    EXPECT_EQ(switch_pt_index_unlist(raw.data(), &index, 77, 2), 0u) << "not in the tree";
    const size_t bytes = switch_pt_tables_bytes(&index, 2);
    std::vector<uint64_t> storage((bytes + 7) / 8, 0);
    struct switch_pt_tables tables;
    EXPECT_EQ(switch_pt_tables_build(&tables, &index, 2, storage.data(), bytes, fnv), SWITCH_PT_OK);
}

namespace {

void be32(std::vector<uint8_t> &out, uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>(v >> shift));
}

// A thin arm64 Mach-O head: a segment command, then LC_CODE_SIGNATURE at (offset, size).
std::vector<uint8_t> macho(uint32_t offset, uint32_t size, uint32_t magic = SWITCH_PT_MH_MAGIC_64)
{
    std::vector<uint8_t> out;
    for (const uint32_t v : {magic, 0x0100000cu, 0u, 2u, 2u, 72u + 16u, 0u, 0u})
        put32(out, v);
    put32(out, 0x19);
    put32(out, 72);
    out.resize(out.size() + 64, 0);
    for (const uint32_t v : {SWITCH_PT_LC_CODE_SIGNATURE, 16u, offset, size})
        put32(out, v);
    return out;
}

// codesign's embedded signature: a SHA-1 CodeDirectory first, the SHA-256 one as the first alternate.
std::vector<uint8_t> signature()
{
    std::vector<uint8_t> out;
    be32(out, SWITCH_PT_CS_SUPERBLOB);
    be32(out, 12 + 16 + 2 * 48);
    be32(out, 2);
    be32(out, SWITCH_PT_CS_SLOT_CD);
    be32(out, 28);
    be32(out, SWITCH_PT_CS_SLOT_ALTERNATE);
    be32(out, 28 + 48);
    for (const uint8_t hash : {1, 2}) {
        const size_t at = out.size();
        be32(out, SWITCH_PT_CS_CODEDIRECTORY);
        be32(out, 48);
        out.resize(at + 48, hash);
        out[at + 37] = hash;
    }
    return out;
}

}  // namespace

TEST(Signature, TheLoadCommandsNameTheSignature)
{
    const std::vector<uint8_t> head = macho(0x8000, 0x900);
    uint32_t offset = 0, size = 0;
    ASSERT_TRUE(switch_pt_macho_signature(head.data(), head.size(), &offset, &size));
    EXPECT_EQ(offset, 0x8000u);
    EXPECT_EQ(size, 0x900u);
}

TEST(Signature, AFatOrTruncatedHeadIsRefused)
{
    uint32_t offset = 0, size = 0;
    const std::vector<uint8_t> fat = macho(0x8000, 0x900, 0xbebafecau);
    EXPECT_FALSE(switch_pt_macho_signature(fat.data(), fat.size(), &offset, &size));
    const std::vector<uint8_t> head = macho(0x8000, 0x900);
    for (size_t n = 0; n < head.size(); ++n)
        EXPECT_FALSE(switch_pt_macho_signature(head.data(), n, &offset, &size)) << n;
    const std::vector<uint8_t> huge = macho(0x8000, SWITCH_PT_MAX_SIGNATURE_BYTES + 1);
    EXPECT_FALSE(switch_pt_macho_signature(huge.data(), huge.size(), &offset, &size));
}

TEST(Signature, TheSha256CodeDirectoryIsTheOneATrustCacheNames)
{
    const std::vector<uint8_t> blob = signature();
    uint32_t offset = 0, length = 0;
    ASSERT_TRUE(switch_pt_code_directory(blob.data(), blob.size(), &offset, &length));
    EXPECT_EQ(offset, 28u + 48u);
    EXPECT_EQ(length, 48u);
    for (size_t n = 0; n < blob.size(); ++n)
        EXPECT_FALSE(switch_pt_code_directory(blob.data(), n, &offset, &length)) << n;
}

namespace {

// A whole file: the Mach-O head, padding, then the signature at its offset.
std::vector<uint8_t> signedFile()
{
    const std::vector<uint8_t> blob = signature();
    std::vector<uint8_t> file = macho(0x8000, static_cast<uint32_t>(blob.size()));
    file.resize(0x8000, 0);
    file.insert(file.end(), blob.begin(), blob.end());
    return file;
}

struct Bytes {
    const std::vector<uint8_t> *file;
    unsigned reads = 0;
};

bool readBytes(void *context, void *buffer, uint32_t size, uint64_t offset)
{
    auto *bytes = static_cast<Bytes *>(context);
    ++bytes->reads;
    if (offset + size > bytes->file->size())
        return false;
    std::memcpy(buffer, bytes->file->data() + offset, size);
    return true;
}

void *allocate(size_t size)
{
    return std::malloc(size);
}

void release(void *bytes, size_t)
{
    std::free(bytes);
}

void sha256(const void *bytes, size_t size, uint8_t digest[SWITCH_PT_SHA256])
{
    CC_SHA256(bytes, static_cast<CC_LONG>(size), digest);
}

std::array<uint8_t, SWITCH_PT_SHA256> cdhashOf(const std::vector<uint8_t> &file)
{
    std::array<uint8_t, SWITCH_PT_SHA256> digest{};
    CC_SHA256(file.data() + 0x8000 + 28 + 48, 48, digest.data());
    return digest;
}

}  // namespace

// The kernel's whole check, as capture runs it: the head, the signature it names, the SHA-256 CodeDirectory's hash.
TEST(Signature, AFileIsSignedAsTheHashOfItsSha256CodeDirectory)
{
    const std::vector<uint8_t> file = signedFile();
    const std::array<uint8_t, SWITCH_PT_SHA256> cdhash = cdhashOf(file);
    Bytes bytes{&file};
    const struct switch_pt_reader reader = {&bytes, readBytes, allocate, release, sha256};
    EXPECT_TRUE(switch_pt_signed_as_cdhash(&reader, file.size(), cdhash.data()));
    EXPECT_EQ(bytes.reads, 2u) << "the head, then the signature";
    std::array<uint8_t, SWITCH_PT_SHA256> other = cdhash;
    other[19] ^= 1;
    EXPECT_FALSE(switch_pt_signed_as_cdhash(&reader, file.size(), other.data()));
    std::vector<uint8_t> changed = file;
    changed[0x8000 + 28 + 48 + 40] ^= 1;
    Bytes changedBytes{&changed};
    const struct switch_pt_reader changedReader = {&changedBytes, readBytes, allocate, release, sha256};
    EXPECT_FALSE(switch_pt_signed_as_cdhash(&changedReader, changed.size(), cdhash.data()));
    EXPECT_FALSE(switch_pt_signed_as_cdhash(&reader, file.size() - 1, cdhash.data())) << "signature past the end";
}

TEST(Signature, NoSha256CodeDirectoryIsNoMatch)
{
    std::vector<uint8_t> blob = signature();
    blob[28 + 48 + 37] = 1;
    uint32_t offset = 0, length = 0;
    EXPECT_FALSE(switch_pt_code_directory(blob.data(), blob.size(), &offset, &length));
}
