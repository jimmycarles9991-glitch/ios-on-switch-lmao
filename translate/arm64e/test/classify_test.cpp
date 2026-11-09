// The classifier and the one-word rewrites on known encodings, and the fast-reject table against the full decoder.
#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

extern "C" {
#include "../a64x/a64x.h"
}

namespace {

constexpr uint32_t kNop = 0xD503201F;
constexpr uint32_t kRet = 0xD65F03C0;

struct Case {
    uint32_t word;
    enum a64x_class cls;
    bool rewrites;
    uint32_t out;
    const char *what;
};

const Case kCases[] = {
    {0xD65F0FFF, A64X_PAC_RET, true, kRet, "retab"},
    {0xD65F0BFF, A64X_PAC_RET, true, kRet, "retaa"},
    {0xD71F0A11, A64X_PAC_BR, true, 0xD61F0200, "braa x16, x17 -> br x16"},
    {0xD61F0A1F, A64X_PAC_BR, true, 0xD61F0200, "braaz x16 -> br x16"},
    {0xD73F0911, A64X_PAC_BLR, true, 0xD63F0100, "blraa x8, x17 -> blr x8"},
    {0xD63F091F, A64X_PAC_BLR, true, 0xD63F0100, "blraaz x8 -> blr x8"},
    {0xDAC10020, A64X_PAC_DP, true, kNop, "pacia x0, x1"},
    {0xDAC11862, A64X_PAC_DP, true, kNop, "autda x2, x3"},
    {0xDAC123E0, A64X_PAC_DP, true, kNop, "paciza x0"},
    {0xDAC143FE, A64X_PAC_XPAC, true, kNop, "xpaci x30"},
    {0x9AC23020, A64X_PAC_PACGA, true, 0xAA1F03E0, "pacga x0, x1, x2 -> mov x0, xzr"},
    {0xF8201420, A64X_PAC_LDRA, true, 0xF9400420, "ldraa x0, [x1, #8] -> ldr x0, [x1, #8]"},
    {0xF87C1C20, A64X_PAC_LDRA, false, 0, "ldraa x0, [x1, #-504]!: out of LDR pre-index range, a stub"},
    {0xF87FF420, A64X_PAC_LDRA, true, 0xF85F8020, "ldraa x0, [x1, #-8] -> ldur x0, [x1, #-8]"},
    {0xF8BFC020, A64X_RCPC_LDAPR, true, 0xC8DFFC20, "ldapr x0, [x1] -> ldar"},
    {0x38BFC020, A64X_RCPC_LDAPR, true, 0x08DFFC20, "ldaprb w0, [x1] -> ldarb"},
    {0xD50B7C20, A64X_DPB, true, 0xD50B7A20, "dc cvap x0 -> dc cvac"},
    {0xF8E20003, A64X_LSE_LDOP, false, 0, "ldaddal x2, x3, [x0]"},
    {0xC8E2FC03, A64X_LSE_CAS, false, 0, "casal x2, x3, [x0]"},
    {0xF8A28003, A64X_LSE_SWP, false, 0, "swpa x2, x3, [x0]"},
    {0x48227C04, A64X_LSE_CASP, false, 0, "casp x2, x3, x4, x5, [x0]"},
    {0x1E7E0000, A64X_JSCVT, false, 0, "fjcvtzs w0, d0"},
    {0x6E80C400, A64X_FCMA, false, 0, "fcmla v0.4s, v0.4s, v0.4s, #0"},
    {0x6EC0FC00, A64X_I8MM_BF16, false, 0, "bfmlalt v0.4s, v0.8h, v0.8h (not FCMA)"},
    {0x6E40EC00, A64X_I8MM_BF16, false, 0, "bfmmla v0.4s, v0.8h, v0.8h (not FCMA)"},
    {0x0EB0F800, A64X_FP16, false, 0, "fminv h0, v0.4h"},
    {0x5EB0C800, A64X_FP16, false, 0, "fminnmp h0, v0.2h"},
    {0x5E30D800, A64X_FP16, false, 0, "faddp h0, v0.2h"},
    {0x8B020020, A64X_OK, false, 0, "add x0, x1, x2"},
    {0xD503237F, A64X_OK, false, 0, "pacibsp (a hint: a NOP on ARMv8.0)"},
    {0xD50323BF, A64X_OK, false, 0, "autiasp (hint)"},
    {0xC85FFC20, A64X_OK, false, 0, "ldaxr x0, [x1]"},
};

TEST(Classify, KnownEncodings)
{
    for (const Case &c : kCases) {
        EXPECT_EQ(c.cls, a64x_classify(c.word)) << c.what << " " << a64x_class_name(a64x_classify(c.word));
        uint32_t out = 0;
        bool rewrote = a64x_rewrite1(c.word, &out);
        EXPECT_EQ(c.rewrites, rewrote) << c.what;
        if (c.rewrites && rewrote)
            EXPECT_EQ(c.out, out) << c.what << std::hex << " got " << out;
    }
}

TEST(Classify, HintSpacePacIsLeftAlone)
{
    EXPECT_TRUE(a64x_is_pac_hint(0xD503237F));  // pacibsp
    EXPECT_TRUE(a64x_is_pac_hint(0xD50323FF));  // autibsp
    EXPECT_FALSE(a64x_is_pac_hint(kNop));
    uint32_t out;
    EXPECT_FALSE(a64x_rewrite1(0xD503237F, &out));
}

TEST(Classify, EveryRewriteIsArmv80)
{
    for (const Case &c : kCases) {
        uint32_t out;
        if (a64x_rewrite1(c.word, &out))
            EXPECT_EQ(A64X_OK, a64x_classify(out)) << c.what;
    }
}

TEST(Classify, BaselineClassesAreTheA12Ones)
{
    for (int c = A64X_PAC_BR; c <= A64X_DPB; c++)
        EXPECT_TRUE(a64x_class_is_baseline(static_cast<enum a64x_class>(c))) << a64x_class_name((enum a64x_class)c);
    EXPECT_FALSE(a64x_class_is_baseline(A64X_OK));
    EXPECT_FALSE(a64x_class_is_baseline(A64X_SVE_SME));
    EXPECT_FALSE(a64x_class_is_baseline(A64X_DOTPROD));
}

// The fast path rejects a word by its top 16 bits from a table generated over all 2^32 words; a decoder change that
// lets a new prefix hold a class needs the table regenerated (arm64e gen-toptable).
TEST(Classify, FastPathAgreesWithTheDecoder)
{
    uint64_t s = 0x2545F4914F6CDD1Dull;
    for (int i = 0; i < 4000000; i++) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        uint32_t w = static_cast<uint32_t>(s >> 16);
        ASSERT_EQ(a64x_classify_slow(w), a64x_classify(w)) << std::hex << w;
    }
    for (uint32_t prefix : {0x5EB0u, 0x0EB0u, 0x4EB0u, 0xF8E2u, 0xD65Fu, 0xDAC1u, 0x6E80u}) {
        for (uint32_t low = 0; low < 0x10000; low++) {
            uint32_t w = prefix << 16 | low;
            ASSERT_EQ(a64x_classify_slow(w), a64x_classify(w)) << std::hex << w;
        }
    }
}

