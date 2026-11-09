// The loader's translation of whole images and the chained-fixup rewrite, on small Mach-Os built here: no compiler
// output and nothing of Apple's.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "../macho/macho.h"
}

namespace {

constexpr uint32_t kNop = 0xD503201F;
constexpr uint32_t kRet = 0xD65F03C0;
constexpr uint32_t kRetab = 0xD65F0FFF;
constexpr uint32_t kPacibsp = 0xD503237F;
constexpr uint32_t kLdaddal = 0xF8E20003;  // ldaddal x2, x3, [x0]
constexpr uint32_t kBraa = 0xD71F0A11;     // braa x16, x17
constexpr uint32_t kAdd = 0x8B020020;      // add x0, x1, x2
constexpr uint64_t kBase = 0x100000000;
constexpr uint32_t kTextOff = 0x1000;     // __text: file offset, and address - kBase
constexpr uint32_t kSeg = 0x4000;         // every segment is 16 KB
constexpr uint32_t kLinkedit = 2 * kSeg;  // __TEXT, __DATA, __LINKEDIT
constexpr uint32_t kFunctionStarts = kLinkedit;
constexpr uint32_t kDataInCode = kLinkedit + 0x100;
constexpr uint32_t kFixups = kLinkedit + 0x200;

struct Spec {
    uint32_t cpusubtype = A64X_CPU_SUBTYPE_ARM64E;
    std::vector<uint32_t> code;
    std::vector<uint32_t> function_starts;                    // word indices into code
    std::vector<std::pair<uint32_t, uint16_t>> data_in_code;  // word index, length in bytes
    bool marked = false;
    bool data_executable = false;                         // a second executable segment
    uint16_t fixup_format = 0;                            // 0: no LC_DYLD_CHAINED_FIXUPS
    std::vector<std::pair<uint32_t, uint64_t>> pointers;  // offset in __DATA, raw chain entry
};

void put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
    memcpy(&b[at], &v, 4);
}

void put64(std::vector<uint8_t> &b, size_t at, uint64_t v)
{
    memcpy(&b[at], &v, 8);
}

uint32_t get32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void putName(std::vector<uint8_t> &b, size_t at, const char *name)
{
    strncpy(reinterpret_cast<char *>(&b[at]), name, 16);
}

