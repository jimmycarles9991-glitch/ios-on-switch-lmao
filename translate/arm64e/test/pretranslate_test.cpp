// Offline translation of whole slices on small signed Mach-Os built here: stub placement in the segment's tail and in
// dead PAC checks, the marker, re-signing, the fallbacks, and the verifier catching what it must.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../macho/codesign.h"
#include "../macho/pretranslate.h"
#include "../macho/stub_split.h"
}

namespace {

constexpr uint32_t kRet = 0xD65F03C0;
constexpr uint32_t kRetab = 0xD65F0FFF;
constexpr uint32_t kPacibsp = 0xD503237F;
constexpr uint32_t kAutibsp = 0xD50323FF;
constexpr uint32_t kStpFp = 0xA9BF7BFD;    // stp x29, x30, [sp, #-16]!
constexpr uint32_t kLdpFp = 0xA8C17BFD;    // ldp x29, x30, [sp], #16
constexpr uint32_t kLdaddal = 0xF8E20003;  // ldaddal x2, x3, [x0]
constexpr uint32_t kCasal = 0xC8E1FC02;    // casal x1, x2, [x0]
constexpr uint32_t kAdd = 0x8B020020;      // add x0, x1, x2
constexpr uint32_t kSeg = 0x4000;
constexpr uint32_t kTextOff = 0x1000;
constexpr uint64_t kBase = 0x100000000;

struct Spec {
    uint32_t cpusubtype = 0x80000002u;  // arm64e, PAC ABI
    std::vector<uint32_t> code;         // __text at kTextOff
    std::vector<uint32_t> starts;       // function starts, word indices
    uint32_t text_words = 0;            // __text size in words (0: code.size())
    bool sign = true;
    uint32_t reserved = 0;
    int fips = 0;  // 1: __TEXT,__fips_hmacs holds the MAC of __text (macho/fips.h); 2: a stale one
};

constexpr uint32_t kFipsOff = 0x800;

void put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
    memcpy(&b[at], &v, 4);
}

void put64(std::vector<uint8_t> &b, size_t at, uint64_t v)
{
    memcpy(&b[at], &v, 8);
}

void putbe32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b[at + i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}

uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void name16(std::vector<uint8_t> &b, size_t at, const char *n)
{
    memcpy(&b[at], n, strlen(n));
}

