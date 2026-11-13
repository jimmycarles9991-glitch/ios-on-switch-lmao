// drivers/NativeMobileDisplay/DisplayWindowProbe.h and DisplayWindowProbeRun.h on the host. A model of a window's fetch
// (L4T window.c's reading of the registers: the source origin in ADDR_H_OFFSET bytes and ADDR_V_OFFSET lines on both
// planes, 4:2:0 chroma on half the lines, SCAN_COLUMN turning a line into a source column walked back from H_OFFSET
// under H_DIRECTION, block-linear addresses as TegraGM20BLayout.h lays them out) renders every step of every plan:
// each claim step must show its reference's panel pixel for pixel, also when the window interpolates chroma between
// neighbours (the crops' edges), and each control another panel; every read stays inside its picture. Then the run's
// frame-end decisions for a pass and for each failure, and the reply HW Test reads.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../DisplayWindowProbeRun.h"
#include "../../SwitchPower/SwitchDisplayMemory.h"

namespace {
using namespace display_window_probe;

struct Model {
    const Plan &plan;
    const std::vector<uint8_t> &memory;
    bool interpolateChroma = false;
    bool outside = false;

    const Surface *surfaceAt(uint64_t start) const
    {
        for (unsigned i = 0; i < plan.surfaces; ++i)
            if (plan.surface[i].offset == start)
                return &plan.surface[i];
        return nullptr;
    }

    // By the registers: SURFACE_KIND's layout and block height, LINE_STRIDE's luma (15:0) or chroma (31:16) stride.
    static uint64_t address(const Window &w, bool chroma, uint32_t xB, uint32_t y)
    {
        const uint32_t stride = chroma ? w.lineStride >> 16 : w.lineStride & 0xffff;
        const uint64_t base = chroma ? w.startU : w.start;
        if ((w.kind & 3) == native_planes::kSurfaceKindBlock)
            return base + tgm_layout_offset_of(stride, static_cast<uint8_t>((w.kind >> 4) & 7), xB, y);
        return base + static_cast<uint64_t>(y) * stride + xB;
    }

    uint8_t at(uint64_t a)
    {
        if (a >= memory.size()) {
            outside = true;
            return 0;
        }
        return memory[a];
    }

    uint16_t chromaPair(const Surface &s, const Window &w, int64_t cx, int64_t cy)
    {
        const int64_t cw = (s.width + 1) / 2, ch = (s.height + 1) / 2;
        cx = cx < 0 ? 0 : cx >= cw ? cw - 1 : cx;
        cy = cy < 0 ? 0 : cy >= ch ? ch - 1 : cy;
        const uint64_t a = address(w, true, static_cast<uint32_t>(cx) * 2, static_cast<uint32_t>(cy));
        return static_cast<uint16_t>(at(a) | at(a + 1) << 8);
    }

    // Panel line `line`, pixel `pixel` as Y | Cb << 8 | Cr << 16 (NV12) or the BGRA word; 0 for an off window.
    uint32_t fetch(const Window &w, uint32_t line, uint32_t pixel)
    {
        if (!w.on())
            return 0;
        const Surface *s = surfaceAt(w.start);
        if (!s) {
            outside = true;
            return 0;
        }
        const uint32_t bpp = s->nv12 ? 1 : 4;
        const bool column = (w.options & native_column_scan::kScanColumn) != 0u;
        const uint32_t xStep = column ? line : pixel, yStep = column ? pixel : line;
        const int64_t x0 = w.hOffset / bpp;
        const int64_t x = (w.options & native_column_scan::kHDirection) ? x0 - xStep : x0 + xStep;
        const int64_t y = (w.options & native_column_scan::kVDirection) ? static_cast<int64_t>(w.vOffset) - yStep
                                                                        : static_cast<int64_t>(w.vOffset) + yStep;
        const uint32_t lines = w.prescaled >> 16, bytes = w.prescaled & 0xffff;
        if (x < 0 || y < 0 || x >= s->width || y >= s->height || (column ? xStep >= lines : yStep >= lines) ||
            (column ? yStep * bpp >= bytes : xStep * bpp >= bytes)) {
            outside = true;
            return 0;
        }
        const uint32_t xB = static_cast<uint32_t>(x) * bpp;
        if (!s->nv12) {
            uint32_t word = 0;
            for (unsigned i = 0; i < 4; ++i)
                word |= static_cast<uint32_t>(at(address(w, false, xB + i, static_cast<uint32_t>(y)))) << (8 * i);
            return word;
        }
        const uint32_t luma = at(address(w, false, xB, static_cast<uint32_t>(y)));
        uint16_t chroma = chromaPair(*s, w, x / 2, y / 2);
        if (interpolateChroma) {  // the sample's neighbour on the side the luma pixel leans to, on both axes
            const uint16_t across = chromaPair(*s, w, x / 2 + (x & 1 ? 1 : -1), y / 2);
            const uint16_t down = chromaPair(*s, w, x / 2, y / 2 + (y & 1 ? 1 : -1));
            const auto mixed = [](uint16_t a, uint16_t b, uint16_t c, unsigned shift) {
                return static_cast<uint32_t>(
                    (2 * ((a >> shift) & 0xff) + ((b >> shift) & 0xff) + ((c >> shift) & 0xff) + 2) / 4);
            };
            chroma = static_cast<uint16_t>(mixed(chroma, across, down, 0) | mixed(chroma, across, down, 8) << 8);
        }
        return luma | static_cast<uint32_t>(chroma) << 8;
    }

