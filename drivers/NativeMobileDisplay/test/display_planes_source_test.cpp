// Hardware planes in the driver's source and in XNU (r18-planes): the rules display_planes_test checks on the host are
// the ones NativeLegacyDisplay.cpp follows (validateSwap through checkUpdated, the kick on the family's effective
// layers with video-only swaps costing no GPU work, window B written with window A in one STATE_CONTROL pair, retire
// at the latch, the panic hook taking window B down, the published limits), the plane accessors are the IOSurface
// functions TegraVIC calls, and backboardd loses CA_DISABLE_DETACHED only when native_legacy_detach_ready() says so
// (planes on, VIC's quarter turns and a row pitch proven; the alignment published from that proof).
#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::Configured;
using switch_ios::test::SourceText;

namespace {

// NativeLegacyDisplay.cpp with the headers its class body includes for direct scanout.
std::string Display()
{
    return SourceText("../NativeLegacyDisplay.cpp") + SourceText("../SwitchScanoutStart.h") +
           SourceText("../SwitchDirectSlots.h");
}

// The planes' share of the class body.
std::string Planes()
{
    return SourceText("../NativePlanesDisplay.h") + SourceText("../SwitchOverlayRegisters.h") +
           SourceText("../SwitchPlaneSelection.h");
}

// A file outside this test's package by its runfiles path (the BUILD's $(rootpath)).
std::string Runfile(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ADD_FAILURE() << "cannot read " << path;
        return {};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// XNU's kern_exec.c (the vendored kernel/xnu-10063.141.1).
std::string XnuKernExec()
{
    return Runfile(XNU_KERN_EXEC);
}

}  // namespace

TEST(DisplayPlanesSource, PlaneAccessorsAreTegraVics)
{
    // The plane accessors are the IOSurface functions TegraVIC calls: one table entry each, both drivers' symbols.
    const std::string iface = SourceText("../../../kits/SwitchABI/interface/IOSurface.json");
    for (const std::string name : {"plane_offset", "plane_bytes_per_row", "plane_height", "plane_width"}) {
        const std::regex entry("\"" + name + "\": \\{[^}]*\"tegra_vic_ios_" + name + "\",\\s*\"native_planes_surface_" +
                               name + "\"");
        EXPECT_TRUE(std::regex_search(iface, entry)) << name;
    }
}

TEST(DisplayPlanesSource, DriverFollowsTheRules)
{
    const std::string source = Display(), planes = Planes();
    const CodeText code(source);
    EXPECT_TRUE(code.Has("#include \"NativePlanesDisplay.h\""));
    // validateSwap mirrors the published limits through checkUpdated; the UI's memory is still bounded.
    const CodeText validate = Block(source, "bool validateSwap(const OriginalLegacySwapRequest *request) override");
    EXPECT_TRUE(validate.Has("native_planes::checkUpdated("));
    EXPECT_TRUE(validate.Has("native_legacy_surface_bounds("));
    // The kick reads the effective layers first; a swap that leaves window A alone changes window B only.
    const CodeText kick = Block(source, "void kickOnce()");
    EXPECT_TRUE(kick.Before("planeDecide(request, pd)", "directCandidate(pd.ui)"));
    EXPECT_TRUE(kick.Has("if (!pd.uiNew) {"));
    EXPECT_TRUE(kick.Has("kickPlaneOnly(request, pd"));
    EXPECT_FALSE(kick.Has("+ 2624"));  // window A's surface is the effective UI layer, not layer 0's slot
    const CodeText decide =
        Block(planes, "void planeDecide(const OriginalLegacySwapRequest *request, PlaneDecision &d)");
    EXPECT_TRUE(decide.Has("swapEffectiveSurfaces"));
    EXPECT_TRUE(decide.Has("native_planes::rolesOf("));
    // Window B goes out with window A in one UPDATE write and one ACT_REQ write.
    const CodeText flip = Block(source, "void writeFlip(int i)");
    EXPECT_TRUE(flip.Has("planeStage(entryPlane[i], update, request)"));
    EXPECT_EQ(flip.Count("dc[kDcStateControl] ="), 2u);
    EXPECT_TRUE(Block(source, "void presentEntry(int i)").Has("entryPlane[i].seq = entrySeq[i]"));
    // A latched window B set retires its swaps in queue order; a stuck one or an underflow run turns planes off.
    const CodeText frame = Block(planes, "void planeFrameEnd()");
    EXPECT_TRUE(frame.Has("retireThrough(now.seq, __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE))"));
    EXPECT_TRUE(frame.Has("beginPlaneRecovery("));
    EXPECT_TRUE(code.Has("if (status & kFrameEndInt) { planeFrameEnd(); landscapeFrameEnd();"));
    // The first client frame's swap retires at its own latch (latchRetire is on from the plane probe, before it).
    EXPECT_TRUE(
        Block(source, "void startRotation(int target = kNoBuffer)").Has("entrySeq[target] = seqOf(flipRequest);"));
    EXPECT_TRUE(Block(source, "void checkRotation()").Has("retireThrough(entrySeq[asked], stamp);"));
    // Swaps wait while the plane probe covers the screen; a window B set out after planes went off never panics.
    EXPECT_TRUE(kick.Has("if (planeProbe.active()) {"));
    EXPECT_TRUE(Block(source, "void checkFlipDeadline()").Has("!(planesOn && planeLatch.requested())"));
    // Planes retire at the latch: a video's previous frame may still be scanned.
    EXPECT_TRUE(Block(planes, "void planeProbeDetach()").Has("latchRetire = true;"));
    // The panic screen never sits under window B.
    const CodeText panic = Block(source, "void restorePitchOnPanic(const char *message)");
    EXPECT_EQ(panic.Count("dc[kWinBBase + native_planes::kOptions] = 0;"), 2u);
    // The limits Core Animation reads at the display's creation.
    for (const char *key : {"IOMFBScalingLimits", "IOMFBMaxSrcPixels", "IOMFBNumLayers",
                            "DisplayPipePlaneBaseAlignment", "RGBLayer_MinScaleFraction", "YUVLayer_MaxScale",
                            "MaxSrcRectWidth", "MaxNumLayers", "PlaneBaseAlignmentLinear"})
        EXPECT_TRUE(code.Has(std::string("\"") + key + "\""));
    // The gate: planes on and the IOSA quarter turns (transforms 4 and 7) proven.
    const CodeText gate = Block(source, "extern \"C\" bool native_legacy_detach_ready(void)");
    EXPECT_TRUE(gate.Has("planesReady()"));
    EXPECT_TRUE(gate.Has("(vic->rotations & 0x90) == 0x90"));
    // And a VIC row pitch the published alignment is a multiple of; the alignment itself is VIC's from start, never raised
    // later (Core Animation and VideoToolbox read it once).
    EXPECT_TRUE(gate.Has("vic->sourcePitchAlign"));
    EXPECT_TRUE(gate.Has("const bool ok = planes && turns && pitched;"));
    EXPECT_FALSE(code.Has("publishDetachAlignment"));
    EXPECT_TRUE(Block(source, "void publishStrideRequirements()")
                    .Has("OSNumber::withNumber(native_planes::kPublishedRowAlignment, 32)"));
    const CodeText limits = Block(source, "void publishPlaneLimits()");
    EXPECT_TRUE(limits.Has("{\"PlaneBaseAlignmentLinear\", native_planes::kPublishedBaseAlignment}"));
    // iOS 27's own limits; iOS 17's dictionary has no key they add.
    EXPECT_TRUE(limits.Has("publish(\"IOMFBMaxSrcPixels\", source, limits.compressionUnsupported ? 8 : 7, 64);"));
    EXPECT_TRUE(limits.Before("#if SWITCH_IPSW_KIND_24B119", "native_planes::kLimits24B119;"));
    EXPECT_TRUE(limits.Has("if (limits.packedYuvUnsupported) setProperty(\"IOMFBYUVPackedUnSupported\", true);"));
    // The service is found as J120's: IOServiceMatching("AppleCLCD").
    EXPECT_TRUE(code.Has(": OSMetaClass(\"AppleCLCD\", NativeLegacyDisplay::superClass, sizeof(NativeLegacyDisplay))"));
}

// The first-boot data migration keeps XNU's console on screen (switch_console_hold). A released console stops drawing, so
// a swap kicked under the hold releases nothing and draws nothing: it retires at once, or with what is still out when
// retiring at the latch, before the kick reaches anything that takes the screen (native_legacy_prepare_scanout, a copy,
// a flip or window B).
TEST(DisplayPlanesSource, TheConsoleHoldKeepsTheScreen)
{
    const std::string source = Display(), planes = Planes();
    EXPECT_EQ(CodeText(source).Count("switch_console_hold"), 4u);  // the declaration, consoleHolds and its comment
    EXPECT_TRUE(Block(source, "bool consoleHolds() const")
                    .Before("if (rotated || &switch_console_hold == nullptr) return false;",
                            "return switch_console_hold != 0;"));
    const CodeText kick = Block(source, "void kickOnce()");
    EXPECT_TRUE(kick.Before("planeDecide(request, pd);", "if (consoleHolds()) { kickHeld(request,"));
    for (const char *taker : {"kickPlaneOnly(request, pd", "native_legacy_prepare_scanout()", "offerDirect(",
                              "offerGpuCopy(", "startCopy(", "presentBlock();"})
        EXPECT_TRUE(kick.Before("kickHeld(request,", taker));
    const CodeText held = Block(source, "void kickHeld(const OriginalLegacySwapRequest *request, uint64_t kickAt)");
    EXPECT_TRUE(held.Before("noteTaken(request, kickAt);", "retireUnshown(seqOf(request));"));
    EXPECT_TRUE(held.Has("finishTransfer(request, true);"));
    for (const char *taker : {"native_legacy_prepare_scanout", "presentBlock", "flip", "ring.", "plane"})
        EXPECT_FALSE(held.Has(taker));
    EXPECT_FALSE(Block(source, "void presentBlock()").Has("hold"));
    const CodeText unshown = Block(planes, "void retireUnshown(uint64_t seq)");
    EXPECT_TRUE(unshown.Before("if (!latchRetire)", "retireThrough(seq, mach_absolute_time());"));
    for (const char *outstanding : {"planeSetOf(planeLatch.armed).seq = seq;", "planeHeld.seq = seq;",
                                    "entrySeq[ring.held] = seq;", "entrySeq[ring.pending] = seq;"})
        EXPECT_TRUE(unshown.Before(outstanding, "retireThrough(seq, mach_absolute_time());"));
    EXPECT_TRUE(Block(planes, "void kickPlaneOnly(").Has("retireUnshown(seq);"));
}

// A plane slot holds what window B scans from it as a direct slot does (the surface, its wired memory, its layout), and
// window B scans a surface the GPU renders block-linear so: the image is made for the GPU's layout of it, and a slot whose
// held layout is not that one is not used.
TEST(DisplayPlanesSource, PlaneSlotsHoldTheSurfaceTheyScan)
{
    const std::string planes = Planes();
    const CodeText make = Block(planes, "int planeSlotFor(");
    EXPECT_TRUE(make.Has("surface->retain();"));
    EXPECT_TRUE(make.Has("planeSlots[k].tiled == tiled"));
    EXPECT_TRUE(make.Before("tegra_gm20b_surface_hold(", "(held != 0) != tiled"));
    EXPECT_TRUE(Block(planes, "void planeDecide(")
                    .Before("planeNativeSlotFor(layer.surface", "planeBuildingSlots |= 1u << unsigned(slot)"));
    const CodeText native(SourceText("../SwitchNativePlaneDisplay.h"));
    EXPECT_TRUE(native.Before("switch_native_scanout_acquire(surface", "planeMapSnapshot(scanout, image)"));
    EXPECT_TRUE(native.Has("surfaceLayout(surface, block)"));
    EXPECT_TRUE(Block(planes, "static const tgm_layout *surfaceLayout(")
                    .Has("tegra_gm20b_surface_layout(pa & ~0xfffULL, &block) == 1"));
    const CodeText write =
        Block(planes, "void planeWriteWindowRegisters(Window w, const switch_display::Overlay &p, bool live)");
    EXPECT_TRUE(write.Has("buf[kSurfaceKind] = m.surfaceKind;"));
    EXPECT_TRUE(write.Has("buf[kVOffset] = m.vOffset;"));
    EXPECT_TRUE(Block(planes, "static bool planeSame(").Has("x.surfaceKind != y.surfaceKind"));
    const CodeText drop = Block(SourceText("../SwitchPlaneSlots.h"), "void planeDropSlot(unsigned k)");
    EXPECT_TRUE(drop.Before("probeDomain.unmap(", "tegra_gm20b_surface_unhold(slot.layout);"));
    EXPECT_TRUE(drop.Before("tegra_gm20b_surface_unhold(slot.layout);", "slot.memory->release();"));
    EXPECT_TRUE(drop.Before("slot.memory->release();", "OSSafeReleaseNULL(slot.surface);"));
}

TEST(DisplayPlanesSource, BackboarddDetachesOnlyWhenTheDisplaySaysSo)
{
    const std::string xnu = XnuKernExec();
    // The weak default: never.
    EXPECT_TRUE(CodeText(xnu).Has("native_legacy_detach_ready(void) { return false; }"));
    EXPECT_TRUE(CodeText(xnu).Has("if (!native_legacy_detach_ready()) {"));
    const CodeText exec = Block(xnu, "native_switch_exec_environment(const struct image_params *imgp)");
    EXPECT_FALSE(exec.Has("if (single_layer) {"));
    EXPECT_FALSE(exec.Has("if (vbl && single_layer) {"));
}

// No kernel has backboardd log why each layer was or was not detached: QuartzCore 24B119's CA_PRINT_DETACHING
// (ca_debug_options[32]) logs four lines a composited frame on the render thread ("ClearUI detach fail", "[CA]",
// "[Server]" and "[Display] Surface ID ... detach result").
TEST(DisplayPlanesSource, NoKernelHasTheRenderServerLogItsDetachReasons)
{
    EXPECT_FALSE(CodeText(Runfile(XNU27_KERN_EXEC)).Has("CA_PRINT_DETACHING"));
    EXPECT_FALSE(CodeText(XnuKernExec()).Has("CA_PRINT_DETACHING"));
}

// iOS 27's CoreVideo registers no compressed pixel format (H12's AGX and HTPC tiled layouts), so nothing hands the display
// a surface in one; iOS 17's MobileGestalt answers have none to begin with.
TEST(DisplayPlanesSource, Ios27CoreVideoMakesNoCompressedSurfaces)
{
    const CodeText setup = Block(Runfile(XNU27_KERN_EXEC), "native_switch_setup_state(");
    EXPECT_TRUE(setup.Has(
        "{\"com.apple.corevideo\", SWITCH_PLIST(\"<key>disable-buffer-compression</key><true/>\"), kSetupManaged},"));
    EXPECT_FALSE(CodeText(XnuKernExec()).Has("disable-buffer-compression"));
}

// A detached layer refused for its format or layout is logged with its surface's geometry on debug kernels only.
TEST(DisplayPlanesSource, RefusedLayoutsLoggedOnlyByDebugKernels)
{
    EXPECT_TRUE(CodeText(Display()).Before(
        "reason = native_planes::checkUpdated(updated, background, hasSurface, layers, width, height);",
        "planeNoteRefused(reason, updated, hasSurface, layers, width, height);"));
    const std::string totals = SourceText("../NativePlanesTotals.h");
    const CodeText debug(Configured(totals, "SWITCH_FRAME_TIMELINE", true));
    const CodeText release(Configured(totals, "SWITCH_FRAME_TIMELINE", false));
    EXPECT_TRUE(debug.Has("switch_refusal_k(key, \"display\", \"plane-layout\","));
    EXPECT_FALSE(release.Has("switch_refusal_k(key, \"display\", \"plane-layout\","));
    EXPECT_TRUE(
        release.Has("static void planeNoteRefused(unsigned, uint32_t, const bool *, const native_planes::Layer *, "
                    "uint32_t, uint32_t) {}"));
}

// Window B changes whose surface the GPU renders follow NativePlaneFence.h: they ride on the window A frame's present,
// which also waits for the plane surface's writers, or on a fence of their own whose completion writes them; a frame no
// present carries drops such a change; nothing writes them from the kick.
TEST(DisplayPlanesSource, WindowBWaitsForTheGpuOnSurfacesItRenders)
{
    const std::string source = Display(), planes = Planes();
    const std::string fence = SourceText("../NativePlaneFenceDisplay.h");
    EXPECT_TRUE(Block(source, "bool offerGpuCopy(")
                    .Before("copy.alsoWritten = planeNextAlsoWritten();", "offerWithCompletionFlip(copy, target)"));
    EXPECT_TRUE(Block(source, "bool offerDirect(")
                    .Before("copy.alsoWritten = planeNextAlsoWritten();", "offerWithCompletionFlip(copy, entry)"));
    const CodeText kick = Block(source, "void kickOnce()");
    EXPECT_FALSE(kick.Has("planeNextUnfenced"));
    EXPECT_TRUE(kick.Has("!(planeNextSet && planeFenced(planeNext))"));
    EXPECT_TRUE(CodeText(SourceText("../SwitchCopiedPlanes.h")).Has("if (!copyPixelsReady || !copyPlaneReady)"));
    EXPECT_TRUE(Block(source, "void startCopy(").Has("beginCopiedPlaneFence()"));
    EXPECT_TRUE(Block(source, "void copyFinished()").Has("copiedPixelsFinished()"));
    const CodeText only = Block(planes, "void kickPlaneOnly(");
    EXPECT_TRUE(only.Before(
        "native_planes::planeRoute(planeFenced(p), false, false) == native_planes::PlaneRoute::kNow", "planeOut(p);"));
    EXPECT_TRUE(only.Before("} else if (planeFence(p)) {", "transferred = true;"));
    const CodeText admission = Block(fence, "bool planeFence(const PlaneSet &p)");
    EXPECT_FALSE(admission.Has("switch_gpu_bridge_fence("));
    EXPECT_TRUE(admission.Before("planeFencePending = true;", "copySource->interruptOccurred"));
    EXPECT_TRUE(Block(source, "void copyWork()").Has("planeFenceWork();"));
    const CodeText worker(SourceText("../SwitchPlaneFenceAdmission.h"));
    EXPECT_TRUE(worker.Has("switch_gpu_bridge_fence(producers, count"));
    EXPECT_TRUE(worker.Has("switch_gpu_bridge_wait_room()"));
    EXPECT_TRUE(Block(fence, "bool planeFenced(const PlaneSet &p) const").Has("planeOverlayWaits(*native)"));
    EXPECT_TRUE(Block(SourceText("../SwitchNativePlaneDisplay.h"), "static bool planeOverlayWaits(")
                    .Has("switch_gpu_backing_token_external_status"));
    EXPECT_TRUE(Block(fence, "void finishPlaneFence(int status)")
                    .Before("if (status || !planesOn) {", "planeOut(planeFencing);"));
    EXPECT_TRUE(Block(source, "void finishGpuSwap(int status)")
                    .Has("if (gpuScanout && planeFencePending) { finishPlaneFence(status);"));
    EXPECT_TRUE(Block(fence, "uint64_t planeFencePa(const PlaneSet &p) const")
                    .Has("planeSlots[overlay.slot].memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone)"));
}
