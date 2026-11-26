// Guards on SwitchXUDC (SwitchXUDC.h and its SwitchXUDC*.cpp) and XudcHardware.cpp for what xudc_transfer_test and xudc_completion_test cannot see (the
// driver itself is not built on the host): the driver lays out its transfers and matches their events through
// XudcTransfer.h, the code the tests run; its buffers' cache maintenance, TRB chaining and contiguous allocations; the
// shared bounce pool's pages freed by one endpoint re-kick the others (after the event drain and after a cancel), or an
// OUT read waiting for pages behind a busy IN endpoint would never start; interrupt moderation is on; only the head
// request ever completes, a lost event panics, a cancel fences until a drain has emptied the event ring, and the event
// ring is the one XudcTransfer.h sizes.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
// The driver's own sources, one after another.
std::string DriverText()
{
    std::string text;
    for (const char *file : {"../SwitchXUDC.h", "../SwitchXUDC.cpp", "../SwitchXUDCEvents.cpp", "../SwitchXUDCData.cpp",
                             "../SwitchXUDCIsoch.cpp", "../SwitchXUDCEndpoints.cpp", "../SwitchXUDCStats.cpp"})
        text += SourceText(file);
    return text;
}
}  // namespace

TEST(SwitchXudcSource, DriverUsesTheHeader)
{
    const std::string source = DriverText();
    for (const char *needle : {
             "#include \"XudcTransfer.h\"",
             "plan(runs, count, r.length, ep.mps, out, kReach",
             "ep.tds->decide(index, e.code(), e.residue(), ep.in,",
             "ep.tds->push({static_cast<uint16_t>(first), static_cast<uint16_t>(n), r.length});",
             "using Tds = TdRing<kDataRingTrbs - 1, kQueueDepth>;",
             "requestSync(r, r.md, 0, r.length, out);",
             "requestSync(r, r.md, 0, actual, true);",
             "TRB_CHAIN",
             "kIOMemoryPhysicallyContiguous",
         })
        EXPECT_THAT(source, HasSubstr(needle));
    EXPECT_THAT(SourceText("../XudcHardware.cpp"), HasSubstr("link[3] = linkControl(link[3], flags, ring.cycle);"));
    const std::string memory = SourceText("../XudcRequestMemory.h");
    const std::string sync = switch_ios::test::Body(memory, "inline void requestSync(");
    EXPECT_THAT(sync, HasSubstr("fromDevice ? SWITCH_DCACHE_INVALIDATE : SWITCH_DCACHE_CLEAN"))
        << "OUT memory is invalidated before and after the transfer, never cleaned first (xudc_transfer_test "
           "XudcOutCache)";
    EXPECT_THAT(sync, Not(HasSubstr("CLEAN_INVALIDATE")));
    EXPECT_THAT(sync, Not(HasSubstr("performOperation")));
}

