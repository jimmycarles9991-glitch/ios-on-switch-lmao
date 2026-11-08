// bsd/vm/switch_ui_scale_sites_21H461.h: iOS 17's UI scale sites against 21H461's cache text mappings, and the slot each
// mapping maps them by (switch_us_site_for_slot, what switch_shared_cache_text_pager() asks once per slot of a mapping).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kernel/tests/xnu_ui_scale_sites_checks.h"

using switch_ui_scale_sites::BranchTarget;
using switch_ui_scale_sites::TextMapping;
using switch_ui_scale_sites::Word;

namespace {
// 21H461's subcaches with a patched page (ipsw dyld info). Slots another pager takes: slot 0 of .25, .13 and .03 (home
// grid, settings icons and keyboard, asked before any UI scale site) and slots 0 and 1 of .07 (the widget deadlines,
// asked after: a UI scale site there would hide one).
const std::vector<TextMapping> kMappings = {
    {".03",
     {0x1b, 0x9c, 0xcf, 0x0c, 0x7b, 0x68, 0x39, 0x63, 0xaf, 0x64, 0x48, 0x4b, 0xcf, 0xda, 0xc1, 0x4c},
     0x186dac000,
     0x6cb8000,
     1u << 0},
    {".07",
     {0xb8, 0x6b, 0xcb, 0x98, 0x4e, 0x86, 0x33, 0x74, 0xb9, 0x27, 0x98, 0xa7, 0x1e, 0x6b, 0x1e, 0xbd},
     0x1948c8000,
     0x4830000,
     1u << 0 | 1u << 1},
    {".09",
     {0x59, 0xec, 0x4b, 0x71, 0xf0, 0x27, 0x33, 0xd1, 0xae, 0x39, 0x4e, 0x04, 0xbd, 0x70, 0x7b, 0x6d},
     0x199130000,
     0x6b7c000,
     0},
    {".11",
     {0x70, 0x56, 0x7d, 0x89, 0x4c, 0xe6, 0x3e, 0xc6, 0xa4, 0xde, 0xe7, 0xb4, 0xf4, 0xec, 0x60, 0x41},
     0x19fce4000,
     0x6a8c000,
     0},
    {".13",
     {0xc0, 0x45, 0x2d, 0x94, 0xa5, 0xf8, 0x3b, 0xe1, 0xbd, 0xfc, 0xe1, 0x5e, 0x71, 0x4a, 0x84, 0xf5},
     0x1a67c8000,
     0x6c60000,
     1u << 0},
    {".25",
     {0xf1, 0x6d, 0xce, 0xf8, 0xc4, 0x46, 0x38, 0x05, 0x91, 0x63, 0xc0, 0x13, 0x8e, 0x4d, 0xca, 0xd1},
     0x1ce094000,
     0x6adc000,
     1u << 0},
    {".31",
     {0x64, 0xa2, 0xf3, 0x51, 0xbe, 0x93, 0x39, 0xa6, 0x93, 0x72, 0xe8, 0x56, 0xb6, 0x64, 0x9c, 0xfc},
     0x1e221c000,
     0x40ac000,
     0},
};

const switch_us_cache &kCache = switch_us_cache_21H461;

const switch_us_site *Named(const std::string &name)
{
    return switch_ui_scale_sites::Named(kCache, name);
}
}  // namespace

TEST(XnuUiScaleSites, EveryEditIsInItsSitesChunkOfItsSubcache)
{
    EXPECT_STREQ(kCache.build, "21H461");
    EXPECT_EQ(std::memcmp(kCache.magic, "dyld_v1   arm64", sizeof(kCache.magic)), 0);
    switch_ui_scale_sites::ExpectEveryEditInItsSitesChunk(kCache, kMappings);
}

TEST(XnuUiScaleSites, EachSiteIsTheOneSiteOfItsSlotInItsMapping)
{
    switch_ui_scale_sites::ExpectEachSiteTheOneSiteOfItsSlot(kCache, kMappings);
}

