// SPDX-License-Identifier: MIT
// raw/runtime/raw_rotation.h and raw_rotated_pass.h on the host. A turned pass rasterises in the plane: each frame pixel must
// land where Metal stores it (metal2vulkan-runtime rotated.rs source_map, Turn::Store), through the viewport words and the
// swizzle the encoder emits, with the winding kept; the clip, scissor and clear rectangles turn by the same map. The device's
// probe (gpu.x1.viewport_swizzle, 11 Nov) saw 0x6402 under the plain viewport exchange x and y as an anti-transpose.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "../raw/runtime/raw_rotated_pass.h"

namespace {

using raw::Rect;
using raw::Rotation;

Rotation turned(uint8_t turn, uint32_t w, uint32_t h)
{
    Rotation r;
    r.turn = turn;
    r.width = w;
    r.height = h;
    return r;
}

// rotated.rs source_map(.., Turn::Store) as the pixel each frame pixel lands at
std::pair<uint32_t, uint32_t> metalStore(uint8_t turn, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
    switch (turn) {
    case 1:
    case 4:
        return {h - 1 - y, x};
    case 2:
        return {w - 1 - x, h - 1 - y};
    case 3:
        return {y, w - 1 - x};
    default:
        return {x, y};
    }
}

// The plain viewport the encoder sets for Metal's (x, y, w, h): scale (w/2, -h/2), offset at the rectangle's centre
switch_gm20b_viewport_state frameViewport(float x, float y, float w, float h)
{
    switch_gm20b_viewport_state v = {};
    v.scale[0] = 0.5f * w;
    v.scale[1] = -0.5f * h;
    v.scale[2] = 1;
    v.offset[0] = x + 0.5f * w;
    v.offset[1] = y + 0.5f * h;
    v.clip_x = uint16_t(x);
    v.clip_y = uint16_t(y);
    v.clip_width = uint16_t(w);
    v.clip_height = uint16_t(h);
    return v;
}

// The window position the hardware gives normalized (xn, yn) under `swizzle` and viewport `v`
std::pair<double, double> window(uint32_t swizzle, const switch_gm20b_viewport_state &v, double xn, double yn)
{
    const auto pick = [&](uint32_t field) {
        switch (field) {
        case 0:
            return xn;
        case 1:
            return -xn;
        case 2:
            return yn;
        case 3:
            return -yn;
        default:
            ADD_FAILURE() << "a swizzle field off x and y: " << field;
            return 0.0;
        }
    };
    return {v.scale[0] * pick(swizzle & 7) + v.offset[0], v.scale[1] * pick((swizzle >> 4) & 7) + v.offset[1]};
}

TEST(RawRotation, PixelRectanglesLandWhereMetalStoresThem)
{
    for (uint8_t turn = 1; turn <= 4; turn++)
        for (const auto [w, h] : {std::pair{5u, 3u}, {60u, 60u}, {1280u, 720u}, {7u, 9u}})
            for (uint32_t y = 0; y < h; y += std::max(1u, h / 11))
                for (uint32_t x = 0; x < w; x += std::max(1u, w / 13)) {
                    const Rect r = turned(turn, w, h).rect({x, y, x + 1, y + 1});
                    const auto [px, py] = metalStore(turn, w, h, x, y);
                    EXPECT_EQ(r, (Rect{px, py, px + 1, py + 1}))
                        << int(turn) << " " << w << "x" << h << " " << x << "," << y;
                }
}

TEST(RawRotation, ViewportAndSwizzleRasteriseIntoTheStoredPixel)
{
    for (uint8_t turn = 1; turn <= 4; turn++)
        for (const auto [w, h] : {std::pair{64u, 64u}, {1280u, 720u}, {60u, 30u}})
            for (const auto &vp : {std::array<float, 4>{0, 0, float(w), float(h)}, {3, 5, 20, 11}}) {
                const Rotation r = turned(turn, w, h);
                switch_gm20b_viewport_state v = frameViewport(vp[0], vp[1], vp[2], vp[3]);
                const switch_gm20b_viewport_state frame = v;
                raw::turnViewport(r, v);
                for (const double xn : {-1.0, -0.25, 0.4, 1.0})
                    for (const double yn : {-1.0, 0.1, 0.75, 1.0}) {
                        const auto [fx, fy] = window(raw::kSwizzleIdentity, frame, xn, yn);
                        const auto [px, py] = window(r.swizzle(), v, xn, yn);
                        // the continuous form of the store map: the pixel (x, y) is the unit square at (x, y)
                        double ex = fx, ey = fy;
                        if (turn == 1 || turn == 4)
                            ex = h - fy, ey = fx;
                        else if (turn == 2)
                            ex = w - fx, ey = h - fy;
                        else if (turn == 3)
                            ex = fy, ey = w - fx;
                        EXPECT_NEAR(px, ex, 1e-3) << int(turn) << " at " << xn << "," << yn;
                        EXPECT_NEAR(py, ey, 1e-3) << int(turn) << " at " << xn << "," << yn;
                    }
                const Rect clip = r.rect({frame.clip_x, frame.clip_y, uint32_t(frame.clip_x + frame.clip_width),
                                          uint32_t(frame.clip_y + frame.clip_height)});
                EXPECT_EQ(
                    (Rect{v.clip_x, v.clip_y, uint32_t(v.clip_x + v.clip_width), uint32_t(v.clip_y + v.clip_height)}),
                    clip);
            }
}

TEST(RawRotation, TheDevicesSwizzleProbeIsAnAntiTranspose)
{
    const uint32_t size = 64;
    const switch_gm20b_viewport_state v = frameViewport(0, 0, size, size);
    for (const double xn : {-0.9, 0.3})
        for (const double yn : {-0.6, 0.8}) {
            const auto [fx, fy] = window(raw::kSwizzleIdentity, v, xn, yn);
            const auto [sx, sy] = window(0x6402, v, xn, yn);
            EXPECT_NEAR(sx, size - fy, 1e-3);
            EXPECT_NEAR(sy, size - fx, 1e-3);
        }
}

TEST(RawRotation, WindingIsKept)
{
    for (uint8_t turn = 0; turn <= 4; turn++) {
        const Rotation r = turned(turn, 300, 200);
        switch_gm20b_viewport_state v = frameViewport(0, 0, 300, 200);
        raw::turnViewport(r, v);
        const auto at = [&](double xn, double yn) {
            return window(r.swizzle(), v, xn, yn);
        };
        const auto [ax, ay] = at(0, 0);
        const auto [bx, by] = at(1, 0);
        const auto [cx, cy] = at(0, 1);
        const double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        EXPECT_LT(area, 0) << "rotation " << int(turn) << " turns the triangle's facing";
    }
}

TEST(RawRotation, SamplePositionsTurnWithThePixel)
{
    const uint8_t standard4[] = {0x26, 0x6e, 0xa2, 0xea};
    for (uint8_t turn = 1; turn <= 3; turn++) {
        const Rotation r = turned(turn, 8, 8);
        for (const uint8_t s : standard4) {
            const uint32_t x = s & 15, y = s >> 4, t = r.samplePosition(s);
            const auto [px, py] = metalStore(turn, 16, 16, x, y);
            // the in-pixel position turned: sixteenths, the far edge 16 rather than 15 - a
            EXPECT_EQ(t & 15, turn == 3 ? px : (px + 1) % 16) << int(turn);
            EXPECT_EQ(t >> 4, turn == 1 ? py : (py + 1) % 16) << int(turn);
        }
    }
    uint32_t words[4] = {0x26266e6e, 0, 0, 0};
    raw::turnPositions(Rotation(), words);
    EXPECT_EQ(words[0], 0x26266e6eu);
}

TEST(RawRotation, ProgramsThatSeeThePlanesAxes)
{
    uint32_t sph[20] = {};
    std::vector<uint32_t> code(16, 0);
    EXPECT_FALSE(raw::turnSensitive(sph, code.data(), uint32_t(code.size()), false));
    sph[5] = 1u << 28;
    EXPECT_TRUE(raw::turnSensitive(sph, code.data(), uint32_t(code.size()), false));
    EXPECT_FALSE(raw::turnSensitive(sph, code.data(), uint32_t(code.size()), true));
    sph[5] = 0;
    sph[14] = 1u << 25;
    EXPECT_TRUE(raw::turnSensitive(sph, code.data(), uint32_t(code.size()), true));
    sph[14] = 0;
    code[1] = 0x50f80000;  // a scheduling word whose bits look like FSWZADD
    EXPECT_FALSE(raw::turnSensitive(sph, code.data(), uint32_t(code.size()), false));
    code[1] = 0;
    code[5] = 0x50f80000;  // the second instruction of the first group
    EXPECT_TRUE(raw::turnSensitive(sph, code.data(), uint32_t(code.size()), true));
}

struct FakeTexture {
    uint8_t rotation = 0;
    uint32_t width = 0, height = 0;
    bool memoryless = false;
    uint8_t bpp = 4;
    uint32_t format = 80;
};

struct FakeAttachment {
    const FakeTexture *texture = nullptr;

