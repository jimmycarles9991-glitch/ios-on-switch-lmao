// SPDX-License-Identifier: MIT
// metallib_functions.h: the function list a library the facade reads itself answers from, with no m2v
// library: the AIR slice Metal loads (a universal file's AIR64 slice, a thin file whole), each name with Metal's
// MTLFunctionType for its TYPE tag, and no answer for a file Metal would not load.
#include "../metallib_functions.h"

#include <gtest/gtest.h>
#include "made_metallib.h"

namespace {

TEST(GM20BMetallibFunctions, AUniversalFileIsReadFromItsAirSlice)
{
    const std::string file = made_metallib::universal({"fixed_vert_spc", "fixed_frag_lph"});
    const auto *bytes = reinterpret_cast<const uint8_t *>(file.data());
    gm20b::MetallibFunctions f;
    ASSERT_TRUE(f.read(bytes, file.size()));
    EXPECT_EQ(f.slice, bytes + 0x110) << "the slice the frontend hands m2v, whose bytes a compile request names";
    EXPECT_EQ(f.sliceBytes, file.size() - 0x110);
    EXPECT_TRUE(f.lists("fixed_vert_spc"));
    EXPECT_EQ(f.type("fixed_frag_lph"), 2u) << "TYPE 1: MTLFunctionTypeFragment";
    EXPECT_FALSE(f.lists("absent"));
    EXPECT_EQ(f.type("absent"), 0u);
}

TEST(GM20BMetallibFunctions, AThinFileIsItsOwnSlice)
{
    const std::string file = made_metallib::universal({"k"});
    const std::string thin = file.substr(0x110);
    gm20b::MetallibFunctions f;
    ASSERT_TRUE(f.read(reinterpret_cast<const uint8_t *>(thin.data()), thin.size()));
    EXPECT_EQ(f.slice, reinterpret_cast<const uint8_t *>(thin.data()));
    EXPECT_TRUE(f.lists("k"));
}

TEST(GM20BMetallibFunctions, AFileMetalWouldNotLoadIsNotRead)
{
    std::string file = made_metallib::universal({"f"});
    gm20b::MetallibFunctions f;
    std::string noAir = file;
    noAir[28] = 0x02;  // the second architecture is no longer AIR64: no slice Metal takes
    EXPECT_FALSE(f.read(reinterpret_cast<const uint8_t *>(noAir.data()), noAir.size()));
    EXPECT_TRUE(f.types.empty());
    const std::string cut = file.substr(0, 0x110 + 0x40);
    EXPECT_FALSE(f.read(reinterpret_cast<const uint8_t *>(cut.data()), cut.size())) << "a function list past the end";
    EXPECT_FALSE(f.read(nullptr, 0));
    EXPECT_FALSE(f.listed);
}

// functionNames and externFunctionNames in list order: the extern entries (TYPE 5, a Core Image kernel library's) apart
TEST(GM20BMetallibFunctions, NamesAndExternsAreListedInOrder)
{
    const std::string file = made_metallib::universal({"b", "tint", "a", "swap_rb"}, 0xA0, 0xEE, {2, 5, 0, 5});
    gm20b::MetallibFunctions f;
    ASSERT_TRUE(f.read(reinterpret_cast<const uint8_t *>(file.data()), file.size()));
    EXPECT_TRUE(f.listed);
    EXPECT_EQ(f.names, (std::vector<std::string>{"b", "a"}));
    EXPECT_EQ(f.externs, (std::vector<std::string>{"tint", "swap_rb"}));
}

// A metallib that lists no function is read (Metal loads it, a library with no functions), not left to m2v
TEST(GM20BMetallibFunctions, AnEmptyListIsRead)
{
    const std::string file = made_metallib::universal({});
    gm20b::MetallibFunctions f;
    ASSERT_TRUE(f.read(reinterpret_cast<const uint8_t *>(file.data()), file.size()));
    EXPECT_TRUE(f.listed);
    EXPECT_TRUE(f.types.empty() && f.names.empty() && f.externs.empty());
    EXPECT_FALSE(f.lists("f"));
    const std::string cut = file.substr(0, file.size() - 1);
    EXPECT_FALSE(f.read(reinterpret_cast<const uint8_t *>(cut.data()), cut.size()));
    EXPECT_FALSE(f.listed);
}

TEST(GM20BMetallibFunctions, EachTypeTagIsMetalsFunctionType)
{
    EXPECT_EQ(gm20b::metalFunctionType(0), 1u);  // vertex
    EXPECT_EQ(gm20b::metalFunctionType(1), 2u);  // fragment
    EXPECT_EQ(gm20b::metalFunctionType(2), 3u);  // kernel
    EXPECT_EQ(gm20b::metalFunctionType(4), 5u);  // visible
    EXPECT_EQ(gm20b::metalFunctionType(6), 6u);  // intersection
    EXPECT_EQ(gm20b::metalFunctionType(7), 7u);  // mesh
    EXPECT_EQ(gm20b::metalFunctionType(8), 8u);  // object
    EXPECT_EQ(gm20b::metalFunctionType(3), 0u) << "a dynamic library's export: m2v answers";
    EXPECT_EQ(gm20b::metalFunctionType(5), 0u) << "a Core Image extern: m2v answers";
    EXPECT_EQ(gm20b::metalFunctionType(9), 0u);
}

}  // namespace
