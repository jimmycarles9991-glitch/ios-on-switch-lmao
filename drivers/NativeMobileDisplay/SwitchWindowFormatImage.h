// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inside namespace native_planes, after pitchReads: the image and the surface checks of a layer in one of
// SwitchWindowCaps.h's formats (kKindExtraYuv), the way imageOf and checkDetached make NV12's and NV16's. Pitch linear
// only. The window reads every row of each plane between the first and last source rows; their whole width is counted
// as read, since the chroma plane's H offset for NV24 is the hardware's own (2 bytes a pixel) rather than ours.

inline const switch_window_caps::Format *extraFormat(const Surface &s)
{
    return switch_window_caps::find(s.format, s.planes);
}

inline bool extraImageOf(const Layer &l, uint32_t width, uint32_t height, Image &image, const tgm_layout *block)
{
    image = Image();
    const switch_window_caps::Format *f = extraFormat(l.surface);
    if (!f || block)
        return false;
    image.yuv = true;
    image.convert = f->pairs && !switch_surface_dc_siting(l.surface.chromaTop, l.surface.chromaBottom);
    Axis x;
    Axis y;
    if (!axisOf(l.src.x, l.src.w, l.dst.x, l.dst.w, width, l.surface.width, 8, f->pairs, x) ||
        !axisOf(l.src.y, l.src.h, l.dst.y, l.dst.h, height, l.surface.height, 15, false, y))
        return false;
    if (!x.visible || !y.visible)
        return true;
    image.visible = true;
    image.colorDepth = f->depth;
    image.byteSwap = f->byteSwap;
    image.lumaBytes = f->lumaBytes;
    image.fetchBytes = f->fetchBytes;
    image.position = y.pos << 16 | x.pos;
    image.size = y.size << 16 | x.size;
    image.prescaled = y.count << 16 | x.count * f->lumaBytes;
    image.dda = y.inc << 16 | x.inc;
    image.hInitial = x.initial;
    image.vInitial = y.initial;
    image.hOffset = x.first * f->lumaBytes;
    const bool scaledX = x.inc != 4096 || (x.initial != 0u);
    const bool scaledY = y.inc != 4096 || (y.initial != 0u);
    image.options = kWinEnable | kCscEnable | (scaledX ? kHFilterOn : 0) | (scaledY ? kVFilterOn : 0);
    uint64_t low = UINT64_MAX;
    uint64_t high = 0;
    for (unsigned p = 0; p < f->planes; ++p) {
        const uint64_t base = f->planes > 1 ? l.surface.offset[p] : 0;
        image.start[p] = base + static_cast<uint64_t>(y.first) * l.surface.stride[p];
        const uint64_t end = image.start[p] + static_cast<uint64_t>(y.count) * l.surface.stride[p];
        low = image.start[p] < low ? image.start[p] : low;
        high = end > high ? end : high;
    }
    image.lineStride = f->planes > 1 ? l.surface.stride[1] << 16 | l.surface.stride[0] : l.surface.stride[0];
    image.readLow = low & ~4095ULL;
    image.readHigh = (high + 4095) & ~4095ULL;
    image.matrix = matrixOf(l.surface);
    image.fullRange = f->fullRange;
    return true;
}

// checkDetached's surface rules for these formats: planes aligned as published, rows long enough, inside the memory.
inline unsigned extraCheck(const Layer &l, uint32_t width, uint32_t height, const Limits &limits)
{
    const switch_window_caps::Format *f = extraFormat(l.surface);
    if (!f)
        return kFormat;
    for (unsigned p = 0; p < f->planes; ++p) {
        const uint64_t base = f->planes > 1 ? l.surface.offset[p] : 0;
        const uint64_t rows = f->planes > 1 ? l.surface.planeHeight[p] : l.surface.height;
        const uint32_t stride = l.surface.stride[p];
        if (!aligned(base, limits.planeBaseAlignment) || !aligned(stride, limits.strideAlignment) || stride > 0xffff)
            return kAlignment;
        const uint64_t minRow = static_cast<uint64_t>(p ? f->chromaBytes : f->lumaBytes) *
                                (f->pairs ? (l.surface.width + 1) / 2 * 2 : l.surface.width);
        if (stride < minRow || rows < l.surface.height || base + rows * stride > l.surface.allocation ||
            l.surface.allocation > l.surface.descriptorBytes)
            return kSurface;
    }
    Image image;
    return extraImageOf(l, width, height, image, nullptr) ? kAccepted : kScale;
}

// The bytes a pixel window B fetches (bandwidth) and counts in PRESCALED_SIZE, for every kind.
inline uint32_t fetchBytesPerPixel(const Image &image)
{
    if (image.fetchBytes)
        return image.fetchBytes;
    return image.yuv ? 2 : 4;
}

inline uint32_t prescaledBytesPerPixel(const Image &image)
{
    if (image.lumaBytes)
        return image.lumaBytes;
    return image.yuv ? 1 : 4;
}
