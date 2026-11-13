// Hardware planes (NativeMobileDisplay/NativePlanes.h, r18-planes) on the host, one TEST per part:
//  - every swap Core Animation can build from the published limits is accepted and has a window B image (the rule that
//    replaces refusals: CA sends a refused swap again every frame), and the ones outside them are refused for their reason;
//  - window B's register image against the formulas of mainline drm/tegra's tegra_dc_setup_window (DDA 4.12 as
//    compute_dda_inc, PRESCALED in bytes, H offset in luma bytes, the NV12 line strides), clipping to the screen;
//  - the CSC table against the float BT.601/BT.709 conversions, and BT.601 video range against the TRM default;
//  - which layer is the UI (window A) and which window B shows, in A10X's blend order;
//  - PlaneLatch against a model of DC A with the ARM copy (assembly -> ARM on UPDATE, ARM -> active at the boundary on
//    ACT_REQ, a boundary anywhere between the writes): a set the window may scan now or at the next boundary is never free.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../NativePlanes.h"

using namespace native_planes;

namespace {
__attribute__((format(printf, 1, 2))) std::string format(const char *f, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(text, sizeof(text), f, ap);
    va_end(ap);
    return text;
}

// A GoogleTest failure at this line, described printf-style; the test goes on.
#define CHECK(cond, ...)                          \
    do {                                          \
        if (!(cond))                              \
            ADD_FAILURE() << format(__VA_ARGS__); \
    } while (0)

uint64_t rng = 0x9e3779b97f4a7c15ull;

uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return n ? (uint32_t)(rng % n) : 0;
}

constexpr uint32_t W = 720, H = 1280;

