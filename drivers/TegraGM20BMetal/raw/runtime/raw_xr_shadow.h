// SPDX-License-Identifier: MIT
// The half shadow of an extended-range colour attachment (metal2vulkan bind/extended_shadow.rs; m2v's runtime holds it in
// render/extended_shadow.rs). Metal's tile holds a BGR10_XR, BGRA10_XR or RGB10A8_2P_XR10 colour as half through a pass and
// rounds it to the codes once, at its store, where the raw path stores the codes at each draw. So a pass in which a draw reads
// such a colour (a blend into it, a fetch of it) that an earlier draw of the pass wrote, or whose clear its codes cannot hold,
// holds an RGBA16Float shadow of the colour, set before the pass: to the clear's halves, else by the pack's tile state of the
// codes. Every draw of the pass that reads or writes the colour then takes its state's " | xrsh=<colours>" variant, whose
// program reads the shadow as the tile and writes it beside the codes inside the pixel interlock. The pass is decided when its
// encoder ends, from all its draws. A pass of several samples or layers, a parallel encoder's, one that executes indirect
// commands, an RGB10A8_2P_XR10 drawn in its two planes and a colour on compressed memory hold none (their draws read the codes,
// as before); so does a pass any of whose draws has no shadowed variant.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "raw_xr.h"

namespace raw {

// The command buffer's backing tag of colour c's shadow: kXrShadowTag + c
constexpr uint32_t kXrShadowTag = 88;
constexpr char kXrShadowVariant[] = " | xrsh=";

inline std::string xrShadowVariant(uint8_t colours)
{
    return colours ? kXrShadowVariant + std::to_string(colours) : std::string();
}

// The colours a key's " | xrsh=" variant holds shadows of, 0 for none
inline uint8_t xrShadowColours(const std::string &key)
{
    const size_t at = key.find(kXrShadowVariant);
    return at == std::string::npos ? 0 : uint8_t(strtoul(key.c_str() + at + sizeof kXrShadowVariant - 1, nullptr, 10));
}

// The form whose tile a shadow holds; None for any other attachment
inline XrForm xrShadowForm(uint32_t mtlPixelFormat, bool twoPlanes)
{
    const XrForm f = xrForm(mtlPixelFormat);
    return f == XrForm::Planes && twoPlanes ? XrForm::None : f;
}

// The pack's state that draws a shadow of the codes (pack/xr.metal): each colour code past 1022 read as 1022, as the tile holds
// none above it
inline const char *xrTileState(XrForm f)
{
    switch (f) {
    case XrForm::Bgr10:
        return "raw.xr.xr_tile_bgr10_frag";
    case XrForm::Bgra10:
        return "raw.xr.xr_tile_bgra10_frag";
    case XrForm::Planes:
        return "raw.xr.xr_tile_planes_frag";
    case XrForm::None:
        break;
    }
    return nullptr;
}

// The halves a Clear puts in the tile (m2v ExtendedRange::clear_tile_value): each channel through float to half, nearest even;
// BGR10_XR's alpha 1
inline void xrClearTile(XrForm f, const double color[4], float out[4])
{
    for (int c = 0; c < 4; c++)
        out[c] = f == XrForm::Bgr10 && c == 3 ? 1.0f : xr_detail::toHalf(double(float(color[c])));
}

// Metal's read of a channel's code (m2v ExtendedRange::code_value): (code - 384) / 510, the two-plane alpha byte over 255
inline float xrCodeValue(XrForm f, uint32_t code, bool alpha)
{
    return f == XrForm::Planes && alpha ? float(double(code) / 255.0) : float((double(code) - 384.0) / 510.0);
}

// Whether the codes of a Clear of `color` hold its tile (m2v ExtendedRange::clear_read_back): each channel's half is the half
// of its own code's value
inline bool xrClearHeldByCodes(XrForm f, const double color[4])
{
    using xr_detail::toHalf;
    for (int c = 0; c < (f == XrForm::Bgr10 ? 3 : 4); c++) {
        const float held = toHalf(double(float(color[c])));
        const float back = toHalf(double(xrCodeValue(f, xr_detail::writeCode(f, held, c == 3), c == 3)));
        if (!(held == back || (std::isnan(held) && std::isnan(back))))
            return false;
    }
    return true;
}

// Metal's tile value of a channel's code (metal2vulkan lower/arith/extended_range.rs tile_value; pack/xr.metal xr_tile_*):
// a colour code past 1022 as 1022, BGRA10_XR's alpha code clamped to [384, 894], each read as the nearest half
inline float xrTileValue(XrForm f, uint32_t code, bool alpha)
{
    if (f == XrForm::Bgr10 && alpha)
        return 1.0f;
    if (alpha && f == XrForm::Bgra10)
        code = code < 384 ? 384 : code > 894 ? 894 : code;
    else if (!alpha)
        code = code > 1022 ? 1022 : code;
    return xr_detail::toHalf(double(xrCodeValue(f, code, alpha)));
}

// A pass's draws, in order, against its eligible colours: the colours it holds shadows of
struct XrShadowPass {
    uint8_t eligible = 0;    // colours of a form whose tile a shadow can hold, in a pass that may hold one
    uint8_t clearedOff = 0;  // of those, the ones the pass clears to a colour their codes cannot hold
    uint8_t written = 0;     // colours a draw before wrote
    uint8_t shadowed = 0;
    bool blocked = false;  // the pass executes indirect commands: none

    // A draw that reads `reads` (blends into, fetches) and writes `writes`
    void draw(uint8_t reads, uint8_t writes)
    {
        shadowed |= reads & eligible & (written | clearedOff);
        written |= writes;
    }

    uint8_t held() const
    {
        return blocked ? 0 : shadowed;
    }
};

}  // namespace raw
