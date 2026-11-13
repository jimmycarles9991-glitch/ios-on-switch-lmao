// What DC A shows for a detached layer (r18-planes' window B under window A), against what Core Animation would have
// composited: the UI, premultiplied, over the detached layer, over opaque black. The blend registers the driver writes
// (NativePlanes.h's values, as NativePlanesDisplay.h writes them) are decoded with the gen2 blender's factor fields
// (mainline drivers/gpu/drm/tegra/dc.h BLEND_FACTOR_*, K1, K2, WINDOW_LAYER_DEPTH, BLEND_BYPASS: lower depth in front,
// bypass replaces what is behind) and run over every 8-bit UI colour and alpha against a set of video colours.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>

#include "../NativePlanes.h"
#include "bazel/testing/source_text.h"

using namespace native_planes;

namespace {

struct Pixel {
    double r, g, b, a;
};

struct Window {
    uint32_t layer, match;  // BLEND_LAYER_CONTROL and the select in use (no colour key: match and nomatch are the same)
    bool visible;
    Pixel pixel;  // premultiplied
};

double k1(uint32_t layer)
{
    return ((layer >> 8) & 0xff) / 255.0;
}

double k2(uint32_t layer)
{
    return ((layer >> 16) & 0xff) / 255.0;
}

unsigned depth(uint32_t layer)
{
    return layer & 0xff;
}

bool bypass(uint32_t layer)
{
    return layer & kBlendBypass;
}

// BLEND_FACTOR_SRC_COLOR_*: ZERO, ONE, K1, K1_TIMES_DST, NEG_K1_TIMES_DST, K1_TIMES_SRC.
double srcColorFactor(uint32_t match, uint32_t layer, const Pixel &src, const Pixel &dst)
{
    switch (match & 0xf) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 2:
        return k1(layer);
    case 3:
        return k1(layer) * dst.a;
    case 4:
        return 1 - k1(layer) * dst.a;
    case 5:
        return k1(layer) * src.a;
    }
    ADD_FAILURE() << "source colour factor " << (match & 0xf);
    return 0;
}

// BLEND_FACTOR_DST_COLOR_*: ZERO, ONE, K1, K2, K1_TIMES_DST, NEG_K1_TIMES_DST, NEG_K1_TIMES_SRC, NEG_K1.
double dstColorFactor(uint32_t match, uint32_t layer, const Pixel &src, const Pixel &dst)
{
    switch ((match >> 4) & 0xf) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 2:
        return k1(layer);
    case 3:
        return k2(layer);
    case 4:
        return k1(layer) * dst.a;
    case 5:
        return 1 - k1(layer) * dst.a;
    case 6:
        return 1 - k1(layer) * src.a;
    case 7:
        return 1 - k1(layer);
    }
    ADD_FAILURE() << "destination colour factor " << ((match >> 4) & 0xf);
    return 0;
}

// What the panel gets: the background, then each visible window from the deepest to the front one.
Pixel scanout(Window *windows, unsigned count)
{
    Pixel out = {0, 0, 0, 1};
    for (unsigned d = 256; d-- > 0;) {
        for (unsigned i = 0; i < count; ++i) {
            const Window &w = windows[i];
            if (!w.visible || depth(w.layer) != d)
                continue;
            if (bypass(w.layer)) {
                out = w.pixel;
                out.a = 1;
                continue;
            }
            const double s = srcColorFactor(w.match, w.layer, w.pixel, out);
            const double t = dstColorFactor(w.match, w.layer, w.pixel, out);
            out = {w.pixel.r * s + out.r * t, w.pixel.g * s + out.g * t, w.pixel.b * s + out.b * t, out.a};
        }
    }
    return out;
}

// Core Animation's composite: the UI (premultiplied) over the opaque detached layer.
Pixel composite(const Pixel &ui, const Pixel &video)
{
    return {ui.r + video.r * (1 - ui.a), ui.g + video.g * (1 - ui.a), ui.b + video.b * (1 - ui.a), 1};
}

constexpr double kStep = 1 / 255.0;

bool near(const Pixel &a, const Pixel &b)
{
    return std::fabs(a.r - b.r) <= kStep / 2 && std::fabs(a.g - b.g) <= kStep / 2 && std::fabs(a.b - b.b) <= kStep / 2;
}

// The windows as the driver programs them while window B shows a detached layer: B opaque at depth 2 (planeSetB),
// A premultiplied over at depth 0 (planeBlendA(true)).
constexpr uint32_t kWindowBLayer = kBlendBypass | 2;

TEST(DisplayBlend, DetachedLayerMatchesCoreAnimationsComposite)
{
    const Pixel videos[] = {{0, 0, 0, 1}, {1, 1, 1, 1}, {0.2, 0.5, 0.8, 1}, {1, 0, 0, 1}, {0.5, 0.5, 0.5, 1}};
    unsigned checked = 0;
    for (const Pixel &video : videos)
        for (unsigned a = 0; a < 256; ++a)
            for (unsigned c = 0; c <= a; c += 3) {
                const Pixel ui = {c / 255.0, (a - c) / 255.0, (c / 2) / 255.0, a / 255.0};
                Window w[2] = {{kBlendPremultiplied, kBlendOver, true, ui}, {kWindowBLayer, 0, true, video}};
                const Pixel got = scanout(w, 2), want = composite(ui, video);
                ASSERT_TRUE(near(got, want)) << "alpha " << a << " colour " << c << ": " << got.r << " vs " << want.r;
                ++checked;
            }
    EXPECT_GT(checked, 50000u);
}

TEST(DisplayBlend, OutsideTheDetachedLayerTheUiIsOverBlack)
{
    for (unsigned a = 0; a < 256; ++a) {
        const Pixel ui = {a / 255.0 / 2, a / 255.0, 0, a / 255.0};
        Window w[2] = {{kBlendPremultiplied, kBlendOver, true, ui}, {kWindowBLayer, 0, false, {}}};
        ASSERT_TRUE(near(scanout(w, 2), composite(ui, {0, 0, 0, 1}))) << a;
    }
}

TEST(DisplayBlend, WindowAIsInFrontOfWindowB)
{
    EXPECT_LT(depth(kBlendPremultiplied), depth(kWindowBLayer));
    EXPECT_FALSE(bypass(kBlendPremultiplied)) << "window A must blend while window B shows something";
    EXPECT_TRUE(bypass(kWindowBLayer)) << "the detached layer is opaque: Core Animation detaches only opaque layers";
    EXPECT_EQ(k1(kBlendPremultiplied), 1.0) << "K1 255: the UI's premultiplied colour as it is";
}

TEST(DisplayBlend, TheDriverWritesTheseValues)
{
    const std::string driver = (switch_ios::test::SourceText("../NativePlanesDisplay.h") +
                                switch_ios::test::SourceText("../SwitchOverlayRegisters.h"));
    EXPECT_NE(driver.find("win[kBlendLayer] = m.yuv ? kBlendBypass | p.depth : kBlendPremultiplied | p.depth;"),
              std::string::npos);
    const std::string blendA = switch_ios::test::Between(driver, "void planeBlendA(bool over", "\n}\n");
    EXPECT_NE(blendA.find("a[native_planes::kBlendLayer] = native_planes::kBlendPremultiplied;"), std::string::npos);
    EXPECT_NE(blendA.find("a[native_planes::kBlendMatch] = native_planes::kBlendOver;"), std::string::npos);
    EXPECT_NE(blendA.find("a[native_planes::kBlendNomatch] = native_planes::kBlendOver;"), std::string::npos);
}

}  // namespace
