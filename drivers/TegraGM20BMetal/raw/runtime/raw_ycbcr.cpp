// SPDX-License-Identifier: MIT
// See raw_ycbcr.h.
#include "raw_ycbcr.h"

#include <algorithm>
#include <cstring>

#include "raw_packed_plane.h"
#include "raw_ycbcr_store.h"

namespace raw {
namespace {

enum Sub : uint8_t {
    Sub420,
    Sub422,
    Sub444
};

bool halvedAcross(Sub s)
{
    return s != Sub444;
}

bool halvedDown(Sub s)
{
    return s == Sub420;
}

enum Reads : uint8_t {
    Converted,
    Srgb,
    Codes
};

struct Format {
    uint32_t mtl;
    uint8_t bits;
    Sub sub;
    bool packed;
    Reads reads;
    bool onePlane = false;
};

constexpr Format kFormats[] = {
    {500, 8, Sub420, false, Converted},  {502, 8, Sub422, false, Converted},  {503, 8, Sub444, false, Converted},
    {505, 10, Sub420, false, Converted}, {506, 10, Sub422, false, Converted}, {507, 10, Sub444, false, Converted},
    {508, 10, Sub420, true, Converted},  {509, 10, Sub422, true, Converted},  {510, 10, Sub444, true, Converted},
    {520, 8, Sub420, false, Srgb},       {522, 8, Sub422, false, Srgb},       {523, 8, Sub444, false, Srgb},
    {525, 10, Sub420, false, Srgb},      {526, 10, Sub422, false, Srgb},      {527, 10, Sub444, false, Srgb},
    {528, 10, Sub420, true, Srgb},       {529, 10, Sub422, true, Srgb},       {530, 10, Sub444, true, Srgb},
    {540, 8, Sub420, false, Codes},      {541, 8, Sub422, false, Codes},      {542, 8, Sub444, false, Codes},
    {543, 10, Sub420, false, Codes},     {544, 10, Sub422, false, Codes},     {545, 10, Sub444, false, Codes},
    {546, 10, Sub420, true, Codes},      {547, 10, Sub422, true, Codes},      {548, 10, Sub444, true, Codes},
};

// one plane of Y0 Cb Y1 Cr bytes a texel pair, whatever the surface's order (an M4 Max)
constexpr Format kOnePlaneFormats[] = {{501, 8, Sub422, false, Converted, true}, {521, 8, Sub422, false, Srgb, true}};

constexpr uint32_t fourcc(const char (&c)[5])
{
    return uint32_t(uint8_t(c[0])) << 24 | uint32_t(uint8_t(c[1])) << 16 | uint32_t(uint8_t(c[2])) << 8 | uint8_t(c[3]);
}

// The surfaces Metal's conversion is measured over (m2v ycbcr.rs SURFACES)
struct Surface {
    uint32_t fourcc;
    uint8_t bits;
    bool packed, full;
    Sub sub;
};

constexpr Surface kSurfaces[] = {
    {fourcc("420v"), 8, false, false, Sub420},  {fourcc("420f"), 8, false, true, Sub420},
    {fourcc("422v"), 8, false, false, Sub422},  {fourcc("422f"), 8, false, true, Sub422},
    {fourcc("444v"), 8, false, false, Sub444},  {fourcc("444f"), 8, false, true, Sub444},
    {fourcc("x420"), 10, false, false, Sub420}, {fourcc("xf20"), 10, false, true, Sub420},
    {fourcc("x422"), 10, false, false, Sub422}, {fourcc("xf22"), 10, false, true, Sub422},
    {fourcc("x444"), 10, false, false, Sub444}, {fourcc("xf44"), 10, false, true, Sub444},
    {fourcc("p420"), 10, true, false, Sub420},  {fourcc("pf20"), 10, true, true, Sub420},
    {fourcc("p422"), 10, true, false, Sub422},  {fourcc("pf22"), 10, true, true, Sub422},
    {fourcc("p444"), 10, true, false, Sub444},  {fourcc("pf44"), 10, true, true, Sub444},
    {fourcc("_RB4"), 8, false, true, Sub444},
};

// the one-plane formats' surfaces of no planes
constexpr Surface kOnePlaneSurfaces[] = {{fourcc("2vuy"), 8, false, false, Sub422},
                                         {fourcc("yuvs"), 8, false, false, Sub422},
                                         {fourcc("yuvf"), 8, false, true, Sub422}};

const Format *findFormat(uint32_t mtl)
{
    for (const Format &f : kFormats)
        if (f.mtl == mtl)
            return &f;
    for (const Format &f : kOnePlaneFormats)
        if (f.mtl == mtl)
            return &f;
    return nullptr;
}

// m2v ycbcr_convert.rs `coefficients`: red's, green's and blue's (kb, kr) by range (video, full) and matrix (BT.601,
// BT.709, BT.2020)
constexpr int32_t kCoefficients[2][3][6] = {
    {{-9, 13123, -3218, -6686, 16591, 9}, {-1, 14743, -1753, -4383, 17373, 1}, {0, 13806, -1540, -5349, 17614, -1}},
    {{-8, 11483, -2816, -5850, 14518, 8}, {-1, 12900, -1534, -3835, 15201, 1}, {0, 12080, -1348, -4680, 15412, -1}}};

constexpr int kLumaFirst = 3;

// none and BT.601 0, BT.709 and SMPTE 240M (Metal reads it as BT.709) 1, BT.2020 2, P3_D65 and DCI_P3 kLumaFirst, anything
// else -1. Under the P3 matrices an M4 Max converts nothing (raw/test/ycbcroracle.mm): a converted format reads and loads its
// codes as red Y, green Cb, blue Cr, each code / max, and samples and stores as the codes format of its surface does; an
// sRGB format's reads differ from process to process there, and it is read as the converted one.
int matrixIndex(const char *m)
{
    if (!m || !strcmp(m, "ITU_R_601_4"))
        return 0;
    if (!strcmp(m, "ITU_R_709_2") || !strcmp(m, "SMPTE_240M_1995"))
        return 1;
    if (!strcmp(m, "ITU_R_2020"))
        return 2;
    if (!strcmp(m, "P3_D65") || !strcmp(m, "DCI_P3"))
        return kLumaFirst;
    return -1;
}

YcbcrOutput outputOf(const Format &f, bool lumaFirst)
{
    switch (f.reads) {
    case Converted:
        return f.packed ? YcbcrConvertedPacked : YcbcrConverted;
    case Srgb:
        return f.packed ? YcbcrSrgbPacked : YcbcrSrgb;
    case Codes:
        break;
    }
    if (lumaFirst)
        return f.packed ? YcbcrLumaFirst10Packed : f.bits == 8 ? YcbcrLumaFirst8 : YcbcrLumaFirst10;
    return f.packed ? YcbcrCodes10Packed : f.bits == 8 ? YcbcrCodes8 : YcbcrCodes10;
}

uint32_t imageFormat(YcbcrOutput o)
{
    switch (o) {
    case YcbcrConverted:
    case YcbcrConvertedPacked:
        return 115;  // RGBA16Float
    case YcbcrSrgb:
    case YcbcrSrgbPacked:
        return 125;  // RGBA32Float
    case YcbcrCodes8:
    case YcbcrLumaFirst8:
        return 70;  // RGBA8Unorm
    case YcbcrCodes10:
    case YcbcrCodes10Packed:
    case YcbcrPackedPlane:
    case YcbcrLumaFirst10:
    case YcbcrLumaFirst10Packed:
        break;
    }
    return 90;  // RGB10A2Unorm
}

// The bytes of a plane row's `codes`: a byte or a 16-bit word each, or packed three to a 32-bit word
uint64_t rowBytes(const Format &f, uint32_t codes)
{
    return f.packed ? uint64_t(codes + 2) / 3 * 4 : uint64_t(codes) * (f.bits == 8 ? 1 : 2);
}

// m2v texture/ycbcr_image.rs `ycbcr_modes`
uint8_t addressMapOf(const Format &f)
{
    const bool wrapped = f.sub != Sub444, clamps = f.reads == Codes;
    return uint8_t((wrapped ? 1 : 0) | (clamps ? 2 : 0));
}

}  // namespace

bool isYcbcrFormat(uint32_t pixelFormat)
{
    return findFormat(pixelFormat) != nullptr;
}

bool ycbcrMetalRefuses(uint32_t pixelFormat, uint32_t width, uint32_t height, uint64_t usage)
{
    constexpr uint64_t kWrites = 2 | 4;  // ShaderWrite, RenderTarget
    const Format *f = findFormat(pixelFormat);
    return f && f->sub != Sub444 && ((width | height) & 1) && (!usage || (usage & kWrites));
}

const char *planYcbcr(const YcbcrFacts &f, YcbcrPlan &out)
{
    if (packedPlaneChannels(f.pixelFormat))
        return planPackedPlane(f, out);
    const Format *format = findFormat(f.pixelFormat);
    if (!format)
        return "not a two-plane YCbCr format";
    if (format->onePlane ? f.planes > 1 || f.plane : f.planes != 2)
        return format->onePlane ? "a one-plane format over a plane of a planar surface" : "not two planes";
    const Surface *surface = nullptr;
    for (const Surface &s : kSurfaces)
        if (s.fourcc == f.fourcc && s.bits == format->bits && s.packed == format->packed && !format->onePlane)
            surface = &s;
    for (const Surface &s : kOnePlaneSurfaces)
        if (s.fourcc == f.fourcc && format->onePlane)
            surface = &s;
    if (!surface)
        return "pixel format";
    if (f.plane > 1)
        return "a plane past 1";
    // Metal reads a plane's rows on past its end where the texture has more (the format's chroma finer than the surface's,
    // plane 1 of a 4:2:0 surface read as luma, a texture taller than its surface): the bytes there, up to the end of the
    // allocation's last page, and 0 past it (an M4 Max)
    const bool planeOne = f.plane == 1;
    const bool fits = planeOne ? !halvedDown(surface->sub)
                               : (halvedAcross(format->sub) || !halvedAcross(surface->sub)) &&
                                     (halvedDown(format->sub) || !halvedDown(surface->sub));
    const bool pastPlanes = !fits || f.height > f.surfaceHeight;
    int matrix = matrixIndex(f.matrix);
    if (matrix < 0)
        return "matrix";
    Format read = *format;
    const bool lumaFirst = matrix == kLumaFirst && format->reads != Codes;
    // stored under a P3 matrix as the codes format of its surface stores (an M4 Max, raw/test/ycbcrstoreoracle.mm), measured
    // for the unpacked 4:2:0 and 4:2:2 converted formats; an sRGB one's stores are no rule's (128 in every plane over 444v)
    const bool lumaFirstStored = format->reads == Converted && !format->packed && format->sub != Sub444;
    if (f.target && lumaFirst && !lumaFirstStored)
        return "a render target under a P3 matrix of an sRGB, packed or 4:4:4 format, whose stores no rule gives";
    if (matrix == kLumaFirst) {
        read.reads = Codes;
        matrix = 0;
    }
    format = &read;
    if (!f.simple)
        return "shape";
    if (!f.width || !f.height)
        return "an empty texture";
    if (f.target) {
        if (format->onePlane)
            return "a one-plane YCbCr render target, whose stores no rule gives";
        if (const char *why = ycbcrTargetRefusal(f.pixelFormat))
            return why;
        if (planeOne || pastPlanes || surface->sub != format->sub || (halvedAcross(format->sub) && (f.width & 1)) ||
            (halvedDown(format->sub) && (f.height & 1)))
            return "a render target over plane 1, taller than its surface, of other chroma than its surface's, or of "
                   "a size its blocks do not tile";
        // an M4 Max's store into one changed words past the texture, and some of its passes faulted
        if (format->packed && (f.width != f.surfaceWidth || f.height != f.surfaceHeight))
            return "a packed render target smaller than its surface";
    }
    const uint32_t bytes = format->bits == 8 ? 1 : 2;
    const uint32_t cw = halvedAcross(format->sub) ? (f.width + 1) / 2 : f.width;
    const uint32_t ch = halvedDown(format->sub) ? (f.height + 1) / 2 : f.height;
    const bool onePlane = format->onePlane;
    const uint64_t luma = f.offset[planeOne ? 1 : 0], chroma = onePlane ? luma : f.offset[1];
    const uint32_t lumaPitch = f.pitch[planeOne ? 1 : 0], chromaPitch = onePlane ? lumaPitch : f.pitch[1];
    if (((luma | chroma) & 15) || ((lumaPitch | chromaPitch) & 15))
        return "plane offset or row not 16-byte";
    // a row of more codes than its plane's pitch holds runs on into the next (Metal reads each row at the pitch)
    const uint64_t lumaRow = onePlane ? 4ull * cw : rowBytes(*format, f.width),
                   chromaRow = onePlane ? lumaRow : rowBytes(*format, 2 * cw);
    const bool overlapping = lumaPitch < lumaRow || (!planeOne && chromaPitch < chromaRow);
    if (f.target && overlapping)
        return "a render target of more codes a row than its plane's pitch holds";
    const uint64_t lumaEnd = luma + uint64_t(lumaPitch) * (f.height - 1) + lumaRow;
    const uint64_t chromaEnd = chroma + uint64_t(chromaPitch) * (ch - 1) + chromaRow;
    const uint64_t readable = std::max(f.allocBytes, f.mappedBytes);
    if (lumaEnd > readable || (!planeOne && chromaEnd > readable))
        return "rows past the allocation's last page (an M4 Max reads 0 there)";

    out = {};
    out.readable = readable;
    out.onePlane = onePlane;
    out.nativePlanes = (f.fourcc == fourcc("420v") || f.fourcc == fourcc("420f")) && !pastPlanes && !overlapping;
    out.target = f.target;
    out.output = outputOf(*format, lumaFirst);
    out.addressMap = addressMapOf(*format);
    out.image = imageFormat(out.output);
    out.lumaFormat = format->packed ? 53 : bytes == 1 ? 10 : 20;    // R32Uint, R8Unorm, R16Unorm
    out.chromaFormat = format->packed ? 53 : bytes == 1 ? 30 : 60;  // R32Uint, RG8Unorm, RG16Unorm
    out.luma = luma;
    out.lumaPitch = lumaPitch;
    out.lumaWidth = format->packed ? uint32_t(lumaRow / 4) : onePlane ? 2 * cw : f.width;
    if (!planeOne) {
        out.chroma = chroma;
        out.chromaPitch = chromaPitch;
        out.chromaWidth = format->packed ? uint32_t(chromaRow / 4) : cw;
        out.chromaHeight = ch;
    }
    uint32_t *s = out.shape;
    s[0] = f.width;
    s[1] = f.height;
    s[2] = surface->full;
    for (int i = 0; i < 6; i++)
        s[3 + i] = uint32_t(kCoefficients[surface->full][matrix][i]);
    s[9] = bytes == 1 ? 255 : 65535;
    s[10] = 8 * bytes - format->bits;  // a 10-bit code is a 16-bit word's high bits (packed: unread)
    s[11] = 10 - format->bits;
    s[12] = (1u << format->bits) - 1;
    s[13] = (halvedAcross(format->sub) ? 1u : 0u) | (halvedDown(format->sub) ? 2u : 0u);
    s[14] = !planeOne;
    if (f.target) {
        const bool across = halvedAcross(format->sub), down = halvedDown(format->sub);
        out.store = format->reads == Codes ? ycbcrCodesShape(format->bits, across, down)
                                           : ycbcrConvertedShape({format->bits, format->reads == Srgb, surface->full,
                                                                  YcbcrMatrix(matrix), across, down});
        if (format->packed)
            out.store = ycbcrPackedShape(out.store);
    }
    return nullptr;
}

}  // namespace raw
