// SPDX-License-Identifier: MIT
// raw/runtime/raw_metallib_functions.h: the function list a library the facade reads itself answers from, with no m2v
// library: the AIR slice Metal loads (a universal file's AIR64 slice, a thin file whole), each name with Metal's
// MTLFunctionType for its TYPE tag, and no answer for a file Metal would not load.
#include "../raw/runtime/raw_metallib_functions.h"

#include <gtest/gtest.h>
#include "made_metallib.h"

namespace {

TEST(RawMetallibFunctions, AUniversalFileIsReadFromItsAirSlice)
{
    const std::string file = made_metallib::universal({"fixed_vert_spc", "fixed_frag_lph"});
    const auto *bytes = reinterpret_cast<const uint8_t *>(file.data());
    raw::MetallibFunctions f;
    ASSERT_TRUE(f.read(bytes, file.size()));
    EXPECT_EQ(f.slice, bytes + 0x110) << "the slice the frontend hands m2v, whose bytes a compile request names";
    EXPECT_EQ(f.sliceBytes, file.size() - 0x110);
    EXPECT_TRUE(f.lists("fixed_vert_spc"));
    EXPECT_EQ(f.type("fixed_frag_lph"), 2u) << "TYPE 1: MTLFunctionTypeFragment";
    EXPECT_FALSE(f.lists("absent"));
    EXPECT_EQ(f.type("absent"), 0u);
}

TEST(RawMetallibFunctions, AThinFileIsItsOwnSlice)
{
    const std::string file = made_metallib::universal({"k"});
    const std::string thin = file.substr(0x110);
    raw::MetallibFunctions f;
    ASSERT_TRUE(f.read(reinterpret_cast<const uint8_t *>(thin.data()), thin.size()));
    EXPECT_EQ(f.slice, reinterpret_cast<const uint8_t *>(thin.data()));
    EXPECT_TRUE(f.lists("k"));
}

TEST(RawMetallibFunctions, AFileMetalWouldNotLoadIsNotRead)
{
    std::string file = made_metallib::universal({"f"});
    raw::MetallibFunctions f;
    std::string noAir = file;
    noAir[28] = 0x02;  // the second architecture is no longer AIR64: no slice Metal takes
    EXPECT_FALSE(f.read(reinterpret_cast<const uint8_t *>(noAir.data()), noAir.size()));
    EXPECT_TRUE(f.types.empty());
    const std::string cut = file.substr(0, 0x110 + 0x40);
    EXPECT_FALSE(f.read(reinterpret_cast<const uint8_t *>(cut.data()), cut.size())) << "a function list past the end";
    EXPECT_FALSE(f.read(nullptr, 0));
}

TEST(RawMetallibFunctions, EachTypeTagIsMetalsFunctionType)
{
    EXPECT_EQ(raw::metalFunctionType(0), 1u);  // vertex
    EXPECT_EQ(raw::metalFunctionType(1), 2u);  // fragment
    EXPECT_EQ(raw::metalFunctionType(2), 3u);  // kernel
    EXPECT_EQ(raw::metalFunctionType(4), 5u);  // visible
    EXPECT_EQ(raw::metalFunctionType(6), 6u);  // intersection
    EXPECT_EQ(raw::metalFunctionType(7), 7u);  // mesh
    EXPECT_EQ(raw::metalFunctionType(8), 8u);  // object
    EXPECT_EQ(raw::metalFunctionType(3), 0u) << "a dynamic library's export: m2v answers";
    EXPECT_EQ(raw::metalFunctionType(5), 0u) << "a Core Image extern: m2v answers";
    EXPECT_EQ(raw::metalFunctionType(9), 0u);
}

}  // namespace
