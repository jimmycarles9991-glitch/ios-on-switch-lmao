// Guards on NativeLegacyDisplay.cpp for how it drives the code the other tests here and in TegraDisplayFlip/test
// exercise: those tests run TegraScanoutRing.h, VsyncPhase.h and DisplaySmmuProbe.h against models of DC A, but the
// display's own wiring (when direct scanout starts, where swaps retire, which registers the vsync phase writes, how the
// probe is held and aborted) only exists in the driver's source.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
const std::string &display()
{
    static const std::string source = SourceText("../NativeLegacyDisplay.cpp");
    return source;
}

// A member function's body: from its signature to the first line that is just "    }".
std::string method(const std::string &source, const char *signature)
{
    return Between(source, signature, "\n    }\n");
}

// The text after the first `start` to the end of the file.
std::string after(const std::string &source, const char *start)
{
    const size_t at = source.find(start);
    if (at == std::string::npos) {
        ADD_FAILURE() << "not found: " << start;
        return {};
    }
    return source.substr(at + std::string(start).size());
}

// `first` occurs in `text`, and before `second`.
void expectBefore(const std::string &text, const char *first, const char *second)
{
    const size_t a = text.find(first), b = text.find(second);
    ASSERT_NE(a, std::string::npos) << first;
    ASSERT_NE(b, std::string::npos) << second;
    EXPECT_LT(a, b) << first << " before " << second;
}
}  // namespace

// Direct scanout (r14-direct) follows the policy TegraDisplayFlip/test:direct_policy_test proves safe: direct frames
// only after the SMMU probe passed, a fence-only GPU present per frame, retirement at the latch in queue order never
// past the frame in flight, no early retire or collapse once direct scanout is on, layouts never replaced across pitch
// and block-linear.
TEST(DirectScanoutSource, OnlyAfterTheProbePassed)
{
    EXPECT_THAT(method(display(), "    bool startDirect()\n"),
                HasSubstr("directAttached = directOn = latchRetire = true"));
    EXPECT_THAT(display(), HasSubstr("d->probe.stage == DisplaySmmuProbe::kDone && d->startDirect()"));
}

TEST(DirectScanoutSource, FenceOnlyPresentNoRetireAtTheOffer)
{
    const std::string offer = method(display(), "    bool offerDirect(");
    EXPECT_THAT(offer, HasSubstr("TEGRA_SCANOUT_COPY_FENCE_ONLY"));
    EXPECT_THAT(offer, Not(HasSubstr("retireSwap")));
}

TEST(DirectScanoutSource, RetireAtTheLatchInQueueOrder)
{
    const std::string retire = method(display(), "    void retireThrough(uint64_t seq, uint64_t shownAt)\n");
    EXPECT_THAT(retire, HasSubstr("at > seq"));
    EXPECT_THAT(retire, HasSubstr("head == gpuRequest"));
    EXPECT_THAT(display(), HasSubstr("retireThrough(entrySeq[asked], stamp)"));
    EXPECT_THAT(display(), HasSubstr("retireThrough(entrySeq[replaced], stamp)"));
}

// Every present of a swap reaches its frame-info clients (the swap's entry first, at the latch's stamp), in both
// retire modes: frameShown comes before retireThrough's latchRetire check.
TEST(DirectScanoutSource, FrameInfoAtEveryPresent)
{
    const std::string retire = method(display(), "    void retireThrough(uint64_t seq, uint64_t shownAt)\n");
    EXPECT_LT(retire.find("frameShown(seq, shownAt);"), retire.find("if (!latchRetire || !seq)"));
    EXPECT_THAT(method(display(), "    void noteTaken("), HasSubstr("noteFrameInfoTaken(request)"));
    EXPECT_THAT(method(display(), "    void frameShown("), HasSubstr("native_legacy_frame_info_notify(this, &frame)"));
    EXPECT_THAT(method(display(), "    IOReturn sendFrameInfo("),
                HasSubstr("native_legacy_frame_info_enqueue(owner, words, n * sizeof(words[0]))"));
}

TEST(DirectScanoutSource, NoEarlyRetireOrCollapseOnceRetiringAtTheLatch)
{
    EXPECT_THAT(display(), HasSubstr("if (!latchRetire && retireSwap(request, true))"));
    EXPECT_THAT(display(), HasSubstr("asyncCopy == 2 && !latchRetire && retireSwap"));
    EXPECT_THAT(display(), HasSubstr("if (!latchRetire) {"));
    expectBefore(method(display(), "    void finishTransfer("), "if (latchRetire)", "retireSwap");
}

