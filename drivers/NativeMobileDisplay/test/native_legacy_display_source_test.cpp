// Guards on NativeLegacyDisplay.cpp for how it drives the code the other tests here and in TegraDisplayFlip/test
// exercise: those tests run TegraScanoutRing.h, VsyncPhase.h and DisplaySmmuProbe.h against models of DC A, but the
// display's own wiring (when direct scanout starts, where swaps retire, which registers the vsync phase writes, how the
// probe is held and aborted) only exists in the driver's source.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {
// NativeLegacyDisplay.cpp with the headers its class body includes for scanout, retirement, resume and the frame end.
const std::string &display()
{
    static const std::string source =
        SourceText("../NativeLegacyDisplay.cpp") + SourceText("../SwitchScanoutStart.h") +
        SourceText("../SwitchDirectSlots.h") + SourceText("../SwitchRetireDisplay.h") +
        SourceText("../SwitchDisplayResume.h") + SourceText("../SwitchFrameEndDisplay.h") +
        SourceText("../SwitchGpuDoneDisplay.h") + SourceText("../SwitchFlipDeadlineDisplay.h");
    return source;
}

const CodeText &code()
{
    static const CodeText text(display());
    return text;
}

CodeText method(const char *signature)
{
    return Block(display(), signature);
}
}  // namespace

// Direct scanout follows the policy TegraDisplayFlip/test:direct_policy_test proves safe: a fence-only GPU present
// per frame, retirement at the latch in queue order never
// past the frame in flight, no early retire or collapse once direct scanout is on, layouts never replaced across pitch
// and block-linear.
TEST(DirectScanoutSource, ProductionInitializesBeforeEnablingScanout)
{
    const CodeText setup(SourceText("../SwitchScanoutSetup.h"));
    EXPECT_TRUE(code().Before("startGpuCopy();", "->initializeScanout();"));
    EXPECT_TRUE(setup.Before("otherWindowEnabled()", "tegra_smmu_attach("));
    EXPECT_TRUE(setup.Before("probeDomain.identity(", "tegra_smmu_attach("));
    EXPECT_TRUE(setup.Before("probeDomain.reserve(kPlaneSlot0", "tegra_smmu_attach("));
    EXPECT_TRUE(setup.Before("tegra_smmu_attach(", "directAttached = planesAttached = latchRetire = true;"));
    EXPECT_TRUE(setup.Has("probeRan = planeProbeRan = true;"));
    EXPECT_FALSE(setup.Has("probe.start()"));
    EXPECT_TRUE(
        method("void probeFrameEnd()").Has("#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)"));
}

TEST(DirectScanoutSource, FenceOnlyPresentNoRetireAtTheOffer)
{
    const CodeText offer = method("bool offerDirect(");
    EXPECT_TRUE(offer.Has("TEGRA_SCANOUT_COPY_FENCE_ONLY"));
    EXPECT_FALSE(offer.Has("retireSwap"));
}

TEST(DirectScanoutSource, RetireAtTheLatchInQueueOrder)
{
    const CodeText retire = method("void retireThrough(uint64_t seq, uint64_t shownAt)");
    EXPECT_TRUE(retire.Has("at > seq"));
    EXPECT_TRUE(retire.Has("head == gpuRequest"));
    EXPECT_TRUE(code().Has("retireThrough(entrySeq[asked], stamp)"));
    EXPECT_TRUE(code().Has("retireThrough(entrySeq[replaced], stamp)"));
}

// Every present of a swap reaches its frame-info clients (the swap's entry first, at the latch's stamp), in both
// retire modes: frameShown comes before retireThrough's latchRetire check.
TEST(DirectScanoutSource, FrameInfoAtEveryPresent)
{
    EXPECT_TRUE(method("void retireThrough(uint64_t seq, uint64_t shownAt)")
                    .Before("frameShown(seq, shownAt);", "if (!latchRetire || !seq)"));
    EXPECT_TRUE(method("void noteTaken(").Has("noteFrameInfoTaken(request)"));
    EXPECT_TRUE(method("void frameShown(").Has("native_legacy_frame_info_notify(this, &frame)"));
    EXPECT_TRUE(
        method("IOReturn sendFrameInfo(").Has("native_legacy_frame_info_enqueue(owner, words, n * sizeof(words[0]))"));
}

TEST(DirectScanoutSource, NoEarlyRetireOrCollapseOnceRetiringAtTheLatch)
{
    EXPECT_TRUE(code().Has("if (!latchRetire && retireSwap(request, true))"));
    EXPECT_TRUE(code().Has("asyncCopy == 2 && !latchRetire && sourceWait->read.lease && retireSwap"));
    EXPECT_TRUE(code().Has("if (!latchRetire) {"));
    EXPECT_TRUE(method("void finishTransfer(").Before("if (latchRetire)", "retireSwap"));
}