// __TEXT (0x4000: __text, and __fips_hmacs before it with s.fips), __LINKEDIT (function starts, then an ad hoc
// SHA-256 signature).
std::vector<uint8_t> build(const Spec &s)
{
    const uint32_t text_cmd = s.fips ? 232 : 152;
    const uint32_t ncmds = s.sign ? 4 : 3, sizeofcmds = text_cmd + 72 + 16 + (s.sign ? 16 : 0);
    const uint32_t fs_off = kSeg, sig_off = kSeg + 0x100, ident_off = 88, hash_off = ident_off + 2;
    const uint32_t pages = (sig_off + 4095) / 4096, cd_len = hash_off + 32 * pages, sig_len = 20 + cd_len;
    std::vector<uint8_t> b(sig_off + (s.sign ? sig_len : 0), 0);
    put32(b, 0, 0xFEEDFACF);
    put32(b, 4, 0x0100000C);
    put32(b, 8, s.cpusubtype);
    put32(b, 12, 6);  // MH_DYLIB
    put32(b, 16, ncmds);
    put32(b, 20, sizeofcmds);
    put32(b, 28, s.reserved);
    size_t at = 32;
    put32(b, at, 0x19);
    put32(b, at + 4, text_cmd);
    name16(b, at + 8, "__TEXT");
    put64(b, at + 24, kBase);
    put64(b, at + 32, kSeg);
    put64(b, at + 48, kSeg);
    put32(b, at + 56, 5);
    put32(b, at + 60, 5);
    put32(b, at + 64, s.fips ? 2 : 1);
    if (s.fips) {
        name16(b, at + 72, "__fips_hmacs");
        name16(b, at + 88, "__TEXT");
        put64(b, at + 104, kBase + kFipsOff);
        put64(b, at + 112, A64X_FIPS_MAC_SIZE);
        put32(b, at + 120, kFipsOff);
        at += 80;
    }
    name16(b, at + 72, "__text");
    name16(b, at + 88, "__TEXT");
    const uint32_t words = s.text_words ? s.text_words : static_cast<uint32_t>(s.code.size());
    put64(b, at + 104, kBase + kTextOff);
    put64(b, at + 112, 4ull * words);
    put32(b, at + 120, kTextOff);
    put32(b, at + 124, 2);
    put32(b, at + 136, 0x80000400);
    at += 152;
    put32(b, at, 0x19);
    put32(b, at + 4, 72);
    name16(b, at + 8, "__LINKEDIT");
    put64(b, at + 24, kBase + kSeg);
    put64(b, at + 32, kSeg);
    put64(b, at + 40, kSeg);
    put64(b, at + 48, b.size() - kSeg);
    put32(b, at + 56, 1);
    put32(b, at + 60, 1);
    at += 72;
    std::vector<uint8_t> fs;
    uint64_t last = kBase;
    for (uint32_t k : s.starts) {
        uint64_t addr = kBase + kTextOff + 4ull * k, d = addr - last;
        last = addr;
        do {
            uint8_t byte = d & 0x7F;
            d >>= 7;
            fs.push_back(static_cast<uint8_t>(byte | (d ? 0x80 : 0)));
        } while (d);
    }
    memcpy(&b[fs_off], fs.data(), fs.size());
    put32(b, at, 0x26);
    put32(b, at + 4, 16);
    put32(b, at + 8, fs_off);
    put32(b, at + 12, static_cast<uint32_t>(fs.size() + 1));
    at += 16;
    for (size_t i = 0; i < s.code.size(); i++)
        put32(b, kTextOff + 4 * i, s.code[i]);
    if (s.fips)
        a64x_fips_mac(&b[kTextOff], 4ull * words, &b[kFipsOff]);
    if (s.fips == 2)
        b[kFipsOff] ^= 1;
    if (!s.sign)
        return b;
    put32(b, at, 0x1D);
    put32(b, at + 4, 16);
    put32(b, at + 8, sig_off);
    put32(b, at + 12, sig_len);
    putbe32(b, sig_off, 0xFADE0CC0);
    putbe32(b, sig_off + 4, sig_len);
    putbe32(b, sig_off + 8, 1);
    putbe32(b, sig_off + 16, 20);
    const size_t cd = sig_off + 20;
    putbe32(b, cd, 0xFADE0C02);
    putbe32(b, cd + 4, cd_len);
    putbe32(b, cd + 8, 0x20400);
    putbe32(b, cd + 12, 2);
    putbe32(b, cd + 16, hash_off);
    putbe32(b, cd + 20, ident_off);
    putbe32(b, cd + 28, pages);
    putbe32(b, cd + 32, sig_off);
    b[cd + 36] = 32;
    b[cd + 37] = 2;
    b[cd + 39] = 12;
    b[cd + ident_off] = 't';
    uint8_t h[A64X_CDHASH_SIZE];
    EXPECT_EQ(a64x_cs_rehash(b.data(), b.size(), sig_off, sig_len, h, nullptr, 0), 0);
    return b;
}

struct Result {
    std::vector<uint8_t> in, out;
    a64x_macho m{};
    a64x_pt_result r{};
    long verified = -2;
    char err[256] = {};
};

void run(const Spec &s, Result &x)
{
    x.in = build(s);
    x.out.assign(x.in.size(), 0);
    ASSERT_EQ(a64x_macho_parse(x.in.data(), x.in.size(), &x.m, x.err, sizeof(x.err)), 0) << x.err;
    ASSERT_EQ(a64x_pretranslate(&x.m, x.out.data(), &x.r, x.err, sizeof(x.err)), 0) << x.err;
    if (x.r.status == A64X_PT_TRANSLATED) {
        a64x_pt_check v;
        x.verified = a64x_pt_verify(&x.m, x.out.data(), &v, x.err, sizeof(x.err));
    }
}