// The rings, the event ring and the contexts are write-combined, so XudcHardware.cpp maintains no cache line: a TRB's
// cycle word is stored after a publish of its other words, an event's words are loaded after a consume behind its cycle
// word, a context edit is published before the register write that has the controller read it, and the doorbell keeps
// its barrier (the data buffers' cache maintenance completes before it). The buffers the CPU copies through stay
// cacheable.
TEST(SwitchXudcSource, RingsEventsAndContextsWriteCombined)
{
    const std::string hardware = SourceText("../XudcHardware.cpp");
    EXPECT_THAT(hardware, Not(HasSubstr("switch_dcache_")));
    for (const char *needle : {"control.allocate(kControlBytes, true)", "events.allocate(kEventBytes, true)",
                               "ring.own.allocate(kDataRingTrbs * 16, true)"})
        EXPECT_THAT(hardware, HasSubstr(needle));
    const auto inOrder = [](const std::string &body, std::initializer_list<const char *> steps) {
        size_t at = 0;
        for (const char *step : steps) {
            const size_t found = body.find(step, at);
            ASSERT_NE(found, std::string::npos) << step;
            at = found + 1;
        }
    };
    inOrder(switch_ios::test::Body(hardware, "void Hardware::fillData("),
            {"t[2] = ", "switch_dma_publish();", "t[3] = flags | slot.cycle;"});
    inOrder(switch_ios::test::Body(hardware, "bool Hardware::nextEvent("),
            {"const uint32_t d3 = e[3];", "return false;", "switch_dma_consume();", "event.d[0] = e[0];"});
    inOrder(switch_ios::test::Body(hardware, "void Hardware::doorbell("), {"barrier();", "xw(DB, "});
    inOrder(switch_ios::test::Body(hardware, "void Hardware::resetEp0("),
            {"c[1] = ", "switch_dma_publish();", "reload(0);"});
    inOrder(switch_ios::test::Body(hardware, "void Hardware::disableEndpoint("),
            {"toggleBit(EP_PAUSE, dci, true);", "c[0] &= ~7u;", "c[i] = 0;", "switch_dma_publish();",
             "xw(EP_RELOAD, bit);", "settled(EP_STCHG, bit, bit);", "xw(EP_STCHG, bit);"});
    inOrder(switch_ios::test::Body(hardware, "void Hardware::resetSequence("),
            {"c[0] &= ~7u;", "switch_dma_publish();", "reload(dci);", "c[5] &= 0x00ffffffu;", "switch_dma_publish();",
             "reloadEndpoint(dci);"});
    inOrder(switch_ios::test::Body(hardware, "void Hardware::dropQueued("),
            {"c[5] &= 0xf8000000u;", "switch_dma_publish();", "reload(dci);"});
    inOrder(switch_ios::test::Body(hardware, "bool Hardware::reload("), {"xw(EP_RELOAD, 1u << dci);", "settled("});
    inOrder(switch_ios::test::Body(hardware, "bool Hardware::initEndpoint("),
            {"resetRing(dci);", "c[11] = address;", "switch_dma_publish();"});
    const std::string driver = DriverText();
    for (const char *cacheable :
         {"ep0Buffer.allocate(kEp0Bytes)", "pool.allocate(kPoolBytes)", "ep.serial.allocate(kSerialBytes)"})
        EXPECT_THAT(driver, HasSubstr(cacheable)) << "copied through by the CPU: cacheable";
}

// BEI (xudc_interrupt_test models it): only an IN request's last TRB is held, and written with BEI only when kick()
// queued another request after it; the ring's last is written without before the doorbell. A cancel, a halt and a kick
// that left a request waiting ask for a drain when an event is there, never inside one or asleep.
TEST(SwitchXudcSource, BlockedInterruptsOnInOnly)
{
    const std::string driver = DriverText();
    EXPECT_EQ(Count(driver, "TRB_BEI"), 1u);
    EXPECT_THAT(switch_ios::test::Body(driver, "void SwitchXUDC::writeHeld("), HasSubstr("another ? TRB_BEI : 0"));
    EXPECT_THAT(driver, HasSubstr("if (lastTrb && !out) {"));
    EXPECT_THAT(driver, HasSubstr("if (lastFrame && !out) {"));
    for (const char *function : {"void SwitchXUDC::kick(", "void SwitchXUDC::kickIsoc("}) {
        SCOPED_TRACE(function);
        const std::string body = switch_ios::test::Body(driver, function);
        const size_t between = body.find("writeHeld(dci, held, last.held);");
        const size_t end = body.find("writeHeld(dci, held, false);");
        const size_t doorbell = body.find("hw.doorbell(dci);");
        const size_t drain = body.find("drainSoon();");
        ASSERT_NE(between, std::string::npos);
        ASSERT_NE(end, std::string::npos);
        EXPECT_LT(between, end);
        EXPECT_LT(end, doorbell) << "the ring's last is written before the doorbell";
        EXPECT_LT(doorbell, drain);
        EXPECT_THAT(body, HasSubstr("waiting = a == Admit::Wait;"));
    }
    for (const char *function : {"void SwitchXUDC::cancel(", "void SwitchXUDC::cancelIsoc("})
        EXPECT_THAT(switch_ios::test::Body(driver, function), HasSubstr("drainSoon();")) << function;
    EXPECT_THAT(driver, HasSubstr("hw.setHalt(dci, true);\n        drainSoon();"));
    const std::string soon = switch_ios::test::Body(driver, "void SwitchXUDC::drainSoon(");
    for (const char *needle :
         {"!draining", "!asleep", "interrupt->isEnabled()", "hw.eventPending()", "interrupt->signalInterrupt();"})
        EXPECT_THAT(soon, HasSubstr(needle));
}