// The GPU driver owns an accepted offer's deadline (TegraScanoutCopy.h): the display waits while the driver holds it, and a
// frame whose rendering was lost is neither flipped to nor copied on the CPU.
TEST(DirectScanoutSource, LateOffersAreTheDriversAndLostFramesAreNotShown)
{
    const CodeText deadline = method("void gpuDeadlineCheck()");
    EXPECT_TRUE(deadline.Before("tegra_gm20b_scanout_outstanding(this)", "panic("));
    EXPECT_TRUE(deadline.Has("fence only, direct slot"));
    EXPECT_TRUE(method("void finishGpuCopy(int status)")
                    .Before("if (status == TEGRA_SCANOUT_SOURCE_LOST) {", "gpuBypass = true;"));
    const CodeText drop = method("void dropLostFrame(");
    EXPECT_TRUE(drop.Has("retireUnshown(seqOf(request));"));
    EXPECT_FALSE(drop.Has("gpuBypass"));
    EXPECT_FALSE(drop.Has("presentEntry"));
}

// A direct slot keeps what window A may scan from it until the slot is dropped: the surface, its wired memory and its
// layout (the GPU driver moves a released surface's texels back to rows otherwise, under the window).
TEST(DirectScanoutSource, SlotsHoldTheSurfaceTheyScan)
{
    const CodeText make = method("int directSlotFor(");
    EXPECT_TRUE(make.Has("surface->retain();"));
    EXPECT_TRUE(make.Has("holdLayout(slot, tiled)"));
    EXPECT_TRUE(make.Has("*slot.read = snapshot;"));
    EXPECT_TRUE(method("void checkFlip(bool atFrameEnd)")
                    .Before("releaseIdleDirectReads();", "retireThrough(entrySeq[asked], stamp);"));
    EXPECT_TRUE(method("static bool holdLayout(").Has("tegra_gm20b_surface_hold("));
    const CodeText drop = method("void dropSlot(unsigned k)");
    EXPECT_TRUE(drop.Before("probeDomain.unmap(", "tegra_gm20b_surface_unhold(slot.layout);"));
    EXPECT_TRUE(drop.Before("tegra_gm20b_surface_unhold(slot.layout);", "OSSafeReleaseNULL(slot.surface);"));
}

TEST(DirectScanoutSource, NeverReplacedAcrossLayouts)
{
    EXPECT_TRUE(method("void presentEntry(int i)").Has("sameLayout(i, ring.pending)"));
}

// A surface the GPU renders tiled is scanned block-linear in place, or copied as a block-linear source; never as pitch.
TEST(DirectScanoutSource, TiledSurfacesScanBlockLinear)
{
    EXPECT_TRUE(method("void layoutOf(int i, uint32_t &kind, uint32_t &stride, uint32_t &hOffset) const")
                    .Has("kind = slots[i - kSlot0].tiled ? kBlockSurfaceKind : 0;"));
    const CodeText copy = method("bool offerGpuCopy(");
    EXPECT_TRUE(copy.Has("copy.flags = blockLog2 >= 0 ? TEGRA_SCANOUT_COPY_SOURCE_BLOCK : 0;"));
    EXPECT_TRUE(copy.Has("copy.sourceBlockHeightLog2 = blockLog2 >= 0 ? static_cast<uint32_t>(blockLog2) : 0;"));
    const CodeText kick = method("void kickOnce()");
    EXPECT_TRUE(
        kick.Before("const int blockLog2 = surfaceBlockLog2(memory);",
                    "offerDirect(request, surface, memory, sourceStride, allocation, width, height, tiled, kickAt)"));
    // window A scans, and the CPU's fallback copies as it is, only a frame in window A's own blocks; a frame tiled in
    // other blocks takes the GPU copy (which reads its layout) or is not shown
    EXPECT_TRUE(kick.Before("const bool asScanned = !tiled || blockLog2 == static_cast<int>(kScanBlockLog2);",
                            "if (direct && asScanned &&"));
    EXPECT_TRUE(kick.Before("offerGpuCopy(request, memory, sourceStride, allocation, width, height, blockLog2, kickAt)",
                            "native_legacy_transfer_observed(kIOReturnUnsupported, request, 0);"));
    EXPECT_TRUE(kick.Before("if (!asScanned) {", "native_legacy_gpu_offer(this, memory, sourceStride, width, height)"));
    EXPECT_TRUE(method("static Swizzled swizzle(").Has("memcpy(out, source, kNativeBlockBytes);"));
    EXPECT_TRUE(
        kick.Has("swizzle(drawSurface(), sourceAddress, source->getLength(), sourceStride, width, height, tiled)"));
}

