// SPDX-License-Identifier: BSD-2-Clause-Patent
// DisplayWindowFormatProbes.h and DisplayWindowProbeExtras.h on the host. A model of windows A and B as L4T window.c
// and mainline plane.c read the registers (COLOR_DEPTH 43/42/49/16 with BYTE_SWAP, the window CSC from the step's extra
// words with saturation, CP_ENABLE through the palette words, the DDAs with the V filter interpolating two lines)
// renders every step of the three plans: each claim shows its reference's panel pixel for pixel and each control
// another. Then the extra words come back on every way a run can end.
#include <gtest/gtest.h>

#include <map>
#include <vector>

#include "../DisplayWindowProbeRun.h"
#include "bazel/testing/source_text.h"

namespace {
using namespace display_window_probe;

struct Registers {
    std::map<uint32_t, uint32_t> word;

    uint32_t &operator[](uint32_t offset)
    {
        return word[offset];
    }
};

const Surface *surfaceAt(const Plan &plan, uint64_t start)
{
    for (unsigned i = 0; i < plan.surfaces; ++i) {
        const Surface &s = plan.surface[i];
        if (start >= s.offset && start < s.offset + s.lumaBytes())
            return &s;
    }
    return nullptr;
}

int32_t signedField(uint32_t v, unsigned bits)
{
    return (v & (1u << (bits - 1))) ? int32_t(v) - int32_t(1u << bits) : int32_t(v);
}

uint8_t saturate(int64_t v256)
{
    const int64_t v = v256 / 256;
    return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
}

struct Pixel {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Pixel &) const = default;
};

struct Panel {
    const Plan &plan;
    const std::vector<uint8_t> &memory;
    std::map<uint32_t, uint32_t> extra;  // the step's extra words, as the run writes them

    uint8_t byte(const Surface &s, uint64_t planeBase, uint32_t xB, uint32_t y) const
    {
        const uint64_t at = s.block() ? planeBase + tgm_layout_offset_of(s.rowBytes, s.yLog2, xB, y)
                                      : planeBase + uint64_t(y) * s.rowBytes + xB;
        return memory.at(at);
    }

    Pixel yuv(uint32_t winc, int y, int cb, int cr) const
    {
        uint32_t k[kCscWords];
        for (uint32_t i = 0; i < kCscWords; ++i) {
            const auto found = extra.find(winc + kCscYofWord + i);
            k[i] = found != extra.end() ? found->second : native_planes::kCsc[native_planes::kBt709][1][i];
        }
        const int64_t luma = int64_t(k[1]) * (y + signedField(k[0], 8));
        const int u = cb - 128, v = cr - 128;
        return {saturate(luma + int64_t(signedField(k[2], 11)) * u + int64_t(signedField(k[3], 11)) * v),
                saturate(luma + int64_t(signedField(k[4], 10)) * u + int64_t(signedField(k[5], 10)) * v),
                saturate(luma + int64_t(signedField(k[6], 11)) * u + int64_t(signedField(k[7], 11)) * v)};
    }