std::vector<uint8_t> build(const Spec &s)
{
    std::vector<uint8_t> b(3 * kSeg, 0);
    size_t lc = 32;
    uint32_t ncmds = 0;
    auto segment = [&](const char *name, uint32_t index, uint32_t prot, std::initializer_list<int> sections) {
        size_t at = lc;
        uint32_t nsects = static_cast<uint32_t>(sections.size());
        put32(b, at, A64X_LC_SEGMENT_64);
        put32(b, at + 4, 72 + 80 * nsects);
        putName(b, at + 8, name);
        put64(b, at + 24, kBase + index * kSeg);
        put64(b, at + 32, kSeg);
        put64(b, at + 40, index * kSeg);
        put64(b, at + 48, kSeg);
        put32(b, at + 56, prot);
        put32(b, at + 60, prot);
        put32(b, at + 64, nsects);
        size_t sc = at + 72;
        for (int kind : sections) {
            if (kind == 0) {
                putName(b, sc, "__text");
                putName(b, sc + 16, "__TEXT");
                put64(b, sc + 32, kBase + kTextOff);
                put64(b, sc + 40, 4 * s.code.size());
                put32(b, sc + 48, kTextOff);
                put32(b, sc + 64, A64X_S_ATTR_PURE_INSTRUCTIONS | A64X_S_ATTR_SOME_INSTRUCTIONS);
            } else {
                putName(b, sc, A64X_TARGET_SECTNAME);
                putName(b, sc + 16, A64X_TARGET_SEGNAME);
                put64(b, sc + 32, kBase + 0x3000);
                put64(b, sc + 40, 16);
                put32(b, sc + 48, 0x3000);
            }
            sc += 80;
        }
        lc = sc;
        ncmds++;
    };
    if (s.marked)
        segment("__TEXT", 0, 5, {0, 1});
    else
        segment("__TEXT", 0, 5, {0});
    segment("__DATA", 1, s.data_executable ? 5 : 3, {});
    segment("__LINKEDIT", 2, 1, {});
    auto linkedit = [&](uint32_t cmd, uint32_t off, uint32_t size) {
        put32(b, lc, cmd);
        put32(b, lc + 4, 16);
        put32(b, lc + 8, off);
        put32(b, lc + 12, size);
        lc += 16;
        ncmds++;
    };
    if (!s.function_starts.empty()) {
        size_t q = kFunctionStarts;
        uint64_t last = kBase;
        for (uint32_t w : s.function_starts) {
            uint64_t addr = kBase + kTextOff + 4ull * w, delta = addr - last;
            last = addr;
            do {
                uint8_t byte = delta & 0x7F;
                delta >>= 7;
                b[q++] = static_cast<uint8_t>(byte | (delta ? 0x80 : 0));
            } while (delta);
        }
        b[q++] = 0;
        linkedit(A64X_LC_FUNCTION_STARTS, kFunctionStarts, static_cast<uint32_t>(q - kFunctionStarts));
    }
    if (!s.data_in_code.empty()) {
        size_t q = kDataInCode;
        for (auto [w, len] : s.data_in_code) {
            put32(b, q, kTextOff + 4 * w);
            memcpy(&b[q + 4], &len, 2);
            q += 8;
        }
        linkedit(A64X_LC_DATA_IN_CODE, kDataInCode, static_cast<uint32_t>(q - kDataInCode));
    }
    if (s.fixup_format) {
        // header, starts_in_image (3 segments: only __DATA has a chain), starts_in_segment with one page
        put32(b, kFixups + 4, 32);
        put32(b, kFixups + 32, 3);
        put32(b, kFixups + 32 + 8, 16);
        size_t ss = kFixups + 32 + 16;
        put32(b, ss, 24);
        uint16_t page = kSeg, zero = 0, one = 1;
        memcpy(&b[ss + 4], &page, 2);
        memcpy(&b[ss + 6], &s.fixup_format, 2);
        put64(b, ss + 8, kSeg);
        memcpy(&b[ss + 20], &one, 2);
        memcpy(&b[ss + 22], &zero, 2);
        linkedit(A64X_LC_DYLD_CHAINED_FIXUPS, kFixups, 32 + 16 + 24);
        for (auto [off, raw] : s.pointers)
            put64(b, kSeg + off, raw);
    }
    put32(b, 0, A64X_MH_MAGIC_64);
    put32(b, 4, A64X_CPU_TYPE_ARM64);
    put32(b, 8, s.cpusubtype);
    put32(b, 12, 2);  // MH_EXECUTE
    put32(b, 16, ncmds);
    put32(b, 20, static_cast<uint32_t>(lc - 32));
    for (size_t i = 0; i < s.code.size(); i++)
        put32(b, kTextOff + 4 * i, s.code[i]);
    return b;
}

struct Translated {
    std::unique_ptr<a64x_macho> m = std::make_unique<a64x_macho>();
    std::vector<uint8_t> in, out;
    a64x_image_xlate x{};
    int rc = -1;
    long sites = -1;
    std::string err;

    ~Translated()
    {
        a64x_image_xlate_free(&x);
    }

    uint32_t word(size_t i) const
    {
        return get32(&out[kTextOff + 4 * i]);
    }
};

void translate(const Spec &s, Translated &t, uint32_t flags = 0)
{
    t.in = build(s);
    t.out.assign(t.in.size(), 0);
    char err[256] = "";
    ASSERT_EQ(0, a64x_macho_parse(t.in.data(), t.in.size(), t.m.get(), err, sizeof(err))) << err;
    t.rc = a64x_translate_image(t.m.get(), 16384, flags, t.out.data(), &t.x, err, sizeof(err));
    t.err = err;
    if (t.rc == 0) {
        t.sites = a64x_verify_image(t.m.get(), t.out.data(), &t.x, err, sizeof(err));
        t.err = err;
    }
}

Spec plainFunction(uint32_t subtype)
{
    Spec s;
    s.cpusubtype = subtype;
    s.code = {kPacibsp, kLdaddal, kBraa, kAdd, kRetab, kNop, kNop, kNop};
    s.function_starts = {0};
    return s;
}