// Every flip, latch check and copy target goes through TegraScanoutRing (TegraDisplayFlip/test:scanout_ring_test): the
// two-buffer state is gone.
TEST(ScanoutRingSource, DisplayUsesTheRing)
{
    EXPECT_TRUE(code().Has("#include \"../TegraDisplayFlip/TegraScanoutRing.h\""));
    for (const char *gone : {"flipPending", "front ^ 1", "front ^= 1"})
        EXPECT_FALSE(code().Has(gone));
    // r14-direct: scan buffers and direct slots share presentEntry.
    EXPECT_TRUE(method("void flip()").Has("presentEntry(target)"));
    EXPECT_TRUE(method("void presentEntry(int i)").Has("ring.ready("));
    const CodeText check = method("void checkFlip(bool atFrameEnd)");
    EXPECT_TRUE(check.Has("ring.latch("));
    // A flip never taken (a lost frame end) is still one line and a panic.
    EXPECT_TRUE(check.Has("panic("));
    // A START_ADDR the model does not explain while flips are replaced is one line and a fallback (never-booted rule);
    // only once double-buffered again is it the old panic.
    EXPECT_TRUE(check.Has("recoverFlip("));
    EXPECT_TRUE(check.Has("if (!ring.replace)"));
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
        EXPECT_TRUE(code().Has(line));
    const CodeText registers = method("void writePhaseRegisters()");
    for (const CodeText &phase : {method("void programPhase(uint32_t leadUs)"), registers}) {
        EXPECT_FALSE(phase.Has("kDcStateControl")) << "the phase never asks for a latch of its own";
        EXPECT_FALSE(phase.Has("kDcRegActControl"));
    }
    EXPECT_TRUE(registers.Has("phaseLine | (phaseLine + 1) << 16"));
}

TEST(VsyncPhaseSource, OneVsyncAFrame)
{
    const CodeText deliver = method("void deliverVsync(uint64_t stamp, uint64_t continuous, uint32_t lead)");
    EXPECT_TRUE(deliver.Has("if (deliveredFrame == frameEnds) return;"));
    EXPECT_TRUE(method("void vblankNotify()").Has("if (phaseDelivering()) return;"));
    EXPECT_TRUE(method("bool phaseDelivering() const").Has("vpulseSeen"));
    // The irregular gap at a source or line change is never passed on as the interval.
    EXPECT_TRUE(deliver.Has("!moved && vblankLast"));
    // A folded action (V_PULSE2 and the frame end together) counts the vsync against that frame, not the next.
    EXPECT_TRUE(method("void vblankTick()")
                    .Before("if (status & kVpulse2Int) vpulseNotify();",
                            "if (status & kFrameEndInt) frameEnds = frameEnds + 1;"));
}

// A failed change of mode, a refused completion flip or an underflow run holds no route off: whether landscape, direct
// scanout and completion flips are tried reads no failure state.
TEST(DisplayRoutesSource, NoFailureHoldsARouteOff)
{
    const CodeText selection(SourceText("../SwitchLandscapeSelection.h"));
    EXPECT_TRUE(selection.Has("return planesReady() && directAttached && !landscapeRunHoldsPortrait();"));
    EXPECT_TRUE(
        CodeText(SourceText("../SwitchScanoutStart.h")).Has("bool directScanChosen() const { return directOn; }"));
    EXPECT_FALSE(code().Has("Hold.on()"));
}

TEST(VsyncPhaseSource, NeverFiringVpulse2CostsOneLine)
{
    const CodeText present = method("void notePhasePresent(uint64_t stamp, const FrameMark &mark)");
    EXPECT_TRUE(present.Has("if (vpulseProgrammed && !vpulseSeen && dc && !hwSuspended) {"))
        << "DC A read only while probing, and never powered down";
    EXPECT_TRUE(present.Has("phaseProbe.flip(true, vpulse2Routed(), false)"));
    EXPECT_TRUE(present.Has("phaseProbeEnded(step);"));
    EXPECT_FALSE(present.Has("panic(")) << "a V_PULSE2 that never fired costs one line, not the boot";
    const CodeText ended = method("void phaseProbeEnded(VsyncPhaseProbe::Step step)");
    EXPECT_TRUE(ended.Has("vsync at V_BLANK"));
    EXPECT_TRUE(ended.Has("dropVpulse2();"));
    EXPECT_TRUE(method("void dropVpulse2()").Has("vpulseProgrammed = false;"));
    EXPECT_TRUE(method("unsigned noteLong(uint32_t frames, const FrameMark &mark)").Has("kLongRate"));
}

