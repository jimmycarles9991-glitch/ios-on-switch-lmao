// SPDX-License-Identifier: MIT
// raw/runtime/raw_comp.h on the host: the compressible kinds the raw path binds are ones the kernel's kind table
// (nouveau gm200_mmu_kind, which gm20b uses) takes as compressible and demotes as raw_comp.h says, the boot ZBC
// table's values read the same through the raw path's Metal formats and through nvdec's NV formats (pack/nvdec_comp.h),
// only the backings the copy engine never touches compress, and a refused want is asked for again as the kernel's answer
// allows.
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

TEST(RawComp, OnlyZetaAndMemorylessColourBackingsCompress)
{
    constexpr uint32_t memoryless = 9;
    EXPECT_EQ(raw::backingKind(true, true, true, 8, 1, 0, memoryless), 0xceu);
    EXPECT_EQ(raw::backingKind(true, true, false, 8, 1, memoryless, memoryless), 0xe9u);
    EXPECT_EQ(raw::backingKind(true, true, false, 4, 1, memoryless + 7, memoryless), 0xdbu);
    for (uint32_t shadow = 1; shadow < memoryless; shadow++)  // the copy engine fills and drains a shadow
        EXPECT_EQ(raw::backingKind(true, true, false, 4, 1, shadow, memoryless), 0u) << shadow;
    EXPECT_EQ(raw::backingKind(true, false, false, 4, 1, memoryless, memoryless), 0u);  // pitch
    EXPECT_EQ(raw::backingKind(false, true, true, 8, 1, 0, memoryless), 0u);            // compression off
    EXPECT_EQ(raw::backingKind(true, true, false, 2, 1, memoryless, memoryless), 0u);   // no compressible kind
    EXPECT_EQ(raw::backingKind(true, true, false, 4, 4, memoryless, memoryless), 0u);   // multisampled
    EXPECT_EQ(raw::backingKind(true, true, true, 8, 2, 0, memoryless), 0u);
}

TEST(RawComp, ShortagesPassAndContractRefusalsDoNot)
{
    EXPECT_FALSE(raw::compLasting(kIOReturnNoMemory));     // the pool's tiers or no free chunk
    EXPECT_FALSE(raw::compLasting(kIOReturnNoResources));  // no comptags left
    EXPECT_FALSE(raw::compLasting(kIOReturnNoSpace));      // no GPU VA
    EXPECT_TRUE(raw::compLasting(kIOReturnBadArgument));
    EXPECT_TRUE(raw::compLasting(kIOReturnUnsupported));
}

TEST(RawComp, RetriesBackOffToTenSecondsOfFrames)
{
    EXPECT_EQ(raw::compRetryAfter(1), 30u);
    EXPECT_EQ(raw::compRetryAfter(2), 60u);
    EXPECT_EQ(raw::compRetryAfter(5), 480u);
    EXPECT_EQ(raw::compRetryAfter(6), 600u);
    EXPECT_EQ(raw::compRetryAfter(1000), 600u);
}

TEST(RawComp, ARefusedWantIsAskedAgainOnceDue)
{
    raw::CompQueue q;
    const raw::CompWant w{0x720000, 0xe9}, other{0x20000, 0xce};
    EXPECT_FALSE(q.asked(w, 0));
    q.wanted.push_back(w);
    EXPECT_TRUE(q.asked(w, 0));
    q.wanted.clear();
    EXPECT_TRUE(q.refuse(w, 100, false));  // its first refusal is the one logged
    EXPECT_TRUE(q.asked(w, 129));
    EXPECT_FALSE(q.asked(w, 130));
    EXPECT_FALSE(q.asked(other, 100));
    EXPECT_FALSE(q.refuse(w, 130, false));
    EXPECT_TRUE(q.asked(w, 189));
    EXPECT_FALSE(q.asked(w, 190));
    q.made(w);
    EXPECT_FALSE(q.asked(w, 0));
    EXPECT_TRUE(q.refuse(w, 200, false));  // a refusal after it was made is a first one again
}

TEST(RawComp, FreedMemoryMakesShortagesDueAndNothingElse)
{
    raw::CompQueue q;
    const raw::CompWant shortOf{0x720000, 0xe9}, refused{0x20000, 0xdb};
    for (uint64_t at = 0; at < 10; at++)
        q.refuse(shortOf, at, false);
    q.refuse(refused, 0, true);
    EXPECT_TRUE(q.asked(shortOf, 100));
    q.freed(100);
    EXPECT_FALSE(q.asked(shortOf, 100));
    EXPECT_TRUE(q.asked(refused, 1000000));  // a lasting refusal is never asked again
}

TEST(RawComp, AReadyBufferAnswersItsWant)
{
    raw::CompQueue q;
    raw::CompBo b;
    b.bo.bytes = 0x20000;
    b.kind = 0xce;
    q.ready.push_back(b);
    EXPECT_TRUE(q.asked({0x20000, 0xce}, 0));
    EXPECT_FALSE(q.asked({0x20000, 0xe9}, 0));
}

