// SPDX-License-Identifier: MIT
// raw/runtime/raw_comp.h on the host: the compressible kinds the raw path binds are ones the kernel's kind table
// (nouveau gm200_mmu_kind, which gm20b uses) takes as compressible and demotes as raw_comp.h says, and the boot ZBC
// table's values read the same through the raw path's Metal formats and through nvdec's NV formats (pack/nvdec_comp.h).
#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "../raw/pack/nvdec_comp.h"
#include "../raw/runtime/raw_comp.h"

namespace {

std::vector<uint32_t> kernelKindTable()
{
    std::ifstream f(GM200_C);
    std::stringstream s;
    s << f.rdbuf();
    const std::string src = s.str();
    const size_t at = src.find("kind[256] = {");
    std::vector<uint32_t> table;
    if (at == std::string::npos)
        return table;
    const std::string body =
        std::regex_replace(src.substr(at, src.find("};", at) - at), std::regex(R"(/\*[^*]*\*/)"), "");
    const std::regex hex("0x([0-9a-f]{2})\\b");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), hex); it != std::sregex_iterator(); ++it)
        table.push_back(uint32_t(std::stoul((*it)[1].str(), nullptr, 16)));
    return table;
}

TEST(RawComp, KindsAreNvnsAndTheKernelDemotesThemAsSaid)
{
    EXPECT_EQ(raw::compressedKind(4, false), 0xdbu);
    EXPECT_EQ(raw::compressedKind(8, false), 0xe9u);
    EXPECT_EQ(raw::compressedKind(16, false), 0xf5u);
    EXPECT_EQ(raw::compressedKind(8, true), 0xceu);
    EXPECT_EQ(raw::compressedKind(1, false), 0u);
    EXPECT_EQ(raw::compressedKind(2, false), 0u);
    const std::vector<uint32_t> table = kernelKindTable();
    ASSERT_EQ(table.size(), 256u);
    for (uint32_t kind : {0xdbu, 0xe9u, 0xf5u, 0xceu}) {
        EXPECT_NE(table[kind], kind) << std::hex << kind;
        EXPECT_NE(table[kind], 0xffu) << std::hex << kind;
        EXPECT_EQ(table[kind], raw::demotedKind(kind)) << std::hex << kind;
        EXPECT_TRUE(nvdec::compressibleKind(kind)) << std::hex << kind;
    }
    EXPECT_EQ(table[raw::demotedKind(0xce)], 0xc3u);  // the zeta's uncompressed kind, raw_zeta.h kZetaPteKind
    EXPECT_EQ(table[raw::demotedKind(0xdb)], 0xfeu);  // block-linear colour's, TGM_LAYOUT_KIND
}

TEST(RawComp, BootTableValuesAgreeAcrossMetalAndNvFormats)
{
    const uint32_t one = 0x3f800000, half = 0x3f000000;
    const uint32_t values[][4] = {{0, 0, 0, 0},      {one, one, one, one},    {0, 0, 0, one},
                                  {one, 0, 0, one},  {half, half, half, one}, {0, 0, 0, half},
                                  {one, one, one, 0}};
    const std::pair<uint32_t, uint32_t> formats[] = {{70, 0xd5}, {71, 0xd6},  {80, 0xcf},
                                                     {81, 0xd0}, {125, 0xc0}, {115, 0xca}};
    for (const auto &f : formats)
        for (const auto &v : values)
            EXPECT_EQ(raw::zbcBootColor(f.first, v), nvdec::zbcBootColorNv(f.second, v)) << f.first;
    const uint32_t black[4] = {0, 0, 0, one}, zero[4] = {}, white[4] = {one, one, one, one};
    EXPECT_TRUE(raw::zbcBootColor(80, black));
    EXPECT_FALSE(raw::zbcBootColor(115, black));  // RGBA16Float: the boot table has no opaque black of it
    EXPECT_TRUE(raw::zbcBootColor(115, zero));
    EXPECT_TRUE(raw::zbcBootColor(125, white));
    EXPECT_TRUE(raw::zbcBootDepth(1.0f));
    EXPECT_TRUE(raw::zbcBootDepth(0.0f));
    EXPECT_FALSE(raw::zbcBootDepth(0.75f));
    EXPECT_EQ(nvdec::zbcBootDepthBits(one), raw::zbcBootDepth(1.0f));
}

TEST(RawComp, MemoryIsWholeComptagLines)
{
    EXPECT_EQ(raw::compBytes(1), 0x20000u);
    EXPECT_EQ(raw::compBytes(0x20000), 0x20000u);
    EXPECT_EQ(raw::compBytes(1280u * 720u * 8u), 0x720000u);
}

}  // namespace