uint32_t roundUp(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

Surface makeSurface(Kind kind, uint32_t width, uint32_t height)
{
    Surface s;
    s.width = width;
    s.height = height;
    if (isYuv(kind)) {
        const bool full = kind == kKindNv12Full || kind == kKindNv16Full;
        s.format = fourcc('4', '2', isNv12(kind) ? '0' : '2', full ? 'f' : 'v');
        s.planes = 2;
        s.stride[0] = s.stride[1] = roundUp(width, 256);  // VideoToolbox's pool: 256-byte rows and planes
        s.planeHeight[0] = height;
        s.planeHeight[1] = isNv12(kind) ? (height + 1) / 2 : height;
        s.offset[0] = 0;
        s.offset[1] = roundUp((uint32_t)(s.stride[0] * (uint64_t)height), 256);
        s.allocation = s.offset[1] + (uint64_t)s.stride[1] * s.planeHeight[1];
    } else {
        s.format = kind == kKindRgba ? fourcc('R', 'G', 'B', 'A') : fourcc('B', 'G', 'R', 'A');
        s.stride[0] = roundUp(width * 4, 128);
        s.planeHeight[0] = height;
        s.allocation = (uint64_t)s.stride[0] * height;
    }
    s.descriptorBytes = (s.allocation + 16383) & ~16383ull;
    return s;
}

Layer uiLayer()
{
    Layer l;
    l.kind = kKindBgra;
    l.surface = makeSurface(kKindBgra, W, H);
    l.src = l.dst = {0, 0, (int32_t)W, (int32_t)H};
    return l;
}

// A detached layer Core Animation may send: a format window B reads, a source within MaxSrcRectWidth/Total and the buffer,
// a destination scaled 1/4..4x on each axis, anywhere on (or partly off) the screen.
Layer randomDetached()
{
    static const Kind kinds[] = {kKindNv12Video, kKindNv12Full, kKindNv16Video, kKindNv16Full, kKindBgra, kKindRgba};
    const Limits limits;
    Layer l;
    l.kind = kinds[rnd(6)];
    const uint32_t sw = 32 + rnd(limits.maxSrcBuffer - 32), sh = 32 + rnd(limits.maxSrcBufferHeight - 32);
    l.surface = makeSurface(l.kind, sw, sh);
    int32_t w = 1 + (int32_t)rnd(limits.maxSrcRectWidth < sw ? limits.maxSrcRectWidth : sw);
    int32_t h = 1 + (int32_t)rnd(sh);
    while ((uint64_t)w * (uint64_t)h > limits.maxSrcRectTotal)
        h = h / 2 + 1;
    l.src = {(int32_t)rnd(sw - (uint32_t)w + 1), (int32_t)rnd(sh - (uint32_t)h + 1), w, h};
    // dst in [ceil(src/4), src*4], kept inside 13-bit window coordinates
    auto pick = [](int32_t s) {
        const int32_t low = (s + 3) / 4, high = s * 4 > 4000 ? 4000 : s * 4;
        return low >= high ? low : low + (int32_t)rnd((uint32_t)(high - low + 1));
    };
    int32_t dw = pick(w), dh = pick(h);
    if (dw > 4000 || dh > 4000) {
        dw = dw > 4000 ? 4000 : dw;
        dh = dh > 4000 ? 4000 : dh;
    }
    l.dst = {(int32_t)rnd(W + 200) - 100, (int32_t)rnd(H + 200) - 100, dw, dh};
    return l;
}

void testAcceptance()
{
    const Limits limits;
    unsigned accepted = 0, total = 0, visible = 0;
    for (int n = 0; n < 200000; ++n) {
        Layer layers[kLayers];
        bool has[kLayers] = {};
        Layer d = randomDetached();
        // Within the published limits: CA's own check (scale by integer quotient on A10X; here the exact ratio).
        if ((uint64_t)d.dst.w * 4 < (uint64_t)d.src.w || (uint64_t)d.dst.h * 4 < (uint64_t)d.src.h ||
            (uint64_t)d.dst.w > 4ull * d.src.w || (uint64_t)d.dst.h > 4ull * d.src.h)
            continue;
        const unsigned slot = isYuv(d.kind) ? 2 : 0;  // YUV in layer 2, RGB in layer 0 (QuartzCore [V])
        layers[slot] = d;
        has[slot] = true;
        const bool withUi = rnd(4) != 0;  // video-only swaps: the UI persists
        if (withUi) {
            layers[1] = uiLayer();
            has[1] = true;
        }
        const uint32_t updated = (has[0] ? 1u : 0) | (has[1] ? 2u : 0) | (has[2] ? 4u : 0) | kBackgroundBit;
        ++total;
        const unsigned why = checkUpdated(updated, kOpaqueBlack, has, layers, W, H, limits);
        CHECK(why == kAccepted,
              "swap within the limits refused (reason %u): kind %d src %d,%d %dx%d of %ux%u dst %d,%d %dx%d", why,
              d.kind, d.src.x, d.src.y, d.src.w, d.src.h, d.surface.width, d.surface.height, d.dst.x, d.dst.y, d.dst.w,
              d.dst.h);
        if (why)
            continue;
        ++accepted;
        Image image;
        CHECK(imageOf(d, W, H, image), "accepted layer has no window image");
        if (!image.visible)
            continue;
        ++visible;
        // The window's registers stay in their fields and the read range inside the surface.
        const uint32_t px = image.position & 0xffff, py = image.position >> 16, dw = image.size & 0xffff,
                       dh = image.size >> 16;
        CHECK(px + dw <= W && py + dh <= H && dw && dh, "window outside the screen: %u,%u %ux%u", px, py, dw, dh);
        CHECK((image.prescaled & 0xffff) <= 0x7fff && (image.prescaled >> 16) <= 0x1fff, "prescaled %#x",
              image.prescaled);
        CHECK((image.dda & 0xffff) <= (image.yuv ? 8u : 4u) << 12 && (image.dda >> 16) <= 15u << 12, "dda %#x",
              image.dda);
        CHECK(image.readHigh <= ((d.surface.allocation + 4095) & ~4095ull) && image.readLow < image.readHigh,
              "read range %#llx-%#llx outside %#llx", (unsigned long long)image.readLow,
              (unsigned long long)image.readHigh, (unsigned long long)d.surface.allocation);
        CHECK(image.readHigh - image.readLow <= switch_window_caps::kPlaneSlotSpan,
              "read range %llu bytes exceeds a slot", (unsigned long long)(image.readHigh - image.readLow));
        CHECK(!(image.start[0] % 64) && (!image.yuv || !(image.start[1] % 64)), "plane start not 64-aligned");
        CHECK(!image.yuv || !(image.hOffset & 1), "4:2:0 window starts on an odd column");
    }
    std::printf("acceptance: %u of %u swaps within the limits accepted, %u visible\n", accepted, total, visible);
    CHECK(accepted == total && total > 100000, "acceptance %u/%u", accepted, total);
    // Every one of the 200000 draws is within the limits, and every one is accepted.
    EXPECT_EQ(total, 200000u);
    EXPECT_EQ(accepted, 200000u);
}

void testRefusals()
{
    Layer layers[kLayers];
    bool has[kLayers] = {false, true, true, false};
    layers[1] = uiLayer();
    Layer &d = layers[2];
    d.kind = kKindNv12Video;
    d.surface = makeSurface(kKindNv12Video, 1280, 720);
    d.src = {0, 0, 1280, 720};
    d.dst = {0, 437, 720, 405};
    CHECK(checkUpdated(7u | kBackgroundBit, kOpaqueBlack, has, layers, W, H) == kAccepted, "the reference swap");
    Layer keep = d;
    d.transform = 4;
    CHECK(checkUpdated(6, 0, has, layers, W, H) == kTransformed, "a transform");
    d = keep;
    d.surface.format = fourcc('x', '4', '2', '0');
    d.kind = kindOf(d.surface.format, d.surface.planes);
    CHECK(checkUpdated(6, 0, has, layers, W, H) == kFormat, "10-bit 4:2:0");
    d = keep;
    d.surface.stride[0] = 1344;  // 64- but not 128-aligned
    CHECK(checkUpdated(6, 0, has, layers, W, H) == kAlignment, "a row not 128-aligned");
    d = keep;
    d.dst = {0, 0, 200, 100};  // 6.4x / 7.2x down
    CHECK(checkUpdated(6, 0, has, layers, W, H) == kScale, "a YUV scale past 1/6");
    d = keep;
    d.surface = makeSurface(kKindNv12Video, 3840, 2160);
    d.src = {0, 0, 3840, 2160};
    d.dst = {0, 437, 720, 405};
    CHECK(checkUpdated(6, 0, has, layers, W, H) == kAccepted, "a 4K picture across the panel, 5.3x down");
    d.kind = kKindBgra;
    d.surface = makeSurface(kKindBgra, 3840, 2160);
    CHECK(checkUpdated(6, 0, has, layers, W, H) == kScale, "an RGB scale past 1/4");
    d.kind = kKindNv12Video;
    d.surface = makeSurface(kKindNv12Video, 3840, 2400);
    d.src = {0, 0, 1920, 1080};
    CHECK(checkUpdated(6, 0, has, layers, W, H) == kGeometry, "a buffer past MaxSrcBufferHeight");
    d = keep;
    CHECK(checkUpdated(6, 0x00ff0000u, has, layers, W, H) == kLayerState, "a coloured background");
    CHECK(checkUpdated(6 | 0x100, 0, has, layers, W, H) == kLayerState, "an unknown mask bit");
    has[0] = true;
    layers[0] = keep;
    layers[0].kind = kKindBgra;
    layers[0].surface = makeSurface(kKindBgra, 512, 512);
    layers[0].src = {0, 0, 512, 512};
    layers[0].dst = {0, 0, 512, 512};
    CHECK(checkUpdated(7, 0, has, layers, W, H) == kAccepted, "UI plus two detached layers");
    has[3] = true;
    layers[3] = layers[0];
    CHECK(checkUpdated(15, 0, has, layers, W, H) == kTwoUi, "four layers exceed the three physical windows");
    // Today's swap (one full-frame BGRA layer in layer 0) is still accepted, as before r18.
    bool one[kLayers] = {true, false, false, false};
    Layer ui[kLayers];
    ui[0] = uiLayer();
    CHECK(checkUpdated(1, 0, one, ui, W, H) == kAccepted, "one full-frame layer");
    CHECK(checkUpdated(1u | kBackgroundBit, kOpaqueBlack, one, ui, W, H) == kAccepted, "with the background bit");
}

void testImage()
{
    Layer l;
    l.kind = kKindNv12Video;
    l.surface = makeSurface(kKindNv12Video, 1280, 720);
    l.surface.colorMatrix = 2;
    l.src = {0, 0, 1280, 720};
    l.dst = {0, 437, 720, 405};
    Image m;
    CHECK(imageOf(l, W, H, m) && m.visible, "image");
    CHECK(m.colorDepth == kDepthYCbCr420SP, "depth %u", m.colorDepth);
    CHECK(m.position == (437u << 16), "position %#x", m.position);
    CHECK(m.size == (405u << 16 | 720u), "size %#x", m.size);
    CHECK(m.prescaled == (720u << 16 | 1280u), "prescaled %#x (bytes: 1 per luma pixel)", m.prescaled);
    // A 4x layer is exactly 4.0: within the H limit at 4 bytes per pixel (mainline's (in-1)/(out-1) would be 4.33).
    {
        Layer q;
        q.kind = kKindBgra;
        q.surface = makeSurface(kKindBgra, 40, 40);
        q.src = {0, 0, 40, 40};
        q.dst = {100, 100, 10, 10};
        Image qi;
        CHECK(imageOf(q, W, H, qi) && (qi.dda & 0xffff) == 0x4000, "4x small layer dda %#x", qi.dda);
    }
    const uint32_t h = (1280u << 12) / 720u, v = (720u << 12) / 405u;  // L4T compute_dda_inc: in / out
    CHECK(m.dda == (v << 16 | h), "dda %#x want %#x", m.dda, v << 16 | h);
    CHECK(m.hInitial == (h - 4096) / 2 && m.vInitial == (v - 4096) / 2, "centre phase %#x %#x", m.hInitial, m.vInitial);
    CHECK(m.lineStride == (1280u << 16 | 1280u), "line stride %#x", m.lineStride);
    CHECK(m.options == (kWinEnable | kCscEnable | kHFilterOn | kVFilterOn), "options %#x", m.options);
    CHECK(m.start[0] == 0 && m.start[1] == l.surface.offset[1] && m.hOffset == 0, "starts");
    CHECK(m.matrix == kBt709 && !m.fullRange, "BT.709 video range");
    // 1:1 BGRA: no filters, the unit DDA.
    Layer b;
    b.kind = kKindBgra;
    b.surface = makeSurface(kKindBgra, 720, 1280);
    b.src = b.dst = {0, 0, 720, 1280};
    CHECK(imageOf(b, W, H, m) && m.dda == 0x10001000 && m.options == kWinEnable && m.prescaled == (1280u << 16 | 2880u),
          "1:1 BGRA: dda %#x options %#x prescaled %#x", m.dda, m.options, m.prescaled);
    // Clipped: half off the left edge, at 1:1.
    b.surface = makeSurface(kKindBgra, 400, 300);
    b.src = {0, 0, 400, 300};
    b.dst = {-200, 100, 400, 300};
    CHECK(imageOf(b, W, H, m) && m.visible && m.position == (100u << 16) && m.size == (300u << 16 | 200u) &&
              m.hOffset == 200 * 4 && m.hInitial == 0 && (m.prescaled & 0xffff) == 200 * 4 &&
              (m.prescaled >> 16) == 300,
          "clipped: position %#x size %#x hOffset %u prescaled %#x", m.position, m.size, m.hOffset, m.prescaled);
    // Clipped while scaled 2x down: the cut source column and phase are where the clipped edge samples.
    b.surface = makeSurface(kKindBgra, 800, 600);
    b.src = {0, 0, 800, 600};
    b.dst = {-100, 0, 400, 300};
    CHECK(imageOf(b, W, H, m) && m.visible && m.size == (300u << 16 | 300u) && (m.dda & 0xffff) == 0x2000 &&
              m.hOffset == (200 + 0) * 4 && m.hInitial == 0x800,
          "clipped 2x: size %#x dda %#x hOffset %u initial %#x", m.size, m.dda, m.hOffset, m.hInitial);
    b.dst = {-500, 0, 400, 300};
    CHECK(imageOf(b, W, H, m) && !m.visible, "wholly off the screen: not shown");
    // An odd 4:2:0 crop starts on the chroma pair.
    l.src = {3, 5, 1001, 501};
    CHECK(imageOf(l, W, H, m) && !(m.hOffset & 1) && m.start[0] == 4ull * l.surface.stride[0] &&
              m.start[1] == l.surface.offset[1] + 2ull * l.surface.stride[1] && m.hInitial >= 4096 &&
              m.vInitial >= 4096,
          "odd crop: hOffset %u start %llu/%llu initial %#x %#x", m.hOffset, (unsigned long long)m.start[0],
          (unsigned long long)m.start[1], m.hInitial, m.vInitial);
    // 4:2:2 (Core Animation's copy of a detached video on iPadOS 27): NV16's depth, and the same crop's chroma starts on
    // its own row, the column still on a pair.
    l.kind = kKindNv16Full;
    l.surface = makeSurface(kKindNv16Full, 1280, 720);
    CHECK(imageOf(l, W, H, m) && m.colorDepth == kDepthYCbCr422SP && m.fullRange && !(m.hOffset & 1) &&
              m.start[0] == 5ull * l.surface.stride[0] &&
              m.start[1] == l.surface.offset[1] + 5ull * l.surface.stride[1],
          "4:2:2 crop: depth %u hOffset %u start %llu/%llu", m.colorDepth, m.hOffset, (unsigned long long)m.start[0],
          (unsigned long long)m.start[1]);
    l.surface.planeHeight[1] = 360;
    l.src = {0, 0, 1280, 720};
    CHECK(checkDetached(l, W, H) == kSurface, "a 4:2:2 chroma plane of half the rows");
}

void testCsc()
{
    struct Std {
        double kr, kb;
    } const stds[3] = {{0.299, 0.114}, {0.2126, 0.0722}, {0.2627, 0.0593}};

    for (unsigned s = 0; s < 3; ++s)
        for (unsigned full = 0; full < 2; ++full) {
            const double kg = 1 - stds[s].kr - stds[s].kb, cy = full ? 1.0 : 255.0 / 219, cc = full ? 1.0 : 255.0 / 224;
            const double want[8] = {full ? 0.0 : -16.0,
                                    cy,
                                    0,
                                    2 * (1 - stds[s].kr) * cc,
                                    -2 * stds[s].kb * (1 - stds[s].kb) / kg * cc,
                                    -2 * stds[s].kr * (1 - stds[s].kr) / kg * cc,
                                    2 * (1 - stds[s].kb) * cc,
                                    0};
            const unsigned bits[8] = {8, 10, 11, 11, 10, 10, 11, 11};
            for (unsigned i = 0; i < 8; ++i) {
                int32_t v = (int32_t)kCsc[s][full][i];
                if (v & (1 << (bits[i] - 1)) && i != 1)
                    v -= 1 << bits[i];  // KYRGB is unsigned
                const double got = i ? v / 256.0 : (double)v;
                CHECK(std::fabs(got - want[i]) <= (i ? 0.5 / 256 + 1e-9 : 0.5), "csc %u/%u[%u] %f want %f", s, full, i,
                      got, want[i]);
            }
        }
    const uint32_t trm[8] = {0xf0, 0x12a, 0, 0x198, 0x39b, 0x32f, 0x204, 0};  // every source's BT.601 default
    for (unsigned i = 0; i < 8; ++i) {
        const int diff = (int)kCsc[kBt601][0][i] - (int)trm[i];
        CHECK(diff >= -1 && diff <= 1, "BT.601 video [%u] %#x vs the TRM's %#x", i, kCsc[kBt601][0][i], trm[i]);
    }
}

void testRoles()
{
    Layer layers[kLayers];
    layers[1] = uiLayer();
    layers[2].kind = kKindNv12Video;
    layers[2].surface = makeSurface(kKindNv12Video, 1280, 720);
    layers[2].src = {0, 0, 1280, 720};
    layers[2].dst = {0, 0, 720, 405};
    bool present[kLayers] = {false, true, true, false};
    Roles r = rolesOf(present, layers, W, H);
    CHECK(r.ui == 1 && r.plane == 2 && !r.extra, "UI 1 over video 2: %d %d %u", r.ui, r.plane, r.extra);
    bool alone[kLayers] = {true, false, false, false};
    layers[0] = uiLayer();
    r = rolesOf(alone, layers, W, H);
    CHECK(r.ui == 0 && r.plane == -1, "the UI alone in layer 0");
    bool both[kLayers] = {true, true, false, false};  // a full-screen detached RGB layer 0 under the UI 1
    r = rolesOf(both, layers, W, H);
    CHECK(r.ui == 1 && r.plane == 0 && !r.extra, "two full-frame layers: %d %d", r.ui, r.plane);
    bool three[kLayers] = {true, true, true, false};
    r = rolesOf(three, layers, W, H);
    CHECK(r.ui == 1 && r.plane == 0 && r.second == 2 && !r.extra,
          "three layers retain both detached layers in stack order");
}

// DC A's window B with the ARM copy: writes go to assembly; UPDATE copies assembly to ARM; at a boundary with ACT_REQ out,
// ARM becomes active. The driver writes a set as: registers, UPDATE, ACT_REQ (three steps a boundary may fall between).
struct WindowModel {
    int assembly = PlaneLatch::kOff, arm = PlaneLatch::kOff, active = PlaneLatch::kOff;
    bool request = false;

    void boundary()
    {
        if (request) {
            active = arm;
            request = false;
        }
    }
};

void testLatch()
{
    unsigned latches = 0, holds = 0;
    for (int run = 0; run < 2000; ++run) {
        PlaneLatch latch;
        WindowModel dc;
        int held = PlaneLatch::kNone;
        for (int step = 0; step < 400; ++step) {
            const unsigned what = rnd(4);
            if (what == 0) {  // a frame end (after the boundary that ended the frame)
                dc.boundary();
                const int scanned = dc.active;
                if (latch.latched(dc.request, scanned)) {
                    ++latches;
                    CHECK(latch.active == dc.active, "latched set %d but the window scans %d", latch.active, dc.active);
                }
                if (held != PlaneLatch::kNone && latch.mayWrite()) {
                    latch.write(held);
                    dc.assembly = held;
                    dc.arm = dc.assembly;
                    dc.request = true;
                    held = PlaneLatch::kNone;
                }
            } else {  // a new set from the driver, maybe a boundary inside its writes
                const int set = (int)rnd(6) - 1;
                if (!latch.mayWrite()) {
                    held = set;
                    ++holds;
                    continue;
                }
                latch.write(set);
                dc.assembly = set;
                if (rnd(3) == 0)
                    dc.boundary();  // between the registers and UPDATE
                dc.arm = dc.assembly;
                if (rnd(3) == 0)
                    dc.boundary();  // between UPDATE and ACT_REQ
                dc.request = true;
            }
            // Safety: whatever the window scans now, or may take at the next boundary, is busy.
            CHECK(dc.active == PlaneLatch::kOff || latch.busy(dc.active), "scanned set %d not busy", dc.active);
            CHECK(!dc.request || dc.arm == PlaneLatch::kOff || latch.busy(dc.arm), "armed set %d not busy", dc.arm);
        }
    }
    std::printf("latch: %u latches seen, %u sets held for a settling frame end\n", latches, holds);
    CHECK(latches > 100000, "too few latches: %u", latches);
}
}  // namespace

