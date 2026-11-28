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

std::vector<uint32_t> stubOf(uint32_t insn, uint32_t dead, bool shared = false)
{
    uint32_t st[A64X_STUB_MAX];
    const int n = a64x_stub_v(insn, kSite, kLaid, dead, 0, shared, st, A64X_STUB_MAX);
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
// stay as expected (with CAS/CASP out-of-line fail paths and 3-register CASP).
TEST(LseStubs, OtherFormsKeepTheirWords)
{
    const uint32_t kRet5 = a64x_branch(kLaid + 4 * 5, kSite + 4);
    const uint32_t kRet7 = a64x_branch(kLaid + 4 * 7, kSite + 4);
    const uint32_t kRet9 = a64x_branch(kLaid + 4 * 9, kSite + 4);
    const Golden forms[] = {
        {0xF8360109, {0xC85F7D09, 0x8B160131, 0xC8107D11, 0x35FFFFB0}},  // ldadd x22, x9, [x8]
        {0xB8690100, {0x885F7D00, 0x0B090011, 0x8810FD11, 0x35FFFFB0}},  // ldaddl w9, w0, [x8]
        {0xF83712A8, {0xC85F7EA8, 0x8A370111, 0xC8107EB1, 0x35FFFFB0}},  // ldclr x23, x8, [x21]
        {0x38288008, {0x085F7C11, 0x08107C08, 0x35FFFFD0, 0x2A1103E8}},  // swpb w8, w8, [x0]
        {0xF8BF8121, {0xC85FFD21, 0xC8107D3F, 0x35FFFFD0}},              // swpa xzr, x1, [x9]
        {0xC8ABFD49,                                                     // casl x11, x9, [x10]
         {0xC85F7D50, 0xCA0B0211, 0xB5000091, 0xC811FD49, 0x35FFFF91, kRet5, 0xD503305F, 0xAA1003EB}},
        {0x08E87E69,  // casab w8, w9, [x19]
         {0x085FFE70, 0x4A080211, 0x12001E31, 0x350000B1, 0x08117E69, 0x35FFFF71, 0x2A1003E8, kRet7, 0xD503305F,
          0x2A1003E8}},
        {0x48207D56,  // casp x0, x1, x22, x23, [x10]
         {0xF8170FE9, 0xC87F4550, 0xCA000209, 0xB50000E9, 0xCA010229, 0xB50000A9, 0xC8295D56, 0x35FFFF49, 0xF84907E9,
          kRet9, 0xC8294550, 0x35FFFEC9, 0xAA1003E0, 0xAA1103E1, 0xF84907E9}},
        {0x38334008,  // ldsmaxb w19, w8, [x0]
         {0xA9B72BE9, 0xD53B4209, 0x13001E6A, 0x085F7C08, 0x13001D11, 0x6B0A023F, 0x1A8AC111, 0x08107C11, 0x35FFFF70,
          0xD51B4209, 0xA8C92BE9}},
    };
    for (const Golden &g : forms)
        expectStub(g);
}

// Acquire+release forms include one DMB ISH after the store-exclusive retry branch on the success path.
TEST(LseStubs, AcquireReleaseFormsGainOneBarrier)
{
    const uint32_t kRet6 = a64x_branch(kLaid + 4 * 6, kSite + 4);
    const uint32_t kRet10 = a64x_branch(kLaid + 4 * 10, kSite + 4);
    const Golden forms[] = {
        {0xF8E90118, {0xC85FFD18, 0x8B090311, 0xC810FD11, 0x35FFFFB0, kDmbIsh}},  // ldaddal x9, x24, [x8]
        {0x38FF8108, {0x085FFD11, 0x0810FD1F, 0x35FFFFD0, kDmbIsh, 0x2A1103E8}},  // swpalb wzr, w8, [x8]
        {0xC8E8FE60,                                                              // casal x8, x0, [x19]
         {0xC85FFE70, 0xCA080211, 0xB50000B1, 0xC811FE60, 0x35FFFF91, kDmbIsh, kRet6, 0xD503305F, 0xAA1003E8}},
        {0x4862FD02,  // caspal x2, x3, x2, x3, [x8]
         {0xF8170FE9, 0xC87FC510, 0xCA020209, 0xB5000109, 0xCA030229, 0xB50000C9, 0xC8298D02, 0x35FFFF49, kDmbIsh,
          0xF84907E9, kRet10, 0xC829C510, 0x35FFFEA9, 0xAA1003E2, 0xAA1103E3, 0xF84907E9}},
        {0x38F34008,  // ldsmaxalb w19, w8, [x0]
         {0xA9B72BE9, 0xD53B4209, 0x13001E6A, 0x085FFC08, 0x13001D11, 0x6B0A023F, 0x1A8AC111, 0x0810FC11, 0x35FFFF70,
          kDmbIsh, 0xD51B4209, 0xA8C92BE9}},
    };
    for (const Golden &g : forms)
        expectStub(g);
}

TEST(LseStubs, XzrStoresAndSwpAvoidSpills)
{
    const uint32_t stadd = 0xF820003Fu;    // stadd x0, [x1]
    const uint32_t stsetl = 0xF862307Fu;   // stsetl x2, [x3]
    const uint32_t swp_xzr = 0xF820803Fu;  // swp x0, xzr, [x1]
    for (uint32_t insn : {stadd, stsetl}) {
        const std::vector<uint32_t> st = stubOf(insn, kX16X17, true);
        ASSERT_FALSE(st.empty());
        EXPECT_FALSE(a64x_stub_spills(st.data(), static_cast<int>(st.size()))) << std::hex << insn;
        EXPECT_EQ(st.back(), 0xD65F03C0u);
    }
    const std::vector<uint32_t> swp1 = stubOf(swp_xzr, 1u << 16, true);
    ASSERT_FALSE(swp1.empty());
    EXPECT_FALSE(a64x_stub_spills(swp1.data(), static_cast<int>(swp1.size())));
    EXPECT_EQ(swp1.back(), 0xD65F03C0u);
}

TEST(LseStubs, SharedCasAndCaspEmitDualRet)
{
    constexpr uint32_t kRet = 0xD65F03C0u;
    for (uint32_t insn : {0xC8ABFD49u, 0xC8E8FE60u, 0x48207D56u, 0x4862FD02u}) {
        const std::vector<uint32_t> st = stubOf(insn, kX16X17, true);
        ASSERT_FALSE(st.empty()) << std::hex << insn;
        EXPECT_EQ(std::count(st.begin(), st.end(), kRet), 2) << std::hex << insn;
        EXPECT_EQ(st.back(), kRet) << std::hex << insn;
    }
}

TEST(LseStubs, SpillTelemetryAndSbAndRdmNullOut)
{
    const std::vector<uint32_t> no_spill = stubOf(0xF8360109u, kX16X17);
    EXPECT_FALSE(a64x_stub_spills(no_spill.data(), static_cast<int>(no_spill.size())));
    const std::vector<uint32_t> gpr_stp = stubOf(0xF8360109u, 0);
    EXPECT_TRUE(a64x_stub_spills(gpr_stp.data(), static_cast<int>(gpr_stp.size())));
    const std::vector<uint32_t> gpr_str = stubOf(0x48207D56u, kX16X17);
    EXPECT_TRUE(a64x_stub_spills(gpr_str.data(), static_cast<int>(gpr_str.size())));
    const std::vector<uint32_t> q_stp = stubOf(0x6E80C400u, kX16X17);
    EXPECT_TRUE(a64x_stub_spills(q_stp.data(), static_cast<int>(q_stp.size())));
    const std::vector<uint32_t> sb = stubOf(0xD50330FFu, 0, true);
    ASSERT_EQ(sb.size(), 3u);
    EXPECT_EQ(sb[0], 0xD503379Fu);
    EXPECT_EQ(sb[1], 0xD5033FDFu);
    EXPECT_EQ(sb[2], 0xD65F03C0u);
    const int rdm_n = a64x_stub_v(0x6E408400u, kSite, kLaid, 0, 0, false, nullptr, 0);
    EXPECT_GT(rdm_n, 0);
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

TEST(FpStubs, EvalJscvtMatchesHardwareInstruction)
{
    const auto check = [](uint64_t bits) {
        uint32_t hw_w = 0;
        uint64_t hw_nzcv = 0;
        asm("fmov d0, %[bits]\n"
            "fjcvtzs %w[w], d0\n"
            "mrs %[nzcv], nzcv\n"
            : [w] "=r"(hw_w), [nzcv] "=r"(hw_nzcv)
            : [bits] "r"(bits)
            : "d0", "cc");
        uint32_t sw_w = 0;
        const bool sw_z = a64x_eval_jscvt(bits, &sw_w);
        const bool hw_z = ((hw_nzcv >> 30) & 1u) != 0;
        EXPECT_EQ(sw_w, hw_w) << "bits=0x" << std::hex << bits;
        EXPECT_EQ(sw_z, hw_z) << "bits=0x" << std::hex << bits;
    };
    const uint64_t edges[] = {
        0x0000000000000000ull,  // +0.0
        0x8000000000000000ull,  // -0.0
        0x0000000000000001ull,  // +min subnormal
        0x8000000000000001ull,  // -min subnormal
        0x3FEFFFFFFFFFFFFFull,  // 1 - eps
        0xBFEFFFFFFFFFFFFFull,  // -(1 - eps)
        0x3FF0000000000000ull,  // +1.0
        0xBFF0000000000000ull,  // -1.0
        0x3FF8000000000000ull,  // +1.5
        0xBFF8000000000000ull,  // -1.5
        0x41DFFFFFFFC00000ull,  // +(2^31 - 1)
        0xC1DFFFFFFFC00000ull,  // -(2^31 - 1)
        0x41E0000000000000ull,  // +2^31
        0xC1E0000000000000ull,  // -2^31 (INT32_MIN, Z=1)
        0xC1E0000000100000ull,  // -2^31 - 0.5 (Z=0)
        0x41E0000000200000ull,  // +2^31 + 1
        0xC1E0000000200000ull,  // -2^31 - 1
        0x41F0000000000000ull,  // +2^32
        0xC1F0000000000000ull,  // -2^32
        0x4330000000000000ull,  // +2^52
        0xC330000000000001ull,  // -(2^52 + 1)
        0x43E0000000000000ull,  // +2^63
        0xC3E0000000000000ull,  // -2^63
        0x452FFFFFFFFFFFFFull,  // near +2^84
        0xC52FFFFFFFFFFFFFull,  // near -2^84
        0x4530000000000000ull,  // +2^84
        0xC530000000000000ull,  // -2^84
        0x7FF0000000000000ull,  // +Inf
        0xFFF0000000000000ull,  // -Inf
        0x7FF8000000000000ull,  // qNaN
        0xFFF8000000000001ull,  // -qNaN
        0x7FF0000000000001ull,  // sNaN
    };
    for (uint64_t b : edges)
        check(b);
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 10000; i++) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        check(s);
        uint64_t near_exp =
            (static_cast<uint64_t>(1020 + (i % 90)) << 52) | (s & ((1ull << 52) - 1)) | ((s & (1ull << 63)));
        check(near_exp);
    }
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
