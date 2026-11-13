// Guards on what NativeSwitchBlock.cpp's command-queue path may and may not do, and on the engine's (SwitchCq.h)
// cache steps the model card cannot tell apart (sd_cq_test.cpp runs the engine against the model; the driver is not
// built for the host). r14-sdcq, r16-sdcq, r18-sdcq.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <iterator>
#include <regex>
#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::Between;
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

const std::string &engine()
{
    static const std::string source = SourceText("../SwitchCq.h");
    return source;
}

// One of the engine's member functions (indented four spaces in its class).
std::string member(const char *signature)
{
    return Between(engine(), signature, "\n    }\n");
}
}  // namespace

// What the driver's command-queue path may and may not do. (The values CqBus::control lets through,
// switchSdPerfWriteAddress's table in SwitchSdCard.h, are tested value by value in sd_card_test's
// ExtensionRegisters, and the model card refuses any other; the queue's enable value 03h likewise.)
TEST(SdCqSource, DriverGuards)
{
    const std::string &source = driver();
    // The only extension-register writes: r16-sdcq: Cache Enable 00h/01h, Flush Cache 01h and the CQ control byte
    // 00h/03h, through CqBus::control, which refuses (panics on) anything else before the command is issued.
    const std::string control = body("int NativeSwitchBlock::CqBus::control(");
    EXPECT_THAT(control, HasSubstr("cqAt != switchSdCqControlAddress(self->extInfo.perf)"));
    EXPECT_THAT(control, HasSubstr("write ? switchSdPerfWriteAddress(self->extInfo.perf, byte, *value)"));
    EXPECT_THAT(control, HasSubstr("address != cqAt - (kSdPerfCqControl - byte)"));
    EXPECT_LT(Index(control, "panic("), Index(control, "cqCommand(write ? 49 : 48"));
    EXPECT_THAT(control, HasSubstr("switchSdExtArgument(self->extInfo.perf.fno, address, 1)"));
    const std::regex cmd49(R"(cqCommand\(write \? 49|cqCommand\(49|cardCommand\(49)");
    EXPECT_EQ(std::distance(std::sregex_iterator(source.begin(), source.end(), cmd49), std::sregex_iterator()), 1);
    // Interrupts, never polling: cqCommand waits by waitSignal (a lost interrupt panics); the only timed waits in the
    // queue's code are the engine's settle() between read-backs of a card slow to show a byte and (r18-sdcq) between
    // probes of a queue a card has not opened yet.
    const std::string command = body("int NativeSwitchBlock::cqCommand(");
    EXPECT_THAT(command, HasSubstr("awaitStatus(done, limit)"));
    EXPECT_THAT(body("uint32_t NativeSwitchBlock::awaitStatus("), HasSubstr("interruptLost(signals)"));
    EXPECT_THAT(command, Not(HasSubstr("IOSleep")));
    EXPECT_THAT(command, Not(HasSubstr("IODelay")));
    // r16-sdcq: a CMD49's busy (the byte applied, a flush done) ends in transfer complete; DAT0 confirms it before the
    // read-back (a card still holding it is released by interrupt, never polled); the limit covers the spec's 1 s of busy
    // or data access. r18-sdcq: and an R1b's (CMD43: no task assigned before it completes); a failed command's response
    // kept, read before any CMD13 could replace it.
    EXPECT_THAT(command, HasSubstr("use = SwitchSdUse::kWrite;"));
    EXPECT_THAT(command, HasSubstr("use = SwitchSdUse::kBusy;"));
    EXPECT_THAT(command, HasSubstr("if (!step && !bus.idle() && awaitRelease())\n        step = 10;"));
    EXPECT_LT(Index(command, "const uint32_t answer = r32(RESPONSE);"), Index(command, "awaitRelease()"));
    // r18-sdcq (r8d): not after a command error, where RESPONSE is still the previous command's.
    EXPECT_THAT(command, HasSubstr("if ((s & INT_COMMAND) && !(s & 0x000f0000u))\n            *response = answer;"));
    // An erase-based trim passes the longer busy its SD Status specifies; every other command keeps the default.
    EXPECT_THAT(command, HasSubstr("limit = limitNs"));
    EXPECT_THAT(source, HasSubstr("int data,\n                  uint64_t limitNs = 1500000000ULL);"));
    EXPECT_EQ(Count(source, "kRegNone, limitNs)"), 1u);
    EXPECT_THAT(body("Batch *NativeSwitchBlock::runGroup("), Not(HasSubstr("13, ")));  // no CMD13 task-status polling
    EXPECT_EQ(Count(engine(), "bus.settle()"), 2u);
    EXPECT_EQ(Count(member("bool readBack(Bus &bus,"), "bus.settle()"), 1u);
    // r18-sdcq: CMD13 only in the recovery, one per recovered error (never polled): through the queue's interrupt-
    // waited command path, card status first (the B-type bits of a command the card did not answer), then CMD43.
    EXPECT_EQ(Count(source, "cqCommand(13,"), 1u);
    EXPECT_THAT(body("int NativeSwitchBlock::CqBus::status("),
                HasSubstr("cqCommand(13, self->rca << 16 | (tasks ? 1u << 15 : 0u), RSP_R1, r1, kRegNone)"));
    EXPECT_EQ(Count(engine(), "bus.status("), 2u);
    const std::string recover = member("Recovery recover(Bus &bus)");
    EXPECT_LT(Index(recover, "bus.status(false, &error.cardStatus)"), Index(recover, "bus.status(true,"));
    EXPECT_LT(Index(recover, "bus.status(true,"), Index(recover, "bus.command(43, switchSdCmd43(kSdAbortQueue"));
    EXPECT_THAT(recover, Not(HasSubstr("fail(")));
    EXPECT_THAT(recover, Not(HasSubstr("useCache = false")));
    // r16-sdcq: every control byte read back lazily (bounded; kReadBacks is checked in sd_cq_test), the enable's value
    // kept for the line.
    const std::string enter = member("bool enter(Bus &bus)");
    EXPECT_THAT(enter, HasSubstr("readBack(bus, kSdPerfCqControl, 3, kEnabled)"));
    EXPECT_THAT(enter, HasSubstr("cqRead = lastRead[2];"));
    // The cache only inside a CQ session: on right before 03h, off right after 00h, off with the queue for good.
    EXPECT_LT(Index(enter, "enableCache(bus"), Index(enter, "bus.control(kSdPerfCqControl, true"));
    // r18-sdcq: CMD43 (abort the entire queue) once 03h reads back, before any CMD44; no failure here stops the queue.
    EXPECT_LT(Index(enter, "readBack(bus, kSdPerfCqControl, 3, kEnabled)"),
              Index(enter, "bus.command(43, switchSdCmd43(kSdAbortQueue, 0), true, &r1)"));
    EXPECT_THAT(enter, Not(HasSubstr("fail(")));
    // r18-sdcq (r8d): every group's assignment starts with CMD43 unless the queue was just emptied (a card may keep a
    // finished group's Task IDs taken until the host aborts the queue); a group whose retry went through is no strike.
    const std::string submit = member("bool submit(Bus &bus, const SwitchCqTask *t, unsigned n)");
    EXPECT_LT(Index(submit, "if (!emptied) {"),
              Index(submit, "bus.command(43, switchSdCmd43(kSdAbortQueue, 0), true, &r1)"));
    EXPECT_LT(Index(submit, "bus.command(43, switchSdCmd43(kSdAbortQueue, 0), true, &r1)"),
              Index(submit, "bus.command(44,"));
    EXPECT_THAT(enter, HasSubstr("emptied = true;"));
    EXPECT_THAT(Between(engine(), "Outcome run(Bus &bus, Host &host,", "\n    }\n\n"),
                HasSubstr("ended(outcome == kGroupLegacy);"));
    EXPECT_THAT(member("bool leave(Bus &bus)"), HasSubstr("return quit(bus, \"the card stays in CQ mode\");"));
    const std::string out = member("bool quit(Bus &bus, const char *reason)");
    EXPECT_LT(Index(out, "if (!off(bus))"),
              Index(out, "if (useCache && !disableCache(bus))\n            useCache = false;"));
    const std::string fail = member("bool fail(Bus &bus, const char *reason, bool maybeOn = false)");
    EXPECT_THAT(fail, HasSubstr("if (out)\n            (void)disableCache(bus);"));
    EXPECT_THAT(fail, HasSubstr("useCache = false;"));
    // Off is proven only by a CMD49 the card answered; no flush on the way (nothing completed is held).
    const std::string disable = member("bool disableCache(Bus &bus)");
    EXPECT_THAT(disable,
                HasSubstr("if (bus.control(kSdPerfCacheControl, true, &value, &r1))\n                return false;"));
    EXPECT_THAT(disable, Not(HasSubstr("flush(")));
    EXPECT_THAT(engine(),
                HasSubstr("bool commitWrite(Bus &bus)\n    {\n        return !cacheOn || flush(bus);\n    }"));
    EXPECT_THAT(member("void powerCycled()"), HasSubstr("cacheOn = false;"));  // Cache Enable is 0 after power-up
    // Reads only: a task write panics before anything is issued.
    const std::string issue = body("int NativeSwitchBlock::issue(");
    EXPECT_THAT(issue, HasSubstr("if (write && task != kNoTask)\n        panic("));
    EXPECT_LT(Index(issue, "if (write && task != kNoTask)\n        panic("), Index(issue, "w16(COMMAND"));
    const std::string group = body("unsigned NativeSwitchBlock::groupLocked(");
    EXPECT_THAT(group, HasSubstr("first->write || sectors > kCqTaskSectors"));
    EXPECT_THAT(group, HasSubstr("switchNextIndependentRead(head, kCqTaskSectors, kMergeScan)"));
    EXPECT_THAT(group, HasSubstr("if (payload != sectors && gapProofs)\n        return 0;"));
    // r18-sdcq: every chain unlinked for a group runs (r8c: with cqMinChains above 2 the chains after the first were
    // dropped, their requests never completed); a pause gathers nothing.
    EXPECT_THAT(group, HasSubstr("return *queue || n > 1 ? n : 0;"));
    EXPECT_LT(Index(group, "if (!cq.admit())\n        return 0;"), Index(group, "switchUnlinkChain(head, tail"));
    // Every task's blocks inside the APFS partition, checked before the group is assigned.
    const std::string run = body("Batch *NativeSwitchBlock::runGroup(");
    EXPECT_LT(Index(run, "outside the APFS partition"), Index(run, "cq.run(cqBus, host, tasks, n, proving, &proven)"));
    EXPECT_LT(Index(run, "CqEngine::runLegacy(host, n);"), Index(run, "cq.run(cqBus"));
    EXPECT_THAT(run, HasSubstr("return &group[n - 1];"));
    EXPECT_THAT(body("void NativeSwitchBlock::worker("), HasSubstr("self->runGroup(group, tasks, pending, queue)"));
    // The first tasks of every boot are held and compared with legacy reads (out of CQ mode) before completing.
    EXPECT_THAT(run, HasSubstr("const bool proving = SwitchAdmaHigh::kLiveProofs && cqProofs != 0;"));
    EXPECT_THAT(run, HasSubstr("self->compareChain(group[i].first, group[i].sectors, &at)"));
    const std::string runner = Between(engine(), "Outcome run(Bus &bus, Host &host,", "\n    }\n\n");
    EXPECT_LT(Index(runner, "if (!leave(bus))"), Index(runner, "host.prove(i)"));
    EXPECT_THAT(runner, HasSubstr("if (!proving)\n                    owed = static_cast<int>(done);"));
    // A proof that differs stops the queue and the whole group is read again the legacy way (from task 0); so does
    // a group whose retry failed while proving.
    EXPECT_TRUE(Search(runner, R"(if \(differs\) \{\s*\(void\)fail\(bus,)"));
    EXPECT_THAT(runner, HasSubstr("for (unsigned i = proving ? 0 : done; i < n; ++i) {"));
    EXPECT_THAT(body("bool NativeSwitchBlock::setup()"),
                HasSubstr("cqProofs = SwitchAdmaHigh::kLiveProofs ? kCqProofs : 0;"));
    // An error: recovered, the group once more, then the legacy way; the stop is fail() for a proof that differs.
    EXPECT_EQ(Count(runner, "fail("), 1u);
    EXPECT_LT(Index(runner, "const Recovery r = recover(bus);"), Index(runner, "host.beginLegacy(i);"));
    // A task's failure is the engine's recovered error, not failed()'s IOLog (but for the host's own ADMA2 error).
    const std::string task = body("int NativeSwitchBlock::endTask(");
    EXPECT_LT(Index(task, "if (step == kStepAdmaError) {"), Index(task, "failed(step"));
    EXPECT_THAT(task, HasSubstr("recover(true);"));
    // One os_log line per recovered error, rate-limited by count; one IOLog line only for a stop for the boot.
    const std::string recovered = body("void NativeSwitchBlock::cqRecovered(");
    EXPECT_THAT(recovered, HasSubstr("if (cq.recoveries > kLoggedErrors && cq.recoveries % 64)\n        return;"));
    EXPECT_THAT(recovered, HasSubstr("os_log(OS_LOG_DEFAULT,"));
    EXPECT_THAT(recovered, Not(HasSubstr("IOLog(")));
    const std::string stopped = body("void NativeSwitchBlock::cqStopped()");
    EXPECT_LT(Index(stopped, "if (cqStopLogged)\n        return;"), Index(stopped, "IOLog("));
    // A legacy command never finds the card in CQ mode: the worker leaves it first, and when its queue runs dry.
    const std::string worker = body("void NativeSwitchBlock::worker(");
    EXPECT_LT(Index(worker, "if (next)\n                self->cqLeave();"),
              Index(worker, "if (next)\n                self->beginBatch(*next);"));
    EXPECT_THAT(worker, HasSubstr("self->cq.state == CqEngine::kOn) {"));
    EXPECT_THAT(body("void NativeSwitchBlock::suspend()"),
                HasSubstr("if (cq.state == CqEngine::kOn)\n        (void)cq.leave(cqBus);"));
    EXPECT_THAT(body("void NativeSwitchBlock::resume()"), HasSubstr("cq.powerCycled();"));
    // A card that will not leave CQ mode is power-cycled the way system sleep does it, never polled forever; r18-sdcq:
    // the queue on again after it.
    EXPECT_THAT(body("void NativeSwitchBlock::cqPowerCycle()"), HasSubstr("resetCard();"));
    EXPECT_THAT(body("void NativeSwitchBlock::cqLeave()"), HasSubstr("cqPowerCycle();"));
    const std::string reset = body("void NativeSwitchBlock::resetCard()");
    for (const char *step : {"powerOffCard();", "powerOnController();", "reinitialize(", "memcmp(again, gptHeader"})
        EXPECT_THAT(reset, HasSubstr(step));
    // Setup: only a card that offers a queue with its control byte off, and only after its test group matched.
    const std::string check = body("const char *NativeSwitchBlock::checkCq(");
    EXPECT_THAT(check, HasSubstr("if (perf.cqDepth < 2)\n        return \"the card offers no queue\";"));
    EXPECT_THAT(check, HasSubstr("if (perf.cqControl & kSdCqEnable)"));
    EXPECT_THAT(
        check,
        HasSubstr("memcmp(bounce + static_cast<size_t>(i) * kBytes, checkBuffer + static_cast<size_t>(i) * kBytes,"));
    EXPECT_LT(Index(check, "cq.start(depth, perf.cache);"), Index(check, "cq.enter(cqBus)"));
    // r18-sdcq: the depth one below byte 6's (the Raspberry Pi kernel's), the test group once more after a recovered
    // protocol error, and the data path's counts from zero.
    EXPECT_THAT(check, HasSubstr("perf.cqDepth > 2 ? perf.cqDepth - 1 : perf.cqDepth"));
    EXPECT_THAT(check, HasSubstr("if (why && protocol && !attempt) {"));
    EXPECT_LT(Index(check, "const CqEngine::Recovery r = cq.recover(cqBus);"),
              Index(check, "if (why)\n            break;"));
    EXPECT_THAT(check, HasSubstr("cq.resetCounts();"));
    // r16-sdcq: sessions with the card's cache (enter turns it on first, 4.19.1); a cache found on goes off before any
    // write; a failure says what read back.
    EXPECT_THAT(check, HasSubstr("cq.start(depth, perf.cache);"));
    EXPECT_LT(Index(check, "cq.cacheOn = true;"), Index(check, "(void)cq.disableCache(cqBus);"));
    EXPECT_LT(Index(check, "cq.fail(cqBus, why)"), Index(check, "describeCq(detail, sizeof(detail));"));
}