    // What the panel shows: window A over window B (the probe never turns both on).
    std::vector<uint32_t> panel(const Step &step)
    {
        std::vector<uint32_t> out(static_cast<size_t>(kPanelWidth) * kPanelHeight);
        const Window &w = step.a.on() ? step.a : step.b;
        for (uint32_t line = 0; line < kPanelHeight; ++line)
            for (uint32_t pixel = 0; pixel < kPanelWidth; ++pixel)
                out[static_cast<size_t>(line) * kPanelWidth + pixel] = fetch(w, line, pixel);
        return out;
    }
};

std::vector<uint8_t> filled(const Plan &plan)
{
    std::vector<uint8_t> memory(plan.bytes, 0xcd);
    for (unsigned i = 0; i < plan.surfaces; ++i)
        fill(memory.data(), plan.surface[i]);
    return memory;
}

uint32_t crcOf(const std::vector<uint32_t> &panel)
{
    uint32_t h = 2166136261u;
    for (const uint32_t v : panel)
        h = (h ^ v) * 16777619u;
    return h ? h : 1;
}

std::vector<Plan> allPlans()
{
    std::vector<Plan> plans;
    for (const Probe p : {kOverlayBlockYuv, kColumnScanYuv, kOffsetCrop}) {
        Plan plan;
        EXPECT_TRUE(planOf(p, plan));
        plans.push_back(plan);
    }
    return plans;
}

TEST(WindowProbePlans, FitOneSlotInAlignedSurfaces)
{
    for (const Plan &plan : allPlans()) {
        SCOPED_TRACE(probeName(plan.probe));
        EXPECT_LE(plan.bytes, 20ULL << 20);  // one plane slot's span
        EXPECT_LE(plan.steps, kMaxSteps);
        uint64_t end = 0;
        for (unsigned i = 0; i < plan.surfaces; ++i) {
            const Surface &s = plan.surface[i];
            EXPECT_EQ(s.offset % kSurfaceAlign, 0u);
            EXPECT_GE(s.offset, end);
            EXPECT_EQ(s.rowBytes % 64, 0u);
            EXPECT_EQ(s.lumaRows % blockRows(s.yLog2), 0u);
            end = s.offset + s.lumaBytes();
            if (s.nv12) {
                EXPECT_EQ(s.chromaOffset % kSurfaceAlign, 0u);
                EXPECT_GE(s.chromaOffset, end);
                EXPECT_EQ(s.chromaRows % blockRows(s.yLog2), 0u);
                EXPECT_GE(s.chromaRows * 2, s.height);
                end = s.chromaOffset + s.chromaBytes();
            }
            EXPECT_LE(end, plan.bytes);
        }
        for (unsigned i = 0; i < plan.steps; ++i) {
            const Step &s = plan.step[i];
            EXPECT_NE(s.a.on(), s.b.on()) << s.name;  // one window at a time: the CRC is that window's alone
            EXPECT_LT(s.against, plan.steps);
            if (s.check != kShown)
                EXPECT_LT(s.against, i) << s.name;  // a reference is scanned before what is judged against it
        }
    }
}

TEST(WindowProbePlans, ClaimStepsShowTheirReferencePixelForPixel)
{
    for (const Plan &plan : allPlans()) {
        SCOPED_TRACE(probeName(plan.probe));
        const std::vector<uint8_t> memory = filled(plan);
        for (const bool interpolate : {false, true}) {
            Model model{plan, memory, interpolate};
            std::vector<std::vector<uint32_t>> panels;
            for (unsigned i = 0; i < plan.steps; ++i)
                panels.push_back(model.panel(plan.step[i]));
            EXPECT_FALSE(model.outside);
            for (unsigned i = 0; i < plan.steps; ++i) {
                const Step &s = plan.step[i];
                if (s.check == kEqual || s.check == kReport)
                    EXPECT_TRUE(panels[i] == panels[s.against]) << s.name << " interpolate " << interpolate;
                if (s.check == kDiffer)
                    EXPECT_FALSE(panels[i] == panels[s.against]) << s.name;
            }
        }
    }
}

TEST(WindowProbePlans, ReferencesShowTheIntendedPictures)
{
    Plan plan;
    ASSERT_TRUE(planOf(kOverlayBlockYuv, plan));
    std::vector<uint8_t> memory = filled(plan);
    Model a{plan, memory};
    std::vector<uint32_t> panel = a.panel(plan.step[0]);
    for (uint32_t y = 0; y < kPanelHeight; y += 13)
        for (uint32_t x = 0; x < kPanelWidth; x += 7)
            ASSERT_EQ(panel[y * kPanelWidth + x], lumaAt(x, y) | static_cast<uint32_t>(chromaAt(x / 2, y / 2, 1)) << 8);

    ASSERT_TRUE(planOf(kColumnScanYuv, plan));
    memory = filled(plan);
    Model b{plan, memory};
    panel = b.panel(plan.step[0]);  // panel line j shows landscape column 1279 - j, its pixel i row i
    for (uint32_t line = 0; line < kPanelHeight; line += 11)
        for (uint32_t pixel = 0; pixel < kPanelWidth; pixel += 5)
            ASSERT_EQ(panel[line * kPanelWidth + pixel],
                      lumaAt(kLandscapeWidth - 1 - line, pixel) | static_cast<uint32_t>(chromaAt(0, 0, 0)) << 8);
    panel = b.panel(plan.step[6]);
    for (uint32_t y = 0; y < kPanelHeight; y += 17)
        for (uint32_t x = 0; x < kPanelWidth; x += 3)
            ASSERT_EQ(panel[y * kPanelWidth + x], bgraAt(x, y));

    ASSERT_TRUE(planOf(kOffsetCrop, plan));
    memory = filled(plan);
    Model c{plan, memory};
    panel = c.panel(plan.step[0]);
    for (uint32_t y = 0; y < kPanelHeight; y += 13)
        for (uint32_t x = 0; x < kPanelWidth; x += 7)
            ASSERT_EQ(panel[y * kPanelWidth + x],
                      lumaAt(kCropX + x, kCropY + y) |
                          static_cast<uint32_t>(chromaAt((kCropX + x) / 2, (kCropY + y) / 2, 4)) << 8);
}

// The run reserves window B as the planes reserve a layer: the memory math must take each plan's heaviest step.
TEST(WindowProbePlans, WindowBsHeaviestStepPassesTheMemoryMath)
{
    for (const Plan &plan : allPlans()) {
        SCOPED_TRACE(probeName(plan.probe));
        native_planes::Image image;
        ASSERT_TRUE(heaviestOverlay(plan.step, plan.steps, image));
        EXPECT_EQ(image.yuv, plan.probe != kColumnScanYuv);  // V3's is the UI's BGRA
        const uint32_t bpp = image.yuv ? 1 : 4;
        switch_display_memory_window out = {};
        EXPECT_TRUE(switch_display::memoryWindow(
            out, 78000000ULL * (image.yuv ? 2 : 4), (image.prescaled & 0xffff) / bpp, image.prescaled >> 16,
            image.size & 0xffff, image.size >> 16, image.yuv, !image.surfaceKind, false));
        EXPECT_TRUE(out.active);
    }
}

TEST(WindowProbePlans, CropOffsetsSitInsideAGobAndABlock)
{
    Plan plan;
    ASSERT_TRUE(planOf(kOffsetCrop, plan));
    const Window &w = plan.step[1].b;
    EXPECT_EQ(w.start, plan.surface[0].offset);  // START on the plane bases, the origin in the offsets
    EXPECT_EQ(w.startU, plan.surface[0].chromaOffset);
    EXPECT_EQ(w.hOffset, kCropX);
    EXPECT_EQ(w.vOffset, kCropY);
    EXPECT_NE(kCropX % 64, 0u);
    EXPECT_NE(kCropY % 16, 0u);
    EXPECT_NE((kCropY / 2) % 16, 0u);
    EXPECT_EQ(kCropX % 2, 0u);
    EXPECT_EQ(kCropY % 2, 0u);
}

TEST(WindowProbePlans, ColumnScanRegistersFollowTheColumnScanRules)
{
    Plan plan;
    ASSERT_TRUE(planOf(kColumnScanYuv, plan));
    const Window &w = plan.step[2].a;
    EXPECT_EQ(w.options, 0x40040011u);           // WIN_ENABLE, CSC, SCAN_COLUMN, H_DIRECTION; no filter at 1:1
    EXPECT_EQ(w.prescaled, 1280u << 16 | 720u);  // V: the source width, H: its height in bytes
    EXPECT_EQ(w.size, 1280u << 16 | 720u);
    EXPECT_EQ(w.dda, 0x10001000u);
    EXPECT_EQ(w.hOffset, 1279u);  // (x + w) * Bpp - 1
    EXPECT_EQ(w.kind, 0x12u);     // BLOCK, 2-GOB blocks: NVDEC's
    EXPECT_EQ(w.lineStride, 1280u << 16 | 1280u);
}

// --- The run -------------------------------------------------------------------------------------------------------
struct FakeDc {
    const Plan &plan;
    std::vector<uint32_t> crcs;  // each step's CRC as the hardware would latch it
    Run run;
    unsigned step = 0;
    bool requestOut = false, restored = false, written = false;
    unsigned latchDelay = 1;
    bool neverQuiet = false, neverLatch = false, badRestore = false;
    int underflowStep = -1;
    std::vector<Run::Action> actions;

