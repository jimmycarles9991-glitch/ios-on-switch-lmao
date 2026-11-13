// Guards on NativeSwitchBlock.cpp's use of the ADMA2 tables (sd_adma_test.cpp tests the tables themselves; the
// driver is not built for the host): every card command range-checked against the APFS partition before it is issued,
// ADMA2 never polled nor taken for SDMA, writes over 256 KiB only as far as they are proven, ADMA2 above 4 GiB only as
// far as it is proven, the pipelined worker's ordering, and read staging (r12-sdio, r14-sdcq).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::After;
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
                      complete = body("int NativeSwitchBlock::complete(");
    EXPECT_THAT(issue, HasSubstr("outside the APFS partition"));
    EXPECT_LT(Index(issue, "outside the APFS partition"), Index(issue, "w16(COMMAND"));
    EXPECT_THAT(body("int NativeSwitchBlock::command("), HasSubstr("issue(lba, sectors, write, descriptors, cmd23)"));
    const std::string begin = body("void NativeSwitchBlock::beginBatch(");
    EXPECT_THAT(begin, HasSubstr("outside the APFS partition"));
    EXPECT_LT(Index(begin, "outside the APFS partition"), Index(begin, "issue("));
    // issue() is only called from command() and beginBatch(), and (r14-sdcq) for command-queue reads from beginTask()
    // and checkCq(): those issue reads (write false) with a task argument, never a write.
    const auto calls = LinesMatching(source, R"(^.*[^:\w]issue\(static_cast|^.*[^:\w]issue\(lba)");
    EXPECT_EQ(calls.size(), 4u) << testing::PrintToString(calls);
    // r18-sdcq: beginTask takes the engine's CMD46 argument from the group runner (SwitchCq.h run).
    for (const char *name : {"void NativeSwitchBlock::beginTask(", "const char *NativeSwitchBlock::checkCq("})
        EXPECT_TRUE(Search(
            body(name), R"(issue\(static_cast<uint32_t>\([^)]*\), \w+(?:\.sectors)?, false, [^;]*argument(?:\(\))?\))"))
            << name;
    // In ADMA2 mode the SDMA boundary interrupt is neither awaited nor acted on (resuming would rewrite the descriptor
    // table's address), and an ADMA error is the host's failure.
    EXPECT_THAT(complete, HasSubstr("INT_TRANSFER | (descriptors ? 0 : INT_DMA) | ERROR_SIGNALS"));
    EXPECT_THAT(complete, HasSubstr("if (!descriptors && (s & INT_DMA))"));
    EXPECT_LT(Index(complete, "return kStepAdmaError"), Index(complete, "resume += kSdmaBoundary"));
    // The data path selects SDMA or ADMA2 for every command, and the bring-up's SDMA read selects SDMA.
    EXPECT_THAT(issue, HasSubstr("(r8(HOST_CONTROL) & ~DMA_SELECT) | (descriptors ? DMA_ADMA2 : 0)"));
    EXPECT_THAT(body("int NativeSwitchBlock::cardCommand("),
                HasSubstr("w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT)"));
    // r12-sdio: ADMA2 writes up to 1 MiB, each boot's first ones over 256 KiB read back (proveWrite); one that differs
    // takes writes over 256 KiB back to the bounce path's 256 KiB SDMA commands (never SDMA over 256 KiB).
    EXPECT_THAT(source, HasSubstr("constexpr uint32_t kDirectWriteSectors = kBounceSectors;"));
    EXPECT_THAT(source, HasSubstr("constexpr uint32_t kWriteChecks = 32, kLargeWriteChecks = 16;"));
    const std::string prove = body("bool NativeSwitchBlock::proveWrite(");
    EXPECT_THAT(prove, HasSubstr("if (sectors > kSafeSectors && largeWriteChecks)\n            --largeWriteChecks;"));
    EXPECT_TRUE(Search(prove, R"(if \(sectors > kSafeSectors\) \{[^}]*writeSectors = kSafeSectors;)"));
    EXPECT_THAT(prove, HasSubstr("admaWrites = false;"));
    EXPECT_THAT(begin, HasSubstr("(admaWrites && sectors <= writeSectors)"));
    EXPECT_THAT(body("void NativeSwitchBlock::endBatch("), HasSubstr("(sectors > kSafeSectors && largeWriteChecks)"));
    EXPECT_THAT(body("Request *NativeSwitchBlock::pickLocked("),
                HasSubstr("const uint32_t limit = first->write ? writeSectors : maxSectors;"));
    EXPECT_THAT(body("bool NativeSwitchBlock::bounceChain("),
                HasSubstr("const uint32_t limit = write ? kSafeSectors : sdmaReadSectors;"));
    EXPECT_THAT(source,
                HasSubstr("largeWriteChecks = SwitchAdmaHigh::kLiveProofs && checkBuffer ? kLargeWriteChecks : 0;"));
    EXPECT_THAT(source, HasSubstr("writeSectors = admaWrites ? kDirectWriteSectors : kSafeSectors;"));
    // Cache maintenance is by line over the table's own ranges: no set/way or whole-cache operation.
    EXPECT_FALSE(Search(source, R"(dc\s+(c?isw|csw)|FlushPoC_Dcache\b|CleanPoC_Dcache\b)"));
}