TEST(Image, Arm64eIsTranslatedInFull)
{
    Translated t;
    translate(plainFunction(A64X_CPU_SUBTYPE_ARM64E), t);
    ASSERT_EQ(0, t.rc) << t.err;
    EXPECT_EQ(A64X_KIND_ARM64E, t.x.kind);
    EXPECT_EQ(1, t.sites) << t.err;
    EXPECT_EQ(kPacibsp, t.word(0)) << "hint-space PAC is a NOP on ARMv8.0 already";
    EXPECT_EQ(0x14000000u, t.word(1) & 0xFC000000u) << "LSE becomes a branch to its stub";
    EXPECT_EQ(0xD61F0200u, t.word(2)) << "braa x16, x17 -> br x16";
    EXPECT_EQ(kAdd, t.word(3));
    EXPECT_EQ(kRet, t.word(4));
    EXPECT_EQ(3u, t.x.changed);
    EXPECT_GT(t.x.nstubs, 0u);
    EXPECT_EQ(int64_t{3 * kSeg}, t.x.stub_delta) << "stubs right after the image's address space";
    for (size_t i = 0; i < 8; i++)
        EXPECT_FALSE(a64x_class_is_baseline(a64x_classify(t.word(i)))) << i;
}

TEST(Image, Arm64KeepsItsPacWordsButNotLse)
{
    Translated t;
    translate(plainFunction(A64X_CPU_SUBTYPE_ARM64_ALL), t);
    ASSERT_EQ(0, t.rc) << t.err;
    EXPECT_EQ(A64X_KIND_ARM64, t.x.kind);
    EXPECT_EQ(A64X_MAP_NO_PAC, t.x.map_flags);
    EXPECT_EQ(1, t.sites);
    EXPECT_EQ(kBraa, t.word(2)) << "a PAC word in a plain arm64 image is data or a fault, not translated";
    EXPECT_EQ(kRetab, t.word(4));
    EXPECT_EQ(1u, t.x.changed);
}

TEST(Image, CleanArm64IsMappedAsIs)
{
    Spec s;
    s.cpusubtype = A64X_CPU_SUBTYPE_ARM64_ALL;
    s.code = {kAdd, kRet};
    Translated t;
    translate(s, t);
    ASSERT_EQ(0, t.rc) << t.err;
    EXPECT_EQ(A64X_KIND_NATIVE, t.x.kind);
    EXPECT_EQ(t.in, t.out);
}

TEST(Image, MarkedImageIsNeverScanned)
{
    Spec s = plainFunction(A64X_CPU_SUBTYPE_ARM64E);
    s.marked = true;
    Translated t;
    translate(s, t);
    ASSERT_EQ(0, t.rc) << t.err;
    EXPECT_EQ(A64X_KIND_MARKED, t.x.kind);
    EXPECT_EQ(t.in, t.out);
}

TEST(Image, DataInCodeIsLeftAlone)
{
    Spec s = plainFunction(A64X_CPU_SUBTYPE_ARM64E);
    s.code[6] = kLdaddal;
    s.code[7] = kRetab;
    s.data_in_code = {{6, 8}};
    Translated t;
    translate(s, t);
    ASSERT_EQ(0, t.rc) << t.err;
    EXPECT_EQ(kLdaddal, t.word(6));
    EXPECT_EQ(kRetab, t.word(7));
    EXPECT_EQ(1, t.sites);
}

TEST(Image, DataIslandAfterATerminatorIsLeftAlone)
{
    // A constant table after the function's RET: unallocated words (op0 = 0001, 0011) mark the piece as data.
    Spec s;
    s.code = {kPacibsp, kAdd, kRetab, 0x06A09E66, kLdaddal, 0x02B504F3, 0x0333F9DE, kLdaddal};
    s.function_starts = {0};
    Translated t;
    translate(s, t);
    ASSERT_EQ(0, t.rc) << t.err;
    EXPECT_EQ(kRet, t.word(2));
    EXPECT_EQ(kLdaddal, t.word(4));
    EXPECT_EQ(kLdaddal, t.word(7));
    EXPECT_GT(t.x.island_words, 0u);
    EXPECT_EQ(0, t.sites);
}

TEST(Image, TwoExecutableSegments)
{
    Spec s = plainFunction(A64X_CPU_SUBTYPE_ARM64E);
    s.data_executable = true;
    Translated t;
    translate(s, t);
    EXPECT_NE(0, t.rc) << "arm64e text the loader cannot lay out must not run untranslated";
    Translated u;
    s.cpusubtype = A64X_CPU_SUBTYPE_ARM64_ALL;
    translate(s, u);
    ASSERT_EQ(0, u.rc) << u.err;
    EXPECT_EQ(A64X_KIND_NATIVE, u.x.kind) << "plain arm64 runs as is";
}

// The pager regenerates each page and its window of stubs from the page alone (xnu/a64x_xnu.c a64x_text_page);
// that must give exactly what the load-time scan laid out.
struct WindowCtx {
    const a64x_macho *m;
    const uint8_t *data;
};