// Nothing in device mode polls. No timer samples state (the lost-interrupt check is gone; the one timer ends
// forceReEnumeration's off time and reads nothing), the event ring is read by the interrupt's drain or one the driver
// asks for, and an endpoint state change, which raises no interrupt or event, is read once after a sleep
// (Hardware::settled), never spun on; a miss is a line, and EP_STCHG is acknowledged before and after
// every change as Linux acknowledges it after. A link state change request raises a port event (Linux sets
// PORTHALT_STCHG_INTR_EN), and a port that keeps changing is a line, not a loop.
TEST(SwitchXudcSource, NothingPolls)
{
    const std::string driver = DriverText();
    for (const char *needle : {"deliveryCheck", "interruptPending", "IODelay", "IOSleep", "switch_mmio_wait32"})
        EXPECT_THAT(driver, Not(HasSubstr(needle))) << needle;
    EXPECT_EQ(Count(driver, "setTimeout"), 1u);
    EXPECT_THAT(switch_ios::test::Body(driver, "void SwitchXUDC::forceReEnumeration("),
                HasSubstr("busReturn->setTimeoutMS(100);"));
    const std::string returned = switch_ios::test::Body(driver, "void SwitchXUDC::busReturned(");
    EXPECT_THAT(returned, HasSubstr("self->goOnBus(true);"));
    EXPECT_THAT(returned, Not(HasSubstr("hw.")));
    EXPECT_THAT(SourceText("../SwitchUSB.cpp"), Not(HasSubstr("switchxudc_host_expected")));
    const std::string hardware = SourceText("../XudcHardware.cpp");
    EXPECT_THAT(hardware, Not(HasSubstr("switch_mmio_wait32(xudc"))) << "no XUDC register is spun on";
    const std::string settle = switch_ios::test::Body(hardware, "bool Hardware::settled(");
    EXPECT_THAT(settle, HasSubstr("IOSleep(kStateChangeMs);"));
    EXPECT_EQ(Count(settle, "xr("), 1u) << "one read after the sleep";
    EXPECT_THAT(settle, HasSubstr("IOLog(\"SwitchUSB: DCI bits %#x: %s reads %#x, not %#x"));
    EXPECT_THAT(settle, Not(HasSubstr("while")));
    EXPECT_THAT(hardware, HasSubstr("constexpr uint32_t kStateChangeMs = 1;"));
    EXPECT_THAT(hardware, Not(HasSubstr("IODelay(kStateChange")));
    EXPECT_EQ(Count(hardware, "settled(EP_"), 6u)
        << "reload, an enable's reload, a halt or pause change, a removal's reload, thread idle, babble's stop";
    const std::string babble = switch_ios::test::Body(hardware, "void Hardware::haltAfterBabble(");
    EXPECT_LT(babble.find("settled(EP_STOPPED, bit, bit)"), babble.find("xw(EP_STOPPED, bit);"));
    EXPECT_LT(babble.find("xw(EP_STOPPED, bit);"), babble.find("toggleBit(EP_HALT, dci, true)"));
    EXPECT_THAT(DriverText(), HasSubstr("if (d.outcome == Outcome::Error && e.code() == COMP_BABBLE)\n        "
                                        "hw.haltAfterBabble(dci);"));
    const std::string deactivate = switch_ios::test::Body(DriverText(), "IOReturn SwitchXUDC::deactivateEndpoint(");
    EXPECT_LT(deactivate.find("hw.disableEndpoint(dci);"), deactivate.find("hw.stop();"))
        << "CTRL.RUN off once no data endpoint is left (T210 TRM 22.9.4.2, SET_CONFIGURATION 0)";
    const std::string change = switch_ios::test::Body(hardware, "bool Hardware::changeState(");
    EXPECT_EQ(Count(change, "xw(EP_STCHG, bits);"), 2u);
    const std::string reload = switch_ios::test::Body(hardware, "bool Hardware::reloadEndpoint(");
    EXPECT_LT(reload.find("reload(dci)"), reload.find("toggleBit(EP_PAUSE, dci, false)"));
    EXPECT_LT(reload.find("toggleBit(EP_PAUSE, dci, false)"), reload.find("toggleBit(EP_HALT, dci, false)"));
    // T210 TRM 22.9.4.2: Reload, Run, the Reload wait, Pause cleared and waited on, Halt cleared and acknowledged.
    const std::string enable = switch_ios::test::Body(hardware, "bool Hardware::enableEndpoint(");
    const size_t reloadAt = enable.find("xw(EP_RELOAD, bit);");
    const size_t runAt = enable.find("run();");
    const size_t waitAt = enable.find("settled(EP_RELOAD, bit, 0)");
    const size_t pauseAt = enable.find("toggleBit(EP_PAUSE, dci, false)");
    const size_t haltAt = enable.find("xw(EP_HALT, ");
    const size_t ackAt = enable.find("xw(EP_STCHG, bit);");
    for (const size_t at : {reloadAt, runAt, waitAt, pauseAt, haltAt, ackAt})
        ASSERT_NE(at, std::string::npos);
    EXPECT_LT(reloadAt, runAt);
    EXPECT_LT(runAt, waitAt);
    EXPECT_LT(waitAt, pauseAt);
    EXPECT_LT(pauseAt, haltAt);
    EXPECT_LT(haltAt, ackAt);
    EXPECT_THAT(switch_ios::test::Body(DriverText(), "IOReturn SwitchXUDC::activateEndpoint("),
                HasSubstr("hw.enableEndpoint(dci)"));
    EXPECT_LT(change.find("(xr(CTRL) & CTRL_RUN) == 0u"), change.find("settled(")) << "no wait with Run clear";
    EXPECT_THAT(hardware, Not(HasSubstr("xw(EP_PAUSE,")));
    EXPECT_EQ(Count(hardware, "xw(EP_HALT,"), Count(enable, "xw(EP_HALT,"))
        << "Halt changes go through toggleBit and its state change, but for enableEndpoint's clear (T210 TRM 22.9.4.2)";
    EXPECT_THAT(switch_ios::test::Body(hardware, "bool Hardware::powerOn("),
                HasSubstr("xw(PORTHALT, xr(PORTHALT) | PORTHALT_STCHG_INTR_EN);"));
    EXPECT_THAT(switch_ios::test::Body(hardware, "PortChange Hardware::portChange("),
                HasSubstr("if (pass == kPortPasses) {"));
}

