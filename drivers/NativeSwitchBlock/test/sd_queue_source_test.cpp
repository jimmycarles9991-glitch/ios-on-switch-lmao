// Guards on NativeSwitchBlock.cpp's use of read and write chains (sd_queue_test.cpp tests SwitchQueue.h's merging; the
// driver is not built for the host): every chain is checked before it is unlinked, write chains never carry gaps and
// check active in-flight ranges, and a read chain's gaps go to the sink and nowhere else (r12-sdio, r14-sdcq).
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

// Reads and contiguous writes merge across the queue; both are checked before unlinking, and writes never take gaps.
TEST(SdQueueSource, MergeChains)
{
    const std::string &source = driver();
    const std::string pick = body("Request *NativeSwitchBlock::pickLocked(");
    EXPECT_THAT(pick, HasSubstr("if (merge && mergeAnywhere) {"));
    EXPECT_THAT(pick, HasSubstr("if (first->write) {"));
    EXPECT_TRUE(Search(pick,
                       R"(switchGatherWrites\(head, first, limit, kMaxChain, kMergeScan, &start, &sectors,\s*)"
                       R"(inFlight,\s*inFlightCount,\s*writes\(windowOf\(first->shared\)\)\.waiters != nullptr\))"));
    EXPECT_TRUE(Search(pick, R"(switchGatherReads\(head, first, maxSectors, kMaxChain, kMergeScan, &start, )"
                             R"(&sectors,\s*maxGap,\s*gapBudget\))"));
    // r14-sdcq: gaps only on reads while gap reads are on, within the cost model's bounds.
    EXPECT_THAT(pick, HasSubstr("const uint32_t maxGap = gapsOn ? kGapSectors : 0;"));
    EXPECT_THAT(pick, HasSubstr("const uint32_t gapBudget = gapsOn ? kGapBudget : 0;"));
    EXPECT_TRUE(
        Search(source, R"(constexpr uint32_t kGapSectors = 32 \* 1024 / kSector, kGapBudget = 64 \* 1024 / kSector)"));
    // The scan reaches as far as a chain can hold: never fewer than the old rule's queue-adjacent requests.
    EXPECT_THAT(source, HasSubstr("constexpr unsigned kMergeScan = kMaxChain;"));
    // Both chains are checked before unlinking; one that fails stays queued, the boot falls back to adjacent-only
    // merging with one line, and nothing panics.
    EXPECT_LT(Index(pick, "switchWriteChainTiles(head, start, sectors, count)"), Index(pick, "switchUnlinkChain("));
    EXPECT_LT(Index(pick, "switchChainFits(head, start, sectors, count, maxGap, gapBudget)"),
              Index(pick, "switchUnlinkChain("));
    EXPECT_THAT(pick, HasSubstr("switchClearChain(head);"));
    EXPECT_THAT(pick, HasSubstr("mergeAnywhere = false;"));
    EXPECT_THAT(pick, Not(HasSubstr("panic(")));
    // Fallback path keeps adjacent-only merging and checks active in-flight overlap for writes.
    EXPECT_THAT(pick, HasSubstr("last->next->lba == last->lba + last->sectors"));
    EXPECT_THAT(pick, HasSubstr("!overlapsInFlight(last->next)"));
    // Worker passes any pending batch's LBA range to pickLocked so writes check overlap against active work.
    const std::string worker = body("void NativeSwitchBlock::worker(");
    EXPECT_THAT(worker, HasSubstr("SwitchLbaRange{pending->first->lba, pending->sectors}"));
    EXPECT_THAT(worker, HasSubstr("self->pickLocked(&next->sectors, &next->count, &active,"));
    // Every request starts unchained.
    EXPECT_TRUE(
        Search(source, R"(static_cast<uint8_t>\(priority >> 6\),\s*direction == kIODirectionOut,\s*false,\s*false,)"));
}