bool windowIsCode(void *ctx, uint64_t off)
{
    auto *c = static_cast<WindowCtx *>(ctx);
    uint64_t k = (off - c->m->exec_fileoff) / 4;
    if (off >= c->m->exec_fileoff && (c->data[k >> 3] & (1u << (k & 7))))
        return false;
    for (uint32_t i = 0; i < c->m->nranges; i++)
        if (off >= c->m->ranges[i].lo && off < c->m->ranges[i].hi)
            return true;
    return false;
}

TEST(Image, PagerWindowsReproduceTheScan)
{
    Spec s;
    s.function_starts = {0};
    for (int i = 0; i < 700; i++)
        s.code.push_back(i % 7 == 3 ? kLdaddal : i % 11 == 5 ? kRetab : kAdd);
    Translated t;
    translate(s, t);
    ASSERT_EQ(0, t.rc) << t.err;
    ASSERT_GT(t.sites, 10);
    WindowCtx c{t.m.get(), t.x.data};
    a64x_code_map cm{&c, windowIsCode, t.x.map_flags, nullptr};
    std::vector<uint8_t> walk_buf(A64X_WALK_BYTES(1024));
    a64x_walk walk = a64x_walk_in(walk_buf.data(), 1024);
    for (uint32_t pg = 0; pg < t.x.npages; pg++) {
        uint32_t c0 = t.x.cursor[pg], c1 = t.x.cursor[pg + 1];
        std::vector<uint32_t> chk(c1 - c0 + 1), src(1024, 0), dst(1024);
        uint64_t off = t.m->exec_fileoff + 4096ull * pg;
        memcpy(src.data(), &t.in[off], 4096);
        a64x_stub_region sr{t.m->exec_fileoff + static_cast<uint64_t>(t.x.stub_delta), chk.data(), c1, c0, &walk};
        uint32_t end =
            a64x_translate_page(src.data(), dst.data(), 1024, off, &cm, c1 > c0 ? &sr : nullptr, c0, nullptr);
        EXPECT_EQ(c1, end) << "page " << pg;
        EXPECT_EQ(0, memcmp(dst.data(), &t.out[off], 4096)) << "page " << pg;
        EXPECT_EQ(0, memcmp(chk.data(), t.x.stubs + c0, 4ull * (c1 - c0))) << "page " << pg;
    }
}

TEST(Image, VerificationCatchesABrokenStub)
{
    Translated t;
    translate(plainFunction(A64X_CPU_SUBTYPE_ARM64E), t);
    ASSERT_EQ(1, t.sites) << t.err;
    t.x.stubs[t.x.nstubs - 1] = kRet;
    char err[256];
    EXPECT_EQ(-1, a64x_verify_image(t.m.get(), t.out.data(), &t.x, err, sizeof(err)));
    EXPECT_NE(nullptr, strstr(err, "register branch")) << err;
}

TEST(Image, FatSlices)
{
    std::vector<uint8_t> a = build(plainFunction(A64X_CPU_SUBTYPE_ARM64E));
    std::vector<uint8_t> b = build(plainFunction(A64X_CPU_SUBTYPE_ARM64_ALL));
    std::vector<uint8_t> fat(0x4000 + a.size() + b.size(), 0);
    auto be = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; i++)
            fat[at + i] = static_cast<uint8_t>(v >> (24 - 8 * i));
    };
    be(0, A64X_FAT_MAGIC);
    be(4, 2);
    be(8, A64X_CPU_TYPE_ARM64);
    be(12, A64X_CPU_SUBTYPE_ARM64E);
    be(16, 0x4000);
    be(20, static_cast<uint32_t>(a.size()));
    be(28, A64X_CPU_TYPE_ARM64);
    be(32, 0);
    be(36, static_cast<uint32_t>(0x4000 + a.size()));
    be(40, static_cast<uint32_t>(b.size()));
    memcpy(&fat[0x4000], a.data(), a.size());
    memcpy(&fat[0x4000 + a.size()], b.data(), b.size());
    a64x_slice sl[4];
    ASSERT_EQ(2, a64x_macho_slices(fat.data(), fat.size(), sl, 4));
    EXPECT_EQ(0x4000u, sl[0].offset);
    EXPECT_EQ(A64X_CPU_SUBTYPE_ARM64E, sl[0].cpusubtype);
    EXPECT_EQ(0x4000u + a.size(), sl[1].offset);
    EXPECT_EQ(-1, a64x_macho_slices(fat.data() + 1, 64, sl, 4));
}

