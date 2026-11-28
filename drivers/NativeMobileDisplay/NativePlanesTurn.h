// SPDX-License-Identifier: BSD-2-Clause-Patent
// A detached layer with a transform (the swap's transform word, which QuartzCore 24B119's fb_swap_set_layer passes as
// Core Animation set it, and which the IOSurfaceAccelerator takes too). Included in NativePlanes.h before imageOf.
//
// The word names which source axis each output axis reads and which way: flip X, flip Y, transpose. VIC's measured
// table (TegraVICConfig.h vicFlips) and window A's proven pair (DisplayWindowProbe.h V3: 4 = SCAN_COLUMN | V_DIRECTION,
// 7 = SCAN_COLUMN | H_DIRECTION) agree, so on windows A, B and C the transpose is SCAN_COLUMN, a flip of the source's x
// H_DIRECTION and of its y V_DIRECTION (TRM 24.x Table 122). Output pixel (ox, oy) of the turned whole surface reads
// source (a, b) = transpose ? (oy, ox) : (ox, oy), then a mirrored when flip X, b when flip Y.
//
// A window scans the source itself when the layer is packed RGB at 1:1 (each output pixel one source pixel: no DDA
// phase to place under a decrementing scan), a quarter turn needing the GPU's block-linear layout (SCAN_COLUMN fetches
// 16Bx4 blocks, TRM 24.x). Anything else is turned by the plane conversion job (VIC, which takes all 8 words) into an
// untransformed BGRA plane: never refused, as Core Animation has no answer to a refused swap.

struct Turn {
    bool flipX, flipY, transpose;
};

inline Turn turnOf(uint32_t transform)
{
    static constexpr Turn kTurns[8] = {{false, false, false}, {true, false, false}, {false, true, false},
                                       {true, true, false},   {false, true, true},  {true, true, true},
                                       {false, false, true},  {true, false, true}};
    return kTurns[transform & 7];
}

constexpr uint32_t kHDirection = 1u << 0, kVDirection = 1u << 2, kScanColumn = 1u << 4;

// A decrementing line starts at its last valid byte: the TRM's rule for ADDR_H_OFFSET under H_DIRECTION=DECREMENT
// (24.11, "it should point to last valid byte"), and L4T window.c's (x + w) * Bpp - 1, which V3 ran for NV12. For
// 32-bpp formats the offset's two low bits are unused (24.11.4), so this and the pixel's first byte name one pixel.
constexpr bool kHDirectionStartsAtLastByte = true;

inline uint32_t turnOptions(uint32_t transform)
{
    const Turn t = turnOf(transform);
    return (t.transpose ? kScanColumn : 0) | (t.flipX ? kHDirection : 0) | (t.flipY ? kVDirection : 0);
}

// The rectangle of the whole turned surface (width x height before the turn) that the layer's source rectangle becomes.
inline Rect turnedRect(const Rect &r, uint32_t width, uint32_t height, uint32_t transform)
{
    const Turn t = turnOf(transform);
    const int32_t a = t.flipX ? static_cast<int32_t>(width) - r.x - r.w : r.x;
    const int32_t b = t.flipY ? static_cast<int32_t>(height) - r.y - r.h : r.y;
    return t.transpose ? Rect{b, a, r.h, r.w} : Rect{a, b, r.w, r.h};
}

// A quarter turn needs the GPU's blocks at 16 GOBs, the TRM's only performant block height for rotated display (p.1221);
// the GPU gives a shorter surface fewer, and the conversion job turns those.
inline bool windowTurns(const Layer &l, const tgm_layout *block)
{
    const Turn t = turnOf(l.transform);
    const int32_t outW = t.transpose ? l.src.h : l.src.w, outH = t.transpose ? l.src.w : l.src.h;
    return (l.kind == kKindBgra || l.kind == kKindRgba) && l.dst.w == outW && l.dst.h == outH &&
           (!t.transpose || (block != nullptr && block->y_log2 == 4));
}

// The destination clipped to the screen: [x0, x1) x [y0, y1); false when nothing of it is on the screen.
inline bool clipped(const Rect &d, uint32_t width, uint32_t height, int64_t &x0, int64_t &y0, int64_t &x1, int64_t &y1)
{
    x0 = d.x < 0 ? 0 : d.x;
    y0 = d.y < 0 ? 0 : d.y;
    x1 = static_cast<int64_t>(d.x) + d.w > width ? width : static_cast<int64_t>(d.x) + d.w;
    y1 = static_cast<int64_t>(d.y) + d.h > height ? height : static_cast<int64_t>(d.y) + d.h;
    return x1 > x0 && y1 > y0;
}

// What the plane conversion job makes of a layer, turned or not: its part on the screen, turned by VIC. Each side of the
// plane is the smaller of that part's size on the screen and its size in the source. VIC scales a layer shown smaller
// down to its size on the screen (an RGB window stops at 1/4 across, where a YUV layer may come at 1/6), and the window
// scales a layer shown larger up, so VIC writes and the window fetches no more pixels than the source has.
// srcX..srcH: VIC's source rectangle in the untransformed surface, 16.16. width x height: the plane, at least VIC's
// smallest surface side, so a sliver one pixel on the screen is a 2-pixel plane the window shows halved.
struct ScreenConversion {
    Rect screen;
    uint64_t srcX, srcY, srcW, srcH;
    int32_t width, height;
};