// r14-sdcq: a read chain's gaps go to the sink and nowhere else; a write chain never has one.
TEST(SdQueueSource, GapChains)
{
    const std::string &source = driver();
    const std::string begin = body("void NativeSwitchBlock::beginBatch(");
    EXPECT_THAT(begin, HasSubstr("if (first->write && payload != sectors)"));
    EXPECT_LT(Index(begin, "outside its partition"), Index(begin, "if (first->write && payload != sectors)"));
    const std::string table = body("SwitchAdmaRefusal NativeSwitchBlock::buildTable(");
    EXPECT_EQ(Count(table, "staging.addSink(sinkPA, gap(r))"), 1u);
    EXPECT_EQ(Count(table, "table.addSealed(sinkPA, gap(r))"), 1u);
    EXPECT_EQ(Count(table, "gap(r) > static_cast<uint64_t>(kGapSectors) * kSector || !sinkPA"), 2u);
    // Toward the requests gaps are skipped; from them (writes) every byte must be covered.
    EXPECT_THAT(body("bool NativeSwitchBlock::stageChain("), HasSubstr("toClient);"));
    // The cache maintenance leaves the sink alone.
    EXPECT_THAT(source, HasSubstr("if (pa - skip < skipBytes)\n            continue;"));
}

TEST(SdQueueSource, PowerCycleChecksBootCardWhenSharedVolumeIsAbsentOrOffline)
{
    const auto startup = body("bool NativeSwitchBlock::setup()");
    EXPECT_LT(Index(startup, "switch_sd_boot_card_cid(cid)"), Index(startup, "findFiles("));
    EXPECT_LT(Index(startup, "memcpy(cid, filesSlice.cid, sizeof(cid))"),
              Index(startup, "__atomic_store_n(&filesOnline, true"));
    const auto init = body("bool NativeSwitchBlock::reinitialize(");
    EXPECT_THAT(init, HasSubstr("readR2(cid);"));
    EXPECT_THAT(init, HasSubstr("else if (haveCid) {"));
    EXPECT_LT(Index(init, "readR2(currentCid)"), Index(init, "memcmp(currentCid, cid, 15)"));
    EXPECT_LT(Index(init, "memcmp(currentCid, cid, 15)"), Index(init, "fail(\"boot card changed\", 0)"));
    EXPECT_THAT(init, Not(HasSubstr("filesPresent()")));
    const auto recovery = body("void NativeSwitchBlock::resume()");
    EXPECT_LT(Index(recovery, "reinitialize("), Index(recovery, "dmaResume();"));
}

TEST(SdQueueSource, SharedPartitionOwnership)
{
    const auto publish = body("extern \"C\" int switch_block_publish(");
    EXPECT_LT(Index(publish, "owner->device = device;"), Index(publish, "device->registerService();"));
    EXPECT_LT(Index(publish, "device->registerService();"), Index(publish, "owner->publishFiles();"));
    EXPECT_THAT(body("void NativeSwitchBlock::publishFiles()"),
                HasSubstr("IOService::serviceMatching(\"IOMediaBSDClient\")"));
    EXPECT_THAT(body("bool ownsDevice("), HasSubstr("gOwner->device"));
    EXPECT_THAT(body("int findVolume("), HasSubstr("entry == gOwner->device"));
    const auto config = body("bool NativeSwitchBlockDevice::configure(");
    EXPECT_THAT(config, HasSubstr("putString(protocol, kIOPropertyPhysicalInterconnectTypeKey, \"SD\")"));
    EXPECT_THAT(config,
                HasSubstr("putString(protocol, kIOPropertyPhysicalInterconnectLocationKey, kIOPropertyInternalKey)"));
    EXPECT_THAT(config, Not(HasSubstr("kIOPropertyExternalKey")));
    EXPECT_THAT(config, HasSubstr("!shared && provider->canUnmap()"));
    EXPECT_THAT(body("IOReturn NativeSwitchBlockDevice::doUnmap("), HasSubstr("shared ? kIOReturnUnsupported"));
    EXPECT_THAT(body("int NativeSwitchBlock::erase("), HasSubstr("!rootWindow.contains(first, last - first + 1)"));
    const auto sleep = body("void NativeSwitchBlock::suspend()");
    EXPECT_LT(Index(sleep, "syncLocked(&rootWindow, false)"), Index(sleep, "powerOffCard();"));
    EXPECT_LT(Index(sleep, "syncLocked(&filesWindow, false)"), Index(sleep, "powerOffCard();"));
    EXPECT_LT(Index(body("void NativeSwitchBlock::resume()"), "verifyFilesCard();"),
              Index(body("void NativeSwitchBlock::resume()"), "suspended = false;"));
}
