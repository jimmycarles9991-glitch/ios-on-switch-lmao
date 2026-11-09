// Hardware planes in the driver's source and in XNU (r18-planes): the rules display_planes_test checks on the host are
// the ones NativeLegacyDisplay.cpp follows (validateSwap through checkUpdated, the kick on the family's effective
// layers with video-only swaps costing no GPU work, window B written with window A in one STATE_CONTROL pair, retire
// at the latch, the panic hook taking window B down, the published limits), the plane accessors are the IOSurface
// functions TegraVIC calls, and backboardd loses CA_DISABLE_DETACHED only when native_legacy_detach_ready() says so
// (planes on, VIC's quarter turns and a row pitch proven; the alignment published from that proof).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

// A member function after `signature` (four-space class indentation), up to its closing brace.
std::string Method(const std::string &source, const std::string &signature)
{
    return Between(source, signature, "\n    }\n");
}

// A function at column 0 (the class-body include, a free function), up to its closing brace.
std::string Function(const std::string &source, const std::string &signature)
{
    return Between(source, signature, "\n}\n");
}

// `a` occurs in `text` before `b` (both present).
void ExpectBefore(const std::string &text, const std::string &a, const std::string &b)
{
    const size_t at = text.find(a), bt = text.find(b);
    ASSERT_NE(at, std::string::npos) << a;
    ASSERT_NE(bt, std::string::npos) << b;
    EXPECT_LT(at, bt) << a << " before " << b;
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
    const std::string source = SourceText("../NativeLegacyDisplay.cpp");
    const std::string planes = SourceText("../NativePlanesDisplay.h");
    EXPECT_THAT(source, HasSubstr("#include \"NativePlanesDisplay.h\""));
    // validateSwap mirrors the published limits through checkUpdated; the UI's memory is still bounded.
    const std::string validate =
        Method(source, "    bool validateSwap(const OriginalLegacySwapRequest *request) override\n");
    EXPECT_THAT(validate, HasSubstr("native_planes::checkUpdated("));
    EXPECT_THAT(validate, HasSubstr("native_legacy_surface_bounds("));
    // The kick reads the effective layers first; a swap that leaves window A alone changes window B only.
    const std::string kick = Method(source, "    void kickOnce()\n");
    ExpectBefore(kick, "planeDecide(request, pd)", "directCandidate(pd.ui)");
    EXPECT_THAT(kick, HasSubstr("if (!pd.uiNew) {"));
    EXPECT_THAT(kick, HasSubstr("kickPlaneOnly(request, pd"));
    EXPECT_THAT(kick, Not(HasSubstr("+ 2624")));  // window A's surface is the effective UI layer, not layer 0's slot
    const std::string decide =
        Function(planes, "void planeDecide(const OriginalLegacySwapRequest *request, PlaneDecision &d)\n");
    EXPECT_THAT(decide, HasSubstr("swapEffectiveSurfaces"));
    EXPECT_THAT(decide, HasSubstr("native_planes::rolesOf("));
    // Window B goes out with window A in one UPDATE write and one ACT_REQ write.
    const std::string flip = Method(source, "    void writeFlip(int i)\n");
    EXPECT_THAT(flip, HasSubstr("planeStage(entryPlane[i], update, request)"));
    EXPECT_EQ(Count(flip, "dc[kDcStateControl] ="), 2u);
    EXPECT_THAT(Method(source, "    void presentEntry(int i)\n"), HasSubstr("entryPlane[i].seq = entrySeq[i]"));
    // A latched window B set retires its swaps in queue order; a stuck one or an underflow run turns planes off.
    const std::string frame = Function(planes, "void planeFrameEnd()\n");
    EXPECT_THAT(frame, HasSubstr("retireThrough(now.seq, __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE))"));
    EXPECT_THAT(frame, HasSubstr("planesOff("));
    EXPECT_THAT(source, HasSubstr("if (status & kFrameEndInt)\n            planeFrameEnd();"));
    // The first client frame's swap retires at its own latch (latchRetire is on from the plane probe, before it).
    EXPECT_THAT(Method(source, "    void startRotation()\n"), HasSubstr("entrySeq[target] = seqOf(flipRequest);"));
    EXPECT_THAT(Method(source, "    void checkRotation()\n"), HasSubstr("retireThrough(entrySeq[asked], stamp);"));
    // Swaps wait while the plane probe covers the screen; a window B set out after planes went off never panics.
    EXPECT_THAT(kick, HasSubstr("if (planeProbe.active()) {"));
    EXPECT_THAT(Method(source, "    void checkFlipDeadline()\n"), HasSubstr("!(planesOn && planeLatch.requested())"));
    // Planes retire at the latch: a video's previous frame may still be scanned.
    EXPECT_THAT(Function(planes, "void planeProbeDetach()\n"), HasSubstr("latchRetire = true;"));
    // The panic screen never sits under window B.
    const std::string panic = Function(source, "void restorePitchOnPanic(const char *message)\n");
    EXPECT_EQ(Count(panic, "dc[kWinBBase + native_planes::kOptions] = 0;"), 2u);
    // The limits Core Animation reads at the display's creation.
    for (const char *key : {"IOMFBScalingLimits", "IOMFBMaxSrcPixels", "IOMFBNumLayers",
                            "DisplayPipePlaneBaseAlignment", "RGBLayer_MinScaleFraction", "YUVLayer_MaxScale",
                            "MaxSrcRectWidth", "MaxNumLayers", "PlaneBaseAlignmentLinear"})
        EXPECT_THAT(source, HasSubstr(std::string("\"") + key + "\""));
    // The gate: planes on and the IOSA quarter turns (transforms 4 and 7) proven.
    const std::string gate = Function(source, "extern \"C\" bool native_legacy_detach_ready(void)\n");
    EXPECT_THAT(gate, HasSubstr("planesReady()"));
    EXPECT_THAT(gate, HasSubstr("(vic->rotations & 0x90) == 0x90"));
    // And a VIC row pitch the published alignment is a multiple of; the alignment itself is VIC's from start, never raised
    // later (Core Animation and VideoToolbox read it once).
    EXPECT_THAT(gate, HasSubstr("vic->pitchAlign"));
    EXPECT_THAT(gate, HasSubstr("const bool ok = planes && turns && pitched;"));
    EXPECT_THAT(source, Not(HasSubstr("publishDetachAlignment")));
    EXPECT_THAT(Method(source, "    void publishStrideRequirements()\n"),
                HasSubstr("OSNumber::withNumber(native_planes::kPublishedRowAlignment, 32)"));
    const std::string limits = Method(source, "    void publishPlaneLimits()\n");
    EXPECT_THAT(limits, HasSubstr("{\"PlaneBaseAlignmentLinear\", native_planes::kPublishedBaseAlignment}"));
    // iOS 27's own limits; iOS 17's dictionary has no key they add.
    EXPECT_THAT(limits,
                HasSubstr("publish(\"IOMFBMaxSrcPixels\", source, limits.compressionUnsupported ? 8 : 7, 64);"));
    ExpectBefore(limits, "#if SWITCH_IPSW_KIND_24B119", "native_planes::kLimits24B119;");
    EXPECT_THAT(
        limits,
        HasSubstr("if (limits.packedYuvUnsupported)\n            setProperty(\"IOMFBYUVPackedUnSupported\", true);"));
    // The service is found as J120's: IOServiceMatching("AppleCLCD").
    EXPECT_THAT(
        source,
        HasSubstr(": OSMetaClass(\"AppleCLCD\", NativeLegacyDisplay::superClass, sizeof(NativeLegacyDisplay))"));
}

