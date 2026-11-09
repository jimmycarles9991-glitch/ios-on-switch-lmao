// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "../fx/fx_image.h"

static void cacheLines(const uint8_t *from, uint64_t bytes, bool invalidate)
{
#if defined(__aarch64__)
    for (uintptr_t a = uintptr_t(from) & ~uintptr_t(63); a < uintptr_t(from) + bytes; a += 64)
        if (invalidate)
            __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
        else
            __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)from;
    (void)bytes;
    (void)invalidate;
#endif
}

// A block-linear texture's CPU copies (replaceRegion, getBytes of level 0) through TegraGM20BLayout.h: the block rows they
// reach are invalidated before a read of what the GPU wrote, cleaned after a write
void cpuTiled(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write)
{
    tgm_layout l;
    if (!r.size.width || !r.size.height || !tgm_layout_tiled(t->_raw.width, t->_raw.height, t->_raw.bpp, &l))
        return;
    const uint64_t blockRow = uint64_t(l.row_bytes) * (8u << l.y_log2);
    const uint64_t from = r.origin.y / (8u << l.y_log2) * blockRow,
                   to = std::min<uint64_t>((r.origin.y + r.size.height + (8u << l.y_log2) - 1) / (8u << l.y_log2) *
                                               blockRow,
                                           uint64_t(l.row_bytes) * l.rows);
    if (write) {
        tgm_layout_write(&l, t->_raw.cpu, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                         uint32_t(r.size.height), bytes, row);
        cacheLines(t->_raw.cpu + from, to - from, false);
        return;
    }
    if (t->_gpuDirty.exchange(false))
        cacheLines(t->_raw.cpu, l.row_bytes * uint64_t(l.rows), true);
    tgm_layout_read(&l, t->_raw.cpu, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                    uint32_t(r.size.height), bytes, row);
}

// replaceRegion/getBytes of level `level` of a shared mipmapped texture: in its chain, in the CPU's view of kind-0xFE memory
void cpuLevel(RMTexture *t, MTLRegion r, NSUInteger level, uint8_t *bytes, NSUInteger row, bool write)
{
    const raw::Texture::Chain &c = *t->_raw.chain;
    if (level >= c.layout.levels)
        return;
    const raw::fx::MipLevel &lv = c.layout.level[level];
    if (r.origin.x + r.size.width > lv.width || r.origin.y + r.size.height > lv.height)
        return;
    uint8_t *chain = t->_raw.cpu;
    const uint64_t at = lv.offset, span = uint64_t(lv.rowBytes) * lv.rows;
    if (write) {
        raw::fx::writeLevel(c.layout, chain, uint32_t(level), uint32_t(r.origin.x), uint32_t(r.origin.y),
                            uint32_t(r.size.width), uint32_t(r.size.height), bytes, row);
        cacheLines(chain + at, span, false);
    } else {
        cacheLines(chain + at, span, true);
        raw::fx::readLevel(c.layout, chain, uint32_t(level), uint32_t(r.origin.x), uint32_t(r.origin.y),
                           uint32_t(r.size.width), uint32_t(r.size.height), bytes, row);
    }
}

// replaceRegion/getBytes of a box of a 3D texture: in NIL's 3D image (fx_image.h), in the CPU's view of kind-0xFE memory
void cpuVolume(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, NSUInteger image, bool write)
{
    const raw::Texture &x = t->_raw;
    raw::fx::MipChain c;
    if (!raw::fx::volumeLayout(x.width, x.height, x.depth, 1, x.bpp, !x.blockDepth, c))
        return;
    const uint32_t origin[3] = {uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.origin.z)},
                   size[3] = {uint32_t(r.size.width), uint32_t(r.size.height), uint32_t(r.size.depth)};
    const size_t planeBytes = image ? image : size_t(row) * r.size.height;
    if (write) {
        raw::fx::writeVolume(c, x.cpu, 0, origin, size, bytes, row, planeBytes);
        cacheLines(x.cpu, c.bytes, false);
        return;
    }
    if (t->_gpuDirty.exchange(false))
        cacheLines(x.cpu, c.bytes, true);
    raw::fx::readVolume(c, x.cpu, 0, origin, size, bytes, row, planeBytes);
}

// replaceRegion/getBytes of a rectangle of a layer of a 2D array or cube: the layer's 2D image, arrayStride on
void cpuLayer(RMTexture *t, MTLRegion r, NSUInteger slice, uint8_t *bytes, NSUInteger row, bool write)
{
    const raw::Texture &x = t->_raw;
    raw::fx::MipChain c;
    if (!raw::fx::arrayLayout(x.width, x.height, x.layers, 1, x.bpp, c))
        return;
    uint8_t *layer = x.cpu + slice * c.arrayStride;
    if (write) {
        raw::fx::writeLevel(c, layer, 0, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                            uint32_t(r.size.height), bytes, row);
        cacheLines(layer, c.arrayStride, false);
        return;
    }
    if (t->_gpuDirty.exchange(false))
        cacheLines(x.cpu, c.bytes, true);
    raw::fx::readLevel(c, layer, 0, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                       uint32_t(r.size.height), bytes, row);
}
