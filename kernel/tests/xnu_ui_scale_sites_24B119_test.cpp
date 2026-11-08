// bsd/vm/switch_ui_scale_sites_24B119.h: iOS 27's UI scale sites against 24B119's cache text mappings (the arm64e
// cache as the image ships it, translated offline with every subcache's UUID and mappings kept).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kernel/tests/xnu_ui_scale_sites_checks.h"

using switch_ui_scale_sites::AddressOf;
using switch_ui_scale_sites::BranchTarget;
using switch_ui_scale_sites::TextMapping;
using switch_ui_scale_sites::Word;

namespace {
// 24B119's subcaches with a patched page. No other cache text pager answers in them: those are keyed to 21H461.
const std::vector<TextMapping> kMappings = {
    {".01",
     {0xe7, 0xc1, 0xb4, 0x11, 0xdb, 0x39, 0x30, 0x4d, 0x91, 0x4a, 0x72, 0xe3, 0x1f, 0xa3, 0x1b, 0x86},
     0x180400000,
     0x7d34000,
     0},
    {".11",
     {0x13, 0xd1, 0x27, 0xbc, 0x9e, 0xd2, 0x36, 0x63, 0xa6, 0x80, 0xfd, 0x90, 0x2f, 0xbb, 0xd2, 0x51},
     0x1a8400000,
     0x7d18000,
     0},
    {".13",
     {0xa5, 0x8c, 0x59, 0x09, 0x8b, 0xb2, 0x30, 0x14, 0xa7, 0xb5, 0x28, 0xb4, 0x36, 0x43, 0x92, 0x5e},
     0x1b0400000,
     0x7d30000,
     0},
    {".15",
     {0x4f, 0x44, 0xc1, 0x4a, 0x9b, 0x0b, 0x35, 0x03, 0xaa, 0x96, 0x94, 0xef, 0xd6, 0x5b, 0xca, 0x4e},
     0x1b8400000,
     0x7d04000,
     0},
    {".65",
     {0x06, 0x08, 0xbd, 0x6b, 0xfc, 0x49, 0x35, 0x50, 0xb8, 0xcd, 0x2f, 0x1f, 0x5d, 0x11, 0xc6, 0xcc},
     0x2a0400000,
     0x7c60000,
     0},
};

const switch_us_cache &kCache = switch_us_cache_24B119;

const switch_us_site *Named(const std::string &name)
{
    return switch_ui_scale_sites::Named(kCache, name);
}

double Double(const uint8_t *bytes)
{
    double value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

struct IdiomEntry {
    double width, height;
    unsigned idiom;
};

// _UIKeyboardComputeKeyboardIdiomFromScreenTraits on an iPad: an entry the screen is at least as large as (width plus
// height) gives its idiom unless the screen is exactly its size, the last such entry winning; none leaves 1.
unsigned KeyboardIdiom(const std::vector<IdiomEntry> &table, double width, double height)
{
    unsigned idiom = 1;
    for (const IdiomEntry &entry : table) {
        if (width + height >= entry.width + entry.height && !(width == entry.width && height == entry.height)) {
            idiom = entry.idiom;
        }
    }
    return idiom;
}
}  // namespace

TEST(XnuUiScaleSites24B119, EveryEditIsInItsSitesChunkOfItsSubcache)
{
    EXPECT_STREQ(kCache.build, "24B119");
    EXPECT_EQ(std::memcmp(kCache.magic, "dyld_v1  arm64e", sizeof(kCache.magic)), 0);
    switch_ui_scale_sites::ExpectEveryEditInItsSitesChunk(kCache, kMappings);
    switch_ui_scale_sites::ExpectEditBytesAtEveryScale(kCache);
}

TEST(XnuUiScaleSites24B119, EachSiteIsTheOneSiteOfItsSlotInItsMapping)
{
    switch_ui_scale_sites::ExpectEachSiteTheOneSiteOfItsSlot(kCache, kMappings);
    switch_ui_scale_sites::ExpectNoTwoSitesOfAMappingShareAPage(kCache, kMappings);
}

TEST(XnuUiScaleSites24B119, ScaleOverrideReplacesTheExternalDisplayScale)
{
    // bl isMainDisplay; movi v8.2d, #0; tbnz w0, #0, +0x10; mov x0, x26; bl ...GetExternalDisplayScale; fcvt d8, s0;
    // mov x0, x26
    const switch_us_site *site = Named("FrontBoardServices main display scale override");
    ASSERT_NE(site, nullptr);
    const switch_us_edit &edit = site->edit[0];
    ASSERT_EQ(edit.length, 28u);
    EXPECT_EQ(Word(edit.stock) >> 26, 0x25u);
    EXPECT_EQ(Word(edit.stock + 4), 0x6f00e408u);
    EXPECT_EQ(BranchTarget(AddressOf(*site, edit, 8), Word(edit.stock + 8)), AddressOf(*site, edit, 24));
    EXPECT_EQ(Word(edit.stock + 16) >> 26, 0x25u);
    EXPECT_EQ(Word(edit.stock + 24), 0xaa1a03e0u);
    // the main display (w0 bit 0) keeps num / den, any other display none, both on to the shipped mov x0, x26
    uint8_t out[20] = {};
    ASSERT_EQ(edit.at, 4u);
    ASSERT_TRUE(switch_ui_scale_edit_bytes(edit.kind, switch_ui_scale_named("1.6"), out, edit.size));
    EXPECT_EQ(Word(out), switch_a64_fmov_d_int(8, 8));
    EXPECT_EQ(Word(out + 4), switch_a64_fmov_d_int(1, 5));
    EXPECT_EQ(BranchTarget(AddressOf(*site, edit, 16), Word(out + 12)), AddressOf(*site, edit, 24));
}

TEST(XnuUiScaleSites24B119, MainScreenScaleIsTheScalesFloat)
{
    const switch_us_site *site = Named("MobileGestalt main-screen-scale");
    ASSERT_NE(site, nullptr);
    ASSERT_EQ(site->edits, 2u);
    const switch_us_edit &read = site->edit[0], &convert = site->edit[1];
    EXPECT_EQ(Word(read.stock), switch_a64_movz_w(4, 1));
    EXPECT_EQ(Word(read.stock + 4) >> 26, 0x25u);
    EXPECT_EQ(Word(read.stock + 8), 0xaa0003f3u);     // mov x19, x0
    EXPECT_EQ(Word(convert.stock + 4), 0x1e220260u);  // scvtf s0, w19
    EXPECT_EQ(Word(convert.patched), 0x1e270260u);    // fmov s0, w19
    uint8_t out[8] = {};
    ASSERT_TRUE(switch_ui_scale_edit_bytes(read.kind, switch_ui_scale_named("1.6"), out, read.size));
    EXPECT_EQ(Word(out), switch_a64_movz_w(19, 0xcccd));
    EXPECT_EQ(Word(out + 4), switch_a64_movk_w_lsl16(19, 0x3fcc));
}

TEST(XnuUiScaleSites24B119, CoreUiRowBytesAndProgressBar)
{
    const std::vector<uint32_t> stock = {0x9e790180u, 0x9e790161u, 0x9e630300u, 0x1e600980u, 0x9e790003u};
    const std::vector<uint32_t> row_bytes = {0x9b187c03u, 0xd503201fu, 0xd503201fu};  // mul x3, x0, x24; nop; nop
    for (const char *name : {"CoreUI PDF vector image row bytes", "CoreUI SVG vector image row bytes"}) {
        SCOPED_TRACE(name);
        const switch_us_site *site = Named(name);
        ASSERT_NE(site, nullptr);
        const switch_us_edit &edit = site->edit[0];
        for (size_t i = 0; i < stock.size(); i++) {
            EXPECT_EQ(Word(edit.stock + 4 * i) & 0xfffffc1fu, stock[i] & 0xfffffc1fu) << i;
        }
        ASSERT_EQ(edit.at, 8u);
        ASSERT_EQ(edit.size, 12u);
        EXPECT_EQ((std::vector<uint32_t>{Word(edit.patched), Word(edit.patched + 4), Word(edit.patched + 8)}),
                  row_bytes);
    }
    const switch_us_site *bar = Named("ProgressUI progress bar");
    ASSERT_NE(bar, nullptr);
    float size[2] = {};
    std::memcpy(size, bar->edit[0].stock, sizeof(size));
    EXPECT_EQ(size[0], 239.0f);
    EXPECT_EQ(size[1], 4.0f);
}

TEST(XnuUiScaleSites24B119, CoverSheetKitRowIsJ181sHeightClassWithoutAHomeButton)
{
    // +[CSDeviceDetermination categoryFromReferenceBounds:]: bl BSFloatGreaterThanFloat (height over 1024); tbz w0, #0,
    // (the 768x1024 test, else 0x22); mov w0, #0x21
    const switch_us_site *row = Named("CoverSheetKit lock screen layout row");
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(row->edits, 1u);
    const switch_us_edit &edit = row->edit[0];
    ASSERT_EQ(edit.length, 12u);
    EXPECT_EQ(Word(edit.stock) >> 26, 0x25u);
    const uint32_t tbz = Word(edit.stock + 4);
    EXPECT_EQ(tbz & 0xfff8001fu, 0x36000000u);
    EXPECT_EQ(BranchTarget(AddressOf(*row, edit, 4), tbz), 0x1b45a36b8u);
    EXPECT_EQ(Word(edit.stock + 8), switch_a64_movz_w(0, 0x21));
    EXPECT_EQ(edit.at, 4u);
    EXPECT_EQ(edit.size, 4u);
    EXPECT_EQ(Word(edit.patched), 0xd503201fu);  // nop
}

TEST(XnuUiScaleSites24B119, KeyboardIsThe10Point2InchIPadsBelowItsSize)
{
    const switch_us_site *site = Named("UIKitCore keyboard class of the 10.2-inch iPad");
    ASSERT_NE(site, nullptr);
    const switch_us_edit &edit = site->edit[0];
    ASSERT_EQ(edit.length, 24u);
    EXPECT_EQ(Double(edit.stock), 768.0);
    EXPECT_EQ(Double(edit.stock + 8), 1024.0);
    EXPECT_EQ(Word(edit.stock + 16), 26u);
    ASSERT_EQ(edit.at, 0u);
    ASSERT_EQ(edit.size, 16u);
    EXPECT_EQ(Double(edit.patched), 0.0);
    EXPECT_EQ(Double(edit.patched + 8), 0.0);

    // iPadWidthFilterTable as 24B119 ships it after its first entry
    const std::vector<IdiomEntry> rest = {
        {810, 1080, 25}, {834, 1112, 24}, {820, 1180, 24}, {834, 1194, 24}, {954, 1366, 23}};
    std::vector<IdiomEntry> stock = {{768, 1024, 26}}, patched = {{0, 0, 26}};
    stock.insert(stock.end(), rest.begin(), rest.end());
    patched.insert(patched.end(), rest.begin(), rest.end());
    EXPECT_EQ(KeyboardIdiom(stock, 810, 1080), 26u);  // J181
    EXPECT_EQ(KeyboardIdiom(stock, 576, 1024), 1u);   // 1.25
    EXPECT_EQ(KeyboardIdiom(stock, 450, 800), 1u);    // 1.6
    EXPECT_EQ(KeyboardIdiom(patched, 576, 1024), 26u);
    EXPECT_EQ(KeyboardIdiom(patched, 450, 800), 26u);
    // every screen of 768x1024 and over keeps its idiom but 768x1024 itself, which no Switch scale gives
    for (const IdiomEntry &screen :
         {IdiomEntry{720, 1280, 0}, IdiomEntry{810, 1080, 0}, IdiomEntry{834, 1194, 0}, IdiomEntry{1024, 1366, 0}}) {
        EXPECT_EQ(KeyboardIdiom(patched, screen.width, screen.height),
                  KeyboardIdiom(stock, screen.width, screen.height))
            << screen.width << "x" << screen.height;
    }
}