// The OUT residue is the event's TRB's (XudcTransfer.h): nothing probes for it or learns it, and no OUT request goes
// one TRB at a time for it. The runs a request may have never bind before the ring does, and the scratch that holds
// them is not in the object.
TEST(SwitchXudcSource, ResidueIsTheTrbsAndRunsNeverBindFirst)
{
    const std::string driver = DriverText();
    for (const char *gone : {"probe", "kResidue", "residue ==", "learned"})
        EXPECT_THAT(driver, Not(HasSubstr(gone))) << gone;
    EXPECT_THAT(driver, HasSubstr("constexpr uint32_t kMaxRuns = kMaxPieces;"));
    EXPECT_THAT(driver, HasSubstr("Run *scratchRuns;"));
    EXPECT_THAT(driver, HasSubstr("Piece *scratchPieces;"));
    EXPECT_THAT(SourceText("../XudcTransfer.h"), HasSubstr("constexpr uint32_t kDataRingTrbs = 1024;"));
}

// The XUDC behind the SMMU: its space is attached before the controller powers on, with all of RAM (the loader's ledger)
// mapped once; a request goes into the window only when its runs would bounce at least kRemapWorthBytes that a mapping
// can change, and every path that gives a request's memory back (completion, a failed head, a cancel) gives its window
// pages back too. Host and device share UsbSmmuSpace: no second copy of the map, the window, the tables or the flush.
// The host has its own layout (the low bank at its own address, the firmware image read only, the 64 MiB window below
// RAM), and a contiguous buffer at its own address takes no slot before anything takes the window.
TEST(SwitchXudcSource, BehindTheSmmu)
{
    const std::string driver = DriverText() + SourceText("../SwitchXUDCSmmu.cpp");
    const std::string begin = switch_ios::test::Body(driver, "bool SwitchXUDC::begin(");
    ASSERT_NE(begin.find("smmuBegin();"), std::string::npos);
    EXPECT_LT(begin.find("smmuBegin();"), begin.find("hw.powerOn(why, whySize)"));
    const std::string smmu = switch_ios::test::Body(driver, "void SwitchXUDC::smmuBegin(");
    for (const char *needle : {"smmu.init(TEGRA_SMMU_ASID_XUSB_DEV, UsbStaticMap::device(ram), why, sizeof(why))",
                               "TEGRA_SMMU_XUSB_DEV_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_2, kTranslationBits"})
        EXPECT_THAT(smmu, HasSubstr(needle));
    EXPECT_THAT(driver, HasSubstr("kTranslationBits = (1u << 12) | (1u << 13);"));
    EXPECT_THAT(switch_ios::test::Body(driver, "const Run *SwitchXUDC::viewOf("),
                HasSubstr("smmu.fixedIova(pa, length)"));
    EXPECT_THAT(driver, HasSubstr("bounceBytes(runs, count, r.length, ep.mps, out) >= kRemapWorthBytes && remap("));
    EXPECT_THAT(driver, HasSubstr("if (bouncedBytes >= kRemapWorthBytes && !r.iovaPages && smmu.attached()"));
    EXPECT_THAT(SourceText("../XudcIova.h"), HasSubstr("constexpr uint32_t kRemapWorthBytes = 3 * 1024;"));
    const std::string spaceText = SourceText("../UsbSmmuSpace.cpp");
    EXPECT_THAT(switch_ios::test::Body(spaceText, "bool UsbSmmuSpace::init("), HasSubstr("layout.install(domain_)"));
    EXPECT_THAT(switch_ios::test::Body(spaceText, "bool UsbSmmuSpace::ram("),
                HasSubstr("getProperty(\"switch-ram-ledger\")"));
    const std::string mapper = SourceText("../UsbDmaMapper.cpp");
    const std::string map = switch_ios::test::Body(mapper, "IOReturn SwitchDMAMapper::iovmMapMemory(");
    ASSERT_NE(map.find("directAddress(physical, length)"), std::string::npos);
    EXPECT_LT(map.find("directAddress(physical, length)"), map.find("switch_usb_smmu_map("));
    EXPECT_THAT(map, Not(HasSubstr("IOLockLock"))) << "a buffer at its own address takes no lock";
    EXPECT_THAT(switch_ios::test::Body(mapper, "uint64_t SwitchDMAMapper::directAddress("),
                HasSubstr("switch_usb_smmu_fixed(physical, length)"));
    const std::string unmap = switch_ios::test::Body(mapper, "IOReturn SwitchDMAMapper::iovmUnmapMemory(");
    EXPECT_LT(unmap.find("switch_dcache_phys_lines(SWITCH_DCACHE_CLEAN_INVALIDATE, mapAddress, mapLength)"),
              unmap.find("IOLockLock"))
        << "nor to give it back";
    for (const char *function :
         {"void SwitchXUDC::complete(", "void SwitchXUDC::cancel(", "void SwitchXUDC::completeIsoc(",
          "void SwitchXUDC::failHeadIsoc(", "void SwitchXUDC::cancelIsoc("}) {
        const std::string body = switch_ios::test::Body(driver, function);
        EXPECT_THAT(body, HasSubstr("releaseMemory(r);")) << function;
        EXPECT_THAT(body, Not(HasSubstr("->complete()"))) << function;
    }
    EXPECT_THAT(SourceText("../XudcHardware.cpp"),
                HasSubstr("(asid & 0x80000000u) && (asid & 0x7fu) != TEGRA_SMMU_ASID_XUSB_DEV"));
    const std::string host = SourceText("../UsbSmmuMapper.cpp");
    for (const char *gone : {"TegraSMMUDomain g", "UsbIovaWindow g", "tegra_smmu_flush", "takeDirty"})
        EXPECT_THAT(host, Not(HasSubstr(gone))) << gone;
    EXPECT_THAT(host, HasSubstr("UsbSmmuSpace gSpace;"));
    EXPECT_THAT(host, HasSubstr("UsbStaticMap::host(ram, firmwarePa, firmwareBytes)"));
    EXPECT_THAT(driver, HasSubstr("UsbSmmuSpace smmu;"));
}