// The probe asks whether V_PULSE2 reaches the CPU, so it only counts flips with V_PULSE2 routed (enabled and unmasked):
// V_PULSE2 is programmed only with a vsync client and unmasked in the same step, and goes with the last client, so a
// masked V_PULSE2 never fails a probe.
TEST(VsyncPhaseSource, Vpulse2IsProbedOnlyWhileRouted)
{
    const CodeText program = method("void programPhase(uint32_t leadUs)");
    EXPECT_TRUE(program.Has("(!vpulseProgrammed && !vsyncArmed)"));
    EXPECT_TRUE(program.Before("dc[kDcIntEnable] = dc[kDcIntEnable] | kVpulse2Int;", "writeIntMask(0, kVpulse2Int);"));
    EXPECT_FALSE(program.Has("else armVsync unmasks it"));
    EXPECT_TRUE(method("bool vpulse2Routed()").Has("dc[kDcIntEnable] & dc[kDcIntMask] & kVpulse2Int"));
    const CodeText notify = method("void vblankNotify()");
    EXPECT_TRUE(notify.Before("writeIntMask(kVblankInt, 0);", "dropVpulse2();"));
    EXPECT_TRUE(method("bool phaseDelivering() const").Has("vpulseProgrammed"));
}

TEST(VsyncPhaseSource, ReplacementsAndResume)
{
    // noteStatsWindow zeroes stReplaced each window: that is no replacement.
    EXPECT_TRUE(method("void notePresent(uint64_t stamp, uint64_t latchedAt, const FrameMark &mark)")
                    .Before("if (stReplaced < phaseReplacedSeen) phaseReplacedSeen = 0;",
                            "if (stReplaced != phaseReplacedSeen)"));
    // SC7 resets the DC: the phase's registers go back before finishResume's latch request.
    EXPECT_TRUE(method("bool finishResume()").Before("writePhaseRegisters()", "kGeneralActReq | kWinAActReq"));
}

// The display's SMMU probe runs through the same DisplaySmmuProbe::perform that display_probe_test drives, is held but
// bounded, aborted when the display suspends, stops or goes off, and stays under the flip deadline.
TEST(DisplayProbeSource, Wiring)
{
    EXPECT_TRUE(code().Has("probe.perform("));
    EXPECT_TRUE(code().Has("abortProbe(\"the display suspended (SC7)\")"));
    EXPECT_TRUE(code().Has("abortProbe(\"the display stopped\")"));
    const CodeText kick = method("void kickOnce()");
    EXPECT_TRUE(kick.Has("(probe.active() || probeRequest) && !releaseProbeHold()"));  // held, but bounded
    const CodeText release = method("bool releaseProbeHold()");
    EXPECT_TRUE(release.Has("kProbeHoldMs"));
    EXPECT_TRUE(release.Has("abortProbe("));
    // No frame end comes with the scan stopped.
    EXPECT_TRUE(method("bool setScanout(bool on)").Has("abortProbe(\"the display went off\")"));
}

TEST(DisplayProbeSource, AcceptedSwapTableNeverDropsThePendingHead)
{
    const CodeText add = method("int addValidated(");
    EXPECT_FALSE(add.Has("if (slot == 8) slot = 0;"));
    EXPECT_TRUE(add.Has("validatedAt[i] < validatedAt[slot]"));
    EXPECT_TRUE(method("void kickOnce()").Has("pruneValidated();"));
}

TEST(DisplayProbeSource, ProbeRequestUnderTheFlipDeadline)
{
    // A probe START_ADDR request is under the flip deadline like a flip: a lost DC A interrupt panics, never freezes.
    EXPECT_TRUE(method("void checkFlipDeadline()").Has("!probeRequest"));
    EXPECT_TRUE(method("struct ProbeIo").Has("flipTimer->setTimeoutMS(kFlipDeadlineMS)"));
}