TEST(DirectScanoutSource, NeverReplacedAcrossLayouts)
{
    EXPECT_THAT(method(display(), "    void presentEntry(int i)\n"), HasSubstr("sameLayout(i, ring.pending)"));
}

// A surface the GPU renders tiled is scanned block-linear in place, or copied as a block-linear source; never as pitch.
TEST(DirectScanoutSource, TiledSurfacesScanBlockLinear)
{
    EXPECT_THAT(
        method(display(), "    void layoutOf(int i, uint32_t &kind, uint32_t &stride, uint32_t &hOffset) const\n"),
        HasSubstr("kind = slots[i - kSlot0].tiled ? kBlockSurfaceKind : 0;"));
    EXPECT_THAT(method(display(), "    bool offerGpuCopy("),
                HasSubstr("copy.flags = tiled ? TEGRA_SCANOUT_COPY_SOURCE_BLOCK : 0;"));
    expectBefore(after(display(), "    void kickOnce()"), "const bool tiled = surfaceTiled(memory);",
                 "offerDirect(request, memory, sourceStride, allocation, width, height, tiled, kickAt)");
    // the CPU's fallback copies a tiled frame as it is: it is already in the scanout's block layout
    EXPECT_THAT(method(display(), "    Swizzled swizzle("), HasSubstr("memcpy(out, source, kNativeBlockBytes);"));
    EXPECT_THAT(
        after(display(), "    void kickOnce()"),
        HasSubstr("swizzle(drawSurface(), sourceAddress, source->getLength(), sourceStride, width, height, tiled)"));
}

// Every flip, latch check and copy target goes through TegraScanoutRing (TegraDisplayFlip/test:scanout_ring_test): the
// two-buffer state is gone.
TEST(ScanoutRingSource, DisplayUsesTheRing)
{
    EXPECT_THAT(display(), HasSubstr("#include \"../TegraDisplayFlip/TegraScanoutRing.h\""));
    for (const char *gone : {"flipPending", "front ^ 1", "front ^= 1"})
        EXPECT_THAT(display(), Not(HasSubstr(gone)));
    // r14-direct: scan buffers and direct slots share presentEntry.
    EXPECT_THAT(method(display(), "    void flip()\n"), HasSubstr("presentEntry(target)"));
    EXPECT_THAT(method(display(), "    void presentEntry(int i)\n"), HasSubstr("ring.ready("));
    const std::string check = method(display(), "    void checkFlip(bool atFrameEnd)\n");
    EXPECT_THAT(check, HasSubstr("ring.latch("));
    // A flip never taken (a lost frame end) is still one line and a panic.
    EXPECT_THAT(check, HasSubstr("panic("));
    // A START_ADDR the model does not explain while flips are replaced is one line and a fallback (never-booted rule);
    // only once double-buffered again is it the old panic.
    EXPECT_THAT(check, HasSubstr("recoverFlip("));
    EXPECT_THAT(check, HasSubstr("if (!ring.replace)"));
}

// The vsync phase (r17-touch, NativeMobileDisplay/test:vsync_phase_test runs VsyncPhase.h): V_PULSE2 programmed as L4T's
// tegra_dc_init_vpulse2_int does, through assembly registers only (the flips' own GENERAL_ACT_REQ latches them), at most
// one vsync a frame, V_BLANK carrying it until V_PULSE2 has fired, a never-firing V_PULSE2 turning the phase off with
// one line (no panic), and the pacing line's "rate" cause for promptly answered swaps.
TEST(VsyncPhaseSource, Vpulse2RegistersAsL4T)
{
    // L4T dc_reg.h / tegra_dc_init_vpulse2_int (T21x).
    for (const char *line :
         {"kVpulse2Int = 1u << 5", "kDcDispSignalOptions0 = 0x400", "kVpulse2Enable = 1u << 19",
          "kDcDispVpulse2Control = 0x422", "kDcDispVpulse2Position = 0x423", "kVpulse2Last = 1u << 8"})
        EXPECT_THAT(display(), HasSubstr(line));
    const std::string registers = method(display(), "    void writePhaseRegisters()");
    for (const std::string &code : {method(display(), "    void programPhase(uint32_t leadUs)"), registers}) {
        EXPECT_THAT(code, Not(HasSubstr("kDcStateControl"))) << "the phase never asks for a latch of its own";
        EXPECT_THAT(code, Not(HasSubstr("kDcRegActControl")));
    }
    EXPECT_THAT(registers, HasSubstr("phaseLine | (phaseLine + 1) << 16"));
}

