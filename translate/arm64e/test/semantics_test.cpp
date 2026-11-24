// The stubs against the instructions they replace, executed side by side on Apple silicon (which has LSE, FP16, FCMA,
// JSCVT and RDM). Each harness keeps process-wide state (JIT pages, signal handlers, counters), so each run gets a
// process of its own.
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

extern "C" {
int lse_semantics_main(int argc, char **argv);
int fp_semantics_main(int argc, char **argv);
void fp_semantics_counts(uint64_t out[4]);
#include "../a64x/a64x.h"
}

namespace {

int runLse(std::initializer_list<const char *> args)
{
    char *argv[8] = {const_cast<char *>("lse_semantics")};
    int argc = 1;
    for (const char *a : args)
        argv[argc++] = const_cast<char *>(a);
    return lse_semantics_main(argc, argv);
}

int runFp(std::initializer_list<const char *> args)
{
    char *argv[8] = {const_cast<char *>("fp_semantics")};
    int argc = 1;
    for (const char *a : args)
        argv[argc++] = const_cast<char *>(a);
    return fp_semantics_main(argc, argv);
}

TEST(LseStubs, SystematicEncodingsMatchTheInstruction)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        EXPECT_EQ(0, runLse({"--gen", "6"}));
    });
}

TEST(LseStubs, RandomEncodingsMatchTheInstruction)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        EXPECT_EQ(0, runLse({"--random", "3000", "4"}));
    });
}

TEST(LseStubs, ACorruptedStubIsCaught)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        setenv("A64X_TEST_BREAK", "1", 1);
        EXPECT_NE(0, runLse({"--gen", "1"}));
    });
}

constexpr uint64_t kSite = 0x100000, kLaid = 0x200000;
constexpr uint32_t kX16X17 = (1u << 16) | (1u << 17), kDmbIsh = 0xD5033BBF;

std::vector<uint32_t> stubOf(uint32_t insn, uint32_t dead)
{
    uint32_t st[A64X_STUB_MAX];
    const int n = a64x_stub_v(insn, kSite, kLaid, dead, 0, st, A64X_STUB_MAX);
    return n > 0 ? std::vector<uint32_t>(st, st + n) : std::vector<uint32_t>();
}

int64_t branchOffset(uint32_t w)
{
    if ((w & 0xFC000000u) == 0x14000000u)
        return static_cast<int64_t>(static_cast<int32_t>(w << 6) >> 4);
    if ((w & 0x7E000000u) == 0x34000000u)
        return static_cast<int64_t>(static_cast<int32_t>(w << 8) >> 13) * 4;
    return 0;
}

// The words a stub runs until its branch back: every CBZ/CBNZ falls through (the compare matched, the store-exclusive
// succeeded), or with failCompare the first one is taken (CAS/CASP: the compare failed).
std::vector<uint32_t> pathOf(const std::vector<uint32_t> &st, bool failCompare)
{
    std::vector<uint32_t> out;
    bool take = failCompare;
    for (size_t k = 0; k < st.size() && out.size() < 64;) {
        const uint32_t w = st[k];
        const uint64_t to = kLaid + 4 * k + static_cast<uint64_t>(branchOffset(w));
        out.push_back(w);
        if ((w & 0xFC000000u) == 0x14000000u) {
            if (to == kSite + 4)
                break;
            k = (to - kLaid) / 4;
        } else if ((w & 0x7E000000u) == 0x34000000u && take) {
            take = false;
            k = (to - kLaid) / 4;
        } else {
            k++;
        }
    }
    return out;
}

bool storeExclusive(uint32_t w)
{
    return (w & 0x3FE00000u) == 0x08000000u || (w & 0xBFE00000u) == 0x88200000u;
}

// LD<op> and SWP (A = bit 23, R = bit 22), CAS and CASP (A = bit 22, R = bit 15) in every size and order, with
// aliasing registers.
std::vector<uint32_t> lseForms()
{
    std::vector<uint32_t> out;
    const uint32_t regs[][3] = {{1, 2, 3}, {4, 4, 5}, {8, 31, 9}, {6, 7, 7}, {12, 13, 31}};
    for (const auto &r : regs) {
        for (uint32_t ar = 0; ar < 4; ar++) {
            for (uint32_t size = 0; size < 4; size++) {
                for (uint32_t op = 0; op < 9; op++)
                    out.push_back((size << 30) | 0x38200000u | (ar << 22) | (r[0] << 16) | ((op == 8 ? 1u : 0u) << 15) |
                                  ((op & 7) << 12) | (r[2] << 5) | r[1]);
                out.push_back((size << 30) | 0x08A07C00u | ((ar >> 1) << 22) | ((ar & 1) << 15) | (r[0] << 16) |
                              (r[2] << 5) | r[1]);
            }
            for (uint32_t x = 0; x < 2; x++)
                out.push_back((x << 30) | 0x08207C00u | ((ar >> 1) << 22) | ((ar & 1) << 15) | (10u << 16) |
                              (r[2] << 5) | 12u);
        }
    }
    return out;
}