// The panic-time screen restore works in every kernel config: PE_arm_debug_panic_hook is a const NULL outside DEVELOPMENT
// and DEBUG, so the driver hangs it on SwitchDiag's switch_display_panic_hook, which switch_diag_panic calls before
// anything draws the panic screen (the first call blacks the pitch area) and switch_panic_halted calls again.
TEST(PanicRestoreSource, EveryKernelConfig)
{
    EXPECT_FALSE(code().Has("PE_arm_debug_panic_hook"));
    EXPECT_FALSE(code().Between("PanicRestore gPanicRestore;", "inline uint64_t counterNow()").Has("DEVELOPMENT"));
    const CodeText chain = method("void chainPanicHook()");
    EXPECT_FALSE(chain.Has("DEVELOPMENT"));
    EXPECT_TRUE(chain.Before("gPanicRestore.start = surfaceAddress(0);", "dmb ish"));
    EXPECT_TRUE(chain.Before("dmb ish", "switch_display_panic_hook = restorePitchOnPanic;"));
    const CodeText panic =
        Block(SourceText("../../../diagnostics/switch/SwitchDiag.c"), "void switch_diag_panic(const char *format");
    EXPECT_TRUE(panic.Before("if (display) display(format);", "switch_panic_begin("));
    EXPECT_TRUE(panic.Before("if (display) display(format);", "switch_diag_pixel("));
    const CodeText halted =
        Block(SourceText("../../../diagnostics/switch/SwitchPanicSave.c"), "void switch_panic_halted(void)");
    EXPECT_TRUE(halted.Has("if (display) display(\"\");"));
    EXPECT_FALSE(halted.Has("PE_panic_hook"));
}

// A window with vsyncs sent and nothing swapped or presented is logged every time, before the window's counters are
// reset: the quiet-window suppression would otherwise hide a hung render-server client after its first window.
TEST(DisplayStatsSource, QuietDisplayWindowsAreLogged)
{
    const CodeText stats = method("void noteStatsWindow()");
    EXPECT_TRUE(stats.Has("if (stVsyncs && !stSwaps && !stPresents)"));
    EXPECT_TRUE(stats.Before("SwitchDisplay: quiet display: ", "stSwaps = stVsyncs = "));
    EXPECT_TRUE(stats.Before("SwitchDisplay: quiet display: ", "if (logged) os_log("));
}

// An idle display wakes no core: FRAME_END reaches the CPU only while something waits for a frame end. Holders outside the
// gate (TegraPanel, the colour unit) write INT_MASK too, so every INT_MASK write is one of the three under its lock; a kick
// unmasks it before any work that waits for a frame end, through the lock only when the gate's shadow says masked; the
// last frame end with nothing waiting masks it.
TEST(FrameEndSource, MaskedWhileNothingWaits)
{
    EXPECT_EQ(code().Count("dc[kDcIntMask] ="), 3u);
    for (const char *helper :
         {"void writeIntMask(uint32_t clear, uint32_t set)", "void unmaskFrameEnd()", "void maskIdleFrameEnd()"})
        EXPECT_TRUE(method(helper).Has("IOSimpleLockLockDisableInterrupt(intMaskLock)")) << helper;
    EXPECT_TRUE(method("void kickSwapQueue() override").StartsWith("if (primary) kickUnmaskFrameEnd();"));
    EXPECT_TRUE(method("void vblankTick()")
                    .EndsWith("if ((status & kFrameEndInt) && !frameEndNeeded() && tegra_panel_holds_frame_end()) { "
                              "maskIdleFrameEnd(); idleFlipDeadline(); }"));
    EXPECT_TRUE(method("void maskIdleFrameEnd()").Has("if (!__atomic_load_n(&gFrameEndHolds"));
    // The kick's shadow: set with the lock held, cleared only by the gate.
    EXPECT_TRUE(method("void kickUnmaskFrameEnd()")
                    .Has("if (!__atomic_load_n(&frameEndUnmasked, __ATOMIC_RELAXED)) unmaskFrameEnd();"));
    EXPECT_EQ(code().Count("__atomic_store_n(&frameEndUnmasked, false"), 2u);
    EXPECT_EQ(code().Count("noteFrameEndMask("), 2u);
}

// A blank swap blacks the head through the colour unit only while no flip is out, and the first flip latched after it
// ends the black: an older flip's latch must never show a frame the blank swap replaced.
// The start's 250 ms check only proves frame ends arrive: INT_ENABLE has no writer outside the display's own start,
// stop, resume and the routes it enables (TegraPanel's DSI steps read it), so nothing re-applies it.
TEST(FrameEndSource, TheStartCheckWritesNoInterruptEnable)
{
    EXPECT_FALSE(method("void vsyncTick()").Has("dc[kDcIntEnable] ="));
    EXPECT_TRUE(method("void vsyncTick()").Has("no DC A frame end in 250 ms"));
    const CodeText panel(SourceText("../../SwitchBrightness/TegraPanel.cpp"));
    EXPECT_TRUE(panel.Has("gDc[DC_CMD_INT_ENABLE]"));
    EXPECT_FALSE(panel.Has("gDc[DC_CMD_INT_ENABLE] ="));
}

