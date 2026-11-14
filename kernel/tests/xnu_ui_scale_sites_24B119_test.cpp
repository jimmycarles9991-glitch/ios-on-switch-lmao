// bsd/vm/switch_ui_scale_sites_24B119.h: iOS 27's UI scale sites against 24B119's cache text mappings (the arm64e
// cache as the image ships it, translated offline with every subcache's UUID and mappings kept).
#include <gtest/gtest.h>

#include <cmath>
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
// 24B119's subcaches with a patched page. SwitchTimebase's pager (switch_timebase_24B119.h) answers .01's slot 1; the
// other cache text pagers are keyed to 21H461.
const std::vector<TextMapping> kMappings = {
    {".01",
     {0xe7, 0xc1, 0xb4, 0x11, 0xdb, 0x39, 0x30, 0x4d, 0x91, 0x4a, 0x72, 0xe3, 0x1f, 0xa3, 0x1b, 0x86},
     0x180400000,
     0x7d34000,
     1u << 1},
    {".03",
     {0x8f, 0x50, 0x44, 0x24, 0x74, 0xa0, 0x3b, 0x04, 0xb1, 0x03, 0xe8, 0xd2, 0x5a, 0x06, 0xe3, 0x8b},
     0x188400000,
     0x7cac000,
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
    {".17",
     {0x4a, 0x4b, 0x97, 0x5a, 0xb4, 0x57, 0x33, 0x56, 0x93, 0xa8, 0x4f, 0x80, 0xc9, 0x09, 0x55, 0x7d},
     0x1c0400000,
     0x7d14000,
     0},
    {".36",
     {0xd8, 0x79, 0x2a, 0xca, 0x66, 0xfc, 0x3c, 0xf6, 0xb5, 0x58, 0xc6, 0xf4, 0x75, 0x99, 0x92, 0x10},
     0x220400000,
     0x7d24000,
     0},
    {".65",
     {0x06, 0x08, 0xbd, 0x6b, 0xfc, 0x49, 0x35, 0x50, 0xb8, 0xcd, 0x2f, 0x1f, 0x5d, 0x11, 0xc6, 0xcc},
     0x2a0400000,
     0x7c60000,
     0},
    {".30",
     {0xff, 0x9f, 0xc5, 0x5c, 0xee, 0x05, 0x3e, 0x45, 0xba, 0x2d, 0x82, 0x65, 0xba, 0xb4, 0x57, 0xee},
     0x208400000,
     0x7ca0000,
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

namespace {
struct Size {
    double width, height;
};

// __SBHScreenTypeLookup's fallback for an iPad without a Home button, after no screen type matched exactly: the first
// of SBDisplayMetrics' sizes the reference bounds are at least as wide and as tall as.
unsigned NoHomeButtonIPadScreenType(Size bounds)
{
    const struct {
        Size size;
        unsigned type;
    } ladder[] = {
        {{1192, 1590}, 0x70}, {{1024, 1366}, 0x6e}, {{970, 1389}, 0x6d}, {{954, 1373}, 0x6b},
        {{834, 1194}, 0x6c},  {{820, 1180}, 0x6a},  {{768, 1024}, 0x6f},
    };

    for (const auto &step : ladder) {
        if (bounds.width >= step.size.width && bounds.height >= step.size.height) {
            return step.type;
        }
    }
    return 0x68;
}

constexpr uint64_t kDefaultRootMetrics = 0x1c61e6580;

// b, bl, b.cond, cbz, cbnz, tbz, tbnz, br, blr or ret
bool IsBranch(uint32_t insn)
{
    return (insn & 0x7c000000u) == 0x14000000u || (insn & 0xff000010u) == 0x54000000u ||
           (insn & 0x7c000000u) == 0x34000000u || (insn & 0xfe000000u) == 0xd6000000u;
}

// -[SBGridLayoutSwitcherModifier scale] after the site: the landscape scale across a wider-than-tall container,
// gridSwitcherPageScale otherwise, rounded to whole points of the width; 0 for a zero width.
double PatchedPageScale(double page_scale, double width, double height, double landscape_scale)
{
    width = std::fabs(width);
    const double scale = width > height ? landscape_scale : page_scale;
    const double rounded = std::round(scale * width) / width;
    return std::isnan(rounded) ? width : rounded;
}
}  // namespace

TEST(XnuUiScaleSites24B119, HomeGridIsTheRowOfTheScreenTypeThisScreenGets)
{
    for (const char *name : {"1", "1.25", "1.6"}) {
        const switch_ui_scale *scale = switch_ui_scale_named(name);
        ASSERT_NE(scale, nullptr);
        const Size bounds = {720.0 * scale->den / scale->num, 1280.0 * scale->den / scale->num};
        EXPECT_EQ(NoHomeButtonIPadScreenType(bounds), 0x68u) << name;
    }
    EXPECT_EQ(NoHomeButtonIPadScreenType({744, 1133}), 0x68u);  // iPad mini
    EXPECT_EQ(NoHomeButtonIPadScreenType({820, 1180}), 0x6au);

    const switch_us_site *site = Named("SpringBoardHome home screen grid 3x5");
    ASSERT_NE(site, nullptr);
    ASSERT_EQ(site->edits, 2u);
    const switch_us_edit &root = site->edit[0], &widgets = site->edit[1];
    // _getRootMetrics' iPad rows are 0x90 bytes: the root grid (portrait columns and rows, then landscape) at +0, the
    // grid for pages with widgets (portrait; landscape is its transpose) at +0x48
    EXPECT_EQ(AddressOf(*site, root, 0), kDefaultRootMetrics + 0x10e0);
    EXPECT_EQ(AddressOf(*site, widgets, 0), AddressOf(*site, root, 0) + 0x48);
    EXPECT_EQ(std::vector<uint8_t>(root.stock, root.stock + root.length),
              (std::vector<uint8_t>{5, 6, 6, 5, 0, 0, 0, 0}));
    EXPECT_EQ(std::vector<uint8_t>(widgets.stock, widgets.stock + widgets.length),
              (std::vector<uint8_t>{4, 6, 0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(std::vector<uint8_t>(root.patched, root.patched + root.size), (std::vector<uint8_t>{3, 5, 5, 3}));
    EXPECT_EQ(std::vector<uint8_t>(widgets.patched, widgets.patched + widgets.size), (std::vector<uint8_t>{3, 5}));
    EXPECT_TRUE(site->every_scale);  // as 21H461's grid, which its own pager maps at every scale
}

TEST(XnuUiScaleSites24B119, SwitcherPageScaleKeepsTheShippedCallsAndExit)
{
    const switch_us_site *site = Named("SpringBoard app switcher landscape card scale");
    ASSERT_NE(site, nullptr);
    ASSERT_EQ(site->edits, 1u);
    const switch_us_edit &edit = site->edit[0];
    ASSERT_EQ(edit.length, 32u);
    EXPECT_EQ(AddressOf(*site, edit, 0), 0x227ae0a98u + 0x38);  // -[SBGridLayoutSwitcherModifier scale]
    EXPECT_EQ(Word(edit.stock) >> 26, 0x25u);                   // bl CGRectGetWidth
    EXPECT_EQ(Word(edit.stock + 4), 0x4ea01c09u);               // mov v9.16b, v0.16b
    EXPECT_EQ(Word(edit.stock + 8) >> 26, 0x25u);               // bl BSFloatIsZero
    EXPECT_EQ(Word(edit.stock + 12), 0x6f00e400u);              // movi v0.2d, #0
    EXPECT_EQ(BranchTarget(AddressOf(*site, edit, 16), Word(edit.stock + 16)), AddressOf(*site, edit, 32));
    EXPECT_EQ(Word(edit.stock + 20), 0x1e690900u);  // fmul d0, d8, d9
    EXPECT_EQ(Word(edit.stock + 24), 0x1e664000u);  // frinta d0, d0
    EXPECT_EQ(Word(edit.stock + 28), 0x1e691800u);  // fdiv d0, d0, d9
    // the patched words fall through to the epilogue the tbnz left for, with the scale in d0
    ASSERT_EQ(edit.at, 0u);
    uint8_t out[32] = {};
    ASSERT_TRUE(switch_ui_scale_edit_bytes(edit.kind, switch_ui_scale_named("1.25"), out, edit.size));
    for (unsigned i = 0; i < 32; i += 4) {
        EXPECT_FALSE(IsBranch(Word(out + i))) << i;
    }
}

TEST(XnuUiScaleSites24B119, SwitcherCardsAreTheLandscapeScaleOnlyInLandscape)
{
    for (const char *name : {"1.25", "1.6"}) {
        SCOPED_TRACE(name);
        const switch_ui_scale *scale = switch_ui_scale_named(name);
        ASSERT_NE(scale, nullptr);
        const double pixels = static_cast<double>(scale->num) / scale->den;
        const double width = 1280 / pixels, height = 720 / pixels;
        const double landscape_scale = static_cast<double>(scale->grid_num) / scale->grid_den;
        EXPECT_EQ(PatchedPageScale(0.3, width, height, landscape_scale), landscape_scale);
        EXPECT_NEAR(PatchedPageScale(0.3, height, width, landscape_scale), 0.3, 0.5 / height);
        EXPECT_EQ(PatchedPageScale(0.3, 0, 0, landscape_scale), 0);
        EXPECT_EQ(PatchedPageScale(0.3, -width, height, landscape_scale), landscape_scale);
    }
    // 1.25: 256 x 144 point cards in landscape (320 x 180 pixels)
    EXPECT_EQ(1024 * PatchedPageScale(0.3, 1024, 576, 0.25), 256);
    EXPECT_EQ(576 * PatchedPageScale(0.3, 1024, 576, 0.25), 144);
}

TEST(XnuUiScaleSites24B119, PlaceholderBatchDeadlineIsNPlusTenTimesFiveInBothCopies)
{
    const switch_us_site *site = Named("WidgetKit placeholder batch deadline (n + 10) x 5 s");
    ASSERT_NE(site, nullptr);
    EXPECT_TRUE(site->every_scale);
    ASSERT_EQ(site->edits, 2u);
    // the getter and DefaultWatchdogTimeoutProvider's function, each + 0x38
    EXPECT_EQ(AddressOf(*site, site->edit[0], 0), 0x18bd532d4u + 0x38);
    EXPECT_EQ(AddressOf(*site, site->edit[1], 0), 0x18bd537b8u + 0x38);
    // clang -arch arm64: fadd d1, d0, d1; fmov d2, #10.0; fmaxnm d1, d1, d2; fadd d0, d0, d2; fmov d2, #20.0;
    // fmaxnm d0, d0, d2; cmp w9, #0; fcsel d0, d1, d0, eq -> fmov d2, #5.0; fmul d0, d0, d2 for the last two of six
    const std::vector<uint32_t> stock = {0x1e612801u, 0x1e649002u, 0x1e626821u, 0x1e622800u,
                                         0x1e669002u, 0x1e626800u, 0x7100013fu, 0x1e600c20u};
    for (unsigned e = 0; e < site->edits; e++) {
        SCOPED_TRACE(e);
        const switch_us_edit &edit = site->edit[e];
        ASSERT_EQ(edit.length, 4 * stock.size());
        for (size_t i = 0; i < stock.size(); i++) {
            EXPECT_EQ(Word(edit.stock + 4 * i), stock[i]) << i;
        }
        EXPECT_EQ(edit.kind, SWITCH_US_FIXED);
        ASSERT_EQ(edit.at, 16u);
        ASSERT_EQ(edit.size, 8u);
        EXPECT_EQ(Word(edit.patched), 0x1e629002u);
        EXPECT_EQ(Word(edit.patched + 4), 0x1e620800u);
    }
    // getPlaceholders(13): 23 s shipped, 115 s after
    const double n = 13;
    EXPECT_EQ(std::fmax(n + 10, 20), 23);
    EXPECT_EQ((n + 10) * 5, 115);
}

namespace {
// The archiver's limit in pixels for a widget of `points` and a display scale (the site's caller's arithmetic): 2.2
// and 2 times the widget's pixels, each at least 80
double ArchivedImageLimit(Size points, double scale)
{
    return std::fmax(2.2 * points.width * scale, 80) * std::fmax(2.0 * points.height * scale, 80);
}
}  // namespace

TEST(XnuUiScaleSites24B119, ArchivedImageLimitIsAt2xInsteadOfTheDisplaysScale)
{
    const switch_us_site *site = Named("WidgetKit archived image limit of a 2x screen");
    ASSERT_NE(site, nullptr);
    EXPECT_TRUE(site->every_scale);
    ASSERT_EQ(site->edits, 1u);
    const switch_us_edit &edit = site->edit[0];
    ASSERT_EQ(edit.length, 32u);
    EXPECT_EQ(edit.kind, SWITCH_US_FIXED);
    // bl size (its CGSize in d0, d1); mov v10.16b, v0.16b; mov v11.16b, v1.16b; mov x0, x20; bl scale;
    // fmul d10, d10, d0; mov x0, x20; bl scale
    EXPECT_EQ(Word(edit.stock) >> 26, 0x25u);
    EXPECT_EQ(Word(edit.stock + 4), 0x4ea01c0au);
    EXPECT_EQ(Word(edit.stock + 8), 0x4ea11c2bu);
    EXPECT_EQ(Word(edit.stock + 12), 0xaa1403e0u);
    EXPECT_EQ(Word(edit.stock + 16) >> 26, 0x25u);
    EXPECT_EQ(Word(edit.stock + 20), 0x1e60094au);
    EXPECT_EQ(Word(edit.stock + 24), 0xaa1403e0u);
    EXPECT_EQ(Word(edit.stock + 28) >> 26, 0x25u);
    // both sends go to the same objc stub
    EXPECT_EQ(BranchTarget(AddressOf(*site, edit, 16), Word(edit.stock + 16)),
              BranchTarget(AddressOf(*site, edit, 28), Word(edit.stock + 28)));
    ASSERT_EQ(edit.at, 16u);
    ASSERT_EQ(edit.size, 16u);
    EXPECT_EQ(Word(edit.patched), switch_a64_fmov_d_int(0, 2));
    EXPECT_EQ(std::memcmp(edit.patched + 4, edit.stock + 20, 8), 0);
    EXPECT_EQ(Word(edit.patched + 12), switch_a64_fmov_d_int(0, 2));

    // boot 15: GameCenter's 310 x 310 artwork in the lock screen's 103.5 point small widget
    const Size widget = {103.5, 103.5};
    EXPECT_NEAR(ArchivedImageLimit(widget, 1.25), 73646.71875, 1e-6);
    EXPECT_GT(310.0 * 310.0, ArchivedImageLimit(widget, 1.25));
    EXPECT_LE(310.0 * 310.0, ArchivedImageLimit(widget, 2));
}

namespace {
enum class KeylineRow {
    k1xOr2x,
    k3x,
    kMagenta
};

// The keyline's ladder for one interface style: 1.0 and 2.0 share a row, 3.0 has its own, and stock UIKit fills any
// other scale with opaque magenta.
KeylineRow KeylineRowFor(double scale, bool patched)
{
    if (scale == 1.0 || scale == 2.0) {
        return KeylineRow::k1xOr2x;
    }
    if (scale == 3.0) {
        return KeylineRow::k3x;
    }
    return patched ? KeylineRow::k1xOr2x : KeylineRow::kMagenta;
}
}  // namespace

TEST(XnuUiScaleSites24B119, ScrollEdgeKeylineTakesThe2xRowAtEveryScaleBut3)
{
    const switch_us_site *site = Named("UIKitCore scroll edge keyline of a 2x screen");
    ASSERT_NE(site, nullptr);
    ASSERT_EQ(site->edits, 2u);
    EXPECT_FALSE(site->every_scale);
    const switch_us_edit &dark = site->edit[0], &light = site->edit[1];
    for (const switch_us_edit *edit : {&dark, &light}) {
        SCOPED_TRACE(edit == &dark ? "dark" : "light");
        ASSERT_EQ(edit->length, 24u);
        ASSERT_EQ(edit->kind, SWITCH_US_FIXED);
        EXPECT_EQ(Word(edit->stock), switch_a64_fmov_d_int(0, 2));
        EXPECT_EQ(Word(edit->stock + 4), 0x1e602140u);                // fcmp d10, d0
        EXPECT_EQ(Word(edit->stock + 8) & 0xff00001fu, 0x54000000u);  // b.eq
        EXPECT_EQ(Word(edit->stock + 12), switch_a64_fmov_d_int(0, 3));
        EXPECT_EQ(Word(edit->stock + 16), 0x1e602140u);
        EXPECT_EQ(Word(edit->stock + 20) & 0xff00001fu, 0x54000001u);  // b.ne
        ASSERT_EQ(edit->at, 20u);
        ASSERT_EQ(edit->size, 4u);
        EXPECT_EQ(Word(edit->patched) & 0xff00001fu, 0x54000001u);
        // past 3.0, to where 2.0 goes
        EXPECT_EQ(BranchTarget(AddressOf(*site, *edit, 20), Word(edit->patched)),
                  BranchTarget(AddressOf(*site, *edit, 8), Word(edit->stock + 8)));
    }
    // stock dark goes on to the light ladder's 1.0 test (fmov d0, #1.0; fcmp d10, d0; b.eq), whose last b.ne is the
    // magenta fill
    EXPECT_EQ(BranchTarget(AddressOf(*site, dark, 20), Word(dark.stock + 20)), AddressOf(*site, light, 0) - 12);
    EXPECT_GT(BranchTarget(AddressOf(*site, light, 20), Word(light.stock + 20)), AddressOf(*site, light, 24));

    for (const char *name : {"1.25", "1.6"}) {
        const switch_ui_scale *scale = switch_ui_scale_named(name);
        ASSERT_NE(scale, nullptr);
        const double pixels = static_cast<double>(scale->num) / scale->den;
        EXPECT_EQ(KeylineRowFor(pixels, false), KeylineRow::kMagenta) << name;
        EXPECT_EQ(KeylineRowFor(pixels, true), KeylineRow::k1xOr2x) << name;
    }
    for (const double apple : {1.0, 2.0, 3.0}) {
        EXPECT_EQ(KeylineRowFor(apple, true), KeylineRowFor(apple, false)) << apple;
    }
}

TEST(XnuUiScaleSites24B119, TableReloadUsesEffectiveFocusAndKeepsItsResetAndDragCalls)
{
    const switch_us_site *site = Named("UIKitCore table reload focus request only when effective focus is allowed");
    ASSERT_NE(site, nullptr);
    EXPECT_TRUE(site->every_scale);
    ASSERT_EQ(site->edits, 2u);
    EXPECT_EQ(site->slot, 3u);
    EXPECT_EQ(switch_ui_scale_sites::TextSlots("../xnu-12377.121.6/osfmk/vm/vm_shared_region.c"), 4u);
    EXPECT_EQ(switch_ui_scale_sites::TextSlots("../xnu-10063.141.1/osfmk/vm/vm_shared_region.c"), 3u);
    const switch_us_edit &first = site->edit[0], &second = site->edit[1];
    EXPECT_EQ(AddressOf(*site, first, 0), 0x184c56508u);
    EXPECT_EQ(AddressOf(*site, second, 0), AddressOf(*site, first, 32));
    for (const switch_us_edit *edit : {&first, &second}) {
        EXPECT_EQ(edit->length, 32u);
        EXPECT_EQ(edit->at, 0u);
        EXPECT_EQ(edit->kind, SWITCH_US_FIXED);
    }
    ASSERT_EQ(first.size, 32u);
    ASSERT_EQ(second.size, 16u);
    EXPECT_EQ(Word(first.patched), 0xaa1303e0u);  // mov x0, x19 (table)
    EXPECT_EQ(BranchTarget(AddressOf(*site, first, 4), Word(first.patched + 4)), 0x188022d50u);
    EXPECT_EQ(Word(first.patched + 8), 0x34000060u);  // cbz w0: only the focus request is skipped
    EXPECT_EQ(BranchTarget(AddressOf(*site, first, 8), Word(first.patched + 8)), AddressOf(*site, first, 20));
    EXPECT_EQ(Word(first.patched + 12), Word(first.patched));
    EXPECT_EQ(BranchTarget(AddressOf(*site, first, 16), Word(first.patched + 16)),
              BranchTarget(AddressOf(*site, first, 4), Word(first.stock + 4)));
    EXPECT_EQ(BranchTarget(AddressOf(*site, first, 16), Word(first.patched + 16)), 0x1880e55c0u);
    EXPECT_EQ(Word(first.patched + 20), Word(first.stock + 8));
    EXPECT_EQ(Word(first.patched + 24), Word(first.stock + 12));
    EXPECT_EQ(Word(first.patched + 28), 0x694056c8u);  // ldpsw x8, x21, [x22]
    EXPECT_EQ(Word(first.stock + 16), 0xb98002c8u);    // ldrsw x8, [x22]
    EXPECT_EQ(Word(second.stock), 0xb98006c8u);        // ldrsw x8, [x22, #4]
    EXPECT_EQ(Word(second.patched), 0xf8686a60u);      // ldr x0, [x19, x8]
    EXPECT_EQ(Word(second.patched + 8), 0xf8756a60u);  // ldr x0, [x19, x21]
    EXPECT_EQ(BranchTarget(AddressOf(*site, second, 4), Word(second.patched + 4)),
              BranchTarget(AddressOf(*site, first, 28), Word(first.stock + 28)));
    EXPECT_EQ(BranchTarget(AddressOf(*site, second, 4), Word(second.patched + 4)), 0x18809cbc0u);
    EXPECT_EQ(Word(second.patched + 12), Word(second.stock + 12));
    EXPECT_EQ(BranchTarget(AddressOf(*site, second, 12), Word(second.patched + 12)), 0x188080c70u);
    // The retained drag token and the subsequent table reset still start at the stock continuation.
    EXPECT_EQ(BranchTarget(AddressOf(*site, second, 16), Word(second.stock + 16)), 0x188143850u);
    EXPECT_EQ(Word(second.stock + 20), 0xaa0003f4u);
    EXPECT_EQ(Word(second.stock + 24), 0xaa1303e0u);
    EXPECT_EQ(BranchTarget(AddressOf(*site, second, 28), Word(second.stock + 28)), 0x188053c80u);
}

TEST(XnuUiScaleSites24B119, OnlyTheGridWidgetKitTimebaseAndTableFocusApplyAtScaleOne)
{
    for (size_t i = 0; i < kCache.count; i++) {
        const std::string name = switch_ui_scale_sites::NameOf(kCache.sites[i]);
        const bool every_scale = name == "SpringBoardHome home screen grid 3x5" ||
                                 name == "WidgetKit placeholder batch deadline (n + 10) x 5 s" ||
                                 name == "WidgetKit archived image limit of a 2x screen" ||
                                 name == "SampleAnalysis mach timebase of a 19.2 MHz counter" ||
                                 name == "UIKitCore table reload focus request only when effective focus is allowed";
        EXPECT_EQ(kCache.sites[i].every_scale, every_scale) << name;
    }
}

TEST(XnuUiScaleSites24B119, SampleAnalysisKnowsTheX1Timebase)
{
    const switch_us_site *site = Named("SampleAnalysis mach timebase of a 19.2 MHz counter");
    ASSERT_NE(site, nullptr);
    ASSERT_EQ(site->edits, 2u);
    EXPECT_TRUE(site->every_scale);
    const switch_us_edit &pairs = site->edit[0], &rates = site->edit[1];
    uint32_t stock[4] = {}, patched[2] = {};
    std::memcpy(stock, pairs.stock, sizeof(stock));
    std::memcpy(patched, pairs.patched, sizeof(patched));
    EXPECT_EQ(stock[0], 1u);
    EXPECT_EQ(stock[1], 1u);
    EXPECT_EQ(static_cast<double>(stock[2]) / stock[3], 1e9 / 24e6);
    EXPECT_EQ(static_cast<double>(patched[0]) / patched[1], 1e9 / 19.2e6);
    EXPECT_EQ(Double(rates.stock), 1e-7);
    EXPECT_EQ(Double(rates.stock + 8), 24e6);
    EXPECT_EQ(Double(rates.patched), 19.2e6);
}