    // Source sample (sx, sy) of a window: the luma line from START, chroma from START_U.
    Pixel sample(const Window &w, uint32_t winc, uint32_t swapOffset, uint32_t sx, uint32_t sy) const
    {
        const Surface *s = surfaceAt(plan, w.start);
        EXPECT_NE(s, nullptr);
        if (!s)
            return {};
        const uint64_t lumaBase = s->block() ? s->offset : w.start;
        const uint32_t row = s->block() ? sy : sy;
        if (w.depth == native_planes::kDepthB8G8R8A8) {
            Pixel p = {byte(*s, lumaBase, 4 * sx + 2, row), byte(*s, lumaBase, 4 * sx + 1, row),
                       byte(*s, lumaBase, 4 * sx, row)};
            if (w.options & kCpEnable) {
                const auto entry = [&](uint8_t i, unsigned shift) {
                    const auto found = extra.find(winc + i);
                    return uint8_t(found == extra.end() ? i : found->second >> shift);
                };
                p = {entry(p.r, 0), entry(p.g, 8), entry(p.b, 16)};
            }
            return p;
        }
        int y = 0, cb = 0, cr = 0;
        if (w.depth == switch_window_caps::kDepthYCbCr422) {
            const auto found = extra.find(swapOffset);
            const uint32_t swap = found == extra.end() ? 0 : found->second;
            const auto at = [&](uint32_t k) {
                return byte(*s, lumaBase, 4 * (sx / 2) + (swap == switch_window_caps::kByteSwap2 ? k ^ 1 : k), row);
            };
            y = at(sx % 2 ? 3 : 1);
            cb = at(0);
            cr = at(2);
        } else {
            y = byte(*s, lumaBase, sx, row);
            const bool full = w.depth == switch_window_caps::kDepthYCbCr444SP || w.depth == 48;
            const uint64_t chromaBase = s->block() ? s->chromaOffset : w.startU;
            const uint32_t cxB = full ? 2 * sx : 2 * (sx / 2), cy = full ? sy : sy / 2;
            const uint8_t first = byte(*s, chromaBase, cxB, cy), second = byte(*s, chromaBase, cxB + 1, cy);
            const bool cbFirst =
                w.depth == native_planes::kDepthYCbCr420SP || w.depth == switch_window_caps::kDepthYCbCr444SP;
            cb = cbFirst ? first : second;
            cr = cbFirst ? second : first;
        }
        return yuv(winc, y, cb, cr);
    }

    // Output pixel (px, py) of a window at the panel, or nothing.
    bool at(const Window &w, uint32_t winc, uint32_t swapOffset, uint32_t px, uint32_t py, Pixel &out) const
    {
        if (!w.on())
            return false;
        const uint32_t x0 = w.position & 0xffff, y0 = w.position >> 16, ww = w.size & 0xffff, wh = w.size >> 16;
        if (px < x0 || py < y0 || px >= x0 + ww || py >= y0 + wh)
            return false;
        const uint64_t hx = w.hInitial + uint64_t(px - x0) * (w.dda & 0xffff);
        const uint64_t vy = w.vInitial + uint64_t(py - y0) * (w.dda >> 16);
        const uint32_t sx = uint32_t(hx >> 12), sy = uint32_t(vy >> 12), frac = uint32_t(vy & 0xfff);
        const uint32_t lines = w.prescaled >> 16;
        Pixel p = sample(w, winc, swapOffset, sx, sy);
        if ((w.options & native_planes::kVFilterOn) && frac && sy + 1 < lines) {
            const Pixel q = sample(w, winc, swapOffset, sx, sy + 1);
            const auto mixed = [&](uint8_t a, uint8_t b) {
                return uint8_t((a * (4096 - frac) + b * frac + 2048) / 4096);
            };
            p = {mixed(p.r, q.r), mixed(p.g, q.g), mixed(p.b, q.b)};
        }
        out = p;
        return true;
    }

    std::vector<Pixel> render(const Step &step) const
    {
        std::vector<Pixel> panel(size_t(kPanelWidth) * kPanelHeight);
        for (uint32_t py = 0; py < kPanelHeight; ++py) {
            for (uint32_t px = 0; px < kPanelWidth; ++px) {
                Pixel p;
                if (at(step.a, kWincA, kDcWinA + native_planes::kByteSwap, px, py, p) ||
                    at(step.b, kWincB, kDcWinB + native_planes::kByteSwap, px, py, p))
                    panel[size_t(py) * kPanelWidth + px] = p;
            }
        }
        return panel;
    }
};

std::vector<Pixel> renderStep(const Plan &plan, const std::vector<uint8_t> &memory, const Step &step)
{
    Panel panel{plan, memory, {}};
    for (unsigned i = 0; i < step.extras; ++i)
        panel.extra[step.extra[i].offset] = step.extra[i].value & step.extra[i].mask;
    return panel.render(step);
}