TEST(RawComp, ClassesAreQuarterStepsOfWholeLines)
{
    EXPECT_EQ(raw::compClassBytes(1), raw::kCompGranule);
    EXPECT_EQ(raw::compClassBytes(3 * raw::kCompGranule), 3 * raw::kCompGranule);
    EXPECT_EQ(raw::compClassBytes(7296u << 10), 8u << 20);  // 720x1280 RGBA16F: 57 lines, class 64
    EXPECT_EQ(raw::compClassBytes(33 * raw::kCompGranule), 40 * raw::kCompGranule);
    EXPECT_EQ(raw::compClassBytes(21376u << 10), 24u << 20);  // 167 lines, class 192
    for (uint64_t lines = 1; lines < 4096; lines++) {
        const uint64_t c = raw::compClassBytes(lines * raw::kCompGranule) / raw::kCompGranule;
        ASSERT_GE(c, lines);
        ASSERT_LT(c - lines, std::max<uint64_t>(lines / 4, 1)) << lines;
    }
}

TEST(RawComp, BackingsOfATagAndKindShareTheLargestMemory)
{
    raw::CompQueue q;
    q.mems.push_back(std::make_unique<raw::CompMem>(raw::CompMem{{}, 0xe9, 9, 0}));
    q.mems.back()->bo.bytes = 2 << 20;
    q.mems.push_back(std::make_unique<raw::CompMem>(raw::CompMem{{}, 0xe9, 9, 0}));
    q.mems.back()->bo.bytes = 8 << 20;
    q.mems.push_back(std::make_unique<raw::CompMem>(raw::CompMem{{}, 0xe9, 10, 0}));
    q.mems.back()->bo.bytes = 16 << 20;
    q.mems.push_back(std::make_unique<raw::CompMem>(raw::CompMem{{}, 0xce, 0, 0}));
    q.mems.back()->bo.bytes = 32 << 20;
    EXPECT_EQ(q.largest(9, 0xe9), q.mems[1].get());
    EXPECT_EQ(q.largest(10, 0xe9), q.mems[2].get());  // another attachment of the same pass: never the same memory
    EXPECT_EQ(q.largest(0, 0xce), q.mems[3].get());
    EXPECT_EQ(q.largest(9, 0xdb), nullptr);
}

TEST(RawComp, AMadeBufferServesEveryNeedItHolds)
{
    raw::CompQueue q;
    for (uint64_t mib : {8, 4, 16}) {
        raw::CompBo b;
        b.bo.bytes = mib << 20;
        b.kind = 0xe9;
        q.ready.push_back(b);
    }
    EXPECT_EQ(q.fit(3 << 20, 0xe9)->bo.bytes, 4u << 20);  // the smallest that holds it
    EXPECT_EQ(q.fit(5 << 20, 0xe9)->bo.bytes, 8u << 20);
    EXPECT_EQ(q.fit(17 << 20, 0xe9), q.ready.end());
    EXPECT_EQ(q.fit(1 << 20, 0xdb), q.ready.end());
    EXPECT_TRUE(q.asked({12 << 20, 0xe9}, 0));  // 16 MiB holds it: no second buffer
    EXPECT_FALSE(q.asked({24 << 20, 0xe9}, 0));
    q.wanted.push_back({32 << 20, 0xe9});
    EXPECT_TRUE(q.asked({24 << 20, 0xe9}, 0));  // a larger ask in flight covers it
    EXPECT_FALSE(q.asked({24 << 20, 0xce}, 0));
}

TEST(RawComp, StaleByCommitsOrTimeAndNeverInTheOpenCommandBuffer)
{
    constexpr uint64_t s = 1'000'000'000;
    // asked for in command buffer 100 (closed count 120), committed at 5 s; on compressed memory unless said
    EXPECT_FALSE(raw::backingStale(700, 100, 720, 120, 6 * s, 5 * s, false, true));
    EXPECT_TRUE(raw::backingStale(701, 100, 721, 120, 6 * s, 5 * s, false, true));  // 600 command buffers since
    EXPECT_FALSE(
        raw::backingStale(101, 100, 121, 120, 15 * s + 1, 5 * s, false, false));         // plain: the commit rule alone
    EXPECT_TRUE(raw::backingStale(101, 100, 121, 120, 15 * s + 1, 5 * s, false, true));  // 10 s with nothing committed
    EXPECT_TRUE(raw::backingStale(101, 100, 121, 120, 5 * s, 5 * s, true, false));       // retired: at once
    EXPECT_FALSE(raw::backingStale(101, 100, 121, 121, 60 * s, 5 * s, true, true));  // asked for since the last close
    EXPECT_FALSE(raw::backingStale(100, 100, 121, 121, 60 * s, 5 * s, false, true));
    EXPECT_FALSE(raw::readyStale(100, 100, 10 * s, 1 * s));
    EXPECT_TRUE(raw::readyStale(100, 100, 11 * s + 1, 1 * s));  // made for a shape gone before it arrived
    EXPECT_TRUE(raw::readyStale(701, 100, 2 * s, 1 * s));
}

TEST(RawComp, SizeClassesTakeAnyGranule)
{
    EXPECT_EQ(raw::sizeClassBytes(1, 4096), 4096u);
    EXPECT_EQ(raw::sizeClassBytes(9 * 65536, 65536), 10u * 65536);
    EXPECT_EQ(raw::sizeClassBytes(7296u << 10, raw::kCompGranule), raw::compClassBytes(7296u << 10));
}
}  // namespace
