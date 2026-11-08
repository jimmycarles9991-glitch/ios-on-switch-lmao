// SPDX-License-Identifier: MIT
// See twod_sim.h. Written apart from fx_mip.cpp on purpose: surface addressing here comes only from the 2D engine's registers
// (width, block height, base address), never from fx's MipChain.
#include "twod_sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace twodsim {

static uint32_t formatBpp(uint32_t f)
{
    switch (f) {
    case 0xcf:
    case 0xd0:
    case 0xd5:
    case 0xd6:
        return 4;
    case 0xca:
        return 8;
    case 0xea:
        return 2;
    case 0xf3:
        return 1;
    default:
        return 0;
    }
}

uint8_t *Sim::at(uint64_t va, uint64_t bytes)
{
    for (Region &r : regions_)
        if (va >= r.va && va + bytes <= r.va + r.bytes->size())
            return r.bytes->data() + (va - r.va);
    return host ? host(va, bytes) : nullptr;
}

bool Sim::run(const uint32_t *w, size_t n)
{
    for (size_t i = 0; i < n && !failed_;) {
        const uint32_t h = w[i++];
        const uint32_t op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7, mthd = (h & 0xfff) << 2;
        switch (op) {
        case 1:  // INC
            for (uint32_t k = 0; k < count && i < n; k++)
                method(subc, mthd + 4 * k, w[i++]);
            break;
        case 3:  // NON_INC
            for (uint32_t k = 0; k < count && i < n; k++)
                method(subc, mthd, w[i++]);
            break;
        case 4:  // IMMD: count is the data
            method(subc, mthd, count);
            break;
        case 5:  // ONE_INC
            for (uint32_t k = 0; k < count && i < n; k++)
                method(subc, mthd + (k ? 4 : 0), w[i++]);
            break;
        default:
            err_ = "unknown header op " + std::to_string(op);
            return false;
        }
    }
    return !failed_;
}

void Sim::method(uint32_t subc, uint32_t mthd, uint32_t data)
{
    if (subc == 0) {  // MAXWELL_B: only the two the fx stream uses
        if (mthd == 0x0110) {
            waits3d++;
            for (Range &r : pending_)
                r.waited = true;
            if (launches)
                sawWait3dAfter2d_ = true;
        } else if (mthd == 0x1288) {
            texInvalidates3d++;
            if (sawWait3dAfter2d_)
                sawInv3dAfter2d_ = true;
        } else {
            char b[64];
            snprintf(b, sizeof b, "unexpected 3D method 0x%04x", mthd);
            err_ = b;
            failed_ = true;
        }
        endsSampleable = sawWait3dAfter2d_ && sawInv3dAfter2d_;
        return;
    }
    if (subc != 3) {
        err_ = "unexpected subchannel " + std::to_string(subc);
        failed_ = true;
        return;
    }
    if (mthd >= 0x4000) {
        err_ = "2D method out of range";
        failed_ = true;
        return;
    }
    r2d_[mthd >> 2] = data;
    if (mthd == 0x0110) {  // 2D WAIT_FOR_IDLE
        waits2d++;
        for (Range &r : pending_)
            r.waited = true;
    } else if (mthd == 0x0240) {  // TWOD_INVALIDATE_TEXTURE_DATA_CACHE: drops what a wait made visible
        texInvalidates2d++;
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [](const Range &r) {
                                          return r.waited;
                                      }),
                       pending_.end());
    } else if (mthd == 0x08dc) {  // the launch
        if (!launch())
            failed_ = true;
        sawWait3dAfter2d_ = sawInv3dAfter2d_ = false;
        endsSampleable = false;
    }
}

Sim::Surface Sim::surface(uint32_t base) const
{
    const uint32_t *r = r2d_ + (base >> 2);
    Surface s;
    s.format = r[0];
    s.bpp = formatBpp(s.format);
    s.layout = r[1] & 1;
    s.yLog2 = (r[2] >> 4) & 7;
    s.pitch = r[5];
    s.width = r[6];
    s.height = r[7];
    s.va = uint64_t(r[8] & 0xff) << 32 | r[9];
    return s;
}

uint64_t Sim::texel(const Surface &s, uint32_t x, uint32_t y) const
{
    const uint32_t xb = x * s.bpp;
    if (s.layout == 1)
        return s.va + uint64_t(y) * s.pitch + xb;
    // block linear: blocks of one GOB (64 B) wide and 2^yLog2 GOBs (8 rows each) high, row-major; TegraColor inside a GOB
    const uint32_t gobsPerRow = (s.width * s.bpp + 63) / 64, blockRows = 8u << s.yLog2;
    const uint64_t block = uint64_t(y / blockRows) * gobsPerRow + xb / 64;
    const uint32_t inGob =
        ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
    return s.va + block * (512ull << s.yLog2) + uint64_t((y % blockRows) / 8) * 512 + inGob;
}

