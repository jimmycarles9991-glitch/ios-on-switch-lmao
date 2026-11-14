// SPDX-License-Identifier: MIT
// raw/runtime/raw_comp.h on the host: the compressible kinds the raw path binds are ones the kernel's kind table
// (nouveau gm200_mmu_kind, which gm20b uses) takes as compressible and demotes as raw_comp.h says, the boot ZBC
// table's values read the same through the raw path's Metal formats and through nvdec's NV formats (pack/nvdec_comp.h),
// only the backings the copy engine never touches compress, and a refused want is asked for again as the kernel's answer
// allows.
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"

#include <cstdint>
#include <cstring>
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
    EXPECT_EQ(raw::compressedKind(4, 0), 0xdbu);
    EXPECT_EQ(raw::compressedKind(8, 0), 0xe9u);
    EXPECT_EQ(raw::compressedKind(16, 0), 0xf5u);
    EXPECT_EQ(raw::compressedKind(8, 260), 0xceu);
    EXPECT_EQ(raw::compressedKind(1, 0), 0u);
    EXPECT_EQ(raw::compressedKind(2, 0), 0u);
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

// Maxwell's dev_mmu.h numbers (gm107): every sample count a target takes (1, 2, 4) and every depth format with a
// compressible kind, each one the kernel compresses and demotes to the kind raw_zeta.h binds that format with uncompressed
TEST(RawComp, SampleCountsAndDepthFormatsHaveTheirKinds)
{
    const std::vector<uint32_t> table = kernelKindTable();
    ASSERT_EQ(table.size(), 256u);
    const uint32_t colour[3][3] = {{0xdb, 0xcc, 0xe2}, {0xe9, 0xcd, 0xf0}, {0xf5, 0xf7, 0xf9}};
    const uint32_t bpps[3] = {4, 8, 16}, samples[3] = {1, 2, 4};
    for (uint32_t b = 0; b < 3; b++)
        for (uint32_t s = 0; s < 3; s++) {
            const uint32_t kind = raw::compressedKind(bpps[b], 0, samples[s]);
            EXPECT_EQ(kind, colour[b][s]) << bpps[b] << " " << samples[s];
            EXPECT_EQ(table[kind], 0xfeu) << std::hex << kind;
            EXPECT_EQ(raw::demotedKind(kind), 0xfeu) << std::hex << kind;
        }
    const std::pair<uint32_t, uint32_t> zeta[] = {{250, 0x01}, {252, 0x7b}, {260, 0xc3}};  // raw_zeta.h plain kinds
    for (const auto &[format, plain] : zeta)
        for (uint32_t s : samples) {
            const uint32_t kind = raw::compressedKind(8, format, s);
            ASSERT_NE(kind, 0u) << format << " " << s;
            EXPECT_EQ(table[kind], plain) << std::hex << kind;
            EXPECT_EQ(raw::demotedKind(kind), plain) << std::hex << kind;
        }
    EXPECT_EQ(raw::compressedKind(1, 253), 0u);  // Stencil8: none
    EXPECT_EQ(raw::compressedKind(4, 0, 8), 0u);
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

// What the copy engine fills or drains compresses only at the kinds gpu.x1.ce_compressed proved (one-sample C32 and C64)
// and gpu.x1.ce_compressed_depth proved as shipped (one-sample ZF32_2CZ), and the ones shipped on for their arms to judge:
// C128_2CR and the 2x and 4x colour kinds (gpu.x1.ce_compressed), Z16_2C and ZF32_X24S8_2CSZV (gpu.raw.stored_depth)
TEST(RawComp, CopiedBackingsCompressOnlyAtProvenKinds)
{
    EXPECT_TRUE(raw::copyEngineReadsCompressed(0xdb));
    EXPECT_TRUE(raw::copyEngineReadsCompressed(0xe9));
    EXPECT_TRUE(raw::copyEngineReadsCompressed(0x86));
    EXPECT_TRUE(raw::copyEngineReadsCompressed(0x02));
    EXPECT_TRUE(raw::copyEngineReadsCompressed(0xce));
    EXPECT_TRUE(raw::copyEngineReadsCompressed(0xf5)) << "C128_2CR, gpu.x1.ce_compressed's RGBA32Float cases";
    for (uint32_t kind : {0xccu, 0xe2u, 0xcdu, 0xf0u, 0xf7u, 0xf9u})
        EXPECT_TRUE(raw::copyEngineReadsCompressed(kind)) << "gpu.x1.ce_compressed's ms cases " << std::hex << kind;
    for (uint32_t kind : {0x03u, 0x04u, 0x87u, 0x88u, 0xcfu, 0xd0u})
        EXPECT_FALSE(raw::copyEngineReadsCompressed(kind)) << std::hex << kind;

    // memoryless: the copy engine never reaches them
    EXPECT_EQ(raw::backingKind(true, true, 0, 4, 1, false), 0xdbu);
    EXPECT_EQ(raw::backingKind(true, true, 0, 8, 1, false), 0xe9u);
    EXPECT_EQ(raw::backingKind(true, true, 0, 16, 1, false), 0xf5u);
    EXPECT_EQ(raw::backingKind(true, true, 0, 4, 4, false), 0xe2u);
    EXPECT_EQ(raw::backingKind(true, true, 0, 8, 2, false), 0xcdu);
    EXPECT_EQ(raw::backingKind(true, true, 260, 8, 1, false), 0xceu);
    EXPECT_EQ(raw::backingKind(true, true, 250, 2, 4, false), 0x04u);
    // shadows: the copy engine fills and stores them
    EXPECT_EQ(raw::backingKind(true, true, 0, 4, 1, true), 0xdbu);
    EXPECT_EQ(raw::backingKind(true, true, 0, 8, 1, true), 0xe9u);
    EXPECT_EQ(raw::backingKind(true, true, 0, 16, 1, true), 0xf5u);
    EXPECT_EQ(raw::backingKind(true, true, 0, 4, 2, true), 0xccu);
    EXPECT_EQ(raw::backingKind(true, true, 0, 8, 4, true), 0xf0u);
    // zetas the shader's depth tests copy with the copy engine: one-sample Depth32Float alone is proven
    EXPECT_EQ(raw::backingKind(true, true, 252, 4, 1, false), 0x86u);
    EXPECT_EQ(raw::backingKind(true, true, 252, 4, 4, false), 0u);
    EXPECT_EQ(raw::backingKind(true, true, 252, 4, 2, false), 0u);
    EXPECT_EQ(raw::backingKind(true, true, 260, 8, 2, false), 0u);
    EXPECT_EQ(raw::backingKind(true, true, 260, 8, 1, true), 0xceu);

    // a debug build's gpu.x1.ce_compressed_depth probe gives those zetas their kinds, and nothing else
    EXPECT_EQ(raw::backingKind(true, true, 252, 4, 1, false, true), 0x86u);
    EXPECT_EQ(raw::backingKind(true, true, 260, 8, 2, false, true), 0xcfu);
    EXPECT_EQ(raw::backingKind(true, true, 260, 8, 4, true, false), 0u) << "a multisampled zeta stays unproven";

    EXPECT_EQ(raw::backingKind(true, false, 0, 4, 1, false), 0u);    // pitch
    EXPECT_EQ(raw::backingKind(false, true, 260, 8, 1, false), 0u);  // compression off
    EXPECT_EQ(raw::backingKind(true, true, 0, 2, 1, false), 0u);     // no compressible kind
    EXPECT_EQ(raw::backingKind(true, true, 253, 1, 1, false), 0u);   // Stencil8
}

// A stored depth texture binds its compressible kind at one sample and one layer, where the copy engine (its readback
// and aspect copies) reads that kind as plain texels
TEST(RawComp, StoredOneSampleDepthBindsItsCompressibleKind)
{
    EXPECT_EQ(raw::storedZetaKind(true, 252, 1, 1), 0x86u);
    EXPECT_EQ(raw::storedZetaKind(true, 250, 1, 1), 0x02u) << "Depth16Unorm: Z16_2C";
    EXPECT_EQ(raw::storedZetaKind(true, 260, 1, 1), 0xceu) << "Depth32Float_Stencil8: ZF32_X24S8_2CSZV";
    EXPECT_EQ(raw::storedZetaKind(true, 260, 1, 6), 0u) << "a cube";
    EXPECT_EQ(raw::storedZetaKind(false, 252, 1, 1), 0u);
    EXPECT_EQ(raw::storedZetaKind(true, 252, 1, 2), 0u) << "an array or cube";
    EXPECT_EQ(raw::storedZetaKind(true, 252, 4, 1), 0u);
    EXPECT_EQ(raw::storedZetaKind(true, 253, 1, 1), 0u) << "Stencil8 has no compressible kind";
    EXPECT_EQ(raw::storedZetaKind(true, 0, 1, 1), 0u);
}

// A ZBC entry holds the texel the ROP writes for the clear: packed in the format's channel order, unorm channels only at
// exact codes, halves rounded to nearest even
TEST(RawComp, ZbcEntriesHoldTheTexelTheRopWrites)
{
    const auto bits = [](float f) {
        uint32_t u;
        memcpy(&u, &f, 4);
        return u;
    };
    TegraGM20BAbiZbc e;
    const uint32_t quarter[4] = {bits(0.0f), bits(64 / 255.0f), bits(128 / 255.0f), bits(1.0f)};
    ASSERT_TRUE(raw::zbcColorEntry(80, quarter, e));  // BGRA8Unorm
    EXPECT_EQ(e.format, raw::kZbcA8R8G8B8);
    EXPECT_EQ(e.l2[0], 0xff004080u);
    ASSERT_TRUE(raw::zbcColorEntry(70, quarter, e));  // RGBA8Unorm
    EXPECT_EQ(e.format, raw::kZbcA8B8G8R8);
    EXPECT_EQ(e.l2[0], 0xff804000u);
    EXPECT_EQ(memcmp(e.ds, quarter, sizeof e.ds), 0);
    const uint32_t tie[4] = {bits(0.5f), 0, 0, bits(1.0f)};  // 127.5: the ROP's tie, not Apple's
    EXPECT_FALSE(raw::zbcColorEntry(80, tie, e));
    EXPECT_FALSE(raw::zbcColorEntry(55, quarter, e));  // R32Float: no ZBC format
    const uint32_t halves[4] = {bits(0.5f), bits(-2.0f), bits(65504.0f), bits(1.0f / 3)};
    ASSERT_TRUE(raw::zbcColorEntry(115, halves, e));
    EXPECT_EQ(e.l2[0], 0x3800u | 0xc000u << 16);
    EXPECT_EQ(e.l2[1], 0x7bffu | 0x3555u << 16);
    EXPECT_EQ(raw::zbcHalf(bits(1e-8f)), 0u);
    EXPECT_EQ(raw::zbcHalf(bits(65520.0f)), 0x7c00u);
    EXPECT_EQ(raw::zbcHalf(bits(5.9604645e-8f)), 1u);
    ASSERT_TRUE(raw::zbcDepthEntry(252, 0.5f, e));
    EXPECT_EQ(e.type, uint32_t(TegraGM20BZbcDepth));
    EXPECT_EQ(e.ds[0], bits(0.5f));
    EXPECT_EQ(e.l2[0], bits(0.5f));
}

TEST(RawComp, ZbcValuesAreAskedForOnceRepeated)
{
    raw::ZbcTable table;
    TegraGM20BAbiZbc e = {}, asked;
    e.type = TegraGM20BZbcColor;
    e.format = raw::kZbcA8R8G8B8;
    bool wanted = false;
    for (uint8_t i = 1; i < raw::ZbcTable::kZbcRepeats; i++) {
        EXPECT_FALSE(table.lookup(e, wanted));
        EXPECT_FALSE(wanted);
    }
    EXPECT_FALSE(table.lookup(e, wanted));
    EXPECT_TRUE(wanted);
    EXPECT_TRUE(table.pending());
    ASSERT_TRUE(table.next(asked));
    EXPECT_FALSE(table.pending());
    table.answer(asked, true);
    EXPECT_TRUE(table.lookup(e, wanted));
    EXPECT_FALSE(wanted);
    // one-off values (an animated clear colour) never fill it: each takes the place of an older one seen once
    for (uint32_t v = 0; v < 1000; v++) {
        TegraGM20BAbiZbc once = e;
        once.ds[0] = v + 1;
        EXPECT_FALSE(table.lookup(once, wanted));
        EXPECT_FALSE(wanted);
    }
    EXPECT_TRUE(table.lookup(e, wanted));
}

// The kernel's answers for want of memory: boot 19 and boot 21 logged only NoMemory (its pool's floor or the client's
// share, "compressible BoAlloc of ... refused"), never another
TEST(RawComp, ShortagesAreTheKernelsMemoryAnswers)
{
    EXPECT_TRUE(raw::compShortage(kIOReturnNoMemory));     // the pool's floor or the client's share
    EXPECT_TRUE(raw::compShortage(kIOReturnNoResources));  // no comptags left
    EXPECT_TRUE(raw::compShortage(kIOReturnNoSpace));      // no GPU VA
    EXPECT_FALSE(raw::compShortage(kIOReturnBadArgument));
    EXPECT_FALSE(raw::compShortage(kIOReturnUnsupported));
}

// A want refused for a shortage is not asked again however many command buffers or seconds pass: only this process
// giving compressed memory back may change the kernel's answer, and the next want after that asks
TEST(RawComp, AShortageWaitsForMemoryGivenBackNotForTime)
{
    raw::CompQueue q;
    const raw::CompWant w{0x2800000, 0xdb}, other{0x20000, 0xce};
    EXPECT_FALSE(q.asked(w));
    q.wanted.push_back(w);
    EXPECT_TRUE(q.asked(w));
    q.wanted.clear();
    EXPECT_TRUE(q.refuse(w, true)) << "its first refusal is logged";
    for (int ask = 0; ask < 100000; ask++)
        ASSERT_TRUE(q.asked(w)) << "no back-off brings it back";
    EXPECT_FALSE(q.asked(other));
    EXPECT_FALSE(q.refuse(w, true)) << "refused again before anything came back: not logged again";
    q.freed();
    EXPECT_FALSE(q.asked(w));
    EXPECT_TRUE(q.refuse(w, true)) << "refused after memory came back: logged";
}

// Any other refusal has never fired: it is logged each time, fails that one ask (its backing plain) and is not
// remembered, so the next want of it is a new ask
TEST(RawComp, AnyOtherRefusalFailsThatAskAlone)
{
    raw::CompQueue q;
    const raw::CompWant w{0x20000, 0xdb};
    for (int ask = 0; ask < 3; ask++) {
        EXPECT_FALSE(q.asked(w));
        EXPECT_TRUE(q.refuse(w, false));
    }
    EXPECT_TRUE(q.refused.empty());
}

TEST(RawComp, NoRetryBackOffIsLeft)
{
    const std::string comp = switch_ios::test::SourceText("../raw/runtime/raw_comp.h") +
                             switch_ios::test::SourceText("../raw/runtime/raw_comp.cpp");
    for (const char *gone : {"compRetryAfter", "kCompRetry", "retryAt", "kCompLastingRefusals"})
        EXPECT_EQ(comp.find(gone), std::string::npos) << gone;
}

TEST(RawComp, AReadyBufferAnswersItsWant)
{
    raw::CompQueue q;
    raw::CompBo b;
    b.bo.bytes = 0x20000;
    b.kind = 0xce;
    q.ready.push_back(b);
    EXPECT_TRUE(q.asked({0x20000, 0xce}));
    EXPECT_FALSE(q.asked({0x20000, 0xe9}));
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
    EXPECT_TRUE(q.asked({12 << 20, 0xe9}));  // 16 MiB holds it: no second buffer
    EXPECT_FALSE(q.asked({24 << 20, 0xe9}));
    q.wanted.push_back({32 << 20, 0xe9});
    EXPECT_TRUE(q.asked({24 << 20, 0xe9}));  // a larger ask in flight covers it
    EXPECT_FALSE(q.asked({24 << 20, 0xce}));
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