// SC7 powers DC A down once its STOP is in force: the active DISPLAY_COMMAND reads mode 0 (TRM 24.6.16), waited for on
// the last frame's FRAME_END and bounded by two frame periods with one error line, in place of a 20 ms sleep. The DSI
// pads are calibrated once and waited for by MIPI_CAL's status (TegraMipiCal.h), in place of two back-to-back starts and a
// 10 ms sleep.
TEST(Sc7Source, StatusReadsNotFixedSleeps)
{
    EXPECT_FALSE(Block(display(), "extern \"C\" void tegra_display_suspend(void)").Has("IOSleep"));
    EXPECT_TRUE(method("void quiesceForSuspend()").Before("setScanout(false);", "waitScanoutHalted();"));
    EXPECT_TRUE(method("void quiesceForSuspend()").Before("waitScanoutHalted();", "vblankSource->disable();"));
    const std::string halt = SourceText("../SwitchScanoutHalt.h");
    const CodeText halted = Block(halt, "void waitScanoutHalted()");
    EXPECT_TRUE(Block(halt, "bool scanoutHalted()").Has("kReadMuxActive"));
    EXPECT_TRUE(halted.Has("assert_wait_deadline(reinterpret_cast<event_t>(&scanoutStopFrames)"));
    EXPECT_TRUE(halted.Has("2 * period"));
    EXPECT_TRUE(halted.Has("IOLog("));
    EXPECT_FALSE(halted.Has("panic("));
    const std::string power = SourceText("../TegraDisplayPower.cpp");
    const CodeText up = Block(power, "extern \"C\" void tegra_display_power_up(void)");
    EXPECT_FALSE(up.Has("for (unsigned pass"));
    EXPECT_EQ(CodeText(power).Count("0x2A000001"), 0u);
    EXPECT_TRUE(up.Before("write(gMipiCal, kMipiDsiCal);", "tegra_mipi_cal::calibrate(gMipiCal"));
    EXPECT_TRUE(up.Before("tegra_mipi_cal::calibrate(gMipiCal", "write(gDc, kDcTimings);"));
}

TEST(BlankSource, ColourBlankEndsAtTheNextLatch)
{
    EXPECT_TRUE(code().Has("rotated && rotationLatched && doubleBuffer && ring.pending == kNoBuffer &&"));
    EXPECT_TRUE(method("void flipLatched()").StartsWith("if (blanked) { blanked = false; color->setBlank(false);"));
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
    for (const char *slot : {"IOReturn changeFrameInfo(uint32_t type, void *client, uint32_t mask) override",
                             "IOReturn sendFrameInfo(void *client, const void *frame) override",
                             "IOReturn setDisplayTemperature(const uint32_t *temperatures) override"})
        EXPECT_TRUE(code().Has(slot));
    EXPECT_TRUE(method("IOReturn setDisplayTemperature(").Has("return kIOReturnSuccess;"));
}

TEST(DirectScanoutSource, FailedExactProducerNeverFallsThroughToCpuCopy)
{
    for (const char *name : {"bool offerDirect(", "bool offerGpuCopy("}) {
        const CodeText lost = method(name).Between("if (offered == TEGRA_SCANOUT_SOURCE_LOST)", "if (stats)");
        EXPECT_TRUE(lost.Has("noteTaken(request, kickAt);"));
        EXPECT_TRUE(lost.Has("planeNextSet = false;"));
        EXPECT_TRUE(lost.Has("kIOReturnAborted"));
        EXPECT_TRUE(lost.Has("dropLostFrame(request,"));
        EXPECT_TRUE(lost.Has("return true;"));
    }
}

TEST(DisplayResumeSource, RestoreRunsOnTheFamilyGateBeforeCompletionCallbacks)
{
    const CodeText source(SourceText("../NativeLegacyDisplay.cpp"));
    const CodeText resume(SourceText("../SwitchDisplayResume.h"));
    EXPECT_TRUE(source.Has("display->getWorkLoop()->runAction("));
    EXPECT_TRUE(source.Has("return d->finishResume();"));
    EXPECT_TRUE(resume.Before("getWorkLoop()->inGate()", "reattachDirect();"));
    EXPECT_TRUE(resume.Before("planesResume();", "hwSuspended = false"));
    EXPECT_TRUE(resume.Before("planesResume();", "setScanout(true)"));
    EXPECT_TRUE(resume.Before("planesResume();", "presentCopied()"));
    EXPECT_TRUE(resume.Before("planesResume();", "presentGpuCopy()"));
}