    FakeDc(const Plan &p, std::vector<uint32_t> c) : plan(p), crcs(std::move(c))
    {
        run.begin(plan.steps);
    }

    Finding go(unsigned maxFrames = 400)
    {
        unsigned sinceWrite = 0;
        bool showing = false;
        for (unsigned f = 0; f < maxFrames && run.busy(); ++f) {
            const bool out = written && !neverLatch && sinceWrite < latchDelay ? true : written && neverLatch;
            const uint32_t crc = showing && sinceWrite >= 2 ? crcs[step] : 0;
            const bool underflow = underflowStep == static_cast<int>(step) && showing;
            const bool asWritten = run.stage == Run::kRestoreLatch ? !badRestore : true;
            const Run::Action a = run.frameEnd(!neverQuiet, out, crc, underflow, asWritten);
            ++sinceWrite;
            if (a != Run::kNothing)
                actions.push_back(a);
            if (a == Run::kStart || a == Run::kShow) {
                step = a == Run::kStart ? 0 : run.step;
                written = true;
                showing = true;
                sinceWrite = 0;
            } else if (a == Run::kRestore) {
                written = true;
                showing = false;
                sinceWrite = 0;
            }
        }
        return judge(plan, run);
    }
};

std::vector<uint32_t> modelCrcs(const Plan &plan)
{
    const std::vector<uint8_t> memory = filled(plan);
    Model model{plan, memory};
    std::vector<uint32_t> crcs;
    for (unsigned i = 0; i < plan.steps; ++i)
        crcs.push_back(crcOf(model.panel(plan.step[i])));
    return crcs;
}

TEST(WindowProbeRun, AWorkingWindowPassesAndIsRestored)
{
    for (const Plan &plan : allPlans()) {
        SCOPED_TRACE(probeName(plan.probe));
        FakeDc dc(plan, modelCrcs(plan));
        const Finding f = dc.go();
        EXPECT_EQ(f.verdict, kPass) << f.note;
        EXPECT_EQ(dc.run.stage, Run::kDone);
        EXPECT_TRUE(dc.run.restored);
        ASSERT_FALSE(dc.actions.empty());
        EXPECT_EQ(dc.actions.front(), Run::kStart);
        EXPECT_EQ(dc.actions.back(), Run::kFinish);
        unsigned shows = 0;
        for (const Run::Action a : dc.actions)
            shows += a == Run::kShow;
        EXPECT_EQ(shows, plan.steps - 1);
        for (unsigned i = 0; i < plan.steps; ++i)
            EXPECT_TRUE(dc.run.result[i].settled) << plan.step[i].name;
        EXPECT_LE(dc.run.frames, 7 * plan.steps + 4);  // V3's 8 steps: under a second at 60 Hz
    }
}

TEST(WindowProbeRun, AWrongPictureFailsWhereOnlyTheRouteDiffers)
{
    Plan plan;
    ASSERT_TRUE(planOf(kOffsetCrop, plan));
    std::vector<uint32_t> crcs = modelCrcs(plan);
    crcs[1] ^= 0x10;
    FakeDc dc(plan, crcs);
    EXPECT_EQ(dc.go().verdict, kFail);
    ASSERT_TRUE(planOf(kColumnScanYuv, plan));
    crcs = modelCrcs(plan);
    crcs[2] ^= 0x10;  // a turn that differs may be another sampling of chroma: not proven wrong
    FakeDc scan(plan, crcs);
    EXPECT_EQ(scan.go().verdict, kInconclusive);
}

TEST(WindowProbeRun, ABlindControlIsInconclusive)
{
    Plan plan;
    ASSERT_TRUE(planOf(kOverlayBlockYuv, plan));
    std::vector<uint32_t> crcs = modelCrcs(plan);
    crcs[3] = crcs[1];
    FakeDc dc(plan, crcs);
    EXPECT_EQ(dc.go().verdict, kInconclusive);
}

TEST(WindowProbeRun, AnUnderflowSaysNothingAboutThePicture)
{
    Plan plan;
    ASSERT_TRUE(planOf(kOverlayBlockYuv, plan));
    FakeDc dc(plan, modelCrcs(plan));
    dc.underflowStep = 1;
    const Finding f = dc.go();
    EXPECT_EQ(f.verdict, kInconclusive);
    EXPECT_EQ(f.step, 1);
    EXPECT_GT(dc.run.result[1].underflows, 0u);
    EXPECT_TRUE(dc.run.restored);
}

TEST(WindowProbeRun, ADisplayThatNeverGoesQuietIsLeftAlone)
{
    Plan plan;
    ASSERT_TRUE(planOf(kOverlayBlockYuv, plan));
    FakeDc dc(plan, modelCrcs(plan));
    dc.neverQuiet = true;
    const Finding f = dc.go();
    EXPECT_EQ(f.verdict, kInconclusive);
    EXPECT_FALSE(dc.run.restoreWritten);
    EXPECT_EQ(dc.actions.size(), 1u);
    EXPECT_EQ(dc.actions.front(), Run::kFinish);
}

TEST(WindowProbeRun, RegistersNoBoundaryTakesEndTheRunRestored)
{
    Plan plan;
    ASSERT_TRUE(planOf(kOverlayBlockYuv, plan));
    FakeDc dc(plan, modelCrcs(plan));
    dc.neverLatch = true;
    const Finding f = dc.go();
    EXPECT_NE(f.verdict, kPass);
    EXPECT_TRUE(dc.run.restoreWritten);
    EXPECT_EQ(dc.actions.back(), Run::kFinish);
}

TEST(WindowProbeRun, AnUnconfirmedRestoreFails)
{
    Plan plan;
    ASSERT_TRUE(planOf(kOffsetCrop, plan));
    FakeDc dc(plan, modelCrcs(plan));
    dc.badRestore = true;
    EXPECT_EQ(dc.go().verdict, kFail);
    EXPECT_FALSE(dc.run.restored);
}

TEST(WindowProbeRun, AnAbortWritesTheSavedWindowsOnce)
{
    Plan plan;
    ASSERT_TRUE(planOf(kColumnScanYuv, plan));
    FakeDc dc(plan, modelCrcs(plan));
    dc.go(12);
    ASSERT_TRUE(dc.run.busy());
    EXPECT_TRUE(dc.run.abort("the display went off"));
    EXPECT_FALSE(dc.run.busy());
    EXPECT_FALSE(dc.run.abort("again"));
    EXPECT_EQ(judge(plan, dc.run).verdict, kFail);  // restore written, never confirmed
}

TEST(WindowProbeRun, TheReplyCarriesEveryStep)
{
    Plan plan;
    ASSERT_TRUE(planOf(kColumnScanYuv, plan));
    const std::vector<uint32_t> crcs = modelCrcs(plan);
    FakeDc dc(plan, crcs);
    dc.go();
    SwitchDisplayWindowProbeReply reply;
    replyOf(plan, dc.run, reply);
    EXPECT_EQ(reply.magic, SWITCH_DISPLAY_WINDOW_PROBE_MAGIC);
    EXPECT_EQ(reply.probe, static_cast<uint32_t>(kColumnScanYuv));
    EXPECT_EQ(reply.state, static_cast<uint32_t>(SWITCH_DISPLAY_WINDOW_PROBE_DONE));
    EXPECT_EQ(reply.verdict, static_cast<uint32_t>(kPass));
    EXPECT_EQ(reply.steps, plan.steps);
    EXPECT_EQ(reply.restored, 1u);
    for (unsigned i = 0; i < plan.steps; ++i) {
        EXPECT_EQ(reply.step[i].crc, crcs[i]);
        EXPECT_EQ(reply.step[i].settled, 1);
        EXPECT_STREQ(reply.step[i].name, plan.step[i].name);
        EXPECT_EQ(reply.step[i].check, plan.step[i].check);
    }
    EXPECT_EQ(offsetof(SwitchDisplayWindowProbeReply, why), 32u);
    EXPECT_EQ(offsetof(SwitchDisplayWindowProbeReply, step), 96u);
}

}  // namespace