constexpr int32_t kConvertedMinimumSide = 2;

// The most source pixels an RGB window's DDA steps over per output pixel: across, and down for every format.
constexpr uint32_t kRgbShrinkAcross = 4, kShrinkDown = 15;

// source: the part's length in the source, 16.16.
inline int32_t convertedSide(int64_t screen, uint64_t source)
{
    const uint64_t pixels = (source + 0xffff) >> 16;
    const int64_t side = pixels < static_cast<uint64_t>(screen) ? static_cast<int64_t>(pixels) : screen;
    return side < kConvertedMinimumSide ? kConvertedMinimumSide : static_cast<int32_t>(side);
}

inline void screenConversionAxis(int32_t s, int32_t sn, int32_t d, int32_t dn, int64_t d0, int64_t d1, uint64_t &start,
                                 uint64_t &length)
{
    const uint64_t first = static_cast<uint64_t>(s) << 16, whole = static_cast<uint64_t>(sn) << 16;
    start = first + static_cast<uint64_t>(d0 - d) * whole / static_cast<uint64_t>(dn);
    length = static_cast<uint64_t>(d1 - d0) * whole / static_cast<uint64_t>(dn);
    if (length < (1u << 16))
        length = 1u << 16;
    if (start + length > first + whole)
        start = first + whole - length;
}

inline bool screenConversionOf(const Layer &l, uint32_t width, uint32_t height, ScreenConversion &c)
{
    int64_t x0, y0, x1, y1;
    if (l.src.w <= 0 || l.src.h <= 0 || l.dst.w <= 0 || l.dst.h <= 0 || !clipped(l.dst, width, height, x0, y0, x1, y1))
        return false;
    const Turn t = turnOf(l.transform);
    const Rect o = turnedRect(l.src, l.surface.width, l.surface.height, l.transform);
    uint64_t a, aw, b, bh;
    screenConversionAxis(o.x, o.w, l.dst.x, l.dst.w, x0, x1, a, aw);
    screenConversionAxis(o.y, o.h, l.dst.y, l.dst.h, y0, y1, b, bh);
    // turnedRect backwards: the transpose, then the flips.
    c.srcX = t.transpose ? b : a;
    c.srcY = t.transpose ? a : b;
    c.srcW = t.transpose ? bh : aw;
    c.srcH = t.transpose ? aw : bh;
    if (t.flipX)
        c.srcX = (static_cast<uint64_t>(l.surface.width) << 16) - c.srcX - c.srcW;
    if (t.flipY)
        c.srcY = (static_cast<uint64_t>(l.surface.height) << 16) - c.srcY - c.srcH;
    c.screen = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<int32_t>(x1 - x0),
                static_cast<int32_t>(y1 - y0)};
    c.width = convertedSide(x1 - x0, aw);
    c.height = convertedSide(y1 - y0, bh);
    return true;
}

// Whether a plane converted as `made` can show what `now` asks for: the same part of the source, at least as many pixels
// as `now` would convert, and at most twice the part's size on the screen each way. A layer wholly on the screen
// converts its whole source rectangle wherever it goes, so moving or resizing still content keeps its plane until it
// grows past it or shrinks below half. Admission charges window B for the plane it fetches, so a plane kept far larger
// than its part on the screen could cost an overlay its place.
constexpr int64_t kConvertedReuseShrink = 2;

inline bool screenConversionShows(const ScreenConversion &made, const ScreenConversion &now)
{
    return made.srcX == now.srcX && made.srcY == now.srcY && made.srcW == now.srcW && made.srcH == now.srcH &&
           made.width >= now.width && made.height >= now.height &&
           made.width <= static_cast<int64_t>(now.screen.w) * kConvertedReuseShrink &&
           made.height <= static_cast<int64_t>(now.screen.h) * kConvertedReuseShrink;
}

// The plane screenConversionOf describes, as the layer the window shows.
inline Layer screenConversionLayer(const ScreenConversion &c, uint32_t pitch)
{
    Layer rgb;
    rgb.kind = kKindBgra;
    rgb.surface.format = fourcc('B', 'G', 'R', 'A');
    rgb.surface.width = static_cast<uint32_t>(c.width);
    rgb.surface.height = static_cast<uint32_t>(c.height);
    rgb.surface.stride[0] = pitch;
    rgb.surface.planeHeight[0] = rgb.surface.height;
    rgb.surface.allocation = rgb.surface.descriptorBytes = static_cast<uint64_t>(pitch) * rgb.surface.height;
    rgb.src = {0, 0, c.width, c.height};
    rgb.dst = c.screen;
    return rgb;
}