    enum Load : uint8_t {
        DontCare,
        Load,
        Clear
    } load = DontCare;

    bool store = true;
};

struct FakeZeta {
    const FakeTexture *texture = nullptr;
    bool loaded = true, stored = true;
};

struct FakePass {
    FakeAttachment color[8];
    uint32_t count = 0;
    FakeZeta depthStencil;
};

TEST(RawRotation, WhichPassesDrawIntoThePlane)
{
    const FakeTexture drawable{1, 720, 1280}, other{3, 720, 1280}, memoryless{0, 1280, 720, true},
        smallMemoryless{0, 640, 360, true}, stored{0, 1280, 720}, depth{0, 1280, 720, true}, storedDepth{0, 1280, 720};
    FakePass p;
    p.count = 2;
    p.color[0].texture = &drawable;
    p.color[1].texture = &memoryless;
    p.depthStencil.texture = &depth;
    Rotation r;
    EXPECT_EQ(raw::passRotation(p, r), raw::RotatedRefusal::None);
    EXPECT_EQ(r.turn, 1);
    EXPECT_EQ(r.width, 1280u);
    EXPECT_EQ(r.height, 720u);
    EXPECT_EQ(r.planeWidth(), 720u);
    p.color[1].texture = &smallMemoryless;
    uint32_t w = 0, h = 0;
    raw::frameArea(p, r, w, h);
    EXPECT_EQ(w, 640u);
    EXPECT_EQ(h, 360u);
    const FakeTexture backing = raw::planeShaped(smallMemoryless, r);
    EXPECT_EQ(backing.width, 720u);
    EXPECT_EQ(backing.height, 1280u);
    p.color[1].texture = &stored;
    EXPECT_EQ(raw::passRotation(p, r), raw::RotatedRefusal::Mixed);
    p.color[1].store = false;
    EXPECT_EQ(raw::passRotation(p, r), raw::RotatedRefusal::None);  // transient: a plane-shaped backing stands in
    EXPECT_TRUE(raw::standsInTurned(stored, r));
    EXPECT_FALSE(raw::standsInTurned(drawable, r));
    p.color[1].load = FakeAttachment::Load;
    EXPECT_EQ(raw::passRotation(p, r), raw::RotatedRefusal::Mixed);
    p.color[1] = {};
    p.color[1].texture = &other;
    EXPECT_EQ(raw::passRotation(p, r), raw::RotatedRefusal::Mixed);
    p.color[1].texture = nullptr;
    p.depthStencil.texture = &storedDepth;
    EXPECT_EQ(raw::passRotation(p, r), raw::RotatedRefusal::Mixed);
    p.depthStencil.loaded = p.depthStencil.stored = false;
    EXPECT_EQ(raw::passRotation(p, r), raw::RotatedRefusal::None);
    p.depthStencil.texture = nullptr;
    const FakeTexture fourth{4, 720, 1280};
    p.color[0].texture = &fourth;
    p.color[0].load = FakeAttachment::Load;
    EXPECT_EQ(raw::passRotation(p, r), raw::RotatedRefusal::LoadsTransposed);
    p.color[0].load = FakeAttachment::Clear;
    EXPECT_EQ(raw::passRotation(p, r), raw::RotatedRefusal::None);
    p.color[0].texture = &stored;
    EXPECT_EQ(raw::passRotation(p, r), raw::RotatedRefusal::None);
    EXPECT_FALSE(r.turned());
}

// The root table's turn words map each plane pixel's centre to the frame pixel Metal stores there
TEST(RawRotation, TurnWordsMapThePlaneOntoTheFrame)
{
    const auto value = [](uint32_t u) {
        float f;
        memcpy(&f, &u, 4);
        return f;
    };
    for (uint8_t turn = 1; turn <= 4; turn++) {
        const Rotation r = turned(turn, 96, 64);
        uint32_t w[raw::kTurnWords];
        raw::turnWords(r, w);
        for (uint32_t px = 0; px < r.planeWidth(); px += 5)
            for (uint32_t py = 0; py < r.planeHeight(); py += 3) {
                const float x = px + 0.5f, y = py + 0.5f;
                const float fx = value(w[1]) * (w[0] ? y : x) + value(w[3]);
                const float fy = value(w[2]) * (w[0] ? x : y) + value(w[4]);
                // the frame texel whose store lands at (px, py)
                const raw::Rect at = r.rect({uint32_t(fx), uint32_t(fy), uint32_t(fx) + 1, uint32_t(fy) + 1});
                EXPECT_EQ(at, (raw::Rect{px, py, px + 1, py + 1})) << int(turn) << " " << px << " " << py;
                EXPECT_EQ(fx - std::floor(fx), 0.5f);
                EXPECT_EQ(fy - std::floor(fy), 0.5f);
            }
    }
}

// A turned pass the raw path draws once its turn copies have moved what it keeps
TEST(RawRotation, WhichPassesTheTurnCopiesMakeDrawable)
{
    const FakeTexture drawable{1, 720, 1280}, fourth{4, 720, 1280}, stored{0, 1280, 720}, small{0, 640, 360},
        odd{0, 1280, 720, false, 3}, depth{0, 1280, 720, false, 4, 252}, depthStencil{0, 1280, 720, false, 8, 260},
        third{3, 720, 1280}, smallTurned{1, 360, 640}, oddTurned{2, 1280, 720, false, 3},
        stencil{0, 1280, 720, false, 1, 253}, smallStencil{0, 640, 360, false, 8, 260};
    FakePass p;
    p.count = 2;
    p.color[0].texture = &drawable;
    p.color[1].texture = &stored;
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::None);
    p.color[1].texture = &small;  // an unturned attachment of another size moves over the render area
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::None);
    p.color[1].texture = &third;  // another turn, and another frame
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::None);
    p.color[1].texture = &smallTurned;
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::None);
    p.color[1].texture = &oddTurned;
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::Mixed);
    p.color[1].texture = &odd;
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::Mixed);
    p.color[1].texture = &fourth;  // rotation 4 stores as rotation 1
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::None);
    p.color[1].load = FakeAttachment::Load;
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::None);
    p.count = 1;
    p.depthStencil.texture = &depth;
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::None);
    p.depthStencil.texture = &depthStencil;  // its stencil moved by bits (raw_stencil_bits.h)
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::None);
    p.depthStencil.texture = &stencil;
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::None);
    p.depthStencil.texture = &smallStencil;  // another size moves over the render area
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::None);
    p.depthStencil.loaded = p.depthStencil.stored = false;
    EXPECT_EQ(raw::turnablePass(p), raw::RotatedRefusal::None);
}

