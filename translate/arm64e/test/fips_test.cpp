// macho/fips.h on headers built here: which text corecrypto's FIPS integrity check covers, and its MAC.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../macho/fips.h"
}

namespace {

struct Section {
    const char *sectname, *segname;
    uint64_t addr, size;
    uint32_t offset;
};

struct Segment {
    const char *name;
    std::vector<Section> sections;
};

void put(std::vector<uint8_t> &b, size_t at, const void *v, size_t n)
{
    memcpy(&b[at], v, n);
}

void put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
    put(b, at, &v, 4);
}

void put64(std::vector<uint8_t> &b, size_t at, uint64_t v)
{
    put(b, at, &v, 8);
}

std::vector<uint8_t> header(const std::vector<Segment> &segments)
{
    std::vector<uint8_t> b(32);
    put32(b, 0, 0xFEEDFACF);
    for (const auto &seg : segments) {
        const size_t at = b.size();
        b.resize(at + 72 + 80 * seg.sections.size());
        put32(b, at, 0x19);
        put32(b, at + 4, (uint32_t)(72 + 80 * seg.sections.size()));
        put(b, at + 8, seg.name, strlen(seg.name));
        put32(b, at + 64, (uint32_t)seg.sections.size());
        for (size_t k = 0; k < seg.sections.size(); k++) {
            const Section &s = seg.sections[k];
            const size_t sp = at + 72 + 80 * k;
            put(b, sp, s.sectname, strlen(s.sectname));
            put(b, sp + 16, s.segname, strlen(s.segname));
            put64(b, sp + 32, s.addr);
            put64(b, sp + 40, s.size);
            put32(b, sp + 48, s.offset);
        }
    }
    put32(b, 16, (uint32_t)segments.size());
    put32(b, 20, (uint32_t)(b.size() - 32));
    return b;
}

TEST(Fips, LibraryTextAndMacInText)
{
    auto h = header({{"__TEXT",
                      {{"__text", "__TEXT", 0x1000, 0x400, 0x1000},
                       {"__cstring", "__TEXT", 0x1400, 0x80, 0x1400},
                       {"__fips_hmacs", "__TEXT", 0x1480, 32, 0x1480}}},
                     {"__DATA", {{"__data", "__DATA", 0x2000, 0x100, 0x2000}}}});
    a64x_fips f{};
    ASSERT_EQ(a64x_fips_find(h.data(), h.size(), &f), 1);
    EXPECT_EQ(f.text_vmaddr, 0x1000u);
    EXPECT_EQ(f.text_fileoff, 0x1000u);
    EXPECT_EQ(f.text_size, 0x400u);
    EXPECT_EQ(f.mac_vmaddr, 0x1480u);
    EXPECT_EQ(f.mac_fileoff, 0x1480u);
}

TEST(Fips, KextTextInTextExec)
{
    auto h = header(
        {{"__TEXT", {{"__const", "__TEXT", 0x1000, 0x100, 0x1000}, {"__fips_hmacs", "__TEXT", 0x1100, 32, 0x1100}}},
         {"__TEXT_EXEC", {{"__text", "__TEXT_EXEC", 0x8000, 0x2000, 0x8000}}}});
    a64x_fips f{};
    ASSERT_EQ(a64x_fips_find(h.data(), h.size(), &f), 1);
    EXPECT_EQ(f.text_vmaddr, 0x8000u);
    EXPECT_EQ(f.text_size, 0x2000u);
    EXPECT_EQ(f.mac_vmaddr, 0x1100u);
}

TEST(Fips, OnlyTextSegmentsCount)
{
    auto h = header({{"__DATA", {{"__text", "__DATA", 0x4000, 0x10, 0x4000}}},
                     {"__TEXT", {{"__fips_hmacs", "__TEXT", 0x1100, 32, 0x1100}}}});
    a64x_fips f{};
    EXPECT_EQ(a64x_fips_find(h.data(), h.size(), &f), 0);
}

TEST(Fips, NoMacNoCheck)
{
    auto h = header({{"__TEXT", {{"__text", "__TEXT", 0x1000, 0x400, 0x1000}}}});
    a64x_fips f{};
    EXPECT_EQ(a64x_fips_find(h.data(), h.size(), &f), 0);
}

TEST(Fips, MacIsHmacSha256WithAZeroKey)
{
    const uint32_t words[] = {0xD503201F, 0xD503201F, 0xD503201F, 0xD65F03C0};
    uint8_t mac[A64X_FIPS_MAC_SIZE];
    a64x_fips_mac(reinterpret_cast<const uint8_t *>(words), sizeof(words), mac);
    std::string hex;
    for (uint8_t c : mac) {
        char two[3];
        snprintf(two, sizeof(two), "%02x", c);
        hex += two;
    }
    // Python: hmac.new(bytes(32), bytes.fromhex('1f2003d5' * 3 + 'c0035fd6'), hashlib.sha256).hexdigest()
    EXPECT_EQ(hex, "2ea886cf1c205e564edb8f7b00413cfa34cc5e691a3ac044e9e76314e31602ce");
}

TEST(Fips, RewritesOnlyAMacThatHeld)
{
    const uint8_t original[8] = {1, 2, 3, 4, 5, 6, 7, 8}, translated[8] = {1, 2, 3, 4, 9, 9, 9, 9};
    uint8_t held[A64X_FIPS_MAC_SIZE], now[A64X_FIPS_MAC_SIZE], want[A64X_FIPS_MAC_SIZE];
    a64x_fips_mac(original, sizeof(original), held);
    a64x_fips_mac(translated, sizeof(translated), want);
    memcpy(now, held, sizeof(now));
    EXPECT_EQ(a64x_fips_rewrite(original, translated, sizeof(original), now), 1);
    EXPECT_EQ(memcmp(now, want, sizeof(now)), 0);
    EXPECT_TRUE(a64x_fips_kept(original, translated, sizeof(original), held, want));
    EXPECT_FALSE(a64x_fips_kept(original, translated, sizeof(original), held, held));

    uint8_t stale[A64X_FIPS_MAC_SIZE] = {0xAB};
    memcpy(now, stale, sizeof(now));
    EXPECT_EQ(a64x_fips_rewrite(original, translated, sizeof(original), now), 0);
    EXPECT_EQ(memcmp(now, stale, sizeof(now)), 0);
    EXPECT_TRUE(a64x_fips_kept(original, translated, sizeof(original), stale, stale));
    EXPECT_FALSE(a64x_fips_kept(original, translated, sizeof(original), stale, want));
}

}  // namespace
