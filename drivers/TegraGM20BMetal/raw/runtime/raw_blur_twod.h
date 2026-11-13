// SPDX-License-Identifier: MIT
// Rule blur.twod: a pass of Core Animation's blur chain drawn as 2D-engine scaled blits. Each chain function samples
// texture0 through sampler0 at the destination texel's centre uv (after texmat0, uv * scale + offset) plus taps from the
// fragment uniform at buffer 1 (eight float2 offsets in texture coordinates, then eight half weights). How each function
// reads them, measured on an M4 Max running QuartzCore's own default.metallib with one weight set at a time:
//  - downsample_4: w[k] s(uv + o[k]), k < 4;
//  - downsample_6: w[k] (s(uv + o[k]) + s(uv - o[k])), k < 3;
//  - downsample_8: (1/8) sum over k < 4 of s(uv + o[k]) + s(uv - o[k]); the weights are unused;
//  - downsample_blur_2 and _4: w[0] s(uv) + w[k] (s(uv + o[k-1]) + s(uv - o[k-1])), k from 1 to 3 (blur_2) or 6 (blur_4).
// Summed through a bilinear sampler, those taps weigh each source texel; where the weights are a box of 2^n x 2^n texels
// aligned to the destination texel and the size ratio is 2^n on both axes, n 2:1 bilinear blits with the 2D engine's
// corner origin at half a step (fx::kGm20bSampling, each step a 2x2 box: 2D-1, chip) give the pass's texels. The proven
// margin is one code a step on 8-bit unorm and within one half ulp on RGBA16Float (gpu.raw.mip_twod), so 8-bit passes
// qualify at one step and RGBA16Float ones at up to three. False until gpu.x1.twod_scale's 2to1_linear cells of those
// formats (the engine's 2:1 bilinear step against the 3D route's linear sample) read exact or near on the chip; the glass
// probe's `taps` lines (raw_glass_probe.h) say which passes of the chain qualify.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace raw {

constexpr bool blurTwodRoute()
{
    return false;
}

enum class BlurFunction : uint8_t {
    None,
    Ds4,
    Ds6,
    Ds8,
    Db2,
    Db4
};

// The chain function a Core Animation state name's texture function names (caspec27.py VARIANT_FAMILIES)
inline BlurFunction blurFunctionOf(const char *name)
{
    if (!name)
        return BlurFunction::None;

    const struct {
        const char *tag;
        BlurFunction f;
    } names[] = {{"_Tds4", BlurFunction::Ds4},
                 {"_Tds6", BlurFunction::Ds6},
                 {"_Tds8", BlurFunction::Ds8},
                 {"_Tdb2", BlurFunction::Db2},
                 {"_Tdb4", BlurFunction::Db4}};

    for (const auto &n : names)
        if (strstr(name, n.tag))
            return n.f;
    return BlurFunction::None;
}

struct BlurUniform {
    float offset[8][2] = {};
    float weight[8] = {};
};

// The uniform's 80 bytes: offsets as floats, weights as halves
inline BlurUniform blurUniform(const uint8_t bytes[80])
{
    BlurUniform u;
    memcpy(u.offset, bytes, sizeof u.offset);
    for (int k = 0; k < 8; k++) {
        _Float16 h;
        memcpy(&h, bytes + 64 + 2 * k, sizeof h);
        u.weight[k] = float(h);
    }
    return u;
}

struct BlurSample {
    float dx = 0, dy = 0, weight = 0;  // texture coordinates from uv, its weight
};

// The samples function `f` takes from `u`, at most 16; 0 for a function this rule does not know
inline uint32_t blurSamples(BlurFunction f, const BlurUniform &u, BlurSample out[16])
{
    uint32_t n = 0;
    auto add = [&](float dx, float dy, float w) {
        if (w != 0)
            out[n++] = {dx, dy, w};
    };
    auto pair = [&](int k, float w) {
        add(u.offset[k][0], u.offset[k][1], w);
        add(-u.offset[k][0], -u.offset[k][1], w);
    };
    switch (f) {
    case BlurFunction::Ds4:
        for (int k = 0; k < 4; k++)
            add(u.offset[k][0], u.offset[k][1], u.weight[k]);
        break;
    case BlurFunction::Ds6:
        for (int k = 0; k < 3; k++)
            pair(k, u.weight[k]);
        break;
    case BlurFunction::Ds8:
        for (int k = 0; k < 4; k++)
            pair(k, 0.125f);
        break;
    case BlurFunction::Db2:
    case BlurFunction::Db4:
        add(0, 0, u.weight[0]);
        for (int k = 1; k <= (f == BlurFunction::Db2 ? 3 : 6); k++)
            pair(k - 1, u.weight[k]);
        break;
    case BlurFunction::None:
        break;
    }
    return n;
}

// 2:1 steps whose blits give a pass of `f` from a sw x sh source into a dw x dh target (each sample bilinear), 0 where the
// samples do not weigh a 2^n box aligned to the destination texel
inline uint32_t blurTwodSteps(BlurFunction f, const BlurUniform &u, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
    if (!dw || !dh || sw % dw || sh % dh || sw / dw != sh / dh)
        return 0;
    const uint32_t r = sw / dw;
    uint32_t steps = 0;
    while ((1u << steps) < r)
        steps++;
    if (steps < 1 || steps > 3 || (1u << steps) != r)
        return 0;
    BlurSample s[16];
    const uint32_t n = blurSamples(f, u, s);
    if (!n)
        return 0;
    constexpr int kSpan = 24;  // texels around the footprint the map covers: taps up to 8 texels outside a 8x8 box
    float map[kSpan][kSpan] = {};
    const float centre = float(r) / 2, origin = 8;
    for (uint32_t i = 0; i < n; i++) {
        const float px = centre + s[i].dx * float(sw) - 0.5f, py = centre + s[i].dy * float(sh) - 0.5f;
        const float fx = std::floor(px), fy = std::floor(py);
        const float wx = px - fx, wy = py - fy;
        const int x0 = int(fx + origin), y0 = int(fy + origin);
        if (x0 < 0 || y0 < 0 || x0 + 1 >= kSpan || y0 + 1 >= kSpan)
            return 0;
        map[y0][x0] += s[i].weight * (1 - wx) * (1 - wy);
        map[y0][x0 + 1] += s[i].weight * wx * (1 - wy);
        map[y0 + 1][x0] += s[i].weight * (1 - wx) * wy;
        map[y0 + 1][x0 + 1] += s[i].weight * wx * wy;
    }
    const float box = 1.0f / float(r * r), tolerance = 1.0f / 1024;
    for (int y = 0; y < kSpan; y++)
        for (int x = 0; x < kSpan; x++) {
            const bool inside =
                x >= int(origin) && x < int(origin) + int(r) && y >= int(origin) && y < int(origin) + int(r);
            if (std::fabs(map[y][x] - (inside ? box : 0)) > tolerance)
                return 0;
        }
    return steps;
}

// MTLPixelFormat `format` drawn in `steps` 2D steps stays within the proven margin
constexpr bool blurTwodFormat(uint32_t format, uint32_t steps)
{
    switch (format) {
    case 70:  // RGBA8Unorm
    case 80:  // BGRA8Unorm
        return steps == 1;
    case 115:  // RGBA16Float
        return steps >= 1 && steps <= 3;
    default:
        return false;
    }
}

}  // namespace raw