// The turn copies' maps send each texel where Metal's store map does, and back
TEST(RawRotation, TurnCopiesFollowTheStoreMap)
{
    const auto at = [](const raw::TurnMap &m, int32_t x, int32_t y) {
        return std::pair<int32_t, int32_t>{m.ax * x + m.ay * y + m.cx, m.bx * x + m.by * y + m.cy};
    };
    for (uint8_t turn = 1; turn <= 4; turn++) {
        const Rotation r = turned(turn, 96, 64);
        for (uint32_t y = 0; y < r.height; y += 3)
            for (uint32_t x = 0; x < r.width; x += 5) {
                const raw::Rect p = r.rect({x, y, x + 1, y + 1});
                const std::pair<int32_t, int32_t> plane{int32_t(p.x0), int32_t(p.y0)}, frame{int32_t(x), int32_t(y)};
                EXPECT_EQ(at(raw::planeToFrame(r), frame.first, frame.second), plane) << int(turn);
                EXPECT_EQ(at(raw::frameToPlane(r), plane.first, plane.second), frame) << int(turn);
            }
    }
    // rotation 4's Load reads the plane transposed: after the mirror, rotation 1's store map reads plane (y, x)
    const Rotation one = turned(1, 96, 64);
    for (uint32_t y = 0; y < 64; y += 7)
        for (uint32_t x = 0; x < 96; x += 11) {
            const raw::Rect p = one.rect({x, y, x + 1, y + 1});
            EXPECT_EQ(at(raw::planeMirror(one.planeWidth()), int32_t(p.x0), int32_t(p.y0)),
                      (std::pair<int32_t, int32_t>{int32_t(y), int32_t(x)}));
        }
}