bool Sim::launch()
{
    launches++;
    const Surface d = surface(0x0200), s = surface(0x0230);
    if (!d.bpp || d.format != s.format) {
        err_ = "unsupported or mismatched 2D formats";
        return false;
    }
    const uint32_t *r = r2d_;
    const uint32_t mode = r[0x088c >> 2], dx0 = r[0x08b0 >> 2], dy0 = r[0x08b4 >> 2], dw = r[0x08b8 >> 2],
                   dh = r[0x08bc >> 2];
    // the source origin is signed 32.32 (a negative origin samples left of texel 0, clamped to the edge)
    const double du = double(uint64_t(r[0x08c4 >> 2]) << 32 | r[0x08c0 >> 2]) / 4294967296.0,
                 dv = double(uint64_t(r[0x08cc >> 2]) << 32 | r[0x08c8 >> 2]) / 4294967296.0,
                 sx0 = double(int64_t(uint64_t(r[0x08d4 >> 2]) << 32 | r[0x08d0 >> 2])) / 4294967296.0,
                 sy0 = double(int64_t(uint64_t(r[0x08dc >> 2]) << 32 | r[0x08d8 >> 2])) / 4294967296.0;
    // origin centre: destination texel x samples at SRC_X0 + (x + 1/2) du; corner: at SRC_X0 + x du
    const bool corner = mode & 1;
    const bool bilinear = (mode >> 4) & 1;
    if (dx0 + dw > d.width || dy0 + dh > d.height) {
        err_ = "destination rectangle outside the surface";
        return false;
    }
    // hazards: every source byte this launch may read must not be in a range written and not yet waited for + invalidated
    const uint64_t srcLo = texel(s, 0, 0),
                   srcHi = s.layout ? srcLo + uint64_t(s.pitch) * s.height
                                    : s.va + ((uint64_t(s.width) * s.bpp + 63) / 64) * 64 *
                                                 ((s.height + (8u << s.yLog2) - 1) / (8u << s.yLog2)) * (8u << s.yLog2);
    for (const Range &w : pending_)
        if (w.begin < srcHi && srcLo < w.end) {
            err_ =
                "hazard: a launch reads what an earlier launch wrote with no wait + texture cache invalidate between";
            return false;
        }
    auto clampi = [](double v, uint32_t n) {
        return uint32_t(std::clamp(int64_t(std::floor(v)), int64_t(0), int64_t(n) - 1));
    };
    auto position = [&](double origin, uint32_t i, double step) {
        if (corner)
            return origin + i * step;
        return quirkCentreHalfTexel ? origin + i * step + 0.5 : origin + (i + 0.5) * step;
    };
    for (uint32_t y = 0; y < dh; y++) {
        const double v = position(sy0, y, dv);
        for (uint32_t x = 0; x < dw; x++) {
            const double u = position(sx0, x, du);
            uint8_t *out = at(texel(d, dx0 + x, dy0 + y), d.bpp);
            if (!out) {
                err_ = "destination unmapped";
                return false;
            }
            if (!bilinear) {
                const uint8_t *in = at(texel(s, clampi(u, s.width), clampi(v, s.height)), s.bpp);
                if (!in) {
                    err_ = "source unmapped";
                    return false;
                }
                memcpy(out, in, d.bpp);
                continue;
            }
            const double uu = u - 0.5, vv = v - 0.5, fu = std::floor(uu), fv = std::floor(vv);
            uint32_t wx = uint32_t(std::lround((uu - fu) * 256.0)), wy = uint32_t(std::lround((vv - fv) * 256.0));
            uint32_t x0 = clampi(fu, s.width), x1 = clampi(fu + 1, s.width), y0 = clampi(fv, s.height),
                     y1 = clampi(fv + 1, s.height);
            if (wx == 256) {
                x0 = x1;
                wx = 0;
            }
            if (wy == 256) {
                y0 = y1;
                wy = 0;
            }
            const uint8_t *a = at(texel(s, x0, y0), s.bpp), *b = at(texel(s, x1, y0), s.bpp),
                          *c = at(texel(s, x0, y1), s.bpp), *e = at(texel(s, x1, y1), s.bpp);
            if (!a || !b || !c || !e) {
                err_ = "source unmapped";
                return false;
            }
            for (uint32_t k = 0; k < d.bpp; k++) {
                const uint32_t top = a[k] * (256 - wx) + b[k] * wx, bot = c[k] * (256 - wx) + e[k] * wx;
                out[k] = uint8_t((top * (256 - wy) + bot * wy + 32768) >> 16);
            }
        }
    }
    // what this launch wrote (the whole destination surface's bytes: conservative)
    const uint64_t lo = d.layout ? d.va : d.va,
                   hi = d.layout ? d.va + uint64_t(d.pitch) * d.height
                                 : d.va + ((uint64_t(d.width) * d.bpp + 63) / 64) * 64 *
                                              ((d.height + (8u << d.yLog2) - 1) / (8u << d.yLog2)) * (8u << d.yLog2);
    pending_.push_back({lo, hi, false});
    return true;
}

}  // namespace twodsim