TEST(SwitchXudcSource, FreedPoolPagesKickEveryWaitingEndpoint)
{
    const std::string source = DriverText();
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
    const std::string source = DriverText();
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
    const std::string source = DriverText();
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
    const std::string source = DriverText();
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
            if (format.compare(c, 2, "ll") == 0) {
                bytes += 2 + 8;
            } else {
                bytes += 2 + (format[c] == 's' ? 6 : 4);
            }
        }
        EXPECT_LE(bytes, 230u) << format;
        ++calls;
    }
    EXPECT_EQ(calls, 3u) << "the stats line, the transfer-events line and the window line";
}

// Release kernels carry no stats, timing or lost-interrupt probe: SwitchXUDCStats.cpp is empty unless DEVELOPMENT or
// DEBUG, XudcStats (SwitchXUDCStats.h) is an empty struct whose clock reads 0 there, the SMMU space times its window
// only inside the same gate, and no other driver file reads the clock (but the isochronous frame stamps, which the
// client gets) or writes an os_log line.
TEST(SwitchXudcSource, StatsCompileOutOfRelease)
{
    const std::string stats = SourceText("../SwitchXUDCStats.cpp");
    const size_t gate = stats.find("#if DEVELOPMENT || DEBUG\n");
    ASSERT_NE(gate, std::string::npos);
    EXPECT_LT(gate, stats.find("SwitchXUDC::")) << "every definition is inside the gate";
    EXPECT_EQ(stats.rfind("#endif\n"), stats.size() - 7) << "the gate closes at the end of the file";
    const std::string header = SourceText("../SwitchXUDCStats.h");
    const std::string release = switch_ios::test::Between(header, "#else\n", "#endif\n");
    EXPECT_THAT(release, HasSubstr("return 0;"));
    EXPECT_THAT(release, Not(HasSubstr("mach_absolute_time")));
    EXPECT_THAT(release, Not(HasSubstr("++")));
    for (const char *file :
         {"../SwitchXUDC.cpp", "../SwitchXUDCEvents.cpp", "../SwitchXUDCData.cpp", "../SwitchXUDCEndpoints.cpp"}) {
        const std::string source = SourceText(file);
        EXPECT_THAT(source, Not(HasSubstr("mach_absolute_time"))) << file;
        EXPECT_THAT(source, Not(HasSubstr("os_log("))) << file;
    }
    for (const char *file : {"../UsbSmmuSpace.h", "../UsbSmmuSpace.cpp"}) {
        std::string release = SourceText(file);
        for (size_t at = release.find("#if DEVELOPMENT || DEBUG\n"); at != std::string::npos;
             at = release.find("#if DEVELOPMENT || DEBUG\n")) {
            const size_t end = release.find("#endif\n", at);
            ASSERT_NE(end, std::string::npos) << file;
            release.erase(at, end + 7 - at);
        }
        EXPECT_THAT(release, Not(HasSubstr("mach_absolute_time"))) << file;
        EXPECT_THAT(release, Not(HasSubstr("cost_"))) << file;
    }
    const std::string isoch = SourceText("../SwitchXUDCIsoch.cpp");
    EXPECT_EQ(Count(isoch, "mach_absolute_time()"), 1u) << "the frames' stamps only";
    EXPECT_THAT(switch_ios::test::Body(isoch, "void SwitchXUDC::isocEvent("), HasSubstr("mach_absolute_time()"));
}

