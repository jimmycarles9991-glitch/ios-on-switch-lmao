// SPDX-License-Identifier: MIT
// See ycbcr.h.
#include "ycbcr.h"

#include <algorithm>
#include <cstring>

#include "packed_plane.h"
#include "ycbcr_store.h"

namespace gm20b {
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

enum Layout : uint8_t {
    TwoPlanes,
    Pairs,          // one plane of Y0 Cb Y1 Cr texel pairs
    PairsReversed,  // one plane of Cb Y0 Cr Y1 texel pairs
    Words           // one plane of a word a texel: Y, Cb, Cr ten bits each from bit 0
};

struct Format {
    uint32_t mtl;
    uint8_t bits;
    Sub sub;
    bool packed;
    Reads reads;
    Layout layout = TwoPlanes;
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

// The one-plane formats: each reads its plane in its own layout whatever the surface's order, YCBCR8_422_1P_REV as
// YCBCR8_422_1P over its bytes swapped in pairs, YCBCR10_444_1P and its sRGB twin as the two-plane 10-bit 4:4:4 formats
// over the same codes in 'x444' (an M4 Max, every matrix: m2v-textures3 probes/onep.mm)
constexpr Format kOnePlaneFormats[] = {{501, 8, Sub422, false, Converted, Pairs},
                                       {521, 8, Sub422, false, Srgb, Pairs},
                                       {562, 8, Sub422, false, Converted, PairsReversed},
                                       {504, 10, Sub444, false, Converted, Words},
                                       {524, 10, Sub444, false, Srgb, Words}};

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

// A one-plane format reads any surface's plane, in full range over CoreVideo's full-range YCbCr formats and in video range
// over every other (an M4 Max: full over 'yuvf' and '420f', video over '2vuy', 'yuvs', 'v410', 'y416', 'BGRA', 'l10r',
// 'w30r' and '420v'; probes/onep.mm)
bool fullRangeSurface(uint32_t f)
{
    if (f == fourcc("yuvf"))
        return true;
    for (const Surface &s : kSurfaces)
        if (s.fourcc == f)
            return s.full;
    return false;
}

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

// none, BT.601 and any other string 0 (an M4 Max reads and stores a fresh surface of an unknown one as of none: m2v-textures4
// probes/reads.mm matrix, stores; a value it cannot parse leaves the surface's last parsed matrix in force), BT.709 and
// SMPTE 240M (Metal reads it as BT.709) 1, BT.2020 2, P3_D65 and DCI_P3 kLumaFirst. Under the P3 matrices an M4 Max converts nothing (harness/ycbcroracle.mm): a converted format reads and loads its
// codes as red Y, green Cb, blue Cr, each code / max, and samples and stores as the codes format of its surface does; an
// sRGB format's reads differ from process to process there, and it is read as the converted one.
int matrixIndex(const char *m)
{
    if (!m)
        return 0;
    if (!strcmp(m, "ITU_R_709_2") || !strcmp(m, "SMPTE_240M_1995"))
        return 1;
    if (!strcmp(m, "ITU_R_2020"))
        return 2;
    if (!strcmp(m, "P3_D65") || !strcmp(m, "DCI_P3"))
        return kLumaFirst;
    return 0;
}

// An sRGB 4:4:4 target's codes under a P3 matrix, luma and chroma alike: 128 at 8 bits, 384 at 10 (an M4 Max over draws,
// clears and any prior bytes): a converted store of no terms, only its offsets
YcbcrStoreShape p3SrgbShape(uint32_t bits)
{
    YcbcrStoreShape s = ycbcrCodesShape(bits, false, false);
    s.converted = 1;
    s.termBits = bits == 10 ? 4 : 6;
    for (int32_t &o : s.offset)
        o = bits == 10 ? 384 : 128;
    return s;
}

// An sRGB 4:2:0 or 4:2:2 target's store under a P3 matrix: its surface's codes format's (luma the green code, Cb the blue,
// Cr the red, a block's code its texels' mean rounded half up), each texel's code k first taken as a linear step of 1/8192
// through the sRGB encode and cut to its low bits, E[k] & max (pack/srgb_encode.h): 8-bit green 0.5 (k 128, E 1078) stores
// 54. Every code an M4 Max stores, both depths, draws and clears, both P3 matrices, either range (harness/p3storeoracle.mm)
YcbcrStoreShape p3SrgbCodesShape(uint32_t bits, bool across, bool down)
{
    YcbcrStoreShape s = ycbcrCodesShape(bits, across, down);
    s.srgb = 1;
    return s;
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

bool ycbcrCpuLuma(uint32_t pixelFormat, uint32_t &bytes)
{
    const Format *f = findFormat(pixelFormat);
    if (!f || f->layout != TwoPlanes)
        return false;
    bytes = f->packed ? 0 : f->bits == 8 ? 1 : 2;
    return true;
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
    const bool onePlane = format->layout != TwoPlanes;
    if (f.plane >= std::max(f.planes, 1u) || f.plane > 1)
        return "a plane the surface does not have";
    // A format reads its planes by its own bits and packing whatever the surface's four-character code, in full range over a
    // full-range CoreVideo format and in video range over every other (an M4 Max over 'x420', 'xf20', '420v', '420f', 'b3a8',
    // 'abcd', 'BGRA', 'y3p8': m2v-textures9 probes/ycbcr.txt 3f-3l, 3u, 3v, 3ac)
    const Surface *known = nullptr;
    for (const Surface &s : kSurfaces)
        if (s.fourcc == f.fourcc)
            known = &s;
    const Surface reads = {f.fourcc, format->bits, format->packed, fullRangeSurface(f.fourcc),
                           known && !onePlane ? known->sub : format->sub};
    const Surface *surface = &reads;
    // Metal reads a plane's rows on past its end where the texture has more (the format's chroma finer than the surface's,
    // plane 1 of a 4:2:0 surface read as luma, a texture taller than its surface): the bytes there, up to the end of the
    // allocation's last page, and 0 past it (an M4 Max)
    const bool planeOne = f.plane == 1 && !onePlane;
    // no plane after the texture's: chroma codes 0, and a target stores its luma alone (probes/ycbcr.txt 3a-3c, 3s, 3z, 3aj)
    const bool lumaAlone = !onePlane && f.plane + 1 >= f.planes;
    const bool fits = planeOne ? !halvedDown(surface->sub)
                               : (halvedAcross(format->sub) || !halvedAcross(surface->sub)) &&
                                     (halvedDown(format->sub) || !halvedDown(surface->sub));
    const bool pastPlanes = !fits || f.height > f.surfaceHeight;
    int matrix = matrixIndex(f.matrix);
    Format read = *format;
    const bool lumaFirst = matrix == kLumaFirst && format->reads != Codes;
    // Stored under a P3 matrix (an M4 Max, harness/ycbcrstoreoracle.mm, harness/p3storeoracle.mm): a converted format as
    // the codes format of its surface, 4:4:4 too; a packed one as its unpacked twin's codes, as every packed target is
    // (an M4 Max faults on Metal's own packed passes); an sRGB 4:4:4 one as one code everywhere, whatever is drawn
    // (p3SrgbShape); an sRGB 4:2:0 or 4:2:2 one as its codes format through the sRGB encode (p3SrgbCodesShape).
    const bool srgbStored = lumaFirst && format->reads == Srgb;
    if (matrix == kLumaFirst) {
        read.reads = Codes;
        matrix = 0;
    }
    format = &read;
    if (!f.simple || f.levels != 1)
        return "shape";
    if (!f.width || !f.height)
        return "an empty texture";
    // A target stores as it reads (an M4 Max, m2v-textures5 probes/targets.mm, edges.mm): over plane 1 its luma codes into
    // that plane and no chroma; taller than its surface or of chroma unlike the surface's, each plane's rows at its pitch on
    // past the plane's end, luma first and chroma over it where they meet; of an odd size (made to be read alone), each
    // chroma block past its last column or row the mean of the texels inside it (pack/ycbcr_store.metal chromaCodes). A
    // one-plane format stores its two-plane twin's codes in its own layout: pairs (the luma and chroma images merged into
    // them, BlitEncoder::mergePlane; an odd width's last pair keeps its second luma byte) or words with bits 30 and 31 clear
    // (an M4 Max over '2vuy', 'yuvs', 'v410': m2v-textures5 probes/targets.mm, m2v-textures6 probes/onep.mm)
    if (f.target) {
        // RGB8_444_2P (542) comes here where the planar path does not take it (rows that overlap, a texture taller than
        // its planes, planes of other elements: TegraGM20BMetalSurfaceTextures.mm planar444) and stores as the codes format it is
        if (const char *why = onePlane || f.pixelFormat == 542 ? nullptr : ycbcrTargetRefusal(f.pixelFormat))
            return why;
        // an M4 Max's store into one changed words past the texture, and some of its passes faulted
        if (format->packed && (f.width != f.surfaceWidth || f.height != f.surfaceHeight))
            return "a packed render target smaller than its surface";
    }
    const uint32_t bytes = format->bits == 8 ? 1 : 2;
    const uint32_t cw = halvedAcross(format->sub) ? (f.width + 1) / 2 : f.width;
    const uint32_t ch = halvedDown(format->sub) ? (f.height + 1) / 2 : f.height;
    const bool words = format->layout == Words;
    // Metal's GPU reads and stores each plane at its offset and rows rounded down to 16 bytes (an M4 Max, two-plane and
    // one-plane formats: probes/ycbcr.txt 3m-3r, 3x, 3y, 3ak-3am)
    const uint64_t luma = f.offset[f.plane] & ~uint64_t(15), chroma = onePlane ? luma : f.offset[1] & ~uint64_t(15);
    // rows under 16 bytes 1 MiB apart: past the allocation's last page they read 0 through the sparse window below, as an
    // M4 Max reads them where nothing else is mapped there
    const uint32_t lumaPitch = ycbcrPlaneStride(f.pitch[f.plane]),
                   chromaPitch = onePlane ? lumaPitch : ycbcrPlaneStride(f.pitch[1]);
    // a row of more codes than its plane's pitch holds runs on into the next (Metal reads each row at the pitch); a target
    // stores each byte from the row whose store lands last (overlap_rows.h)
    const uint64_t lumaRow = onePlane ? 4ull * cw : rowBytes(*format, f.width),
                   chromaRow = onePlane ? lumaRow : rowBytes(*format, 2 * cw);
    const bool overlapping = lumaPitch < lumaRow || (!lumaAlone && chromaPitch < chromaRow);
    const uint64_t lumaEnd = luma + uint64_t(lumaPitch) * (f.height - 1) + lumaRow;
    const uint64_t chromaEnd = chroma + uint64_t(chromaPitch) * (ch - 1) + chromaRow;
    const uint64_t mapped = std::max(f.allocBytes, f.mappedBytes),
                   rowsEnd = std::max(lumaEnd, lumaAlone ? 0 : chromaEnd);
    // An M4 Max reads 0 past the last page (m2v-textures4 probes/more.mm); a store's rows there drop, as a sparse page drops a
    // write (what Metal stores there no read can see)
    const bool window = rowsEnd > mapped;
    if (window && !f.sparseTail)
        return kRowsPastWithoutSparse;

    out = {};
    out.readable = window ? rowsEnd : mapped;
    out.buffer = f.buffer;
    out.window = window;
    out.onePlane = onePlane && !words;
    out.pairsReversed = format->layout == PairsReversed;
    out.chromaGreenBlue = words;
    out.nativePlanes = (f.fourcc == fourcc("420v") || f.fourcc == fourcc("420f")) && !lumaAlone && !pastPlanes &&
                       !overlapping && !window;
    out.target = f.target;
    out.output = outputOf(*format, lumaFirst);
    out.addressMap = addressMapOf(*format);
    out.image = imageFormat(out.output);
    out.lumaFormat = words            ? 90
                     : format->packed ? 53
                     : bytes == 1     ? 10
                                      : 20;  // RGB10A2Unorm, R32Uint, R8Unorm, R16Unorm
    out.chromaFormat = words            ? 90
                       : format->packed ? 53
                       : bytes == 1     ? 30
                                        : 60;  // RGB10A2Unorm, R32Uint, RG8Unorm, RG16Unorm
    out.luma = luma;
    out.lumaPitch = lumaPitch;
    if (!onePlane && planeStoreStride(f.pitch[f.plane]) != lumaPitch)
        out.lumaStorePitch = planeStoreStride(f.pitch[f.plane]);
    out.lumaWidth = format->packed ? uint32_t(lumaRow / 4) : onePlane && !words ? 2 * cw : f.width;
    if (!lumaAlone) {
        out.chroma = chroma;
        out.chromaPitch = chromaPitch;
        if (!onePlane && planeStoreStride(f.pitch[1]) != chromaPitch)
            out.chromaStorePitch = planeStoreStride(f.pitch[1]);
        out.chromaWidth = format->packed ? uint32_t(chromaRow / 4) : cw;
        out.chromaHeight = ch;
        const uint64_t lumaStored = luma + uint64_t(planeStoreStride(f.pitch[f.plane])) * (f.height - 1) + lumaRow;
        const uint64_t chromaStored = chroma + uint64_t(planeStoreStride(f.pitch[1])) * (ch - 1) + chromaRow;
        out.meet = f.target && !onePlane && !format->packed && luma < chromaStored && chroma < lumaStored;
    }
    uint32_t *s = out.shape;
    s[0] = f.width;
    s[1] = f.height;
    s[2] = surface->full;
    for (int i = 0; i < 6; i++)
        s[3 + i] = uint32_t(kCoefficients[surface->full][matrix][i]);
    s[9] = words ? 1023 : bytes == 1 ? 255 : 65535;
    s[10] = words ? 0 : 8 * bytes - format->bits;  // a 10-bit code is a 16-bit word's high bits (packed: unread)
    s[11] = 10 - format->bits;
    s[12] = (1u << format->bits) - 1;
    s[13] = (halvedAcross(format->sub) ? 1u : 0u) | (halvedDown(format->sub) ? 2u : 0u);
    s[14] = !lumaAlone;
    if (f.target) {
        const bool across = halvedAcross(format->sub), down = halvedDown(format->sub);
        out.store = format->reads == Codes ? ycbcrCodesShape(format->bits, across, down)
                                           : ycbcrConvertedShape({format->bits, format->reads == Srgb, surface->full,
                                                                  YcbcrMatrix(matrix), across, down});
        if (srgbStored)
            out.store =
                format->sub == Sub444 ? p3SrgbShape(format->bits) : p3SrgbCodesShape(format->bits, across, down);
        if (format->packed)
            out.store = ycbcrPackedShape(out.store);
        out.store.words = words;
    }
    return nullptr;
}

}  // namespace gm20b