TEST(XnuUiScaleSites, BothUIKitCoreBatteryFillsAreOneEditOfTheSameCode)
{
    // SpringBoard's status bar and Control Center draw _UIBatteryViews, UIKit's own status bar a
    // _UIStatusBarBatteryView: two copies of _fillLayerFrame:cornerRadius:, the same but for one bl's displacement
    const switch_us_site *battery = Named("UIKitCore _UIBatteryView fill");
    const switch_us_site *status_bar = Named("UIKitCore _UIStatusBarBatteryView fill");
    ASSERT_NE(battery, nullptr);
    ASSERT_NE(status_bar, nullptr);
    ASSERT_EQ(battery->edits, 2u);
    ASSERT_EQ(status_bar->edits, 2u);
    EXPECT_EQ(battery->edit[1].fileoff - battery->edit[0].fileoff,
              status_bar->edit[1].fileoff - status_bar->edit[0].fileoff);
    for (unsigned i = 0; i < 2; i++) {
        SCOPED_TRACE(i);
        const switch_us_edit &a = battery->edit[i], &b = status_bar->edit[i];
        EXPECT_EQ(a.kind, b.kind);
        EXPECT_EQ(a.length, b.length);
        EXPECT_EQ(a.at, b.at);
        EXPECT_EQ(a.size, b.size);
        EXPECT_EQ(std::memcmp(a.patched, b.patched, sizeof(a.patched)), 0);
    }
    EXPECT_EQ(std::memcmp(battery->edit[0].stock, status_bar->edit[0].stock, battery->edit[0].length), 0);
    EXPECT_EQ(std::memcmp(battery->edit[1].stock, status_bar->edit[1].stock, 16), 0);
    EXPECT_EQ(Word(battery->edit[1].stock + 16) >> 26, 0x25u);  // bl CGRectGetWidth
    EXPECT_EQ(Word(status_bar->edit[1].stock + 16) >> 26, 0x25u);
}

TEST(XnuUiScaleSites, CoverSheetKitRowIsTheIPadWithoutAHomeButton)
{
    // +[CSDeviceDetermination categoryFromScreenSize]: bl BSSizeEqualToSize; tbz w0, #0, (mov w0, #28); mov w0, #24
    const switch_us_site *row = Named("CoverSheetKit lock screen layout row");
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(row->edits, 1u);
    const switch_us_edit &edit = row->edit[0];
    ASSERT_EQ(edit.kind, SWITCH_US_FIXED);
    ASSERT_EQ(edit.length, 12u);
    EXPECT_EQ(Word(edit.stock) >> 26, 0x25u);
    const uint32_t tbz = Word(edit.stock + 4);
    EXPECT_EQ(tbz & 0xfff8001fu, 0x36000000u);
    const int32_t imm14 = static_cast<int32_t>((tbz >> 5) & 0x3fff);
    const int64_t tbz_address = static_cast<int64_t>(row->page_vmaddr + (edit.fileoff - row->page_fileoff) + 4);
    EXPECT_EQ(tbz_address + (imm14 >= 0x2000 ? imm14 - 0x4000 : imm14) * 4, 0x1ac8c2410);
    EXPECT_EQ(Word(edit.stock + 8), switch_a64_movz_w(0, 24));
    EXPECT_EQ(edit.at, 4u);
    EXPECT_EQ(edit.size, 4u);
    EXPECT_EQ(Word(edit.patched), 0xd503201fu);  // nop
}

