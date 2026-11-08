// Guards on SwitchXUDC.cpp and XudcHardware.cpp for what xudc_transfer_test and xudc_completion_test cannot see (the
// driver itself is not built on the host): the driver lays out its transfers and matches their events through
// XudcTransfer.h, the code the tests run; its buffers' cache maintenance, TRB chaining and contiguous allocations; the
// shared bounce pool's pages freed by one endpoint re-kick the others (after the event drain and after a cancel), or an
// OUT read waiting for pages behind a busy IN endpoint would never start; interrupt moderation is on; only the head
// request ever completes, a lost event panics, a cancel fences until a drain has emptied the event ring, and the event
// ring is the one XudcTransfer.h sizes.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(SwitchXudcSource, DriverUsesTheHeader)
{
    const std::string source = SourceText("../SwitchXUDC.cpp");
    for (const char *needle : {
             "#include \"XudcTransfer.h\"",
             "plan(runs, runCount, r.length, ep.mps, out, probe, kReach",
             "ep.tds->decide(index, e.code(), e.residue(), ep.in, residue,",
             "ep.tds->push({static_cast<uint16_t>(first), static_cast<uint16_t>(n), r.length, probe && n > 1});",
             "using Tds = TdRing<kDataRingTrbs - 1, kQueueDepth>;",
             "requestSync(r, r.md, 0, r.length, out);",
             "requestSync(r, r.md, 0, actual, true);",
             "TRB_CHAIN",
             "kIOMemoryPhysicallyContiguous",
         })
        EXPECT_THAT(source, HasSubstr(needle));
    EXPECT_THAT(SourceText("../XudcHardware.cpp"), HasSubstr("link[3] = linkControl(link[3], flags, ring.cycle);"));
    const std::string memory = SourceText("../XudcRequestMemory.h");
    EXPECT_THAT(memory, HasSubstr("md->performOperation(invalidate ? kIOMemoryIncoherentIOFlush : "
                                  "kIOMemoryIncoherentIOStore, offset, length);"));
}

TEST(SwitchXudcSource, FreedPoolPagesKickEveryWaitingEndpoint)
{
    const std::string source = SourceText("../SwitchXUDC.cpp");
    EXPECT_THAT(source, HasSubstr("poolFreed = true;"));
    EXPECT_EQ(Count(source, "    kickWaiting();"), 3u)
        << "after the event drain and after a cancel, bulk or isochronous";
}

TEST(SwitchXudcSource, InterruptModerationOn)
{
    const std::string hardware = SourceText("../XudcHardware.cpp");
    EXPECT_THAT(hardware, HasSubstr("xw(RT_IMOD, (kImodUs * 4) << 16 | (kImodUs * 4));"));
    EXPECT_THAT(hardware, Not(HasSubstr("xw(RT_IMOD, 0);")));
}

TEST(SwitchXudcSource, OnlyTheHeadCompletes)
{
    const std::string source = SourceText("../SwitchXUDC.cpp");
    // An event for a later request never fails the ones before it.
    EXPECT_THAT(source, Not(HasSubstr("ep.in ? kIOReturnSuccess : kIOReturnIOError")));
    EXPECT_THAT(
        source,
        HasSubstr("void SwitchXUDC::complete(uint32_t dci, IOReturn status, uint32_t actual, const Decision *event)"));
    EXPECT_THAT(source, HasSubstr("case Verdict::Lost:\n        lostEvent(dci, e, index, d);\n        return;"));
    EXPECT_THAT(source, HasSubstr("panic(\"SwitchUSB: XUDC transfer event lost on DCI %u"));
    EXPECT_THAT(source, HasSubstr("panic(\"SwitchUSB: XUDC event ring full, events were lost"));
}

TEST(SwitchXudcSource, CancelFencesUntilTheEventRingIsRead)
{
    const std::string source = SourceText("../SwitchXUDC.cpp");
    EXPECT_THAT(source,
                HasSubstr("if (ep.tds->cancel() && !draining && !hw.eventPending())\n        ep.tds->liftFence();"));
    EXPECT_THAT(source, HasSubstr("endpoints[dci].tds->liftFence();"));
    EXPECT_THAT(source, HasSubstr("!ep.tds->fenced()")) << "kick puts nothing on a fenced ring";
    EXPECT_THAT(source, HasSubstr("if (!hw.nextEvent(e)) {")) << "the drain runs until the event ring is empty";
}

TEST(SwitchXudcSource, EventRingSizedByTheHeader)
{
    const std::string hardware = SourceText("../XudcHardware.cpp");
    EXPECT_THAT(hardware, HasSubstr("constexpr uint32_t kEventBytes = 2 * kEventSegmentTrbs * 16;"));
    EXPECT_THAT(hardware, HasSubstr("xw(ERSTSZ, kEventSegmentTrbs << 16 | kEventSegmentTrbs);"));
    EXPECT_THAT(hardware, HasSubstr("if (++eventIndex == 2 * kEventSegmentTrbs) {"));
}