TEST(Image, Kinds)
{
    EXPECT_EQ(A64X_KIND_ARM64E, a64x_kind_of(A64X_CPU_TYPE_ARM64, 0x80000002u, 0)) << "ptrauth ABI bits ignored";
    EXPECT_EQ(A64X_KIND_ARM64, a64x_kind_of(A64X_CPU_TYPE_ARM64, A64X_CPU_SUBTYPE_ARM64_V8, 0));
    EXPECT_EQ(A64X_KIND_MARKED, a64x_kind_of(A64X_CPU_TYPE_ARM64, A64X_CPU_SUBTYPE_ARM64E, 1));
    EXPECT_EQ(A64X_KIND_NATIVE, a64x_kind_of(A64X_CPU_TYPE_ARM64, 3, 0));
    EXPECT_EQ(A64X_KIND_NATIVE, a64x_kind_of(0x01000007u, 3, 1)) << "x86_64";
}

constexpr uint64_t auth(uint64_t v)
{
    return v | 1ull << 63;
}

constexpr uint64_t bind(uint64_t v)
{
    return v | 1ull << 62;
}

constexpr uint64_t next(uint64_t n)
{
    return n << 51;
}

TEST(Fixups, Arm64eAuthRebaseBecomesAPlainRebaseOfTheSameTarget)
{
    // target 0x1234 (runtime offset), diversity 0xABCD, address diversity, key DA, next 3
    uint64_t raw = auth(0x1234 | 0xABCDull << 32 | 1ull << 48 | 2ull << 49 | next(3));
    a64x_fixup f;
    ASSERT_EQ(0, a64x_fixup_decode(A64X_PTR_ARM64E, raw, &f));
    EXPECT_TRUE(f.auth);
    EXPECT_FALSE(f.bind);
    EXPECT_EQ(0x1234u, f.target);
    EXPECT_EQ(0xABCDu, f.diversity);
    EXPECT_EQ(1u, f.addr_div);
    EXPECT_EQ(2u, f.key);
    EXPECT_EQ(3u, f.next);
    uint64_t plain;
    ASSERT_EQ(0, a64x_fixup_strip_auth(A64X_PTR_ARM64E, raw, kBase, &plain));
    ASSERT_EQ(0, a64x_fixup_decode(A64X_PTR_ARM64E, plain, &f));
    EXPECT_FALSE(f.auth);
    EXPECT_EQ(kBase + 0x1234, f.target) << "DYLD_CHAINED_PTR_ARM64E plain rebases hold a vmaddr";
    EXPECT_EQ(0u, f.high8);
    EXPECT_EQ(3u, f.next);
    ASSERT_EQ(0, a64x_fixup_strip_auth(A64X_PTR_ARM64E_USERLAND, raw, kBase, &plain));
    ASSERT_EQ(0, a64x_fixup_decode(A64X_PTR_ARM64E_USERLAND, plain, &f));
    EXPECT_EQ(0x1234u, f.target) << "USERLAND plain rebases hold a runtime offset";
}

TEST(Fixups, AuthBindBecomesAPlainBindOfTheSameImport)
{
    uint64_t raw = auth(bind(0x123456 | 0x5555ull << 32 | 1ull << 49 | next(7)));
    uint64_t plain;
    a64x_fixup f;
    ASSERT_EQ(0, a64x_fixup_strip_auth(A64X_PTR_ARM64E_USERLAND24, raw, 0, &plain));
    ASSERT_EQ(0, a64x_fixup_decode(A64X_PTR_ARM64E_USERLAND24, plain, &f));
    EXPECT_FALSE(f.auth);
    EXPECT_TRUE(f.bind);
    EXPECT_EQ(0x123456u, f.ordinal);
    EXPECT_EQ(0, f.addend);
    EXPECT_EQ(7u, f.next);
    ASSERT_EQ(0, a64x_fixup_decode(A64X_PTR_ARM64E, raw, &f));
    EXPECT_EQ(0x3456u, f.ordinal) << "16-bit ordinals outside the 24-bit format";
}

