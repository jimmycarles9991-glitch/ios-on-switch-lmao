// SPDX-License-Identifier: MIT
// MetallibEntries.h against what Metal answers (macOS 27 on an Apple GPU; the same for iOS 17's and 27's Core Image metallibs):
// externFunctionNames lists the TYPE 5 entries and functionNames the others, whatever the header's file type; a function's
// patch is its entry's TESS byte, as Xcode's metal writes it (xcrun -sdk iphoneos metal and metallib).
#include "drivers/NativeMetal/MetallibEntries.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {
using switch_metal::forEachMetallibEntry;
using switch_metal::FunctionTess;
using switch_metal::MetallibEntry;
using switch_metal::metallibExternNames;
using switch_metal::metallibFunctionTess;

struct Entry {
    std::string name;
    uint8_t type;
    uint64_t bitcodeOffset, bitcodeBytes;
    bool placed = true;
    int tess = -1;  // the TESS byte; -1: no tag
};

void put(std::vector<uint8_t> &out, const void *bytes, size_t n)
{
    out.insert(out.end(), static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + n);
}

template <class T> void put(std::vector<uint8_t> &out, T value)
{
    put(out, &value, sizeof value);
}

void tag(std::vector<uint8_t> &out, const char *fourcc, const std::vector<uint8_t> &value)
{
    put(out, fourcc, 4);
    put(out, uint16_t(value.size()));
    put(out, value.data(), value.size());
}

template <class... T> std::vector<uint8_t> words(T... values)
{
    std::vector<uint8_t> out;
    (put(out, values), ...);
    return out;
}

// As `metal -fcikernel` and metallib -cikernel lay one out (format 2.9): each entry NAME, TYPE, HASH, OFFT, VERS, MDSZ, ENDT; the
// bitcode section after the list
std::vector<uint8_t> metallib(uint8_t fileType, const std::vector<Entry> &entries, uint64_t bitcodeBytes = 64)
{
    std::vector<uint8_t> list;
    put(list, uint32_t(entries.size()));
    for (const Entry &e : entries) {
        std::vector<uint8_t> tags;
        tag(tags, "NAME", std::vector<uint8_t>(e.name.c_str(), e.name.c_str() + e.name.size() + 1));
        tag(tags, "TYPE", {e.type});
        tag(tags, "HASH", std::vector<uint8_t>(32, 0xab));
        if (e.placed) {
            tag(tags, "OFFT", words(uint64_t(0), uint64_t(0), e.bitcodeOffset));
            tag(tags, "VERS", words(uint16_t(2), uint16_t(9), uint16_t(4), uint16_t(1)));
            tag(tags, "MDSZ", words(e.bitcodeBytes));
        }
        if (e.tess >= 0)
            tag(tags, "TESS", {uint8_t(e.tess)});
        put(tags, "ENDT", 4);
        put(list, uint32_t(4 + tags.size()));
        put(list, tags.data(), tags.size());
    }
    const uint64_t listAt = switch_metal::kMetallibHeaderBytes, bitcodeAt = listAt + list.size();
    std::vector<uint8_t> file;
    put(file, "MTLB", 4);
    put(file, uint16_t(0x8001));
    put(file, uint16_t(2));
    put(file, uint16_t(9));
    put(file, fileType);
    put(file, uint8_t(0x81));
    put(file, uint16_t(27));
    put(file, uint16_t(0));
    put(file, uint64_t(bitcodeAt + bitcodeBytes));
    for (uint64_t word :
         {listAt, uint64_t(list.size() - 4), bitcodeAt, uint64_t(0), bitcodeAt, uint64_t(0), bitcodeAt, bitcodeBytes})
        put(file, word);
    EXPECT_EQ(file.size(), switch_metal::kMetallibHeaderBytes);
    put(file, list.data(), list.size());
    file.resize(file.size() + bitcodeBytes, 0x42);
    return file;
}

std::vector<std::string> externs(const std::vector<uint8_t> &file)
{
    return metallibExternNames(file.data(), file.size());
}

constexpr uint8_t kKernel = 2, kVisible = 4, kExtern = 5;

// A Core Image kernel library built from two kernels: Metal answers functionNames empty and externFunctionNames {swap_rb, tint}
TEST(MetallibEntries, CoreImageKernelLibraryListsEveryFunctionAsExtern)
{
    const auto file = metallib(1, {{"tint", kExtern, 0, 64}, {"swap_rb", kExtern, 0, 64}});
    EXPECT_EQ(externs(file), (std::vector<std::string>{"tint", "swap_rb"}));
}

// Core Image's stitchable library and a library of kernels: every function in functionNames, externFunctionNames empty
TEST(MetallibEntries, ExecutableLibraryHasNoExterns)
{
    EXPECT_TRUE(externs(metallib(0, {{"_add", kVisible, 0, 32}, {"_multiply", kVisible, 32, 32}})).empty());
    EXPECT_TRUE(externs(metallib(0, {{"convert_rgb8_to_rgba8_texture", kKernel, 0, 64}})).empty());
}