// The first-boot data migration keeps XNU's console on screen (switch_console_hold). A released console stops drawing, so
// a swap kicked under the hold releases nothing and draws nothing: it retires at once, or with what is still out when
// retiring at the latch, before the kick reaches anything that takes the screen (native_legacy_prepare_scanout, a copy,
// a flip or window B).
TEST(DisplayPlanesSource, TheConsoleHoldKeepsTheScreen)
{
    const std::string source = SourceText("../NativeLegacyDisplay.cpp");
    const std::string planes = SourceText("../NativePlanesDisplay.h");
    EXPECT_EQ(Count(source, "switch_console_hold"), 4u);  // the declaration, consoleHolds and its comment
    const std::string holds = Method(source, "    bool consoleHolds() const\n");
    ExpectBefore(holds, "if (rotated || &switch_console_hold == nullptr)\n            return false;",
                 "return switch_console_hold != 0;");
    const std::string kick = Method(source, "    void kickOnce()\n");
    ExpectBefore(kick, "planeDecide(request, pd);", "if (consoleHolds()) {\n            kickHeld(request,");
    for (const char *taker : {"kickPlaneOnly(request, pd", "native_legacy_prepare_scanout()", "offerDirect(",
                              "offerGpuCopy(", "startCopy(", "presentBlock();"})
        ExpectBefore(kick, "kickHeld(request,", taker);
    const std::string held =
        Method(source, "    void kickHeld(const OriginalLegacySwapRequest *request, uint64_t kickAt)\n");
    ExpectBefore(held, "noteTaken(request, kickAt);", "retireUnshown(seqOf(request));");
    EXPECT_THAT(held, HasSubstr("finishTransfer(request, true);"));
    for (const char *taker : {"native_legacy_prepare_scanout", "presentBlock", "flip", "ring.", "plane"})
        EXPECT_THAT(held, Not(HasSubstr(taker))) << taker;
    EXPECT_THAT(Method(source, "    void presentBlock()\n"), Not(HasSubstr("hold")));
    const std::string unshown = Function(planes, "void retireUnshown(uint64_t seq)\n");
    ExpectBefore(unshown, "if (!latchRetire)", "retireThrough(seq, mach_absolute_time());");
    for (const char *outstanding : {"planeSetOf(planeLatch.armed).seq = seq;", "planeHeld.seq = seq;",
                                    "entrySeq[ring.held] = seq;", "entrySeq[ring.pending] = seq;"})
        ExpectBefore(unshown, outstanding, "retireThrough(seq, mach_absolute_time());");
    EXPECT_THAT(Function(planes, "void kickPlaneOnly("), HasSubstr("retireUnshown(seq);"));
}