// One process, in this order: the scenarios draw from one random stream (rng), as the harness always did.
TEST(DisplayPlanes, AcceptanceWithinThePublishedLimits)
{
    testAcceptance();
}

TEST(DisplayPlanes, RefusalsForTheirReason)
{
    testRefusals();
}

TEST(DisplayPlanes, WindowBImageAgainstDrmTegra)
{
    testImage();
}

TEST(DisplayPlanes, CscAgainstFloatConversions)
{
    testCsc();
}

TEST(DisplayPlanes, LayerRoles)
{
    testRoles();
}

TEST(DisplayPlanes, LatchAgainstDcAModel)
{
    testLatch();
}

// The alignment the display publishes suits every engine that reads a surface sized by it: window B's validation
// (Limits), the GPU's in-place linear rows (128) and VIC's pitch (256). Core Animation's portrait UI at those rows is
// accepted as the UI layer.
TEST(DisplayPlanes, PublishedAlignmentSuitsEveryReader)
{
    const Limits limits;
    EXPECT_EQ(kPublishedRowAlignment % limits.strideAlignment, 0u);
    EXPECT_EQ(kPublishedBaseAlignment % limits.planeBaseAlignment, 0u);
    EXPECT_EQ(kPublishedRowAlignment % 128, 0u);
    EXPECT_EQ(kPublishedRowAlignment % 256, 0u);
    EXPECT_EQ(kPublishedBaseAlignment % 256, 0u);
    Layer layers[kLayers];
    bool has[kLayers] = {};
    layers[1] = uiLayer();
    layers[1].surface.stride[0] = roundUp(W * 4, kPublishedRowAlignment);
    layers[1].surface.allocation = (uint64_t)layers[1].surface.stride[0] * H;
    layers[1].surface.descriptorBytes = (layers[1].surface.allocation + 16383) & ~16383ull;
    has[1] = true;
    EXPECT_EQ(layers[1].surface.stride[0], 3072u);
    EXPECT_EQ(checkUpdated(2u | kBackgroundBit, kOpaqueBlack, has, layers, W, H, limits), (unsigned)kAccepted);
}