// ci_stdlib with its file type set to 0 still lists all its functions as extern; ci_stdlib_stitchable with 1 lists none
TEST(MetallibEntries, TheEntryTypeDecidesNotTheFileType)
{
    EXPECT_EQ(externs(metallib(0, {{"_add", kExtern, 0, 64}})), (std::vector<std::string>{"_add"}));
    EXPECT_TRUE(externs(metallib(1, {{"_add", kVisible, 0, 64}})).empty());
}

TEST(MetallibEntries, EachEntryCarriesItsNameTypeAndBitcode)
{
    const auto file =
        metallib(0, {{"first", kKernel, 0, 24}, {"second", kVisible, 24, 40}, {"bare", kKernel, 0, 0, false}});
    std::vector<MetallibEntry> seen;
    EXPECT_TRUE(forEachMetallibEntry(file.data(), file.size(), [&](const MetallibEntry &e) {
        seen.push_back(e);
        return true;
    }));
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0].name, "first");
    EXPECT_EQ(seen[0].type, kKernel);
    EXPECT_TRUE(seen[0].placed);
    EXPECT_EQ(seen[0].bitcodeBytes, 24u);
    EXPECT_EQ(seen[1].bitcodeOffset, 24u);
    EXPECT_EQ(seen[1].bitcodeBytes, 40u);
    EXPECT_EQ(seen[2].name, "bare");
    EXPECT_FALSE(seen[2].placed);
}

TEST(MetallibEntries, AVisitThatAnswersFalseEndsTheWalk)
{
    const auto file = metallib(1, {{"a", kExtern, 0, 64}, {"b", kExtern, 0, 64}});
    unsigned visits = 0;
    EXPECT_TRUE(forEachMetallibEntry(file.data(), file.size(), [&](const MetallibEntry &) {
        ++visits;
        return false;
    }));
    EXPECT_EQ(visits, 1u);
}

TEST(MetallibEntries, NoBytesOrAMalformedListHasNoExterns)
{
    auto file = metallib(1, {{"tint", kExtern, 0, 64}});
    EXPECT_TRUE(metallibExternNames(nullptr, 0).empty());
    EXPECT_TRUE(metallibExternNames(file.data(), switch_metal::kMetallibHeaderBytes - 1).empty());
    auto universal = file;
    const uint8_t fat[4] = {0xca, 0xfe, 0xba, 0xbe};
    std::copy(fat, fat + 4, universal.begin());
    EXPECT_TRUE(externs(universal).empty());
    auto oversized = file;
    const uint32_t size = uint32_t(file.size());
    memcpy(oversized.data() + switch_metal::kMetallibHeaderBytes + 4, &size, 4);
    EXPECT_FALSE(forEachMetallibEntry(oversized.data(), oversized.size(), [](const MetallibEntry &) {
        return true;
    }));
    EXPECT_TRUE(externs(oversized).empty());
    auto truncatedTag = file;
    const uint16_t past = 0xffff;
    memcpy(truncatedTag.data() + switch_metal::kMetallibHeaderBytes + 8 + 4, &past, 2);
    EXPECT_TRUE(externs(truncatedTag).empty());
}

TEST(MetallibEntries, DecodesThePatchBytesXcodeWrites)
{
    const FunctionTess triangle = switch_metal::decodeTess(0x0d), quad = switch_metal::decodeTess(0x42),
                       any = switch_metal::decodeTess(0xfe);
    EXPECT_EQ(triangle.patchType, 1);
    EXPECT_EQ(triangle.controlPoints, 3);
    EXPECT_EQ(quad.patchType, 2);
    EXPECT_EQ(quad.controlPoints, 16);
    EXPECT_EQ(any.patchType, 2);
    EXPECT_EQ(any.controlPoints, -1);
}

TEST(MetallibEntries, EachFunctionHasItsOwnPatch)
{
    const auto file = metallib(0, {{"quad_vertex", 0, 0, 16},
                                   {"tri_post", 0, 16, 16, true, 0x0d},
                                   {"quad_post", 0, 32, 16, true, 0x42},
                                   {"solid_fragment", 1, 48, 16}});
    FunctionTess t;
    ASSERT_TRUE(metallibFunctionTess(file.data(), file.size(), "tri_post", t));
    EXPECT_EQ(t.patchType, 1);
    EXPECT_EQ(t.controlPoints, 3);
    ASSERT_TRUE(metallibFunctionTess(file.data(), file.size(), "quad_post", t));
    EXPECT_EQ(t.patchType, 2);
    EXPECT_EQ(t.controlPoints, 16);
    ASSERT_TRUE(metallibFunctionTess(file.data(), file.size(), "quad_vertex", t));
    EXPECT_EQ(t.patchType, 0);
    EXPECT_EQ(t.controlPoints, -1);
    EXPECT_FALSE(metallibFunctionTess(file.data(), file.size(), "missing", t));
}

TEST(MetallibEntries, ACutListIsNoPatch)
{
    const auto file = metallib(0, {{"a", 0, 0, 0}, {"tri_post", 0, 0, 0, true, 0x0d}}, 0);
    const std::vector<uint8_t> cut(file.begin(), file.end() - 6);
    FunctionTess t;
    t.patchType = 7;
    EXPECT_FALSE(metallibFunctionTess(cut.data(), cut.size(), "tri_post", t));
    EXPECT_EQ(t.patchType, 7);
}

}  // namespace