// A plane slot holds what window B scans from it as a direct slot does (the surface, its wired memory, its layout), and
// window B scans a surface the GPU renders block-linear so: the image is made for the GPU's layout of it, and a slot whose
// held layout is not that one is not used.
TEST(DisplayPlanesSource, PlaneSlotsHoldTheSurfaceTheyScan)
{
    const std::string planes = SourceText("../NativePlanesDisplay.h");
    const std::string make = Function(planes, "int planeSlotFor(");
    EXPECT_THAT(make, HasSubstr("surface->retain();"));
    EXPECT_THAT(make, HasSubstr("planeSlots[k].tiled == tiled"));
    ExpectBefore(make, "tegra_gm20b_surface_hold(", "(held != 0) != tiled");
    const std::string decide = Function(planes, "void planeDecide(");
    ExpectBefore(decide, "surfaceLayout(r.surface, block)", "image.surfaceKind != 0);");
    EXPECT_THAT(Function(planes, "static const tgm_layout *surfaceLayout("),
                HasSubstr("tegra_gm20b_surface_layout(pa & ~0xfffULL, &block) == 1"));
    const std::string write = Function(planes, "void planeWriteRegisters(const PlaneSet &p)");
    EXPECT_THAT(write, HasSubstr("buf[kSurfaceKind] = m.surfaceKind;"));
    EXPECT_THAT(write, HasSubstr("buf[kVOffset] = m.vOffset;"));
    EXPECT_THAT(Function(planes, "static bool planeSame("), HasSubstr("x.surfaceKind == y.surfaceKind"));
    const std::string drop = Function(planes, "void planeDropSlot(unsigned k)");
    ExpectBefore(drop, "probeDomain.unmap(", "tegra_gm20b_surface_unhold(slot.layout);");
    ExpectBefore(drop, "tegra_gm20b_surface_unhold(slot.layout);", "slot.memory->release();");
    ExpectBefore(drop, "slot.memory->release();", "OSSafeReleaseNULL(slot.surface);");
}

TEST(DisplayPlanesSource, BackboarddDetachesOnlyWhenTheDisplaySaysSo)
{
    const std::string xnu = XnuKernExec();
    // The weak default: never.
    EXPECT_THAT(xnu, HasSubstr("\nnative_legacy_detach_ready(void)\n{\n\treturn false;\n}"));
    EXPECT_THAT(xnu, HasSubstr("\t\tconst int single = !native_legacy_detach_ready();"));
    const std::string exec = Between(xnu, "\nnative_switch_exec_environment(const struct image_params *imgp)", "\n}\n");
    ASSERT_FALSE(exec.empty());
    EXPECT_THAT(exec, Not(HasSubstr("if (single_layer) {")));
    EXPECT_THAT(exec, Not(HasSubstr("if (vbl && single_layer) {")));
}

// Debug iOS 27 kernels have backboardd log why each layer was or was not detached; release kernels and iOS 17 do not.
TEST(DisplayPlanesSource, DetachReasonsLoggedOnlyByDebugIos27Kernels)
{
    const std::string xnu27 = Runfile(XNU27_KERN_EXEC);
    const std::string print =
        Between(xnu27, "#if DEVELOPMENT || DEBUG\n#define NATIVE_SWITCH_CA_PRINT_DETACHING", "#endif");
    EXPECT_THAT(print, HasSubstr("\"CA_PRINT_DETACHING=1\","));
    EXPECT_THAT(print, HasSubstr("#else\n#define NATIVE_SWITCH_CA_PRINT_DETACHING\n"));
    EXPECT_EQ(Count(xnu27, "CA_PRINT_DETACHING=1"), 1u);
    EXPECT_EQ(Count(xnu27, "NATIVE_SWITCH_CA_PRINT_DETACHING NULL"), 2u);
    EXPECT_THAT(XnuKernExec(), Not(HasSubstr("CA_PRINT_DETACHING")));
}