namespace {
// -[SBMixedGridSwitcherModifier _fullScreenCardScale] after the site: the settings' row scale (0.3 for two rows,
// App Exposé's 0.42 for one), the landscape scale for two rows across a wider-than-tall container, the card width
// rounded to whole points, 0 for a zero-width container.
double PatchedCardScale(double row_scale, double width, double height, double landscape_scale)
{
    const double scale = row_scale < 0.34375 && width > height ? landscape_scale : row_scale;
    const double rounded = std::round(scale * width) / width;  // frinta: ties away from zero, as std::round
    return std::isnan(rounded) ? width : rounded;              // fminnm(rounded, width), width >= 1 > rounded
}

double RoundForScale(double value, double scale)
{
    return std::round(value * scale) / scale;
}

// Six apps in the switcher, per -[SBMixedGridSwitcherModifier _unpaddedCoplanarFrameForIndex:...]: three columns of
// two rows laid out from the trailing edge, the rows and one 45 point header (icon 29 + 16) centred above the dock.
// Spacing (SBAppSwitcherSettings, a reference height of at most 1080): 44 between columns and 63 between rows in
// landscape, 42.5 and 68 in portrait, each rounded to pixels. The dock's 120 points (platter 96, 24 below it) are
// IMG_0042's at 1.25.
struct SixAppGrid {
    double card_width, card_height, top_margin, leading_margin;
    bool scrolls;
};

SixAppGrid SixApps(double width, double height, double pixels_per_point, double card_scale)
{
    constexpr double kHeader = 45, kDock = 120;
    const bool landscape = width > height;
    const double column_gap = RoundForScale(landscape ? 44 : 42.5, pixels_per_point);
    const double row_gap = RoundForScale(landscape ? 63 : 68, pixels_per_point);
    SixAppGrid grid{};
    grid.card_width = width * card_scale;
    grid.card_height = height * card_scale;
    grid.top_margin = (height - kDock - (2 * grid.card_height + row_gap + kHeader)) / 2;
    const double content = 3 * (grid.card_width + column_gap) - column_gap + 2 * column_gap;
    grid.scrolls = content > width;
    grid.leading_margin = width - column_gap - 3 * grid.card_width - 2 * column_gap;
    return grid;
}
}  // namespace

TEST(XnuUiScaleSites, SwitcherCardScaleKeepsTheShippedPathsAndBranch)
{
    const switch_us_site *site = Named("SpringBoard app switcher landscape card scale");
    ASSERT_NE(site, nullptr);
    ASSERT_EQ(site->edits, 2u);
    const switch_us_edit &branches = site->edit[0], &arithmetic = site->edit[1];
    const auto address = [&](const switch_us_edit &edit, unsigned at) {
        return site->page_vmaddr + (edit.fileoff - site->page_fileoff) + at;
    };
    // shipped: mov x0, x20; bl disable...; tbz -> mov x0, x20; fmov d9, d8; b -> after the rounding; mov; bl bounds
    ASSERT_EQ(branches.length, 28u);
    EXPECT_EQ(Word(branches.stock + 4) >> 26, 0x25u);
    EXPECT_EQ(Word(branches.stock + 24) >> 26, 0x25u);
    EXPECT_EQ(BranchTarget(address(branches, 8), Word(branches.stock + 8)), address(branches, 20));
    const uint64_t after_rounding = BranchTarget(address(branches, 16), Word(branches.stock + 16));
    EXPECT_EQ(after_rounding, address(arithmetic, arithmetic.length));
    // patched: the rounding-disabled path still leaves with the scale as set, to the same place
    uint8_t out[12] = {};
    ASSERT_EQ(branches.at, 8u);
    ASSERT_TRUE(switch_ui_scale_edit_bytes(branches.kind, switch_ui_scale_named("1.25"), out, branches.size));
    EXPECT_EQ(Word(out), 0x1e604109u);  // fmov d9, d8
    EXPECT_EQ(BranchTarget(address(branches, 12), Word(out + 4)), after_rounding);
    // clang -arch arm64: fmov d0, #0.34375; fcmp d8, d0; fccmp d2, d3, #4, mi; fcsel d8, d11, d8, gt;
    // fmul d0, d8, d2; frinta d0, d0; fdiv d9, d0, d2; fminnm d9, d9, d2
    const std::vector<uint32_t> patched = {0x1e6ad000u, 0x1e602100u, 0x1e634444u, 0x1e68cd68u,
                                           0x1e620900u, 0x1e664000u, 0x1e621809u, 0x1e627929u};
    ASSERT_EQ(arithmetic.size, 4 * patched.size());
    for (size_t i = 0; i < patched.size(); i++) {
        EXPECT_EQ(Word(arithmetic.patched + 4 * i), patched[i]) << i;
    }
}

