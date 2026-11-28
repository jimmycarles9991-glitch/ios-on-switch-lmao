// Guards on NativeSwitchBlock.cpp's use of the ADMA2 tables (sd_adma_test.cpp tests the tables themselves; the
// driver is not built for the host): every card command range-checked against its originating partition before it is
// issued, ADMA2 never polled nor taken for SDMA, SDMA commands never past 256 KiB, the pipelined worker's ordering,
// and read staging (r12-sdio, r14-sdcq).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::Index;
using switch_ios::test::LinesMatching;
using switch_ios::test::RIndex;
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

// The driver keeps its partition guards, never polls ADMA2 and never treats its DMA interrupt as SDMA's.
TEST(SdAdmaSource, DriverGuards)
{
    const std::string &source = driver();
    // Every card write is range-checked where every data command is issued (issue(), which command() and the worker's
    // ADMA2 commands both go through), besides beginBatch's check of the whole chain.
    const std::string issue = body("int NativeSwitchBlock::issue("),
                      emit = body("void NativeSwitchBlock::emitSlotCommand("),
                      complete = body("int NativeSwitchBlock::complete(");
    EXPECT_THAT(issue, HasSubstr("outside its partition"));
    EXPECT_LT(Index(issue, "outside its partition"), Index(issue, "w16(COMMAND"));
    EXPECT_THAT(emit, HasSubstr("outside its partition"));
    EXPECT_LT(Index(emit, "outside its partition"), Index(emit, "w16(COMMAND"));
    EXPECT_THAT(body("int NativeSwitchBlock::command("),
                HasSubstr("issue(lba, sectors, write, descriptors, cmd23, window, descPA)"));
    const std::string prep = body("void NativeSwitchBlock::prepareAndPublishSlot(");
    EXPECT_THAT(prep, HasSubstr("outside its partition"));
    EXPECT_LT(Index(prep, "outside its partition"), Index(prep, "prepareDirect("));
    // issue() is only called from command(); emitSlotCommand() is only called from interruptHandler and launchSlot.
    const auto calls = LinesMatching(source, R"(^.*[^:\w]issue\(static_cast|^.*[^:\w]issue\(lba)");
    EXPECT_EQ(calls.size(), 1u) << testing::PrintToString(calls);
    EXPECT_THAT(body("void NativeSwitchBlock::interruptHandler("), HasSubstr("emitSlotCommand("));
    EXPECT_THAT(body("void NativeSwitchBlock::launchSlot("), HasSubstr("emitSlotCommand("));
    // In ADMA2 mode the SDMA boundary interrupt is neither awaited nor acted on (resuming would rewrite the descriptor
    // table's address), and an ADMA error is the host's failure.
    EXPECT_THAT(complete, HasSubstr("INT_TRANSFER | (descriptors ? 0 : INT_DMA) | ERROR_SIGNALS"));
    EXPECT_THAT(complete, HasSubstr("if (!descriptors && (s & INT_DMA))"));
    EXPECT_LT(Index(complete, "return kStepAdmaError"), Index(complete, "resume += kSdmaBoundary"));
    // The data path selects SDMA or ADMA2 for every command, and the bring-up's SDMA read selects SDMA.
    EXPECT_THAT(issue, HasSubstr("(r8(HOST_CONTROL) & ~DMA_SELECT) | (descriptors ? DMA_ADMA2 : 0)"));
    EXPECT_THAT(body("int NativeSwitchBlock::cardCommand("),
                HasSubstr("w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT)"));
    // ADMA2 commands up to 1 MiB both ways; the bounce path's SDMA commands stay at 256 KiB (never past a boundary).
    EXPECT_THAT(source, HasSubstr("constexpr uint32_t kDirectWriteSectors = kBounceSectors;"));
    EXPECT_THAT(body("Request *NativeSwitchBlock::pickLocked("),
                HasSubstr("const uint32_t limit = first->write ? maxWriteSectors : maxSectors;"));
    EXPECT_THAT(body("bool NativeSwitchBlock::bounceChain("),
                HasSubstr("sectors - done < kSafeSectors ? sectors - done : kSafeSectors"));
    EXPECT_THAT(body("SwitchAdmaRefusal NativeSwitchBlock::buildTable("), HasSubstr("table.limit = kAdmaDmaLimit;"));
    // Cache maintenance is by line over the table's own ranges: no set/way or whole-cache operation.
    EXPECT_FALSE(Search(source, R"(dc\s+(c?isw|csw)|FlushPoC_Dcache\b|CleanPoC_Dcache\b)"));
}