// System sleep: SC7 resets the controller and its event ring, so suspend goes off the bus with the interrupt off, and
// resume runs the whole powerOn before it lifts the fences and goes back on the bus.
TEST(SwitchXudcSource, SleepPowersOnWholeBeforeTheBus)
{
    const std::string source = DriverText();
    const std::string sleep = switch_ios::test::Between(source, "IOReturn SwitchXUDC::systemSleep(bool entering)",
                                                        "void switchxudc_sleep(bool entering)");
    const size_t off = sleep.find("setBus(false);");
    const size_t masked = sleep.find("interrupt->disable();");
    const size_t power = sleep.find("hw.powerOn(why, sizeof(why))");
    const size_t lift = sleep.find("liftFence();");
    const size_t unmasked = sleep.find("interrupt->enable();");
    const size_t on = sleep.find("setBus(true);");
    for (const size_t at : {off, masked, power, lift, unmasked, on})
        ASSERT_NE(at, std::string::npos);
    EXPECT_LT(off, masked);
    EXPECT_LT(masked, power);
    EXPECT_LT(power, lift);
    EXPECT_LT(lift, unmasked);
    EXPECT_LT(unmasked, on);
    EXPECT_THAT(source, HasSubstr("if (on && (lostToSleep || asleep ||"));
    EXPECT_THAT(switch_ios::test::Body(source, "void SwitchXUDC::setBus("), HasSubstr("&gLaneReady"));
}