TEST(XnuUiScaleSites, SwitcherCardsFitBetweenTheStatusBarAndTheDock)
{
    constexpr double kStatusBar = 24;
    for (const char *name : {"1.25", "1.6"}) {
        SCOPED_TRACE(name);
        const switch_ui_scale *scale = switch_ui_scale_named(name);
        ASSERT_NE(scale, nullptr);
        const double pixels = static_cast<double>(scale->num) / scale->den;
        const double width = 1280 / pixels, height = 720 / pixels;
        const double landscape_scale = static_cast<double>(scale->grid_num) / scale->grid_den;

        const SixAppGrid landscape =
            SixApps(width, height, pixels, PatchedCardScale(0.3, width, height, landscape_scale));
        EXPECT_GE(landscape.top_margin, kStatusBar);
        EXPECT_FALSE(landscape.scrolls);
        EXPECT_GE(landscape.leading_margin, 0);
        EXPECT_EQ(landscape.card_width * pixels, std::round(landscape.card_width * pixels));
        EXPECT_EQ(landscape.card_height * pixels, std::round(landscape.card_height * pixels));
        // the largest such scale: the next fmov immediate puts the titles under the status bar
        const double next = landscape_scale + (landscape_scale < 0.25 ? 1.0 / 128 : 1.0 / 64);
        EXPECT_LT(SixApps(width, height, pixels, PatchedCardScale(0.3, width, height, next)).top_margin, kStatusBar);
        // shipped 0.3 leaves no room
        EXPECT_LT(SixApps(width, height, pixels, PatchedCardScale(0.3, width, height, 0.3)).top_margin, kStatusBar);

        // portrait and App Exposé's single row keep their scales; a zero-width container still gives 0
        EXPECT_NEAR(PatchedCardScale(0.3, height, width, landscape_scale), 0.3, 0.5 / height);
        EXPECT_GE(SixApps(height, width, pixels, PatchedCardScale(0.3, height, width, landscape_scale)).top_margin,
                  kStatusBar);
        EXPECT_NEAR(PatchedCardScale(0.42, width, height, landscape_scale), 0.42, 0.5 / width);
        EXPECT_EQ(PatchedCardScale(0.3, 0, 0, landscape_scale), 0);
    }
    // 1.25: 256 x 144 point cards, 29.9 points above and below the rows
    const SixAppGrid grid = SixApps(1024, 576, 1.25, PatchedCardScale(0.3, 1024, 576, 0.25));
    EXPECT_EQ(grid.card_width, 256);
    EXPECT_EQ(grid.card_height, 144);
    EXPECT_NEAR(grid.top_margin, 29.9, 0.01);
    EXPECT_EQ(grid.leading_margin, 124);
}

TEST(XnuUiScaleSites, NoTwoSitesOfAMappingShareAPage)
{
    switch_ui_scale_sites::ExpectNoTwoSitesOfAMappingShareAPage(kCache, kMappings);
}

TEST(XnuUiScaleSites, SystemStatusUiRowIsIconSize15OfTheBatteryTables)
{
    // +[STUIStatusBarStaticBatteryView _lineWidthAndInterspaceForIconSize:] and _batterySizeForIconSize:'s height:
    // 17 doubles from 0x1cf3eb848 and 0x1cf3eba68, indexed by icon size - 1, 1.25 and 14 at 15
    const switch_us_site *site = Named("SystemStatusUI expanded battery row");
    ASSERT_NE(site, nullptr);
    ASSERT_EQ(site->edits, 2u);

    struct Entry {
        switch_ui_scale_edit_kind kind;
        uint64_t table;
        double stock;
    };

    const Entry entries[] = {{SWITCH_US_EXPANDED_BATTERY_LINE, 0x1cf3eb848, 1.25},
                             {SWITCH_US_EXPANDED_BATTERY_HEIGHT, 0x1cf3eba68, 14.0}};
    for (unsigned i = 0; i < 2; i++) {
        SCOPED_TRACE(i);
        const switch_us_edit &edit = site->edit[i];
        EXPECT_EQ(edit.kind, entries[i].kind);
        EXPECT_EQ(site->page_vmaddr + (edit.fileoff - site->page_fileoff), entries[i].table + (15 - 1) * 8);
        ASSERT_EQ(edit.length, 8u);
        EXPECT_EQ(edit.at, 0u);
        EXPECT_EQ(edit.size, 8u);
        double stock = 0;
        std::memcpy(&stock, edit.stock, sizeof(stock));
        EXPECT_EQ(stock, entries[i].stock);
    }
}