// Two-slot prepared ADMA2 pipeline + primary-IRQ fast-issue; waits stay on the interrupt.
TEST(SdAdmaSource, PipelinedWorker)
{
    const std::string worker = body("void NativeSwitchBlock::worker(");
    EXPECT_LT(Index(worker, "pipe.takeDone()"), Index(worker, "finishSlowActive("));
    EXPECT_LT(Index(worker, "finishSlowActive("), Index(worker, "if (self->pipe.stashed)"));
    EXPECT_LT(Index(worker, "pickLocked("), Index(worker, "prepareAndPublishSlot("));
    // prepareAndPublishSlot checks PA/LBA overlap BEFORE prepareDirect and never waits for the card.
    const std::string prep = body("void NativeSwitchBlock::prepareAndPublishSlot(");
    EXPECT_LT(Index(prep, "switchAdmaTablesOverlap("), Index(prep, "prepareDirect("));
    EXPECT_LT(Index(prep, "canOverlap("), Index(prep, "prepareDirect("));
    EXPECT_FALSE(Search(prep, R"(\b(complete|command|run|transfer|bounceChain|waitSignal)\()"));
    // interruptHandler acquires hwLock, runs pipe.onIrq, unlocks hwLock BEFORE thread_wakeup.
    const std::string isr = body("void NativeSwitchBlock::interruptHandler(");
    EXPECT_LT(Index(isr, "IOSimpleLockLock(self->hwLock)"), Index(isr, "pipe.onIrq("));
    EXPECT_LT(Index(isr, "IOSimpleLockUnlock(self->hwLock)"), Index(isr, "thread_wakeup(&self->workerEvent)"));
    // waitForPipeline asserts wait under hwLock, unlocks before thread_block, and re-checks active before interruptLost.
    const std::string wait = body("void NativeSwitchBlock::waitForPipeline(");
    const size_t blockAt = Index(wait, "thread_block(THREAD_CONTINUE_NULL)");
    const size_t assertAt = Index(wait, "assert_wait_deadline(&workerEvent");
    EXPECT_LT(assertAt, blockAt);
    EXPECT_LT(assertAt, RIndex(wait.substr(0, blockAt), "IOSimpleLockUnlockEnableInterrupt("));
    EXPECT_LT(Index(wait, "pipe.active == expected && !expected->irqDone"), Index(wait, "interruptLost(awaited)"));
    // complete(): STATUS first (a command that finished meanwhile takes no interrupt), then the interrupt; its time limit
    // starts there; a lost interrupt panics, and there is no polled mode.
    const std::string complete = body("int NativeSwitchBlock::complete(");
    EXPECT_THAT(complete, HasSubstr("const uint64_t start = switch_now_ns();"));
    EXPECT_LT(Index(complete, "s = r32(STATUS);"), Index(complete, "waitSignal("));
    EXPECT_THAT(complete, HasSubstr("interruptLost(awaited)"));
    EXPECT_THAT(complete, Not(HasSubstr("IOSleep")));
    EXPECT_THAT(body("void NativeSwitchBlock::interruptLost("), HasSubstr("panic(\"SwitchSD: SDMMC1 interrupt lost"));
    // Card commands, the recovery's CMD12 and each tuning block sleep on the interrupt too.
    for (const char *waiter : {"int NativeSwitchBlock::cardCommand(", "void NativeSwitchBlock::recover(",
                               "void NativeSwitchBlock::tuneStep("})
        EXPECT_THAT(body(waiter), HasSubstr("awaitStatus(")) << waiter;
    // finishSlowActive retries with the slot's window before completeSlot invalidates and copies staged segments.
    const std::string slow = body("void NativeSwitchBlock::finishSlowActive(");
    EXPECT_THAT(slow, HasSubstr("retry(step, status, slot.lba, slot.sectors, slot.write, slot.table.count, "
                                "slot.window, slot.tablePA)"));
    EXPECT_LT(Index(slow, "retry("), Index(slow, "completeSlot("));
    const std::string done = body("void NativeSwitchBlock::completeSlot(");
    EXPECT_LT(Index(done, "finishDirect("), Index(done, "copyStaged("));
}

// The data path does the least maintenance that is correct: writes clean, reads invalidate before and after (every line
// is whole and the card overwrites it, so a dirty line is dropped, never written back). A non-cacheable page has no
// line to maintain (XNU's dcache_incoherent_io_store64 skips it) and goes straight to the card.
TEST(SdAdmaSource, MinimalMaintenance)
{
    const std::string &source = driver();
    const std::string prepare = body("void NativeSwitchBlock::prepareDirect(");
    EXPECT_THAT(prepare,
                HasSubstr("syncLines(SWITCH_DCACHE_CLEAN, slot.table, slot.tableUncached, slot.descUncached);"));
    EXPECT_THAT(
        prepare,
        HasSubstr("syncLines(SWITCH_DCACHE_INVALIDATE, slot.table, slot.tableUncached, slot.descUncached, sinkPA,"));
    EXPECT_THAT(body("void NativeSwitchBlock::finishDirect("),
                HasSubstr("syncLines(SWITCH_DCACHE_INVALIDATE, slot.table, slot.tableUncached, slot.descUncached,"));
    EXPECT_EQ(Count(source, "prepareDirect("), 4u);  // declaration, definition, prepareAndPublishSlot, worker stashed
    const std::string page = body("static DmaPage dmaPage(");
    EXPECT_THAT(page, HasSubstr("if (attributes == kWimgDefault)\n        return DmaPage::kCacheable;"));
    EXPECT_THAT(page, HasSubstr("(attributes & kWimgNotCacheable) != 0 ? DmaPage::kUncached : DmaPage::kRefused"));
    const std::string sync = body("static void syncLines(");
    EXPECT_THAT(sync, HasSubstr("uncachedBits[i >> 6]"));
    EXPECT_THAT(sync, Not(HasSubstr("dmaPage(")));
    EXPECT_THAT(body("SwitchAdmaRefusal NativeSwitchBlock::buildTable("), HasSubstr("slot.tableUncached = false;"));
}

// r12-sdio: a read's refused segments alone go through the bounce buffer.
TEST(SdAdmaSource, ReadStaging)
{
    // Reads only, when the stages have memory; writes keep the whole-chain bounce.
    EXPECT_THAT(body("SwitchAdmaRefusal NativeSwitchBlock::buildTable("), HasSubstr("if (!write && slot.stages) {"));
    // The staged segments are copied out after the read's invalidate.
    const std::string done = body("void NativeSwitchBlock::completeSlot(");
    EXPECT_LT(Index(done, "finishDirect("), Index(done, "copyStaged("));
}