// r12-sdio: ADMA2 above 4 GiB is only used as far as it is proven, and a failed proof never reaches a client.
TEST(SdAdmaSource, HighProofGuards)
{
    const std::string &source = driver();
    const std::string header = SourceText("../SwitchAdma.h");
    // Every table takes its limit and page filter from the proof state, per direction.
    const std::string build = body("SwitchAdmaRefusal NativeSwitchBlock::buildTable(");
    // r14-sdcq: a command-queue task stays below 4 GiB while that is unproven (its proof is a legacy command).
    EXPECT_THAT(build, HasSubstr("const uint64_t limit = task && high.state == SwitchAdmaHigh::kProving ? kAdma4GiB : "
                                 "high.limit(write);"));
    EXPECT_EQ(Count(build, "table.limit = limit;"), 2u);
    EXPECT_THAT(build, HasSubstr("return kind != DmaPage::kRefused && high.pageOk(page, write);"));
    // Writes never go above 4 GiB while that is unproven; the IRAM alias window stays out of proving reads.
    EXPECT_THAT(header, HasSubstr("state == kOn || (state == kProving && !write) ? kAdmaDmaLimit : kAdma4GiB"));
    EXPECT_THAT(header, HasSubstr("state != kProving || write || page - kAdmaIramAlias >= kAdmaIramBytes"));
    // Only reads are poisoned, and a mismatch puts SDMA's bytes into the requests before they complete.
    EXPECT_THAT(body("void NativeSwitchBlock::prepareDirect("), HasSubstr("fill == ReadFill::kPoisonHigh && !write"));
    EXPECT_THAT(After(source, "void NativeSwitchBlock::beginBatch("),
                HasSubstr("prepareDirect(b.write, b.proving ? ReadFill::kPoisonHigh : ReadFill::kNone);"));
    const std::string prove = body("bool NativeSwitchBlock::proveHigh(");
    EXPECT_THAT(prove, HasSubstr("high.refuse();"));
    EXPECT_THAT(prove, HasSubstr("stageChain(first, 0, length, checkBuffer, true)"));
    EXPECT_LT(Index(prove, "high.refuse();"), Index(prove, "stageChain(first, 0, length, checkBuffer, true)"));
    // A failed SDMA reference read proves nothing: the chain goes through the bounce buffer again.
    EXPECT_THAT(prove, HasSubstr("return bounceChain(first, sectors);"));
    EXPECT_THAT(After(source, "void NativeSwitchBlock::beginBatch("), HasSubstr("high.proofDue(b.write, table.high)"));
    const std::string batch = body("void NativeSwitchBlock::endBatch(");
    EXPECT_THAT(batch, HasSubstr("if (!proveHigh(first, sectors))\n                result = kIOReturnIOError;"));
    // A write from pages above 4 GiB is read back while those checks last.
    EXPECT_THAT(batch, HasSubstr("high.writeCheckDue(write, table.high)"));
}

// r12-sdio: the next chain's command is issued before the previous chain completes; waits stay on the interrupt.
TEST(SdAdmaSource, PipelinedWorker)
{
    const std::string worker = body("void NativeSwitchBlock::worker(");
    EXPECT_LT(Index(worker, "beginBatch(*next)"), Index(worker, "completeBatch(*pending)"));
    EXPECT_LT(Index(worker, "completeBatch(*pending)"), Index(worker, "endBatch(*next)"));
    // suspend() waits for `working`: it is set before the command is begun and cleared only after endBatch (r14-sdcq: and
    // around leaving CQ mode when the queue runs dry, before the pick).
    EXPECT_LT(Index(worker, "working = true", Index(worker, "pickLocked(")), Index(worker, "beginBatch(*next)"));
    EXPECT_LT(Index(worker, "endBatch(*next)"), RIndex(worker, "working = false"));
    EXPECT_LT(Index(worker, "runGroup("), RIndex(worker, "working = false"));
    // beginBatch never waits for the card: it only issues (no complete(), command(), run() or transfer() there).
    EXPECT_FALSE(Search(body("void NativeSwitchBlock::beginBatch("),
                        R"(\b(complete|command|run|transfer|bounceChain|waitSignal)\()"));
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
                               "void NativeSwitchBlock::tuneStep()"})
        EXPECT_THAT(body(waiter), HasSubstr("awaitStatus(")) << waiter;
    // endBatch: a split command's errors take run()'s retry, and a read's pages are invalidated after the command.
    const std::string end = body("void NativeSwitchBlock::endBatch(");
    EXPECT_THAT(end, HasSubstr("retry(step, status, first->lba, sectors, write, table.count)"));
    EXPECT_LT(Index(end, "retry("), Index(end, "finishDirect(write)"));
    EXPECT_LT(Index(end, "finishDirect(write)"), Index(end, "proveHigh("));
}