void checkPlan(Probe probe)
{
    Plan plan;
    ASSERT_TRUE(planOf(probe, plan));
    EXPECT_STRNE(probeName(probe), "?");
    ASSERT_LE(plan.steps, kMaxSteps);
    ASSERT_LE(plan.surfaces, kMaxSurfaces);
    EXPECT_LE(plan.bytes, switch_window_caps::kPlaneSlotSpan);
    std::vector<uint8_t> memory(plan.bytes);
    for (unsigned i = 0; i < plan.surfaces; ++i)
        fill(memory.data(), plan.surface[i]);
    std::vector<std::vector<Pixel>> panels;
    for (unsigned i = 0; i < plan.steps; ++i)
        panels.push_back(renderStep(plan, memory, plan.step[i]));
    for (unsigned i = 0; i < plan.steps; ++i) {
        const Step &s = plan.step[i];
        if (s.check == kEqual)
            EXPECT_TRUE(panels[i] == panels[s.against]) << probeName(probe) << " " << s.name;
        if (s.check == kDiffer)
            EXPECT_FALSE(panels[i] == panels[s.against]) << probeName(probe) << " " << s.name;
    }
}

TEST(WindowFormatProbes, EveryClaimShowsItsReferenceAndEveryControlAnother)
{
    checkPlan(kWindowFormats);
    checkPlan(kWindowWide);
    checkPlan(kWindowPalette);
}

// The formats plan is only a test of the depth codes if a wrong code shows another panel: NV12 read Cr first, NV24 as
// NV42, packed 4:2:2 without its byte swap.
TEST(WindowFormatProbes, AWrongDepthOrSwapShowsAnotherPanel)
{
    Plan plan;
    ASSERT_TRUE(planOf(kWindowFormats, plan));
    std::vector<uint8_t> memory(plan.bytes);
    for (unsigned i = 0; i < plan.surfaces; ++i)
        fill(memory.data(), plan.surface[i]);
    const auto reference = renderStep(plan, memory, plan.step[0]);
    Step nv42 = plan.step[7];
    nv42.b.depth = 48;
    EXPECT_FALSE(renderStep(plan, memory, nv42) == renderStep(plan, memory, plan.step[6]));
    Step uyvyAsSwapped = plan.step[3];
    uyvyAsSwapped.extra = kSwappedExtras.word;
    uyvyAsSwapped.extras = kSwappedExtras.count;
    EXPECT_FALSE(renderStep(plan, memory, uyvyAsSwapped) == reference);
}

TEST(WindowFormatProbes, TheWideStepsScanTheirLinesAndTheVFilterIsOnWhereClaimed)
{
    Plan plan;
    ASSERT_TRUE(planOf(kWindowWide, plan));
    for (unsigned i = 0; i < plan.steps; ++i) {
        const Window &b = plan.step[i].b;
        ASSERT_TRUE(b.on()) << plan.step[i].name;
        EXPECT_EQ(b.dda >> 16, (2160u << 12) / 405) << plan.step[i].name;
        const bool claim = plan.step[i].check == kEqual;
        EXPECT_EQ((b.options & native_planes::kVFilterOn) != 0, claim) << plan.step[i].name;
        EXPECT_TRUE(b.options & native_planes::kHFilterOn) << plan.step[i].name;
    }
    // The window fetches up to the last pixel its DDA samples (NativePlanes.h axisOf): a few short of the source.
    EXPECT_GT(plan.step[1].b.prescaled & 0xffff, 3800u);
    EXPECT_GT(plan.step[4].b.prescaled & 0xffff, 4064u);
    EXPECT_NE(plan.step[2].b.kind, 0u);
}

// The run's every ending against a register file: the extra words are back as they were before the run, whichever
// way it ended, and each step's words are back before the next step's go in.
struct Harness {
    Plan plan;
    Run run;
    Registers dc;
    ExtraRegisters extras;
    std::map<uint32_t, uint32_t> before;

    explicit Harness(Probe probe)
    {
        EXPECT_TRUE(planOf(probe, plan));
        for (unsigned i = 0; i < plan.steps; ++i)
            for (unsigned k = 0; k < plan.step[i].extras; ++k)
                dc[plan.step[i].extra[k].offset] = 0x5a000000u | plan.step[i].extra[k].offset;
        before = dc.word;
        run.begin(plan.steps);
    }