// The kernel's os_log encodes a message's arguments into a 256-byte buffer (XNU libkern/os/log.h OS_LOG_BUFFER_MAX_SIZE
// under KERNEL), less its tracepoint header (the caller's location, about 10 bytes): one that does not fit is dropped
// whole (log_encode.c, ENOMEM). Each argument takes 2 bytes and its value (%llu 8, %u 4, %s a 6-byte range). The stats
// line is 196 bytes and arrives; every os_log call here stays at most 230, so a field added cannot make it vanish.
TEST(SwitchXudcSource, StatsLinesFitTheKernelsOsLogBuffer)
{
    const std::string source = SourceText("../SwitchXUDC.cpp");
    size_t calls = 0;
    for (size_t at = source.find("os_log(OS_LOG_DEFAULT,"); at != std::string::npos;
         at = source.find("os_log(OS_LOG_DEFAULT,", at + 1)) {
        std::string format;
        size_t i = at + 22;
        for (;;) {  // the adjacent string literals
            while (i < source.size() && (source[i] == ' ' || source[i] == '\n'))
                ++i;
            if (i >= source.size() || source[i] != '"')
                break;
            const size_t end = source.find('"', i + 1);
            format += source.substr(i + 1, end - i - 1);
            i = end + 1;
        }
        size_t bytes = 2;
        for (size_t k = format.find('%'); k != std::string::npos; k = format.find('%', k + 1)) {
            size_t c = k + 1;
            while (c < format.size() && std::string("0123456789#-+ .").find(format[c]) != std::string::npos)
                ++c;
            if (c < format.size() && format[c] == '%') {  // %%
                k = c;
                continue;
            }
            bytes += 2 + (format.compare(c, 2, "ll") == 0 ? 8 : format[c] == 's' ? 6 : 4);
        }
        EXPECT_LE(bytes, 230u) << format;
        ++calls;
    }
    EXPECT_EQ(calls, 2u) << "the stats line and the transfer-events line";
}

// System sleep: SC7 resets the controller and its event ring, so suspend goes off the bus with the interrupt and the
// delivery check off, and resume runs the whole powerOn before it lifts the fences and goes back on the bus.
TEST(SwitchXudcSource, SleepPowersOnWholeBeforeTheBus)
{
    const std::string source = SourceText("../SwitchXUDC.cpp");
    const std::string sleep = switch_ios::test::Between(source, "IOReturn SwitchXUDC::systemSleep(bool entering)",
                                                        "void switchxudc_sleep(bool entering)");
    const size_t cancel = sleep.find("delivery->cancelTimeout();"), off = sleep.find("setBus(false);"),
                 masked = sleep.find("interrupt->disable();"), power = sleep.find("hw.powerOn(why, sizeof(why))"),
                 lift = sleep.find("liftFence();"), unmasked = sleep.find("interrupt->enable();"),
                 on = sleep.find("setBus(true);");
    for (const size_t at : {cancel, off, masked, power, lift, unmasked, on})
        ASSERT_NE(at, std::string::npos);
    EXPECT_LT(cancel, off);
    EXPECT_LT(off, masked);
    EXPECT_LT(masked, power);
    EXPECT_LT(power, lift);
    EXPECT_LT(lift, unmasked);
    EXPECT_LT(unmasked, on);
    EXPECT_THAT(source, HasSubstr("if (on && lostToSleep)\n        return;"));
}

// An attached host holds idle sleep off as an iPad's powerd does in USB device mode: every change of hostPresent goes
// through setHostPresent, which takes and drops the kernel's idle-sleep assertion with it.
TEST(SwitchXudcSource, AttachedHostHoldsIdleSleep)
{
    const std::string source = SourceText("../SwitchXUDC.cpp");
    EXPECT_EQ(Count(source, "hostPresent = "), 1u) << "only setHostPresent writes it";
    EXPECT_EQ(Count(source, "setHostPresent(true);"), 2u);   // bus reset with a host, resume from a bus suspend
    EXPECT_EQ(Count(source, "setHostPresent(false);"), 3u);  // disconnect, bus suspend, off the bus
    const std::string set = switch_ios::test::Between(source, "void SwitchXUDC::setHostPresent(bool present)", "\n}\n");
    EXPECT_THAT(set, HasSubstr("thread_call_enter(idleHoldCall);"));
    EXPECT_THAT(set, Not(HasSubstr("PMAssertion"))) << "never on the work loop";
    const std::string hold =
        switch_ios::test::Between(source, "void SwitchXUDC::applyIdleHold(thread_call_param_t owner", "\n}\n");
    EXPECT_THAT(hold, HasSubstr("kIOPMDriverAssertionPreventSystemIdleSleepBit"));
    EXPECT_THAT(hold, HasSubstr("releasePMAssertion(self->idleHold);"));
    EXPECT_THAT(hold, Not(HasSubstr("kIOPMDriverAssertionCPUBit")));
}

// Isochronous requests keep their frame list where the family put it (completeEndpointIO hands it to the client): no
// isochronous completion writes the bulk actual length over it, and every frame goes on the ring as an Isoch TRB the
// controller starts as soon as it can.
TEST(SwitchXudcSource, IsochronousCompletionKeepsTheFrameList)
{
    const std::string source = SourceText("../SwitchXUDC.cpp");
    for (const char *function : {"void SwitchXUDC::completeIsoc(", "void SwitchXUDC::finishIsoc(",
                                 "void SwitchXUDC::failHeadIsoc(", "void SwitchXUDC::cancelIsoc("}) {
        const std::string body = switch_ios::test::Body(source, function);
        ASSERT_FALSE(body.empty()) << function;
        EXPECT_THAT(body, Not(HasSubstr("kReqActual"))) << function;
        EXPECT_THAT(body, Not(HasSubstr("finish("))) << function;
    }
    EXPECT_THAT(source,
                HasSubstr("trbType(TRB_ISOCH) | TRB_SIA | (out ? TRB_ISP : 0) | (i + 1 == count ? TRB_IOC : 0)"));
    EXPECT_THAT(source, HasSubstr("ep.iso->event(index, e.code(), e.residue())"));
}