// The data path does the least maintenance that is correct: writes clean, reads invalidate before and after (every line
// is whole and the card overwrites it, so a dirty line is dropped, never written back). Only a read whose pages hold a
// fill to keep (the setup checks, the poison of a proof above 4 GiB) cleans as well. A non-cacheable page has no line to
// maintain (XNU's dcache_incoherent_io_store64 skips it) and goes straight to the card.
TEST(SdAdmaSource, MinimalMaintenance)
{
    const std::string &source = driver();
    const std::string prepare = body("void NativeSwitchBlock::prepareDirect(");
    EXPECT_THAT(prepare, HasSubstr("syncLines(SWITCH_DCACHE_CLEAN, table, tableUncached);"));
    EXPECT_THAT(prepare,
                HasSubstr("fill == ReadFill::kNone ? SWITCH_DCACHE_INVALIDATE : SWITCH_DCACHE_CLEAN_INVALIDATE"));
    EXPECT_THAT(body("void NativeSwitchBlock::finishDirect("), HasSubstr("syncLines(SWITCH_DCACHE_INVALIDATE, table,"));
    EXPECT_THAT(body("void NativeSwitchBlock::beginTask("), HasSubstr("prepareDirect(false, ReadFill::kNone);"));
    EXPECT_EQ(Count(source, "prepareDirect(false, ReadFill::kCpuFill);"), 4u);
    EXPECT_EQ(Count(source, "prepareDirect("), 8u);  // declaration, definition, beginBatch, beginTask, four checks
    const std::string page = body("static DmaPage dmaPage(");
    EXPECT_THAT(page, HasSubstr("if (attributes == kWimgDefault)\n        return DmaPage::kCacheable;"));
    EXPECT_THAT(page, HasSubstr("(attributes & kWimgNotCacheable) != 0 ? DmaPage::kUncached : DmaPage::kRefused"));
    const std::string sync = body("static void syncLines(");
    EXPECT_THAT(sync, HasSubstr("if (dmaPage(pa) == DmaPage::kCacheable)"));
    EXPECT_THAT(body("SwitchAdmaRefusal NativeSwitchBlock::buildTable("), HasSubstr("tableUncached = false;"));
}

// r12-sdio: a read's refused segments alone go through the bounce buffer, once setup() has run that path.
TEST(SdAdmaSource, ReadStaging)
{
    // Reads only, and only once the self-check has passed; writes keep the whole-chain bounce.
    EXPECT_THAT(body("SwitchAdmaRefusal NativeSwitchBlock::buildTable("), HasSubstr("if (!write && stagingOn) {"));
    // The staged segments are copied out after the read's invalidate and before any proof reuses the buffer.
    const std::string end = body("void NativeSwitchBlock::endBatch(");
    EXPECT_LT(Index(end, "finishDirect(write)"), Index(end, "copyStaged()"));
    EXPECT_LT(Index(end, "copyStaged()"), Index(end, "proveHigh("));
    // proveHigh compares the requests' own pages only, never the bounce buffer's runs.
    EXPECT_THAT(body("bool NativeSwitchBlock::proveHigh("),
                HasSubstr("pa >= bouncePA && pa - bouncePA < kBounceBytes"));
    // setup()'s check runs the staging path on the hardware; a failure only keeps it off (no panic in it).
    const std::string check = body("const char *NativeSwitchBlock::checkAdma(");
    EXPECT_THAT(check, HasSubstr("stagingOn = (stagingWhy == nullptr);"));
    EXPECT_LT(Index(check, "copyStaged()"), Index(check, "memcmp(pageBytes, reference, kTestBytes)"));
    EXPECT_THAT(check, Not(HasSubstr("panic(")));
}