TEST(Fixups, KernelCacheAuthLosesOnlyItsKey)
{
    uint64_t raw = auth(0x3FFFFFFF | 1ull << 30 | 0x7777ull << 32 | 1ull << 48 | 3ull << 49 | 0xFFFull << 51);
    uint64_t plain;
    ASSERT_EQ(0, a64x_fixup_strip_auth(A64X_PTR_64_KERNEL_CACHE, raw, 0, &plain));
    EXPECT_EQ(0x3FFFFFFFull | 1ull << 30 | 0xFFFull << 51, plain);
    EXPECT_EQ(4u, a64x_fixup_stride(A64X_PTR_64_KERNEL_CACHE));
}

TEST(Fixups, PlainEntriesAreUnchangedAndUnknownFormatsRefused)
{
    uint64_t raw = 0x100004000 | next(2), plain;
    ASSERT_EQ(0, a64x_fixup_strip_auth(A64X_PTR_ARM64E, raw, kBase, &plain));
    EXPECT_EQ(raw, plain);
    EXPECT_EQ(-1, a64x_fixup_strip_auth(A64X_PTR_ARM64E_SHARED_CACHE, raw, 0, &plain));
    uint64_t far = auth(0xFFFFFFFF);
    EXPECT_EQ(-1, a64x_fixup_strip_auth(A64X_PTR_ARM64E, far, 1ull << 43, &plain)) << "target past 43 bits";
}

TEST(Fixups, ImageChainsAreWalkedAndWrittenPlain)
{
    Spec s;
    s.code = {kRet};
    s.fixup_format = A64X_PTR_ARM64E;
    s.pointers = {
        {0, auth(0x2000 | 0x1111ull << 32 | next(2))},  // auth rebase, next entry 16 bytes on
        {16, 0x100002000 | next(2)},                    // plain rebase
        {32, auth(bind(5))},                            // auth bind, last
        {40, 0xDEADBEEF},                               // not in the chain
    };
    std::vector<uint8_t> in = build(s), out = in;
    auto m = std::make_unique<a64x_macho>();
    char err[256] = "";
    ASSERT_EQ(0, a64x_macho_parse(in.data(), in.size(), m.get(), err, sizeof(err))) << err;
    a64x_fixup_stats st;
    ASSERT_EQ(0, a64x_fixups_strip_auth(m.get(), out.data(), &st, err, sizeof(err))) << err;
    EXPECT_EQ(2u, st.rebases);
    EXPECT_EQ(1u, st.binds);
    EXPECT_EQ(1u, st.auth_rebases);
    EXPECT_EQ(1u, st.auth_binds);
    EXPECT_EQ(2u, st.stripped);
    uint64_t v;
    memcpy(&v, &out[kSeg], 8);
    EXPECT_EQ((kBase + 0x2000) | next(2), v);
    memcpy(&v, &out[kSeg + 16], 8);
    EXPECT_EQ(0x100002000 | next(2), v);
    memcpy(&v, &out[kSeg + 32], 8);
    EXPECT_EQ(bind(5), v);
    memcpy(&v, &out[kSeg + 40], 8);
    EXPECT_EQ(0xDEADBEEFu, v);
}

TEST(Fixups, AChainPastItsSegmentIsAnError)
{
    Spec s;
    s.code = {kRet};
    s.fixup_format = A64X_PTR_ARM64E;
    s.pointers = {{0, 0x100002000 | next(2047)}, {8 * 2047, 0x100002000 | next(2047)}};
    std::vector<uint8_t> in = build(s);
    auto m = std::make_unique<a64x_macho>();
    char err[256] = "";
    ASSERT_EQ(0, a64x_macho_parse(in.data(), in.size(), m.get(), err, sizeof(err))) << err;
    a64x_fixup_stats st;
    EXPECT_EQ(-1, a64x_fixups_strip_auth(m.get(), nullptr, &st, err, sizeof(err)));
}

TEST(Parse, TruncatedLoadCommandsAreRefused)
{
    std::vector<uint8_t> in = build(plainFunction(A64X_CPU_SUBTYPE_ARM64E));
    auto m = std::make_unique<a64x_macho>();
    char err[256];
    put32(in, 20, 0x7FFFFFFF);
    EXPECT_EQ(-1, a64x_macho_parse(in.data(), in.size(), m.get(), err, sizeof(err)));
    in = build(plainFunction(A64X_CPU_SUBTYPE_ARM64E));
    put32(in, 32 + 4, 0x10000);
    EXPECT_EQ(-1, a64x_macho_parse(in.data(), in.size(), m.get(), err, sizeof(err)));
    EXPECT_EQ(-1, a64x_macho_parse(in.data(), 16, m.get(), err, sizeof(err)));
}

}  // namespace