uint32_t text(const Result &x, size_t k)
{
    return get32(x.out.data() + kTextOff + 4 * k);
}

uint64_t target(uint32_t w, uint64_t pc)
{
    return pc + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(w << 6) >> 6) * 4);
}

// f0: x30 dead at its LDADDAL (reloaded by the ldp); f1: x30 live at its CASAL (its RET reads it); f2: a dead LR check.
Spec typical()
{
    Spec s;
    s.code = {kPacibsp,   kStpFp,     kLdaddal,   kLdpFp,     kRetab,  // f0: 0..4
              kCasal,     kRet,                                        // f1: 5..6
              kAutibsp,   0xCA1E07D0, 0xB6F00050, 0xD4388E20, kRet,    // f2: 7..11
              0x1FFFFFFF, 0,          0x3FFFFFFF, 0};                  // a mask table: 12..15
    s.starts = {0, 5, 7};
    return s;
}

TEST(Pretranslate, PlacesStubsInTheTailAndMarksAndSigns)
{
    Result x;
    run(typical(), x);
    ASSERT_EQ(x.r.status, A64X_PT_TRANSLATED) << x.r.why;
    EXPECT_EQ(x.verified, 2) << x.err;
    EXPECT_EQ(get32(x.out.data() + 28), A64X_PRETRANSLATED);
    EXPECT_EQ(text(x, 0), kPacibsp);
    EXPECT_EQ(text(x, 4), kRet);
    uint64_t tail_lo, tail_hi;
    a64x_pt_tail(&x.m, &tail_lo, &tail_hi);
    const uint64_t site0 = kTextOff + 8, site1 = kTextOff + 20;
    EXPECT_EQ(text(x, 2) & 0xFC000000u, 0x94000000u) << "x30 dead: BL to a shared stub";
    EXPECT_EQ(text(x, 5) & 0xFC000000u, 0x14000000u) << "x30 live: B to its own stub";
    for (uint64_t site : {site0, site1}) {
        uint64_t t = target(get32(x.out.data() + site), site);
        EXPECT_GE(t, tail_lo);
        EXPECT_LT(t, tail_hi);
    }
    EXPECT_EQ(text(x, 7), 0x14000004u) << "the LR check passes by construction";
    EXPECT_EQ(text(x, 12), 0x1FFFFFFFu) << "a table between zero words is data";
    EXPECT_EQ(x.r.shared_sites, 1u);
    EXPECT_EQ(x.r.dead_checks, 1u);
    uint8_t h[A64X_CDHASH_SIZE];
    uint64_t so, ss;
    ASSERT_EQ(a64x_cs_find(x.out.data(), x.out.size(), &so, &ss), 0);
    EXPECT_EQ(a64x_cs_check(x.out.data(), x.out.size(), so, ss, h, x.err, sizeof(x.err)), 0) << x.err;
    EXPECT_EQ(memcmp(h, x.r.cdhash_out, sizeof(h)), 0);
    EXPECT_NE(memcmp(x.r.cdhash_in, x.r.cdhash_out, sizeof(h)), 0);
}

// An address of the end of __text (here an ADR; in libsystem_malloc_debug a rebased section$end pointer) is the first
// word of the stub tail: it is not code, so it is no entry and the stubs there stand.
TEST(Pretranslate, AnAddressOfTheTextEndIsNoEntry)
{
    Spec s = typical();
    const uint32_t adr = static_cast<uint32_t>(s.code.size());
    s.code.push_back(0x10000041);  // adr x1, . + 8: the word after __text
    s.code.push_back(kRet);
    s.starts.push_back(adr);
    Result x;
    run(s, x);
    ASSERT_EQ(x.r.status, A64X_PT_TRANSLATED) << x.r.why;
    EXPECT_EQ(x.verified, 2) << x.err;
    uint64_t tail_lo, tail_hi;
    a64x_pt_tail(&x.m, &tail_lo, &tail_hi);
    ASSERT_EQ(tail_lo, kTextOff + 4ull * (adr + 2));
    EXPECT_NE(get32(x.out.data() + tail_lo), 0u) << "a stub word starts the tail";
}