TEST(XnuUiScaleSites, CoreUiRowBytes)
{
    // both CoreUI sites: ... ucvtf d0, x24; fmul d0, dW, d0; fcvtzu x3, d0 -> mul x3, x0, x24; nop; nop
    const std::vector<uint32_t> row_bytes = {0x9b187c03u, 0xd503201fu, 0xd503201fu};
    for (const char *name : {"CoreUI PDF vector image row bytes", "CoreUI SVG vector image row bytes"}) {
        SCOPED_TRACE(name);
        const switch_us_site *site = Named(name);
        ASSERT_NE(site, nullptr);
        const switch_us_edit &edit = site->edit[0];
        ASSERT_EQ(edit.size, 12u);
        EXPECT_EQ((std::vector<uint32_t>{Word(edit.patched), Word(edit.patched + 4), Word(edit.patched + 8)}),
                  row_bytes);
    }
}

TEST(XnuUiScaleSitesSource, BothTreesAndNoPagerAtScaleOne)
{
    for (const char *header :
         {"switch_ui_scale_sites.h", "switch_ui_scale_sites_21H461.h", "switch_ui_scale_sites_24B119.h"}) {
        const std::string vm = "/bsd/vm/";
        const bool same = switch_ios::test::SourceText("../xnu-12377.121.6" + vm + header) ==
                          switch_ios::test::SourceText("../xnu-10063.141.1" + vm + header);
        EXPECT_TRUE(same) << "the two trees' " << header << " differ";
    }
    for (const char *tree : {"../xnu-10063.141.1/bsd/vm/vm_unix.c", "../xnu-12377.121.6/bsd/vm/vm_unix.c"}) {
        SCOPED_TRACE(tree);
        const std::string vm_unix = switch_ios::test::SourceText(tree);
        const std::string pager =
            switch_ios::test::Body(vm_unix, "switch_ui_scale_pager(const struct switch_us_site *site");
        const size_t unit = pager.find("if (switch_ui_scale_is_unit(switch_ui_scale())) {");
        ASSERT_NE(unit, std::string::npos);
        EXPECT_LT(unit, pager.find("vn_rdwr("));
        EXPECT_LT(unit, pager.find("apple_protect_pager_setup("));
        // a site with no bytes at the boot's scale (the battery fills at 1.6) maps no pager either
        const size_t no_bytes = pager.find("!switch_ui_scale_edit_bytes(site->edit[i].kind, switch_ui_scale()");
        ASSERT_NE(no_bytes, std::string::npos);
        EXPECT_LT(no_bytes, pager.find("apple_protect_pager_setup("));
        // the dispatch asks the kernel's kind's table which site a slot maps, and the pager its cache's magic
        const std::string dispatch = switch_ios::test::Body(vm_unix, "\nswitch_shared_cache_text_pager(");
        EXPECT_NE(dispatch.find("switch_us_site_for_slot(SWITCH_US_CACHE, slot, unslid_address, size, file_offset)"),
                  std::string::npos);
        EXPECT_NE(pager.find("memcmp(header, SWITCH_US_CACHE->magic, sizeof(SWITCH_US_CACHE->magic))"),
                  std::string::npos);
    }
}