TEST(Stubs, BranchEncoding)
{
    EXPECT_EQ(0x14000001u, a64x_branch(0x1000, 0x1004));
    EXPECT_EQ(0x17FFFFFFu, a64x_branch(0x1004, 0x1000));
    EXPECT_EQ(0u, a64x_branch(0, 1ull << 28));  // out of range
}

TEST(Stubs, LseStubEndsWithTheReturnBranch)
{
    uint32_t st[A64X_STUB_MAX];
    const uint64_t site = 0x100000, stub = 0x200000;
    int n = a64x_stub(0xF8E20003, site, stub, 0, st, A64X_STUB_MAX);
    ASSERT_GT(n, 0);
    ASSERT_LE(n, A64X_STUB_MAX);
    EXPECT_EQ(a64x_branch(stub + 4ull * (n - 1), site + 4), st[n - 1]);
    for (int i = 0; i < n; i++)
        EXPECT_FALSE(a64x_class_is_baseline(a64x_classify(st[i]))) << i;
    EXPECT_EQ(-1, a64x_stub(0x8B020020, site, stub, 0, st, A64X_STUB_MAX));  // nothing to replace
}

// A site, then RET: V16-V31 (caller-saved, no return values) and x16/x17 are dead after the site.
TEST(Liveness, TheWalkCoversTheWindowOrFindsNothingDead)
{
    const uint32_t window[] = {0x0EB0F800, kRet};
    std::vector<uint8_t> buf(A64X_WALK_BYTES(2));
    a64x_walk fits = a64x_walk_in(buf.data(), 2), short_by_one = a64x_walk_in(buf.data(), 1);
    EXPECT_EQ(0xFFFF0000u, a64x_dead_vregs_flow(window, 2, 0, 0xFFFFFFFFu, 100, false, &fits));
    EXPECT_EQ(0x30000u, a64x_dead_regs_flow(window, 2, 0, 0x7FFFFFFFu, 100, &fits));
    EXPECT_EQ(0u, a64x_dead_vregs_flow(window, 2, 0, 0xFFFFFFFFu, 100, false, &short_by_one));
    EXPECT_EQ(0u, a64x_dead_regs_flow_fn(window, 2, 0, 0x7FFFFFFFu, 100, &short_by_one));
}

// The words of a page found data by the island rule, as a mask over them.
uint64_t islandMask(const std::initializer_list<uint32_t> words)
{
    const std::vector<uint32_t> w(words);
    uint8_t data[8] = {};
    a64x_islands is = {data, nullptr, 0, 0, false, false, 0, 0, 0, 0};
    a64x_islands_page(&is, w.data(), static_cast<uint32_t>(w.size()), 0, 0x1000, nullptr);
    a64x_islands_end(&is, w.size());
    uint64_t mask = 0;
    for (size_t i = 0; i < w.size(); i++)
        mask |= static_cast<uint64_t>((data[i >> 3] >> (i & 7)) & 1) << i;
    return mask;
}

TEST(Islands, ATableOfSmallSixtyFourBitValuesIsData)
{
    // lzfse's mask table behind lzfse_decode_literals: the low half of 2^29 - 1 decodes as an FP16 FNMSUB.
    EXPECT_EQ(0b0001010100u, islandMask({0xAA0003E8, kRet, 0x1FFFFFFF, 0, 0x3FFFFFFF, 0, 0x7FFFFFFF, 0, 0, 0}));
}

TEST(Islands, CodeThatEndsInACallOrABranchBeforePaddingStaysCode)
{
    EXPECT_EQ(0u, islandMask({0xAA0003E8, 0x94000010, 0, 0xAA0003E8, 0xD65F0FFF, 0, 0xAA0003E8, 0xD63F0100}));
}

TEST(Islands, DataRightBehindItsCodeIsDataAndTheCodeStays)
{
    // A call that does not return, then a constant, then padding.
    EXPECT_EQ(0b010000u, islandMask({0xAA0003E8, 0xD65F0FFF, 0xAA0003E8, 0x94000010, 0x1FFFFFFF, 0}));
}

}  // namespace