// An attachment beside the pass's turned one, turned another way or of another frame, goes into a stand-in of the pass's
// plane through its own load map and back through its store map: the pass sees each frame texel as Metal loads it, and the
// attachment ends with each texel inside the render area where Metal stores it and every other texel kept
TEST(RawRotation, StandInsMoveEachTexelByTheAttachmentsOwnMaps)
{
    const auto at = [](const raw::TurnMap &m, int32_t x, int32_t y, int32_t &sx, int32_t &sy) {
        sx = m.ax * x + m.ay * y + m.cx;
        sy = m.bx * x + m.by * y + m.cy;
        return sx >= m.x0 && sy >= m.y0 && sx < m.x1 && sy < m.y1;
    };
    const auto value = [](int32_t i, int32_t j) {
        return i + 4096 * j + 7;
    };
    for (uint8_t own = 1; own <= 4; own++)
        for (uint8_t other = 1; other <= 4; other++)
            for (const auto &frame : {std::pair<uint32_t, uint32_t>{96, 64}, {128, 80}, {64, 40}})
                for (bool draws : {false, true}) {
                    Rotation pass = turned(own == 4 ? 1 : own, 96, 64);
                    const Rotation s = turned(other, frame.first, frame.second);
                    const raw::Rect area{0, 0, std::min(pass.width, s.width), std::min(pass.height, s.height)};
                    const raw::TurnMap load = raw::after(raw::planeLoad(s, draws), raw::frameToPlane(pass));
                    const raw::TurnMap back =
                        raw::within(raw::after(raw::planeToFrame(pass), raw::frameToPlane(s)), pass.rect(area));
                    const raw::TurnMap loadMap = raw::planeLoad(s, draws), storeMap = raw::frameToPlane(s);
                    const auto sources = [&](const raw::TurnMap &m) {
                        raw::TurnMap b = m;
                        b.x1 = std::min(b.x1, int32_t(s.planeWidth()));
                        b.y1 = std::min(b.y1, int32_t(s.planeHeight()));
                        return b;
                    };
                    for (uint32_t y = 0; y < area.y1; y += 3)
                        for (uint32_t x = 0; x < area.x1; x += 5) {
                            int32_t u, v, i, j, li, lj;
                            ASSERT_TRUE(at(raw::planeToFrame(pass), int32_t(x), int32_t(y), u, v));
                            ASSERT_TRUE(at(sources(load), u, v, i, j));
                            ASSERT_TRUE(at(loadMap, int32_t(x), int32_t(y), li, lj));
                            EXPECT_EQ(value(i, j), value(li, lj)) << int(own) << " " << int(other) << " " << draws;
                        }
                    for (uint32_t j = 0; j < s.planeHeight(); j += 3)
                        for (uint32_t i = 0; i < s.planeWidth(); i += 5) {
                            int32_t x, y, u, v;
                            ASSERT_TRUE(at(storeMap, int32_t(i), int32_t(j), x, y));
                            const bool inside = uint32_t(x) < area.x1 && uint32_t(y) < area.y1;
                            EXPECT_EQ(at(back, int32_t(i), int32_t(j), u, v), inside) << int(own) << " " << int(other);
                            if (!inside)
                                continue;
                            int32_t fx, fy;
                            ASSERT_TRUE(at(raw::frameToPlane(pass), u, v, fx, fy));
                            EXPECT_EQ(fx, x);
                            EXPECT_EQ(fy, y);
                        }
                }
}