TEST(VsyncPhaseSource, OneVsyncAFrame)
{
    const std::string deliver =
        method(display(), "    void deliverVsync(uint64_t stamp, uint64_t continuous, uint32_t lead)");
    EXPECT_THAT(deliver, HasSubstr("if (deliveredFrame == frameEnds)\n            return;"));
    EXPECT_THAT(method(display(), "    void vblankNotify()"), HasSubstr("if (phaseDelivering())\n            return;"));
    EXPECT_THAT(method(display(), "    bool phaseDelivering() const"), HasSubstr("vpulseSeen"));
    // The irregular gap at a source or line change is never passed on as the interval.
    EXPECT_THAT(deliver, HasSubstr("!moved && vblankLast"));
    // A folded action (V_PULSE2 and the frame end together) counts the vsync against that frame, not the next.
    expectBefore(method(display(), "    void vblankTick()"), "if (status & kVpulse2Int)\n            vpulseNotify();",
                 "if (status & kFrameEndInt)\n            frameEnds = frameEnds + 1;");
}

TEST(VsyncPhaseSource, NeverFiringVpulse2CostsOneLine)
{
    const std::string present = method(display(), "    void notePhasePresent(uint64_t stamp, const FrameMark &mark)");
    EXPECT_THAT(present, HasSubstr("phaseFailed = true;"));
    EXPECT_THAT(present, Not(HasSubstr("panic("))) << "a V_PULSE2 that never fired costs one line, not the boot";
    EXPECT_THAT(present, HasSubstr("IOLog(\"SwitchDisplay: vsync phase off for this boot"));
    EXPECT_THAT(method(display(), "    unsigned noteLong(uint32_t frames, const FrameMark &mark)"),
                HasSubstr("kLongRate"));
}

TEST(VsyncPhaseSource, ReplacementsAndResume)
{
    // noteStatsWindow zeroes stReplaced each window: that is no replacement.
    expectBefore(after(display(), "    void notePresent(uint64_t stamp, uint64_t latchedAt, const FrameMark &mark)"),
                 "if (stReplaced < phaseReplacedSeen)\n            phaseReplacedSeen = 0;",
                 "if (stReplaced != phaseReplacedSeen)");
    // SC7 resets the DC: the phase's registers go back before finishResume's latch request.
    expectBefore(after(display(), "    bool finishResume()"), "writePhaseRegisters()", "kGeneralActReq | kWinAActReq");
}

// The display's SMMU probe runs through the same DisplaySmmuProbe::perform that display_probe_test drives, is held but
// bounded, aborted when the display suspends, stops or goes off, and stays under the flip deadline.
TEST(DisplayProbeSource, Wiring)
{
    EXPECT_THAT(display(), HasSubstr("probe.perform("));
    EXPECT_THAT(display(), HasSubstr("abortProbe(\"the display suspended (SC7)\")"));
    EXPECT_THAT(display(), HasSubstr("abortProbe(\"the display stopped\")"));
    const std::string kick = method(display(), "    void kickOnce()\n");
    EXPECT_THAT(kick, HasSubstr("(probe.active() || probeRequest) && !releaseProbeHold()"));  // held, but bounded
    const std::string release = method(display(), "    bool releaseProbeHold()\n");
    EXPECT_THAT(release, HasSubstr("kProbeHoldMs"));
    EXPECT_THAT(release, HasSubstr("abortProbe("));
    // No frame end comes with the scan stopped.
    EXPECT_THAT(method(display(), "    bool setScanout(bool on)\n"), HasSubstr("abortProbe(\"the display went off\")"));
}