// iOS 27's CoreVideo registers no compressed pixel format (H12's AGX and HTPC tiled layouts), so nothing hands the display
// a surface in one; iOS 17's MobileGestalt answers have none to begin with.
TEST(DisplayPlanesSource, Ios27CoreVideoMakesNoCompressedSurfaces)
{
    const std::string setup = Between(Runfile(XNU27_KERN_EXEC), "\nnative_switch_setup_state(", "\n}\n");
    EXPECT_THAT(setup,
                HasSubstr("{\"com.apple.corevideo\", SWITCH_PLIST(\"<key>disable-buffer-compression</key><true/>\"), "
                          "kSetupManaged},"));
    EXPECT_THAT(XnuKernExec(), Not(HasSubstr("disable-buffer-compression")));
}

// A detached layer refused for its format or layout is logged with its surface's geometry on debug kernels only.
TEST(DisplayPlanesSource, RefusedLayoutsLoggedOnlyByDebugKernels)
{
    const std::string source = SourceText("../NativeLegacyDisplay.cpp");
    const std::string totals = SourceText("../NativePlanesTotals.h");
    ExpectBefore(source,
                 "reason = native_planes::checkUpdated(updated, background, hasSurface, layers, width, height);",
                 "planeNoteRefused(reason, updated, hasSurface, layers, width, height);");
    const std::string debug = Between(totals, "#if SWITCH_FRAME_TIMELINE\n", "#else\n");
    EXPECT_THAT(debug, HasSubstr("switch_refusal_k(key, \"display\", \"plane-layout\","));
    EXPECT_THAT(
        Between(totals, "#else\n", "#endif\n"),
        HasSubstr("static void planeNoteRefused(unsigned, uint32_t, const bool *, const native_planes::Layer *, "
                  "uint32_t, uint32_t)\n{}"));
}

// Window B changes whose surface the GPU renders follow NativePlaneFence.h: they ride on the window A frame's present,
// which also waits for the plane surface's writers, or on a fence of their own whose completion writes them; a frame no
// present carries drops such a change; nothing writes them from the kick.
TEST(DisplayPlanesSource, WindowBWaitsForTheGpuOnSurfacesItRenders)
{
    const std::string source = SourceText("../NativeLegacyDisplay.cpp");
    const std::string planes = SourceText("../NativePlanesDisplay.h");
    const std::string fence = SourceText("../NativePlaneFenceDisplay.h");
    for (const char *offer : {"    bool offerGpuCopy(", "    bool offerDirect("}) {
        const std::string body = Method(source, offer);
        ExpectBefore(body, "copy.alsoWritten = planeNextAlsoWritten();", "tegra_gm20b_scanout_copy(&copy)");
    }
    const std::string kick = Method(source, "    void kickOnce()\n");
    ExpectBefore(kick, "planeNextUnfenced();  // no GPU present carries this frame from here on",
                 "native_legacy_gpu_offer(this, memory, sourceStride, width, height)");
    ExpectBefore(kick, "offerGpuCopy(request, memory, sourceStride, allocation, width, height, tiled, kickAt)",
                 "planeNextUnfenced();  // no GPU present carries this frame from here on");
    const std::string only = Function(planes, "void kickPlaneOnly(");
    ExpectBefore(only, "native_planes::planeRoute(planeFenced(p), false, false) == native_planes::PlaneRoute::kNow",
                 "planeOut(p);");
    ExpectBefore(only, "} else if (planeFence(p)) {", "transferred = true;");
    EXPECT_THAT(Function(fence, "bool planeFence(const PlaneSet &p)"),
                HasSubstr("tegra_gm20b_scanout_fence(this, pa)"));
    const std::string done = Function(fence, "void finishPlaneFence(int status)");
    ExpectBefore(done, "if (status || !planesOn) {", "planeOut(planeFencing);");
    EXPECT_THAT(Method(source, "    void finishGpuSwap(int status)\n"),
                HasSubstr("if (gpuScanout && planeFencePending) {\n            finishPlaneFence(status);"));
    EXPECT_THAT(Function(fence, "uint64_t planeFencePa(const PlaneSet &p) const"),
                HasSubstr("planeSlots[p.slot].memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone)"));
}