// An acquire+release form's stub runs one DMB ISH, right after the retry branch of its store-exclusive, when it stores;
// a failed compare stores nothing and runs none, and neither does any other form.
TEST(LseStubs, OnlyAcquireReleaseStoresAreFollowedByABarrier)
{
    int al = 0;
    for (uint32_t insn : lseForms()) {
        const bool cas = a64x_classify(insn) == A64X_LSE_CAS || a64x_classify(insn) == A64X_LSE_CASP;
        const bool both = cas ? ((insn >> 22) & 1) && ((insn >> 15) & 1) : ((insn >> 22) & 3) == 3;
        for (uint32_t dead : {0u, kX16X17}) {
            const std::vector<uint32_t> st = stubOf(insn, dead);
            ASSERT_FALSE(st.empty()) << std::hex << insn;
            const std::vector<uint32_t> ok = pathOf(st, false);
            const auto store = std::find_if(ok.begin(), ok.end(), storeExclusive);
            ASSERT_LT(store + 2, ok.end()) << std::hex << insn;
            EXPECT_EQ(std::count(ok.begin(), ok.end(), kDmbIsh), both ? 1 : 0) << std::hex << insn;
            EXPECT_EQ(store[2] == kDmbIsh, both) << std::hex << insn << " dead " << dead;
            if (cas) {
                const std::vector<uint32_t> failed = pathOf(st, true);
                EXPECT_EQ(std::count(failed.begin(), failed.end(), kDmbIsh), 0) << std::hex << insn;
            }
            al += both;
        }
    }
    EXPECT_EQ(al, 2 * 5 * (4 * 10 + 2));
}

struct Golden {
    uint32_t insn;
    std::vector<uint32_t> body;  // the stub without its branch back
};

void expectStub(const Golden &g)
{
    const std::vector<uint32_t> st = stubOf(g.insn, kX16X17);
    ASSERT_EQ(st.size(), g.body.size() + 1) << std::hex << g.insn;
    EXPECT_EQ(std::vector<uint32_t>(st.begin(), st.end() - 1), g.body) << std::hex << g.insn;
    EXPECT_EQ(st.back(), a64x_branch(kLaid + 4 * g.body.size(), kSite + 4)) << std::hex << g.insn;
}

// Shared stubs of iOS 27's (24B119) dyld cache plan, read back from it: the forms without both acquire and release
// stay as they were, word for word.
TEST(LseStubs, OtherFormsKeepTheirWords)
{
    const Golden forms[] = {
        {0xF8360109, {0xC85F7D09, 0x8B160131, 0xC8107D11, 0x35FFFFB0}},  // ldadd x22, x9, [x8]
        {0xB8690100, {0x885F7D00, 0x0B090011, 0x8810FD11, 0x35FFFFB0}},  // ldaddl w9, w0, [x8]
        {0xF83712A8, {0xC85F7EA8, 0x8A370111, 0xC8107EB1, 0x35FFFFB0}},  // ldclr x23, x8, [x21]
        {0x38288008, {0x085F7C11, 0x08107C08, 0x35FFFFD0, 0x2A1103E8}},  // swpb w8, w8, [x0]
        {0xF8BF8121, {0xC85FFD21, 0xC8107D3F, 0x35FFFFD0}},              // swpa xzr, x1, [x9]
        {0xC8ABFD49,                                                     // casl x11, x9, [x10]
         {0xC85F7D50, 0xCA0B0211, 0xB5000091, 0xC811FD49, 0x35FFFF91, 0x14000002, 0xD503305F, 0xAA1003EB}},
        {0x08E87E69,  // casab w8, w9, [x19]
         {0x085FFE70, 0x4A080211, 0x12001E31, 0x35000091, 0x08117E69, 0x35FFFF71, 0x14000002, 0xD503305F, 0x2A1003E8}},
        {0x48207D56,  // casp x0, x1, x22, x23, [x10]
         {0xA9B72FE9, 0xC87F4550, 0xCA000209, 0xCA01022B, 0xAA0B0129, 0xB5000089, 0xC8295D56, 0x35FFFF49, 0x14000003,
          0xC8294550, 0x35FFFEE9, 0xAA1003E0, 0xAA1103E1, 0xA8C92FE9}},
        {0x38334008,  // ldsmaxb w19, w8, [x0]
         {0xA9B72BE9, 0xD53B4209, 0x13001E6A, 0x085F7C08, 0x13001D11, 0x6B0A023F, 0x1A8AC111, 0x08107C11, 0x35FFFF70,
          0xD51B4209, 0xA8C92BE9}},
    };
    for (const Golden &g : forms)
        expectStub(g);
}

