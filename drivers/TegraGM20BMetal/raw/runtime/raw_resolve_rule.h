// SPDX-License-Identifier: MIT
// Rule resolve.twod: where an MSAA resolve is the 2D engine's 2:1 box of the sample grid (raw_resolve_twod.cpp) instead
// of the pack's resolve pass.
//
// The engine reads each unorm8 code c as round(c * 2^15 / 255) before its box and stores the mean by the ROP's unorm
// rule (x1-units UNITS.md: cut to n+4 fractional bits, nearest, ties down), where Metal's resolve is (sum + n/2) / n,
// ties up; no 902d method sets either (cl902d.h). That reading gives gpu.raw.resolve_twod's per-channel 2x counts on
// boots 19 and 21 exactly (raw_resolve_rule_test.cpp). A half-code mean lands on either side of the tie by the codes'
// rounding, so the plain box is within one code of Metal and so is its complement (resolve.exact: every sample made
// 255 - c in place, the box read under ROP3 NOTSRCCOPY): on 896 of the 65,536 sample pairs both land one under Metal,
// where the pair (k, k) gives the same two codes, so no ROP3 of the two readings is exact either. The complement is
// therefore no route; it stays the probe's (RawMetalProbe.mm switchRawProbeResolveExact:) as a reading of the engine.
#pragma once
#include <cstdint>

namespace raw {

// What can read a resolve texture's texels as data
struct ResolveTarget {
    bool sharedStorage = false;  // its storage, its parent's or its heap's, is CPU-visible
    // over no-copy pages, or over an IOSurface this texture can read back (usage beyond RenderTarget): the CPU or another
    // process maps them. A render-target-only surface texture is a framebufferOnly drawable, which only the compositor reads
    bool imported = false;
    bool overBuffer = false;  // over a buffer's bytes
    bool storage = false;     // ShaderWrite usage: a kernel or pass reads and writes it as data
    // a blit copied out of it or a kernel took it, as recorded: later in this command buffer (the tape is encoded at the
    // commit) or in an earlier one
    bool readAsData = false;
};

constexpr bool resolveTargetReadable(const ResolveTarget &t)
{
    return t.sharedStorage || t.imported || t.overBuffer || t.storage || t.readAsData;
}

struct ResolveAsk {
    uint16_t format = 0;        // MTLPixelFormat of the resolve texture
    uint8_t samples = 0;        // the attachment's
    bool readable = false;      // resolveTargetReadable
    bool wholeTarget = false;   // the pass's render area is the resolve texture's whole level 0, slice 0
    bool shadowTarget = false;  // a pitch target the render hardware does not take (Texture::shadowTarget)
};

// How close the engine's box of a format's grid at 2x or 4x comes to Metal's resolve, as gpu.x1.twod_resolve read it on
// the chip and gpu.raw.resolve_twod's format arms judge it: exact, within one code, or not read by the engine
enum class TwodResolveFit : uint8_t {
    None,
    Near,
    Exact,
};

constexpr TwodResolveFit twodResolveFit(uint16_t format, uint8_t samples)
{
    if (samples != 2 && samples != 4)
        return TwodResolveFit::None;
    switch (format) {
    case 70:  // RGBA8Unorm
    case 80:  // BGRA8Unorm
    case 71:  // RGBA8Unorm_sRGB: the engine decodes before its box and encodes after (fx::twodFormat)
    case 81:  // BGRA8Unorm_sRGB
    case 90:  // RGB10A2Unorm, which the pack has no resolve state for
    case 92:  // RG11B10Float
        return TwodResolveFit::Near;
    case 115:  // RGBA16Float
        return samples == 2 ? TwodResolveFit::Exact : TwodResolveFit::Near;
    default:
        return TwodResolveFit::None;
    }
}

// The 2D engine reads the sample grid at 2:1 over the whole target, not a shadow target
constexpr bool twodResolveShape(const ResolveAsk &a)
{
    return twodResolveFit(a.format, a.samples) != TwodResolveFit::None && a.wholeTarget && !a.shadowTarget;
}

// resolve.twod: taken for every resolve where the box is Metal's rule, and where it is within one code of it only where
// no reader takes the texels as data, as fetch.blend is: a screen-bound target (a drawable, a display-bound surface) or a
// private intermediate only passes sample. A first readback in a later command buffer sees the one code: the owner's
// rule for screen-bound targets
constexpr bool twodResolveAcceptable(const ResolveAsk &a)
{
    return twodResolveShape(a) && (!a.readable || twodResolveFit(a.format, a.samples) == TwodResolveFit::Exact);
}

// The probe's complement inverts unorm8 codes in place; any other format's bits would not invert to its complement
constexpr bool twodResolveComplementShape(const ResolveAsk &a)
{
    return twodResolveShape(a) && (a.format == 70 || a.format == 80);
}

enum ResolveRoute : uint8_t {
    ResolveByTwod,
    ResolveByComplement,
    ResolveByPass
};

// A resolve a reader takes as data takes the pack's pass: on the chip it is Metal's rule byte for byte and faster than
// the complement (gpu.raw.resolve_twod, boot 21: 2x 1.871 ms against 2.416, 4x 3.271 against 5.223)
constexpr ResolveRoute resolveRoute(const ResolveAsk &a)
{
    return twodResolveAcceptable(a) ? ResolveByTwod : ResolveByPass;
}

constexpr uint32_t resolveSampleSlot(uint32_t samples)
{
    return samples == 2 ? 0 : samples == 4 ? 1 : 2;
}

}  // namespace raw