// retireScanoutStorage sleeps up to kFlipDeadlineMS with the family's gate held, woken by the DC A filter (which needs no
// gate): only the display's teardown (stop, free, through stopVsync's gated stopVblank) may reach it, never a swap, a
// frame end or SC7.
TEST(ScanoutRetirementSource, OnlyTheDisplaysTeardownWaitsUnderTheGate)
{
    EXPECT_EQ(code().Count("retireScanoutStorage();"), 1u);
    EXPECT_TRUE(method("void stopRotation()").Has("retireScanoutStorage();"));
    EXPECT_EQ(code().Count("stopRotation();"), 1u);
    EXPECT_TRUE(method("void stopVblank()").Has("stopRotation();"));
    EXPECT_EQ(code().Count("->stopVblank();"), 1u);
    EXPECT_TRUE(method("void stopVsync()").Has("->stopVblank();"));
    EXPECT_EQ(code().Count("stopVsync();"), 2u);
    EXPECT_TRUE(method("void stop(IOService *provider) override").Has("stopVsync();"));
    EXPECT_TRUE(method("void free() override").Has("stopVsync();"));
    const CodeText wait(SourceText("../SwitchScanoutRetirement.h"));
    EXPECT_TRUE(wait.Before("assert_wait_deadline(", "if (scanoutStorageRetired() ||"));
    EXPECT_TRUE(code().Has("thread_wakeup(reinterpret_cast<event_t>(&self->scanoutStopFrames));"));
}

// A release kernel's swap path counts no client surfaces: noteSurface and its two volatile increments are debug-only.
TEST(ReleaseSource, NoSurfaceCensusOnAReleaseSwap)
{
    const char *const debug = "#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)";
    EXPECT_TRUE(code().Has(std::string(debug) + " noteSurface(surface, memory); #endif"));
    EXPECT_TRUE(
        code().Has(std::string(debug) + " static void noteSurface(OSObject *surface, IOMemoryDescriptor *memory)"));
    EXPECT_EQ(code().Count("noteSurface("), 2u);
}

// The DC A filter hands the action per-cause counts only it writes: no read-modify-write on the filter's side or the
// action's, and every reader of what is pending goes through frameEndPending.
TEST(FrameEndSource, TheFilterHandsOffWithoutReadModifyWrites)
{
    const CodeText filter = method("template <class Self> static bool vblankFilter(");
    for (const char *rmw : {"__atomic_fetch_", "__atomic_exchange", "__atomic_compare_exchange"})
        EXPECT_FALSE(filter.Has(rmw)) << rmw;
    EXPECT_FALSE(method("uint32_t takeFrameEndStatus()").Has("__atomic_exchange"));
    EXPECT_FALSE(code().Has("dcPending"));
    EXPECT_TRUE(method("int waitTapeFrame()").Has("frameEndPending(kVblankInt | kFrameEndInt | kVpulse2Int)"));
    EXPECT_TRUE(method("void checkFlipDeadline()").Has("frameEndPending(kFrameEndInt)"));
}

// One gate pass a period: a completion that wrote window A's flip leaves its pass to the frame end's takeCompletionFlip
// unless the gate wants it now; one deadline while anything is out, never armed or cancelled per flip or per offer.
TEST(FrameEndSource, OnePassAndOneDeadlineAPeriod)
{
    const CodeText complete = method("void gpuComplete(int status)");
    EXPECT_TRUE(complete.Before("writeCompletionFlip(status);", "if (!completionFlipCarries() ||"));
    EXPECT_TRUE(method("void kickSwapQueue() override").Has("kickCompletionPass("));
    EXPECT_TRUE(method("bool setScanout(bool on)").Before("scanoutStopped = !on;", "publishCompletionPass();"));
    EXPECT_FALSE(method("void flipLatched()").Has("cancelTimeout"));
    EXPECT_FALSE(method("void cancelGpuDeadline()").Has("cancelTimeout"));
    EXPECT_TRUE(method("void armDeadlineTimer()").StartsWith("if (flipDeadlineArmed || !flipTimer) return;"));
    EXPECT_TRUE(method("void checkFlipDeadline()")
                    .Has("if (frameEnds != flipDeadlineFrames || frameEndPending(kFrameEndInt))"));
    EXPECT_TRUE(method("bool frameEndNeeded()").Has("(offerAged && gpuScanout)"));
    EXPECT_FALSE(code().Has("__atomic_store_n(&gpuScanout"));
    EXPECT_FALSE(code().Has("__atomic_exchange_n(&gpuDonePending"));
    EXPECT_FALSE(code().Has("__atomic_exchange_n(&self->gpuDonePending"));
}