// r16-sdcq: write-through, nothing panics. The card's cache is on only inside a CQ session (reads only), so every write
// finds it off and completes on flash as today, synchronize has nothing to flush; a cache that would not go off has
// each write flushed before it completes, and a failed flush fails that write; and the queue only runs where its own
// measurement says it is faster.
TEST(SdCqSource, CacheContract)
{
    const std::string &source = driver();
    EXPECT_THAT(source, HasSubstr("IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override\n    {\n        "
                                  "return kIOReturnSuccess;\n    }"));
    EXPECT_THAT(source,
                HasSubstr("IOReturn doSynchronizeCache() override\n    {\n        return kIOReturnSuccess;\n    }"));
    EXPECT_THAT(source,
                Not(HasSubstr("kIOStorageOptionForceUnitAccess")));  // every completed write is on flash already
    // Every write's last step, after every path that wrote it (direct, bounce, a proof's rewrite).
    const std::string end = body("void NativeSwitchBlock::endBatch(");
    EXPECT_THAT(end, HasSubstr("if (write && !result && !commitWrite())\n        result = kIOReturnIOError;"));
    EXPECT_LT(Index(end, "bounceChain(first, sectors)"), Index(end, "!commitWrite()"));
    EXPECT_LT(Index(end, "!commitWrite()"), Index(end, "b.result = result;"));
    const std::string commit = body("bool NativeSwitchBlock::commitWrite()");
    EXPECT_THAT(commit, HasSubstr("if (!cq.cacheOn)\n        return true;"));
    EXPECT_THAT(commit, HasSubstr("cq.commitWrite(cqBus)"));
    EXPECT_THAT(commit, Not(HasSubstr("panic(")));
    // No panic and no flush on the way to sleep or a power cycle: nothing completed is held in the cache.
    for (const char *name : {"void NativeSwitchBlock::suspend()", "void NativeSwitchBlock::resetCard()",
                             "void NativeSwitchBlock::cqPowerCycle()", "void NativeSwitchBlock::cqStopped()",
                             "void NativeSwitchBlock::resume()"})
        EXPECT_THAT(body(name), Not(HasSubstr("flush("))) << name;
    EXPECT_THAT(body("void NativeSwitchBlock::suspend()"), Not(HasSubstr("panic(")));
    EXPECT_FALSE(Search(source, R"(panic\([^;]*(cache|flush))", true));
    // The queue only where it is faster, and from legacy mode only for groups that pay for the way in and out.
    const std::string setup = body("bool NativeSwitchBlock::setup()");
    EXPECT_THAT(setup, HasSubstr("if (!cqOff && (!saving || need > cq.depth))"));
    EXPECT_THAT(setup, HasSubstr("(void)cq.fail(cqBus, \"no faster\");"));
    EXPECT_THAT(setup, HasSubstr("cqMinChains = need < 2 ? 2 : static_cast<unsigned>(need);"));
    EXPECT_THAT(body("unsigned NativeSwitchBlock::groupLocked("),
                HasSubstr("n >= (cq.state == CqEngine::kOn ? 1u : cqMinChains)"));
}