TEST(Pretranslate, TheVerifierCatchesAStubThatDoesNotReturn)
{
    Result x;
    run(typical(), x);
    ASSERT_EQ(x.r.status, A64X_PT_TRANSLATED);
    const uint64_t site1 = kTextOff + 20, stub = target(get32(x.out.data() + site1), site1);
    uint64_t end = stub;
    while ((get32(x.out.data() + end) & 0xFC000000u) != 0x14000000u)
        end += 4;
    put32(x.out, end, 0x14000000u | 1);  // b . + 4: runs into the next stub words instead of returning
    a64x_pt_check v;
    EXPECT_LT(a64x_pt_verify(&x.m, x.out.data(), &v, x.err, sizeof(x.err)), 0);
}

TEST(Pretranslate, TheSignatureNoLongerChecksAfterAByteChanges)
{
    Result x;
    run(typical(), x);
    ASSERT_EQ(x.r.status, A64X_PT_TRANSLATED);
    x.out[kTextOff + 3] ^= 1;
    uint8_t h[A64X_CDHASH_SIZE];
    uint64_t so, ss;
    ASSERT_EQ(a64x_cs_find(x.out.data(), x.out.size(), &so, &ss), 0);
    EXPECT_NE(a64x_cs_check(x.out.data(), x.out.size(), so, ss, h, x.err, sizeof(x.err)), 0);
}

TEST(Pretranslate, NoRoomLeavesTheSliceForTheLoader)
{
    Spec s;
    s.code.assign((kSeg - kTextOff) / 4, kAdd);
    s.code[0] = kCasal;
    s.code[1] = kRet;
    s.code.back() = kRet;
    s.starts = {0, 2};
    Result x;
    run(s, x);
    EXPECT_EQ(x.r.status, A64X_PT_FALLBACK);
    EXPECT_NE(strstr(x.r.why, "no room"), nullptr) << x.r.why;
    EXPECT_EQ(x.out, x.in);
}

// No tail, only 3-word holes: a CASAL with x30 live needs its own stub, longer than any hole, so it is split.
TEST(Pretranslate, AStubLongerThanEveryHoleIsSplitOverSeveral)
{
    Spec s;
    s.code.assign((kSeg - kTextOff) / 4, kAdd);
    s.code[0] = kCasal;
    s.code[1] = kRet;
    s.starts = {0, 2};
    const uint32_t lr_check[] = {kAutibsp, 0xCA1E07D0, 0xB6F00050, 0xD4388E20, kRet};
    for (uint32_t f = 2; f + 5 <= s.code.size(); f += 5)
        memcpy(&s.code[f], lr_check, sizeof(lr_check));
    s.code.back() = kRet;
    Result x;
    run(s, x);
    ASSERT_EQ(x.r.status, A64X_PT_TRANSLATED) << x.r.why;
    EXPECT_EQ(x.verified, 1) << x.err;
    EXPECT_EQ(x.r.tail_words, 0u);
    const uint64_t site = kTextOff;
    ASSERT_EQ(text(x, 0) & 0xFC000000u, 0x14000000u) << "x30 live: B to its own stub";
    uint64_t pc = target(text(x, 0), site), holes = 1;
    for (int words = 0; words < 64; words++, pc += 4) {
        const uint32_t w = get32(x.out.data() + pc);
        if ((w & 0xFC000000u) != 0x14000000u)
            continue;
        if (target(w, pc) == site + 4)
            break;
        pc = target(w, pc) - 4;
        holes++;
    }
    EXPECT_GT(holes, 1u) << "the stub spans several holes";
}