TEST(DisplayProbeSource, AcceptedSwapTableNeverDropsThePendingHead)
{
    const std::string add = method(display(), "    bool addValidated(");
    EXPECT_THAT(add, Not(testing::ContainsRegex("if \\(slot == 8\\)[[:space:]]+slot = 0;")));
    EXPECT_THAT(add, HasSubstr("validatedAt[i] < validatedAt[slot]"));
    EXPECT_THAT(method(display(), "    void kickOnce()\n"), HasSubstr("pruneValidated();"));
}

TEST(DisplayProbeSource, ProbeRequestUnderTheFlipDeadline)
{
    // A probe START_ADDR request is under the flip deadline like a flip: a lost DC A interrupt panics, never freezes.
    EXPECT_THAT(method(display(), "    void checkFlipDeadline()\n"), HasSubstr("!probeRequest"));
    EXPECT_THAT(Between(display(), "struct ProbeIo", "};"), HasSubstr("flipTimer->setTimeoutMS(kFlipDeadlineMS)"));
}

// The panic-time screen restore works in every kernel config: PE_arm_debug_panic_hook is a const NULL outside DEVELOPMENT
// and DEBUG, so the driver hangs it on SwitchDiag's switch_display_panic_hook, which switch_diag_panic calls before
// anything draws the panic screen (the first call blacks the pitch area) and switch_panic_halted calls again.
TEST(PanicRestoreSource, EveryKernelConfig)
{
    EXPECT_THAT(display(), Not(HasSubstr("PE_arm_debug_panic_hook")));
    EXPECT_THAT(display(), Not(HasSubstr("DEVELOPMENT")));
    const std::string chain = method(display(), "    void chainPanicHook()\n");
    expectBefore(chain, "gPanicRestore.start = surfaceAddress(0);", "dmb ish");
    expectBefore(chain, "dmb ish", "switch_display_panic_hook = restorePitchOnPanic;");
    static const std::string diag = SourceText("../../../diagnostics/switch/SwitchDiag.c");
    const std::string panic = Between(diag, "void switch_diag_panic(const char *format", "\n}\n");
    expectBefore(panic, "if (display)\n        display(format);", "switch_panic_begin(");
    expectBefore(panic, "if (display)\n        display(format);", "switch_diag_pixel(");
    static const std::string save = SourceText("../../../diagnostics/switch/SwitchPanicSave.c");
    const std::string halted = Between(save, "void switch_panic_halted(void)\n", "\n}\n");
    EXPECT_THAT(halted, HasSubstr("if (display)\n        display(\"\");"));
    EXPECT_THAT(halted, Not(HasSubstr("PE_panic_hook")));
}

// A window with vsyncs sent and nothing swapped or presented is logged every time, before the window's counters are
// reset: the quiet-window suppression would otherwise hide a hung render-server client after its first window.
TEST(DisplayStatsSource, QuietDisplayWindowsAreLogged)
{
    const std::string stats = method(display(), "    void noteStatsWindow()");
    EXPECT_THAT(stats, HasSubstr("if (stVsyncs && !stSwaps && !stPresents)"));
    expectBefore(stats, "SwitchDisplay: quiet display: ", "stSwaps = stVsyncs = ");
    expectBefore(stats, "SwitchDisplay: quiet display: ", "if (logged)\n            os_log(");
}

// An idle display wakes no core: FRAME_END reaches the CPU only while something waits for a frame end. Holders outside the
// gate (TegraPanel, the colour unit) write INT_MASK too, so every INT_MASK write is one of the three under its lock; a kick
// unmasks it before any work that waits for a frame end; the last frame end with nothing waiting masks it.
TEST(FrameEndSource, MaskedWhileNothingWaits)
{
    const std::string &source = display();
    size_t writes = 0;
    for (size_t at = source.find("dc[kDcIntMask] ="); at != std::string::npos;
         at = source.find("dc[kDcIntMask] =", at + 1))
        ++writes;
    EXPECT_EQ(writes, 3u);
    for (const char *helper : {"    void writeIntMask(uint32_t clear, uint32_t set)", "    void unmaskFrameEnd()",
                               "    void maskIdleFrameEnd()"})
        EXPECT_THAT(method(source, helper), HasSubstr("IOSimpleLockLockDisableInterrupt(intMaskLock)")) << helper;
    EXPECT_THAT(method(source, "    void kickSwapQueue() override"), HasSubstr("{\n        unmaskFrameEnd();"));
    EXPECT_THAT(method(source, "    void vblankTick()"),
                testing::EndsWith("if ((status & kFrameEndInt) && !frameEndNeeded() && tegra_panel_holds_frame_end())\n"
                                  "            maskIdleFrameEnd();"));
    EXPECT_THAT(method(source, "    void maskIdleFrameEnd()"), HasSubstr("if (!__atomic_load_n(&gFrameEndHolds"));
}