// An attached host holds idle sleep off as an iPad's powerd does in USB device mode: every change of hostPresent goes
// through setHostPresent, which takes and drops the kernel's idle-sleep assertion with it.
TEST(SwitchXudcSource, AttachedHostHoldsIdleSleep)
{
    const std::string source = DriverText();
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
    const std::string source = DriverText();
    for (const char *function : {"void SwitchXUDC::completeIsoc(", "void SwitchXUDC::finishIsoc(",
                                 "void SwitchXUDC::failHeadIsoc(", "void SwitchXUDC::cancelIsoc("}) {
        const std::string body = switch_ios::test::Body(source, function);
        ASSERT_FALSE(body.empty()) << function;
        EXPECT_THAT(body, Not(HasSubstr("kReqActual"))) << function;
        EXPECT_THAT(body, Not(HasSubstr("finish("))) << function;
    }
    EXPECT_THAT(source, HasSubstr("trbType(TRB_ISOCH) | TRB_SIA | (out ? TRB_ISP : 0) | (lastFrame ? TRB_IOC : 0)"));
    EXPECT_THAT(source, HasSubstr("ep.iso->event(index, e.code(), e.residue())"));
}

// Device mode releases only the fake SS port, under the pad controller lock: ELPG_PROGRAM_0's wake bits, the other SS
// ports' clamps and PCIe's AUX clamps belong to others (xusb_padctl_test has the writes).
TEST(SwitchXudcSource, ConnectLeavesOtherElpgBitsAlone)
{
    const std::string connect = switch_ios::test::Body(SourceText("../XudcHardware.cpp"), "void Hardware::connect(");
    ASSERT_FALSE(connect.empty());
    EXPECT_THAT(connect, HasSubstr("switch_usb_padctl_fake_port_up(padctl);"));
    EXPECT_THAT(connect, Not(HasSubstr("ELPG_PROGRAM")));
}

// FS logic at 48 MHz in device mode too: FO_48M with the divisor cleared (Tegra X1 TRM 22.8.4 step 6).
TEST(SwitchXudcSource, FsClockIsFo48Undivided)
{
    const std::string power = switch_ios::test::Body(SourceText("../XudcHardware.cpp"), "bool Hardware::powerOn(");
    EXPECT_THAT(power, HasSubstr("(switch_mmio_read32(car, CLK_SOURCE_XUSB_FS) & 0x1fffff00u) | 2u << 29"));
}