// iOS 27 publishes iOS 17's limits and, where QuartzCore 24B119 no longer goes by J120's chip id, packed 10-bit YUV and
// compressed surfaces ruled out.
TEST(DisplayPlanes, Limits24B119AreIos17sWithoutPackedOrCompressed)
{
    const Limits ios17;
    const Limits &ios27 = kLimits24B119;
    EXPECT_FALSE(ios17.packedYuvUnsupported);
    EXPECT_TRUE(ios27.packedYuvUnsupported);
    EXPECT_FALSE(ios17.compressionUnsupported);
    EXPECT_TRUE(ios27.compressionUnsupported);
    EXPECT_EQ(ios27.minScaleFraction, ios17.minScaleFraction);
    EXPECT_EQ(ios27.maxScale, ios17.maxScale);
    EXPECT_EQ(ios27.maxSrcRectWidth, ios17.maxSrcRectWidth);
    EXPECT_EQ(ios27.maxSrcRectTotal, ios17.maxSrcRectTotal);
    EXPECT_EQ(ios27.maxSrcBuffer, ios17.maxSrcBuffer);
    EXPECT_EQ(ios27.maxSrcBufferHeight, ios17.maxSrcBufferHeight);
    EXPECT_EQ(ios27.yuvMinScaleFraction, ios17.yuvMinScaleFraction);
    EXPECT_EQ(ios27.planeBaseAlignment, ios17.planeBaseAlignment);
    EXPECT_EQ(ios27.strideAlignment, ios17.strideAlignment);
    EXPECT_EQ(ios27.maxLayers, ios17.maxLayers);
    EXPECT_EQ(ios27.maxLayers, 3u);
}