// The words a turned pass's draw state is: the swizzle, the viewport and the scissor through the kit, read back as methods
TEST(RawRotation, MethodStreamOfATurnedPass)
{
    const Rotation r = turned(1, 1280, 720);
    std::vector<uint32_t> words(64);
    nv_push p;
    nv_push_init(&p, words.data(), words.size(), SUBC_MASK_ALL);
    switch_gm20b_viewport_swizzle(&p, 0, r.swizzle());
    switch_gm20b_viewport_state v = frameViewport(0, 0, 1280, 720);
    raw::turnViewport(r, v);
    switch_gm20b_viewport(&p, 0, &v);
    const Rect s = r.rect({100, 50, 300, 90});
    switch_gm20b_scissor(&p, 0, uint16_t(s.x0), uint16_t(s.x1), uint16_t(s.y0), uint16_t(s.y1));
    switch_gm20b_viewport_swizzle(&p, 0, raw::kSwizzleIdentity);
    switch_gm20b_method m[64];
    const long n = switch_gm20b_push_decode(words.data(), size_t(p.end - words.data()), m, 64);
    ASSERT_GT(n, 0);
    std::vector<std::pair<uint32_t, uint32_t>> seen;
    for (long i = 0; i < n; i++)
        seen.push_back({m[i].method, m[i].value});
    const auto value = [&](uint32_t method, size_t nth = 0) {
        for (const auto &[k, val] : seen)
            if (k == method && !nth--)
                return val;
        ADD_FAILURE() << "no method 0x" << std::hex << method;
        return 0u;
    };
    const auto bits = [](float f) {
        uint32_t u;
        memcpy(&u, &f, 4);
        return u;
    };
    EXPECT_EQ(seen.front(), (std::pair<uint32_t, uint32_t>{0x0a18, 0x6402}));
    EXPECT_EQ(seen.back(), (std::pair<uint32_t, uint32_t>{0x0a18, 0x6420}));
    EXPECT_EQ(value(0x0a00), bits(360.0f));     // SET_VIEWPORT_SCALE_X: the frame's y half-extent, sign turned
    EXPECT_EQ(value(0x0a04), bits(640.0f));     // SET_VIEWPORT_SCALE_Y: the frame's x half-extent
    EXPECT_EQ(value(0x0a0c), bits(360.0f));     // SET_VIEWPORT_OFFSET_X: 720 - 360
    EXPECT_EQ(value(0x0a10), bits(640.0f));     // SET_VIEWPORT_OFFSET_Y
    EXPECT_EQ(value(0x0c00), 720u << 16 | 0);   // SET_VIEWPORT_CLIP_HORIZONTAL: the plane's width
    EXPECT_EQ(value(0x0c04), 1280u << 16 | 0);  // SET_VIEWPORT_CLIP_VERTICAL
    EXPECT_EQ(value(0x0e04), uint32_t(670) << 16 | 630);  // SET_SCISSOR_HORIZONTAL: 720 - 90 .. 720 - 50
    EXPECT_EQ(value(0x0e08), uint32_t(300) << 16 | 100);  // SET_SCISSOR_VERTICAL
}

}  // namespace