// A layer the conversion job turns: where its plane will be shown, and the whole source VIC reads.
inline bool convertedTurnImageOf(const Layer &l, uint32_t width, uint32_t height, Image &image)
{
    image = Image();
    image.yuv = isYuv(l.kind);
    image.convert = true;
    int64_t x0, y0, x1, y1;
    if (!clipped(l.dst, width, height, x0, y0, x1, y1))
        return true;
    image.visible = true;
    image.colorDepth = kDepthB8G8R8A8;
    image.position = static_cast<uint32_t>(y0) << 16 | static_cast<uint32_t>(x0);
    image.size = static_cast<uint32_t>(y1 - y0) << 16 | static_cast<uint32_t>(x1 - x0);
    image.prescaled = (image.size & 0xffff0000u) | ((image.size & 0xffffu) * 4);
    image.dda = 0x10001000u;
    image.options = kWinEnable;
    image.matrix = matrixOf(l.surface);
    image.fullRange = l.kind == kKindNv12Full || l.kind == kKindNv16Full;
    image.readHigh = (l.surface.allocation + 4095) & ~4095ULL;
    return true;
}

// A packed layer window B or C scans turned (windowTurns): START and the offsets on the first source pixel the scan
// reads, whichever way it goes; PRESCALED counts the source the scan crosses (for SCAN_COLUMN: columns as lines, a
// column's pixels in bytes).
inline bool windowTurnImageOf(const Layer &l, uint32_t width, uint32_t height, Image &image, const tgm_layout *block)
{
    image = Image();
    const Turn t = turnOf(l.transform);
    int64_t x0, y0, x1, y1;
    if (!clipped(l.dst, width, height, x0, y0, x1, y1))
        return true;
    image.visible = true;
    const Rect o = turnedRect(l.src, l.surface.width, l.surface.height, l.transform);
    const int64_t nx = x1 - x0, ny = y1 - y0;
    const int64_t ox = o.x + (x0 - l.dst.x), oy = o.y + (y0 - l.dst.y);
    // The output's first pixel and the source columns (a) and rows (b) the scan crosses, before the flips.
    const int64_t aFirst = t.transpose ? oy : ox, aCount = t.transpose ? ny : nx;
    const int64_t bFirst = t.transpose ? ox : oy, bCount = t.transpose ? nx : ny;
    const int64_t w = l.surface.width, h = l.surface.height;
    const int64_t colStart = t.flipX ? w - 1 - aFirst : aFirst, colLow = t.flipX ? w - aFirst - aCount : aFirst;
    const int64_t rowStart = t.flipY ? h - 1 - bFirst : bFirst, rowLow = t.flipY ? h - bFirst - bCount : bFirst;
    if (colLow < 0 || rowLow < 0 || colLow + aCount > w || rowLow + bCount > h)
        return false;
    image.colorDepth = l.kind == kKindRgba ? kDepthR8G8B8A8 : kDepthB8G8R8A8;
    image.position = static_cast<uint32_t>(y0) << 16 | static_cast<uint32_t>(x0);
    image.size = static_cast<uint32_t>(ny) << 16 | static_cast<uint32_t>(nx);
    const int64_t lines = t.transpose ? aCount : bCount, across = t.transpose ? bCount : aCount;
    image.prescaled = static_cast<uint32_t>(lines) << 16 | static_cast<uint32_t>(across * 4);
    image.dda = 0x10001000u;
    image.hOffset = static_cast<uint32_t>(t.flipX && kHDirectionStartsAtLastByte ? colStart * 4 + 3 : colStart * 4);
    image.options = kWinEnable | turnOptions(l.transform) | switch_window_palette::options();
    uint64_t startRow = static_cast<uint64_t>(rowLow);
    uint64_t rowBytes = l.surface.stride[0];
    uint64_t endRow = static_cast<uint64_t>(rowLow + bCount);
    if (block) {
        if (block->bpp != 4 || (block->row_bytes % 64) != 0 || block->width < l.surface.width ||
            block->height < l.surface.height || block->y_log2 > 5)
            return false;
        const uint64_t blockRows = 8ULL << block->y_log2;
        startRow -= startRow % blockRows;
        endRow = (endRow + blockRows - 1) / blockRows * blockRows;
        rowBytes = block->row_bytes;
        image.surfaceKind = static_cast<uint8_t>(kSurfaceKindBlock | static_cast<uint32_t>(block->y_log2)
                                                                         << kSurfaceKindBlockHeightShift);
    } else if (t.transpose) {
        return false;
    }
    image.lineStride = static_cast<uint32_t>(rowBytes);
    image.start[0] = startRow * rowBytes;
    image.vOffset = static_cast<uint16_t>(rowStart - static_cast<int64_t>(startRow));
    image.readLow = image.start[0] & ~4095ULL;
    image.readHigh = (endRow * rowBytes + 4095) & ~4095ULL;
    return image.readHigh <= ((l.surface.descriptorBytes + 4095) & ~4095ULL);
}

// A layer the window would scan but cannot after all (its read range or block layout) is the conversion job's too.
inline bool turnImageOf(const Layer &l, uint32_t width, uint32_t height, Image &image, const tgm_layout *block)
{
    if (windowTurns(l, block) && windowTurnImageOf(l, width, height, image, block))
        return true;
    return convertedTurnImageOf(l, width, height, image);
}