// A blank swap blacks the head through the colour unit only while no flip is out, and the first flip latched after it
// ends the black: an older flip's latch must never show a frame the blank swap replaced.
TEST(BlankSource, ColourBlankEndsAtTheNextLatch)
{
    EXPECT_THAT(display(), HasSubstr("rotated && rotationLatched && doubleBuffer && ring.pending == kNoBuffer &&"));
    EXPECT_THAT(
        method(display(), "    void flipLatched()"),
        HasSubstr("{\n        if (blanked) {\n            blanked = false;\n            color->setBlank(false);"));
}

// iOS 17's layout of the original framebuffer is the one this driver always ran on: its table must keep these values, or
// an iOS 17 kernel would read the swap queue and the swap requests somewhere else.
TEST(LegacyFramebufferTable, Ios17ValuesStayPut)
{
    const std::string table = SourceText("../../../kits/SwitchABI/kinds/21H461/IOMobileFramebufferLegacy.json");
    const std::string constants = Between(table, "\"constants\": {", "}");
    for (const char *entry :
         {"\"swapQueueCurrent\": \"0x1560\"", "\"swapQueueHead\": \"0x1568\"", "\"swapQueueTail\": \"0x1570\"",
          "\"swapNext\": \"0xb90\"", "\"swapPrevious\": \"0xb98\"", "\"swapSourceRects\": \"0x11\"",
          "\"swapDestinationRects\": \"0x51\"", "\"swapTransforms\": \"0x244\"", "\"swapUpdated\": \"0x318\"",
          "\"swapReady\": \"0x31c\"", "\"swapBackground\": \"0x320\"", "\"swapOptions\": \"0x324\"",
          "\"swapSize\": \"0x330\"", "\"swapSurfaces\": \"0xa40\"", "\"swapEffectiveSurfaces\": \"0xa60\"",
          "\"vsyncClientList\": \"0x15d8\"", "\"swapsCompleted\": \"0x16bc\"", "\"swapsPresented\": \"0x16c0\"",
          "\"framesCompleted\": \"0xfc\""})
        EXPECT_THAT(constants, HasSubstr(entry));
    const std::string fields = Between(table, "\"fields\": {", "}");
    EXPECT_THAT(fields, HasSubstr("\"displayWidth\": \"0x16b0\""));
    EXPECT_THAT(fields, HasSubstr("\"displayHeight\": \"0x16b4\""));
    EXPECT_THAT(table, HasSubstr("\"size\": \"0x18a0\""));
    EXPECT_THAT(table, HasSubstr("\"slots\": 358"));
}

// The slots iPadOS reaches through IOMobileFramebuffer that this display answers as J120's AppleCLCD does (21H461
// kernelcache: AppleCLCD's vtable at 0xfffffff006fd1358; the family's callers named in the interface).
TEST(LegacyFramebufferTable, ContractSlotsOverridden)
{
    const std::string table = SourceText("../../../kits/SwitchABI/kinds/21H461/IOMobileFramebufferLegacy.json");
    const std::string layout = Between(table, "\"layout\": {", "}");
    for (const char *entry :
         {"\"211\": \"changeFrameInfo\"", "\"213\": \"supportedFrameInfo\"", "\"274\": \"setDisplayTemperature\"",
          "\"306\": \"platformFrameInfoNotify\"", "\"307\": \"sendFrameInfo\""})
        EXPECT_THAT(layout, HasSubstr(entry));
    const std::string source = SourceText("../NativeLegacyDisplay.cpp");
    for (const char *method : {"    IOReturn changeFrameInfo(uint32_t type, void *client, uint32_t mask) override",
                               "    IOReturn sendFrameInfo(void *client, const void *frame) override",
                               "    IOReturn setDisplayTemperature(const uint32_t *temperatures) override"})
        EXPECT_THAT(source, HasSubstr(method));
    EXPECT_THAT(Between(source, "    IOReturn setDisplayTemperature(", "\n    }\n"),
                HasSubstr("return kIOReturnSuccess;"));
}