    void perform(Run::Action action)
    {
        if (action == Run::kStart || action == Run::kShow) {
            const Step &s = plan.step[run.step];
            extras.apply(dc, s.extra, s.extras);
            for (unsigned k = 0; k < s.extras; ++k)
                EXPECT_EQ(dc[s.extra[k].offset], s.extra[k].value);
        } else if (action == Run::kRestore) {
            extras.restore(dc);
        } else if (action == Run::kFinish) {
            extras.restore(dc);
        }
    }

    void frame(bool quiet, bool requestOut, uint32_t crc)
    {
        perform(run.frameEnd(quiet, requestOut, crc, false, true));
    }

    bool restored()
    {
        return dc.word == before && extras.pending() == 0;
    }
};

TEST(WindowFormatProbes, ExtraWordsComeBackOnEveryEnding)
{
    for (const Probe probe : {kWindowFormats, kWindowPalette}) {
        {
            Harness h(probe);  // a pass: every step settles on its own CRC
            for (unsigned n = 0; n < 400 && h.run.stage != Run::kDone; ++n)
                h.frame(true, false, 0x1000 + h.run.step);
            EXPECT_EQ(h.run.stage, Run::kDone);
            EXPECT_TRUE(h.restored());
        }
        {
            Harness h(probe);  // the CRC unit reads 0 in the second step: the run fails and restores
            for (unsigned n = 0; n < 400 && h.run.stage != Run::kDone; ++n)
                h.frame(true, false, h.run.step >= 1 ? 0 : 0x77);
            EXPECT_TRUE(h.run.why);
            EXPECT_TRUE(h.restored());
        }
        {
            Harness h(probe);  // a step's request never latches: timed out
            for (unsigned n = 0; n < 400 && h.run.stage != Run::kDone; ++n)
                h.frame(true, h.run.stage == Run::kLatch && h.run.step == 1, 0x99);
            EXPECT_TRUE(h.run.why);
            EXPECT_TRUE(h.restored());
        }
        {
            Harness h(probe);  // the CRC never settles: each step is recorded unsettled and the run goes on
            uint32_t crc = 1;
            for (unsigned n = 0; n < 2000 && h.run.stage != Run::kDone; ++n)
                h.frame(true, false, ++crc);
            EXPECT_TRUE(h.restored());
        }
        {
            Harness h(probe);  // the display goes off in the middle of a step
            for (unsigned n = 0; n < 12; ++n)
                h.frame(true, false, 0x42);
            ASSERT_TRUE(h.run.busy());
            if (h.run.abort("display off"))
                h.extras.restore(h.dc);
            h.perform(Run::kFinish);
            EXPECT_TRUE(h.restored());
        }
    }
}

TEST(WindowFormatProbes, TheDriverAppliesAndRestoresTheExtraWords)
{
    const std::string driver = switch_ios::test::SourceText("../DisplayWindowProbeDisplay.h");
    EXPECT_NE(switch_ios::test::Between(driver, "void windowProbeShow(", "\n}\n")
                  .find("windowProbe.extras.apply(dc, step.extra, step.extras);"),
              std::string::npos);
    EXPECT_NE(switch_ios::test::Between(driver, "void windowProbeRestoreWindows()", "\n}\n")
                  .find("windowProbe.extras.restore(dc);"),
              std::string::npos);
    EXPECT_NE(switch_ios::test::Between(driver, "void windowProbeFinish()", "\n}\n").find("s.extras.restore(dc);"),
              std::string::npos);
}

TEST(WindowFormatProbes, VisibleFramesStayWithinARunOfEightSteps)
{
    for (const Probe probe : {kWindowFormats, kWindowWide, kWindowPalette}) {
        Harness h(probe);
        unsigned frames = 0;
        for (; frames < 400 && h.run.stage != Run::kDone; ++frames)
            h.frame(true, false, 0x1000 + h.run.step);
        EXPECT_LE(h.run.frames, 8u * 7 + 8) << probeName(probe);
        std::printf("%s: %u steps, %u frame ends from start to finish (%u ms at 60 Hz)\n", probeName(probe),
                    h.plan.steps, h.run.frames, h.run.frames * 1000 / 60);
    }
}

}  // namespace