// The same plan's stubs for acquire+release forms, with the DMB ISH after the store-exclusive's retry branch (a CAS or
// CASP failure branch now jumps one word further).
TEST(LseStubs, AcquireReleaseFormsGainOneBarrier)
{
    const Golden forms[] = {
        {0xF8E90118, {0xC85FFD18, 0x8B090311, 0xC810FD11, 0x35FFFFB0, kDmbIsh}},  // ldaddal x9, x24, [x8]
        {0x38FF8108, {0x085FFD11, 0x0810FD1F, 0x35FFFFD0, kDmbIsh, 0x2A1103E8}},  // swpalb wzr, w8, [x8]
        {0xC8E8FE60,                                                              // casal x8, x0, [x19]
         {0xC85FFE70, 0xCA080211, 0xB50000B1, 0xC811FE60, 0x35FFFF91, kDmbIsh, 0x14000002, 0xD503305F, 0xAA1003E8}},
        {0x4862FD02,  // caspal x2, x3, x2, x3, [x8]
         {0xA9B72BE9, 0xC87FC510, 0xCA020209, 0xCA03022A, 0xAA0A0129, 0xB50000A9, 0xC8298D02, 0x35FFFF49, kDmbIsh,
          0x14000003, 0xC829C510, 0x35FFFEC9, 0xAA1003E2, 0xAA1103E3, 0xA8C92BE9}},
        {0x38F34008,  // ldsmaxalb w19, w8, [x0]
         {0xA9B72BE9, 0xD53B4209, 0x13001E6A, 0x085FFC08, 0x13001D11, 0x6B0A023F, 0x1A8AC111, 0x0810FC11, 0x35FFFF70,
          kDmbIsh, 0xD51B4209, 0xA8C92BE9}},
    };
    for (const Golden &g : forms)
        expectStub(g);
}

// Every sampled form of the class has a stub that matches the instruction; for these classes the classifier is exact
// (no form it flags is unallocated).
void expectExact(enum a64x_class cls, const char *samples)
{
    switch_ios::test::ExpectPassesInFreshProcess([cls, samples] {
        char c[8];
        snprintf(c, sizeof(c), "%d", static_cast<int>(cls));
        EXPECT_EQ(0, runFp({"300", "--gen", samples, c}));
        uint64_t n[4];
        fp_semantics_counts(n);
        EXPECT_GT(n[0], 0u);
        EXPECT_EQ(0u, n[1]) << "mismatching words";
        EXPECT_EQ(0u, n[2]) << "words without a stub";
        EXPECT_EQ(0u, n[3]) << "flagged words this host does not execute";
    });
}

TEST(FpStubs, FcmaFormsMatchTheInstruction)
{
    expectExact(A64X_FCMA, "3000000");
}

TEST(FpStubs, JscvtMatchesTheInstruction)
{
    expectExact(A64X_JSCVT, "3000000");
}

// FP16 and RDM: the classifier also flags unallocated encodings in these groups (valid code never holds them; data
// in __text is kept away by the island pass). Every form the host executes must match.
void expectMatches(enum a64x_class cls, const char *samples)
{
    switch_ios::test::ExpectPassesInFreshProcess([cls, samples] {
        char c[8];
        snprintf(c, sizeof(c), "%d", static_cast<int>(cls));
        EXPECT_EQ(0, runFp({"200", "--gen", samples, c}));
        uint64_t n[4];
        fp_semantics_counts(n);
        EXPECT_GT(n[0], 50u);
        EXPECT_EQ(0u, n[1]) << "mismatching words";
        EXPECT_EQ(0u, n[2]) << "words without a stub";
    });
}

TEST(FpStubs, Fp16FormsMatchTheInstruction)
{
    expectMatches(A64X_FP16, "3000000");
}

TEST(FpStubs, RdmFormsMatchTheInstruction)
{
    expectMatches(A64X_RDM, "3000000");
}

// The min forms of the FP16 across-lanes and scalar pairwise reductions differ from the max forms only in bit 23.
TEST(FpStubs, Fp16MinReductionsMatchTheInstruction)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        // FMINV h, FMINNMV h, FMINP h, FMINNMP h
        EXPECT_EQ(0, runFp({"2000", "0eb0fa66", "4eb0c8d3", "5eb0f97a", "5eb0c927"}));
        uint64_t n[4];
        fp_semantics_counts(n);
        EXPECT_EQ(4u, n[0]);
    });
}

}  // namespace