// validateSwap runs off the family's gate (21H461 and 24B119 swap_submit call it between their gated actions): it
// touches no table, its verdict goes through the ring, and the gate drains the ring before it reads the table.
TEST(FrameEndSource, VerdictsReachTheTableThroughTheGate)
{
    const CodeText validate = method("bool validateSwap(const OriginalLegacySwapRequest *request) override");
    EXPECT_TRUE(validate.Has("publishVerdict(verdict);"));
    for (const char *table : {"noteSubmitted(", "removeValidated(", "addValidated(", "validated["})
        EXPECT_FALSE(validate.Has(table)) << table;
    EXPECT_FALSE(method("void publishVerdict(").Has("validated["));
    EXPECT_TRUE(method("void kickSwapQueue() override").Before("drainVerdicts();", "kickOnce();"));
    EXPECT_TRUE(method("void vblankTick()").StartsWith("drainVerdicts();"));
    EXPECT_FALSE(code().Has("__atomic_fetch_add(&validatedEvicted"));
    EXPECT_FALSE(code().Has("__atomic_exchange_n(&validatedEvicted"));
}

// Debug kernels log the frame path's locks per 100 frame ends by lock, and the work-loop passes, with the 5 s stats; a
// release kernel counts nothing (stats is SWITCH_FRAME_TIMELINE: DEVELOPMENT || DEBUG).
TEST(DisplayStatsSource, LocksPerFrameEndLine)
{
    const CodeText window = method("void noteLocksWindow()");
    EXPECT_TRUE(window.StartsWith("if (!stats) return;"));
    EXPECT_TRUE(window.Has("os_log(OS_LOG_DEFAULT, \"SwitchDisplay locks: %s\", line);"));
    for (const char *count : {"color->lockTakes()", "intMaskTakes", "switch_vic_surface_cache_locks()", "flipClaims()"})
        EXPECT_TRUE(window.Has(count)) << count;
    EXPECT_TRUE(method("void noteStatsWindow()").Has("noteLocksWindow();"));
    EXPECT_TRUE(method("void notePass()").StartsWith("if (stats) ++stPasses;"));
}

// validateSwap runs on the client's thread: it decides and observes, and everything the gate owns (the overlay's
// bandwidth admission, the rejection stats, the swap timeline's first stage) happens where the gate drains its verdict.
TEST(FrameEndSource, ValidateSwapTouchesNoGateState)
{
    const CodeText validate = method("bool validateSwap(const OriginalLegacySwapRequest *request) override");
    for (const char *gate : {"admitPlaneBandwidth(", "stRejected", "stRejectReason", "native_legacy_swap_stage("})
        EXPECT_FALSE(validate.Has(gate)) << gate;
    EXPECT_TRUE(validate.Has("verdict.admit = true;"));
    const CodeText drain = method("void drainVerdicts()");
    EXPECT_TRUE(drain.Before("admitPlaneBandwidth(verdict.present, verdict.layers, verdict.width, verdict.height);",
                             "noteSubmitted(verdict.request, verdict.at, verdict.tl);"));
    EXPECT_TRUE(drain.Has("native_legacy_swap_stage(0);"));
    EXPECT_TRUE(drain.Has("++stRejected;"));
    const CodeText observed =
        Block(SourceText("../../../kits/qemu/LegacyDisplayProbe.cpp"), "void native_legacy_swap_validation_observed(");
    EXPECT_FALSE(observed.Has("native_legacy_swap_stage("));
}

// Core Animation never arms the vsync (its frame clock is its own), so the frame end stands in for the frame timeline as it
// does for the GPU deadline: each swap's timeline then carries the vsync before it, the slow-frame line its phase, and
// render.c a frame begin for SwitchFrameRender (boot 21 had none, and every SwitchFrame line read "origin swap").
TEST(FrameTimelineSource, TheFrameEndStandsInForAnUnarmedVsync)
{
    const CodeText tick = method("void vblankTick()");
    EXPECT_TRUE(tick.Before("tegra_gm20b_compositor_vsync(frameEnd, 0, refreshTicks());",
                            "noteTimelineVsync(frameEnd, refreshTicks());"));
    EXPECT_TRUE(method("void deliverVsync(").Has("noteTimelineVsync(stamp, interval);"));
    const CodeText note = method("void noteTimelineVsync(");
    EXPECT_TRUE(note.Has("if (!SWITCH_FRAME_TIMELINE) return;"));
    EXPECT_TRUE(note.Has("switch_sched_render_vsync(stamp, interval);"));
    EXPECT_TRUE(method("FrameTimeline swapTimeline(uint64_t at)").Has("tl.vsyncUs = timelineVsyncUs(at);"));
}
