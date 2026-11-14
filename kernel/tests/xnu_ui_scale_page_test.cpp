// switch_ui_scale_page in both trees, over 24B119's table focus site: its two edits are only right together (the first
// moves the second's ivar load into an ldpsw), so the hook must check every edit's whole window before writing any.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <vm/switch_ui_scale_sites.h>

#include "xnu_ui_scale_page_harness.h"

namespace {

using PageHook = int (*)(const void *, void *, unsigned long long, const switch_us_site *);

struct Tree {
    const char *name;
    PageHook page;
};

const Tree kTrees[] = {{"xnu-10063", xnu10063_ui_scale_page}, {"xnu-12377", xnu12377_ui_scale_page}};
constexpr size_t kChunk = 4096;
constexpr const char *kFocus = "table reload focus request only when effective focus is allowed";

const switch_us_site *FocusSite()
{
    return xnu_ui_scale_page_site("UIKitCore", kFocus);
}

uint64_t ChunkStart(const switch_us_site &site)
{
    return site.page_fileoff + site.chunk_offset;
}

std::vector<uint8_t> StockChunk(const switch_us_site &site)
{
    std::vector<uint8_t> chunk(kChunk);
    for (size_t i = 0; i < kChunk; i++)
        chunk[i] = static_cast<uint8_t>(i * 7 + 3);
    for (unsigned e = 0; e < site.edits; e++)
        memcpy(&chunk[site.edit[e].fileoff - ChunkStart(site)], site.edit[e].stock, site.edit[e].length);
    return chunk;
}

std::vector<uint8_t> Page(const Tree &tree, const switch_us_site &site, const std::vector<uint8_t> &src,
                          uint64_t src_offset)
{
    std::vector<uint8_t> dst(kChunk, 0xee);
    EXPECT_EQ(tree.page(src.data(), dst.data(), src_offset, &site), 0);
    return dst;
}

std::string RefusedLine(const switch_us_edit &edit)
{
    char line[160];
    snprintf(line, sizeof line,
             "SwitchUIScale: UIKitCore cache bytes at 0x%llx are not the shipped ones, page left stock\n",
             static_cast<unsigned long long>(edit.fileoff));
    return line;
}

}  // namespace

TEST(XnuUiScalePage, TheFocusSiteHasTwoFixedEditsInOneChunk)
{
    const switch_us_site *site = FocusSite();
    ASSERT_NE(site, nullptr);
    ASSERT_EQ(site->edits, 2u);
    for (unsigned e = 0; e < site->edits; e++) {
        EXPECT_EQ(site->edit[e].kind, SWITCH_US_FIXED);
        EXPECT_GE(site->edit[e].fileoff, ChunkStart(*site));
        EXPECT_LE(site->edit[e].fileoff + site->edit[e].length, ChunkStart(*site) + kChunk);
    }
    EXPECT_LT(site->edit[1].size, site->edit[1].length);
}

TEST(XnuUiScalePage, TheShippedBytesTakeBothEditsAndNothingElse)
{
    const switch_us_site *site = FocusSite();
    ASSERT_NE(site, nullptr);
    for (const Tree &tree : kTrees) {
        SCOPED_TRACE(tree.name);
        xnu_ui_scale_page_reset();
        const std::vector<uint8_t> src = StockChunk(*site);
        std::vector<uint8_t> want = src;
        for (unsigned e = 0; e < site->edits; e++) {
            const switch_us_edit &edit = site->edit[e];
            memcpy(&want[edit.fileoff - ChunkStart(*site) + edit.at], edit.patched, edit.size);
        }
        EXPECT_EQ(Page(tree, *site, src, ChunkStart(*site)), want);
        EXPECT_EQ(std::string(xnu_ui_scale_page_log()),
                  std::string("SwitchUIScale: UIKitCore ") + kFocus + " at 1.25\n");
    }
}

TEST(XnuUiScalePage, ADifferenceAnywhereInEitherCheckedWindowLeavesTheChunkAsRead)
{
    const switch_us_site *site = FocusSite();
    ASSERT_NE(site, nullptr);
    for (const Tree &tree : kTrees) {
        for (unsigned e = 0; e < site->edits; e++) {
            const switch_us_edit &edit = site->edit[e];
            // the first byte, the first one checked but not written (the second edit writes 16 of its 32), the last
            for (unsigned at : {0u, unsigned(edit.at + edit.size), edit.length - 1u}) {
                if (at >= edit.length)
                    continue;
                SCOPED_TRACE(std::string(tree.name) + " edit " + std::to_string(e) + " byte " + std::to_string(at));
                xnu_ui_scale_page_reset();
                std::vector<uint8_t> src = StockChunk(*site);
                src[edit.fileoff - ChunkStart(*site) + at] ^= 0x01;
                EXPECT_EQ(Page(tree, *site, src, ChunkStart(*site)), src);
                EXPECT_EQ(std::string(xnu_ui_scale_page_log()), RefusedLine(edit));
            }
        }
    }
}

TEST(XnuUiScalePage, ARefusedSiteSaysSoOnce)
{
    const switch_us_site *site = FocusSite();
    ASSERT_NE(site, nullptr);
    for (const Tree &tree : kTrees) {
        SCOPED_TRACE(tree.name);
        xnu_ui_scale_page_reset();
        std::vector<uint8_t> src = StockChunk(*site);
        src[site->edit[1].fileoff - ChunkStart(*site)] ^= 0x80;
        for (int page_in = 0; page_in < 3; page_in++)
            EXPECT_EQ(Page(tree, *site, src, ChunkStart(*site)), src);
        EXPECT_EQ(xnu_ui_scale_page_log_lines(), 1u);
    }
}

TEST(XnuUiScalePage, AChunkThatDoesNotHoldTheEditsIsCopiedAsRead)
{
    const switch_us_site *site = FocusSite();
    ASSERT_NE(site, nullptr);
    for (const Tree &tree : kTrees) {
        SCOPED_TRACE(tree.name);
        xnu_ui_scale_page_reset();
        const std::vector<uint8_t> src = StockChunk(*site);
        EXPECT_EQ(Page(tree, *site, src, ChunkStart(*site) + kChunk), src);
        EXPECT_EQ(xnu_ui_scale_page_log_lines(), 0u);
    }
}