// A tail-call LR check shared with an unsigned early return (libsystem_trace's _logDestructor in iOS 27): the CBZ enters
// the check at its EOR, so those words stay live and no stub piece may go there; the other checks still take theirs.
// Holes are taken from the end of the segment first, so the entered check is there.
constexpr uint32_t kWords = (kSeg - kTextOff) / 4, kEntered = kWords - 7;

Spec shared_check()
{
    Spec s;
    s.code.assign(kWords, kAdd);
    s.code[0] = kCasal;
    s.code[1] = kRet;
    const uint32_t lr_check[] = {kAutibsp, 0xCA1E07D0, 0xB6F00050, 0xD4388E20, kRet};
    for (uint32_t f = 2; f + 5 <= kEntered; f += 5)
        memcpy(&s.code[f], lr_check, sizeof(lr_check));
    const uint32_t entered[] = {0xB4000040, kAutibsp, 0xCA1E07D0, 0xB6F00050, 0xD4388E20, kRet, kRet};  // cbz x0, . + 8
    memcpy(&s.code[kEntered], entered, sizeof(entered));
    s.starts = {0, 2, kEntered};
    return s;
}

TEST(Pretranslate, NoStubWordGoesWhereABranchEntersACheck)
{
    Result x;
    run(shared_check(), x);
    ASSERT_EQ(x.r.status, A64X_PT_TRANSLATED) << x.r.why;
    EXPECT_EQ(x.verified, 1) << x.err;
    EXPECT_EQ(text(x, kEntered), 0xB4000040u);
    EXPECT_EQ(text(x, kEntered + 1), 0x14000004u) << "the check still passes by construction";
    EXPECT_EQ(text(x, kEntered + 2), 0xCA1E07D0u);
    EXPECT_EQ(text(x, kEntered + 3), 0xB6F00050u);
    EXPECT_EQ(text(x, kEntered + 4), 0xD4388E20u);
    int used = 0;
    for (uint32_t f = 2; f + 5 <= kEntered; f += 5)
        used += text(x, f + 1) != 0xCA1E07D0u;
    EXPECT_GT(used, 0) << "the other checks' words hold the stub";
}

TEST(Pretranslate, TheVerifierCatchesAStubWordABranchEnters)
{
    Result x;
    run(shared_check(), x);
    ASSERT_EQ(x.r.status, A64X_PT_TRANSLATED) << x.r.why;
    put32(x.out, kTextOff + 4 * (kEntered + 2), kAdd);
    a64x_pt_check v;
    EXPECT_LT(a64x_pt_verify(&x.m, x.out.data(), &v, x.err, sizeof(x.err)), 0);
    EXPECT_EQ(v.entered_stub_words, 1u);
    EXPECT_NE(strstr(x.err, "where something branches"), nullptr) << x.err;
}

// Every branch of a split stub reaches the new place of the word it reached before; one out of the stub keeps its target.
TEST(StubSplit, BranchesFollowTheWordsTheyTarget)
{
    const uint64_t laid = 0x10000, site = 0x8000;
    uint32_t st[A64X_STUB_MAX];
    const int n = a64x_stub_v(kCasal, site, laid, (1u << 16) | (1u << 17), 0, st, A64X_STUB_MAX);
    ASSERT_GT(n, 5);
    std::vector<uint64_t> addr(n), links;
    for (int j = 0; j < n; j++) {
        addr[j] = 0x40000 + 0x100 * static_cast<uint64_t>(j / 2) + 4 * static_cast<uint64_t>(j % 2);
        if (j % 2 == 1 && j + 1 < n)
            links.push_back(addr[j] + 4);
    }
    std::vector<a64x_placed> out(n + links.size());
    const int np = a64x_stub_split(st, n, laid, addr.data(), links.data(), static_cast<int>(links.size()), out.data());
    ASSERT_EQ(np, n + static_cast<int>(links.size()));
    int checked = 0;
    for (const a64x_placed &w : out) {
        bool is;
        const int64_t off = a64x_branch_offset(w.word, &is);
        const auto j = std::find(addr.begin(), addr.end(), w.at);
        if (j == addr.end()) {
            EXPECT_TRUE(is && w.at + off == *(std::find(addr.begin(), addr.end(), w.at - 4) + 1)) << "a link";
            continue;
        }
        bool was;
        const int64_t old = a64x_branch_offset(st[j - addr.begin()], &was);
        ASSERT_EQ(is, was);
        if (!is)
            continue;
        const uint64_t before = laid + 4 * static_cast<uint64_t>(j - addr.begin()) + old;
        const uint64_t after = before >= laid && before < laid + 4ull * n ? addr[(before - laid) / 4] : before;
        EXPECT_EQ(w.at + off, after);
        checked++;
    }
    EXPECT_GE(checked, 3) << "the loop, the failure branch and the return";
}