// XUSBB before XUSBA, each clock enabled by the partition's power-up and not before it (Tegra X1 TRM 22.8.7.5).
TEST(SwitchXudcSource, PartitionsInTheTrmOrder)
{
    const std::string power = switch_ios::test::Body(SourceText("../XudcHardware.cpp"), "bool Hardware::powerOn(");
    const size_t b = power.find("powerUp(xusb::PART_XUSBB");
    const size_t a = power.find("powerUp(xusb::PART_XUSBA");
    ASSERT_NE(b, std::string::npos);
    ASSERT_NE(a, std::string::npos);
    EXPECT_LT(b, a);
    EXPECT_THAT(power, Not(HasSubstr("CLK_ENB_U_SET, U_XUSB_DEV")));
    EXPECT_THAT(power, Not(HasSubstr("CLK_ENB_W_SET, W_XUSB_SS")));
}

// The port assignment (pad mux, pad 0, SS port map) is written before the partitions release the controller's resets
// (Tegra X1 TRM 22.8.4 step 5 note, p.1350), as the host's power-up does.
TEST(SwitchXudcSource, PortAssignmentUnderReset)
{
    const std::string power = switch_ios::test::Body(SourceText("../XudcHardware.cpp"), "bool Hardware::powerOn(");
    const size_t partitions = power.find("powerUp(xusb::PART_XUSBB");
    ASSERT_NE(partitions, std::string::npos);
    for (const char *step : {"USB2_PAD_MUX", "xpad::powerOn(", "SS_PORT_MAP"})
        EXPECT_LT(power.find(step), partitions) << step;
}

// Device mode off clamps the fake SS port again (tegra210_usb2_phy_power_off), so the SuperSpeed lane never programs an
// unclamped port.
TEST(SwitchXudcSource, DisconnectClampsTheFakePort)
{
    const std::string off = switch_ios::test::Body(SourceText("../XudcHardware.cpp"), "void Hardware::disconnect(");
    EXPECT_THAT(off, HasSubstr("switch_usb_padctl_fake_port_down(padctl);"));
}

// The XUDC's BAR is memory space with posted writes before FPCI goes on; CFG_1 without IO space (TRM 22.8.4 step 9).
TEST(SwitchXudcSource, BarIsPostedMemorySpace)
{
    const std::string power = switch_ios::test::Body(SourceText("../XudcHardware.cpp"), "bool Hardware::powerOn(");
    const size_t bar = power.find("xw(IPFS_FPCI_BAR0, xr(IPFS_FPCI_BAR0) & ~1u);");
    ASSERT_NE(bar, std::string::npos);
    EXPECT_LT(bar, power.find("xw(IPFS_CONFIGURATION"));
    EXPECT_THAT(power, HasSubstr("xw(FPCI_CFG_1, xr(FPCI_CFG_1) | 6);"));
}

// Device mode's VBUS follows the PD controller (Tegra X1 TRM 22.9.11: the VBUS override on only while connected to a
// host port), never connect(); a port change latched while off the bus is cleared before the controller is enabled, so
// a role change back with nothing attached reports no connection.
TEST(SwitchXudcSource, VbusFollowsThePdController)
{
    const std::string connect = switch_ios::test::Body(SourceText("../XudcHardware.cpp"), "void Hardware::connect(");
    ASSERT_FALSE(connect.empty());
    EXPECT_THAT(connect, Not(HasSubstr("VBUS_ID_VBUS_ON")));
    const size_t cleared = connect.find("clearPortChange(latched);");
    ASSERT_NE(cleared, std::string::npos);
    EXPECT_LT(cleared, connect.find("xw(CTRL, xr(CTRL) | CTRL_ENABLE);"));
    const std::string bus = switch_ios::test::Body(SourceText("../SwitchXUDC.cpp"), "void SwitchXUDC::setBus(");
    EXPECT_LT(bus.find("hw.connect();"), bus.find("hw.setVbus(__atomic_load_n(&gHostVbus, __ATOMIC_ACQUIRE));"));
    const std::string usb = SourceText("../SwitchUSB.cpp");
    EXPECT_THAT(switch_ios::test::Body(usb, "void pdChanged("),
                HasSubstr("switch_pd_usb_device_vbus(sourcing, state->status1)"));
    EXPECT_THAT(switch_ios::test::Body(usb, "void applyRole("),
                HasSubstr("switchxudc_set_vbus(deviceVbus && !gHostRole, deviceLaneReady());"));
}
