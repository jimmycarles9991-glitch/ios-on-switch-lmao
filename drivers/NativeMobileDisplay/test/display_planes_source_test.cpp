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

// XNU's kern_exec.c (the vendored kernel/xnu-10063.141.1), by its runfiles path (XNU_KERN_EXEC, the BUILD's
// $(rootpath)): not a file of this test's package.
std::string XnuKernExec()
{
    std::ifstream file(XNU_KERN_EXEC, std::ios::binary);
    if (!file) {
        ADD_FAILURE() << "cannot read " << XNU_KERN_EXEC;
        return {};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
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
    EXPECT_THAT(Method(source, "    void publishPlaneLimits()\n"),
                HasSubstr("{\"PlaneBaseAlignmentLinear\", native_planes::kPublishedBaseAlignment}"));
    // The service is found as J120's: IOServiceMatching("AppleCLCD").
    EXPECT_THAT(
        source,
        HasSubstr(": OSMetaClass(\"AppleCLCD\", NativeLegacyDisplay::superClass, sizeof(NativeLegacyDisplay))"));
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