TEST(Pretranslate, UnsignedSlicesAreLeftAlone)
{
    Spec s = typical();
    s.sign = false;
    Result x;
    run(s, x);
    EXPECT_EQ(x.r.status, A64X_PT_FALLBACK);
    EXPECT_EQ(x.out, x.in);
}

TEST(Pretranslate, PlainArm64KeepsPacWordsAndLosesLse)
{
    Spec s = typical();
    s.cpusubtype = 0;
    Result x;
    run(s, x);
    ASSERT_EQ(x.r.status, A64X_PT_TRANSLATED) << x.r.why;
    EXPECT_EQ(x.verified, 2) << x.err;
    EXPECT_EQ(text(x, 4), kRetab);
    EXPECT_EQ(text(x, 7), kAutibsp);
    EXPECT_EQ(x.r.dead_checks, 0u);
}

TEST(Pretranslate, MarkedSlicesStayAsTheyAre)
{
    Spec s = typical();
    s.reserved = A64X_PRETRANSLATED;
    Result x;
    run(s, x);
    EXPECT_EQ(x.r.status, A64X_PT_UNCHANGED);
    EXPECT_EQ(x.out, x.in);
}

TEST(Pretranslate, AFipsMacThatHeldIsThatOfTheTranslatedText)
{
    Spec s = typical();
    s.fips = 1;
    Result x;
    run(s, x);
    ASSERT_EQ(x.r.status, A64X_PT_TRANSLATED) << x.r.why;
    EXPECT_EQ(x.r.fips_mac, 1);
    uint8_t mac[A64X_FIPS_MAC_SIZE];
    a64x_fips_mac(x.out.data() + kTextOff, 4 * s.code.size(), mac);
    EXPECT_EQ(memcmp(mac, x.out.data() + kFipsOff, sizeof(mac)), 0);
    EXPECT_GE(x.verified, 0) << x.err;
}

TEST(Pretranslate, AStaleFipsMacStays)
{
    Spec s = typical();
    s.fips = 2;
    Result x;
    run(s, x);
    ASSERT_EQ(x.r.status, A64X_PT_TRANSLATED) << x.r.why;
    EXPECT_EQ(x.r.fips_mac, 0);
    EXPECT_EQ(memcmp(x.out.data() + kFipsOff, x.in.data() + kFipsOff, A64X_FIPS_MAC_SIZE), 0);
    EXPECT_GE(x.verified, 0) << x.err;
}

TEST(Pretranslate, TheVerifierCatchesAFipsMacLeftAsItWas)
{
    Spec s = typical();
    s.fips = 1;
    Result x;
    run(s, x);
    ASSERT_EQ(x.r.status, A64X_PT_TRANSLATED) << x.r.why;
    memcpy(x.out.data() + kFipsOff, x.in.data() + kFipsOff, A64X_FIPS_MAC_SIZE);
    a64x_pt_check v;
    EXPECT_LT(a64x_pt_verify(&x.m, x.out.data(), &v, x.err, sizeof(x.err)), 0);
    EXPECT_NE(std::string(x.err).find("FIPS"), std::string::npos) << x.err;
}

}  // namespace
