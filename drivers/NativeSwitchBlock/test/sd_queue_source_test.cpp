// Guards on NativeSwitchBlock.cpp's use of the read chains (sd_queue_test.cpp tests SwitchQueue.h's merging; the
// driver is not built for the host): reads only, the chain checked before it is unlinked, and a read chain's gaps go to
// the sink and nowhere else (r12-sdio, r14-sdcq).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::Index;
using switch_ios::test::Search;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
const std::string &driver()
{
    static const std::string source = SourceText("../NativeSwitchBlock.cpp");
    return source;
}

std::string body(const char *signature)
{
    return Body(driver(), signature);
}
}  // namespace

// Writes keep their FIFO rule; the read chain is checked to tile its command before it goes anywhere.
TEST(SdQueueSource, ReadsOnly)
{
    const std::string &source = driver();
    const std::string pick = body("Request *NativeSwitchBlock::pickLocked(");
    EXPECT_THAT(pick, HasSubstr("if (merge && mergeAnywhere && !first->write) {"));
    EXPECT_THAT(pick, HasSubstr("switchGatherReads(head, first, maxSectors, kMaxChain, kMergeScan, &start, &sectors, "
                                "maxGap, gapBudget)"));
    // r14-sdcq: gaps only while gap reads are on, within the cost model's bounds.
    EXPECT_THAT(pick, HasSubstr("const uint32_t maxGap = gapsOn ? kGapSectors : 0;"));
    EXPECT_THAT(pick, HasSubstr("const uint32_t gapBudget = gapsOn ? kGapBudget : 0;"));
    EXPECT_TRUE(
        Search(source, R"(constexpr uint32_t kGapSectors = 32 \* 1024 / kSector, kGapBudget = 64 \* 1024 / kSector)"));
    // The scan reaches as far as a chain can hold: never fewer than the old rule's queue-adjacent reads.
    EXPECT_THAT(source, HasSubstr("constexpr unsigned kMergeScan = kMaxChain;"));
    // The chain is checked before it is unlinked; one that fails stays queued, the boot falls back to the old rule
    // with one line, and nothing panics.
    EXPECT_LT(Index(pick, "switchChainFits(head, start, sectors, count, maxGap, gapBudget)"),
              Index(pick, "switchUnlinkChain("));
    EXPECT_THAT(pick, HasSubstr("switchClearChain(head);"));
    EXPECT_THAT(pick, HasSubstr("mergeAnywhere = false;"));
    EXPECT_THAT(pick, Not(HasSubstr("panic(")));
    // The write path: only the requests right behind, in queue order.
    EXPECT_THAT(pick, HasSubstr("last->next->lba == last->lba + last->sectors"));
    // Every request starts unchained.
    EXPECT_TRUE(Search(source, R"(static_cast<uint8_t>\(priority >> 6\),\s*0,\s*false\};)"));
}

// r14-sdcq: a read chain's gaps go to the sink and nowhere else; a write chain never has one; the first chains with
// gaps are compared with SDMA before they complete, and a difference turns gap reads off.
TEST(SdQueueSource, GapChains)
{
    const std::string &source = driver();
    const std::string begin = body("void NativeSwitchBlock::beginBatch(");
    EXPECT_THAT(begin, HasSubstr("if (first->write && payload != sectors)"));
    EXPECT_LT(Index(begin, "outside the APFS partition"), Index(begin, "if (first->write && payload != sectors)"));
    const std::string table = body("SwitchAdmaRefusal NativeSwitchBlock::buildTable(");
    EXPECT_EQ(Count(table, "staging.addSink(sinkPA, gap(r))"), 1u);
    EXPECT_EQ(Count(table, "table.addSealed(sinkPA, gap(r))"), 1u);
    EXPECT_EQ(Count(table, "gap(r) > static_cast<uint64_t>(kGapSectors) * kSector || !sinkPA"), 2u);
    EXPECT_THAT(body("void NativeSwitchBlock::endBatch("), HasSubstr("} else if (b.gapSectors && gapProofs) {"));
    const std::string prove = body("bool NativeSwitchBlock::proveGaps(");
    EXPECT_THAT(prove, HasSubstr("gapsOn = false;"));
    EXPECT_THAT(prove, HasSubstr("compareChain(first, sectors, &at)"));
    // compareChain: the requests get the SDMA read's bytes when theirs differ.
    EXPECT_THAT(body("NativeSwitchBlock::ChainProof NativeSwitchBlock::compareChain("),
                HasSubstr("stageChain(first, 0, sectors * kSector, checkBuffer, true)"));
    EXPECT_THAT(body("void NativeSwitchBlock::releaseChecks()"), HasSubstr("(gapsOn && gapProofs)"));
    // Toward the requests gaps are skipped; from them (writes, read-back copies) every byte must be covered.
    EXPECT_THAT(body("bool NativeSwitchBlock::stageChain("), HasSubstr("toClient);"));
    // The cache maintenance and the high-memory proof leave the sink alone.
    EXPECT_THAT(source, HasSubstr("if (pa - skip < skipBytes)\n            continue;"));
    EXPECT_THAT(body("bool NativeSwitchBlock::proveHigh("), HasSubstr("|| inSink(pa)) {"));
}