TEST(DisplayPlanes, MatrixFollowsMetadataWithoutResolutionInference)
{
    Surface surface = makeSurface(kKindNv12Video, 1920, 1080);
    EXPECT_EQ(matrixOf(surface), kBt601);
    surface.colorMatrix = 1;
    EXPECT_EQ(matrixOf(surface), kBt601);
    surface.width = surface.height = 32;
    surface.colorMatrix = 2;
    EXPECT_EQ(matrixOf(surface), kBt709);
    surface.colorMatrix = 3;
    EXPECT_EQ(matrixOf(surface), kBt2020);
}

TEST(DisplayPlanes, WindowSitingDecidesConversionAndNoColourTagRefusesTheSwap)
{
    EXPECT_TRUE(switch_surface_dc_siting(0, 0));
    EXPECT_TRUE(switch_surface_dc_siting(1, 1));
    EXPECT_FALSE(switch_surface_dc_siting(2, 2));
    EXPECT_FALSE(switch_surface_dc_siting(1, 3));
    Layer l;
    l.kind = kKindNv12Video;
    l.surface = makeSurface(l.kind, 1920, 1080);
    l.src = {0, 0, 1920, 1080};
    l.dst = {0, 0, 720, 405};
    const uint32_t tags[][5] = {{16, 9, 3, 1, 1}, {18, 9, 3, 1, 1}, {1, 12, 2, 1, 1},
                                {1, 1, 7, 1, 1},  {1, 1, 2, 2, 2},  {1, 1, 2, 1, 3}};
    for (const auto &tag : tags) {
        l.surface.transfer = tag[0];
        l.surface.primaries = tag[1];
        l.surface.colorMatrix = tag[2];
        l.surface.chromaTop = tag[3];
        l.surface.chromaBottom = tag[4];
        EXPECT_EQ(checkDetached(l, W, H), kAccepted) << tag[0] << " " << tag[1] << " " << tag[3];
        Image image;
        ASSERT_TRUE(imageOf(l, W, H, image));
        EXPECT_EQ(image.convert, tag[3] != 1 || tag[4] != 1);
        EXPECT_EQ(image.matrix, tag[2] == 3 ? kBt2020 : tag[2] == 2 ? kBt709 : kBt601);
    }
    l.kind = kKindBgra;
    l.surface = makeSurface(l.kind, 720, 405);
    l.surface.transfer = 16;
    l.surface.primaries = 12;
    l.src = {0, 0, 720, 405};
    Image image;
    ASSERT_TRUE(imageOf(l, W, H, image));
    EXPECT_FALSE(image.convert);
}
