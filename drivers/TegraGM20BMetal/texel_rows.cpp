// SPDX-License-Identifier: MIT
// A texel buffer over a surface the kernel laid out block-linear: its pool, the copy engine's moves of its bytes between
// the pool and the surface's rows, and its CPU copies (texel_rows.h)
#include "lock.h"
#include "gpu.h"
#include "surface_refusal.h"
#include "texel_rows.h"

#include <cstring>

#include "gm20b_gpu_internal.h"

namespace gm20b {

namespace {
// The surface's block-linear image as the copy engine's side of a part, its texels `unit` bytes each
Texture rowsSide(const Import &i, uint32_t unit)
{
    Texture rows;
    rows.import = const_cast<Import *>(&i);
    rows.va = i.bo.va;
    rows.pitch = i.layout.row_bytes;
    rows.bpp = uint8_t(unit);
    rows.width = i.layout.row_bytes / unit;
    rows.height = i.layout.height;
    rows.blockLinear = true;
    rows.blockHeight = i.layout.y_log2;
    rows.layerBytes = i.layout.bytes;
    rows.storage = {i.bo.va, i.layout.bytes, !i.id};
    return rows;
}
}  // namespace

// lock_ held
bool Device::createTexelRows(std::unique_lock<gm20b::Mutex> &l, Import *i, uint64_t offset, uint32_t bytesPerRow,
                             uint32_t elements, uint32_t format, Texture &t, DeferredFrees &deferred)
{
    const FormatInfo *f = formatInfo(format);
    const uint64_t bytes = uint64_t(elements) * f->bpp;
    const TexelRowsParts parts = texelRowsParts(i->layout, offset, bytesPerRow, bytes);
    t = {};
    if (!poolAlloc(l, bytes, t.span)) {
        surfaceRefusal = "no memory for the pool a texel buffer over a block-linear surface is held in";
        return false;
    }
    t.format = format;
    t.bpp = f->bpp;
    t.width = t.texelWidth = elements;
    t.height = t.texelHeight = 1;
    t.pitch = uint32_t(bytes);
    t.type = fx::kTexOneDBuffer;
    t.va = t.span.va;
    if (!(t.tic = allocTic())) {
        Lease *none = nullptr;
        freeSpan(l, t.span, memoryOf(t), none, deferred);
        surfaceRefusal = "no texture header left";
        return false;
    }
    t.texelRows = new TexelRows{i, offset, bytesPerRow, parts};
    writeTic(t);
    return true;
}

// replaceRegion:/getBytes: of a texel buffer over a surface's rows: the copy engine moves the region's bytes between the
// caller's and the rows themselves, a part at a time (TextureTransfers::copy); bytes with no storage there read 0, and
// what is written there lands nowhere
TextureTransferResult Device::transferTexelRows(const Texture &t, const uint32_t origin[3], const uint32_t size[3],
                                                void *bytes, bool write, std::shared_ptr<void> owner)
{
    if (origin[1] || origin[2] || size[1] != 1 || size[2] != 1 || !size[0] || origin[0] > t.width ||
        size[0] > t.width - origin[0])
        return TextureTransferResult::Unsupported;
    const TexelRows &r = *t.texelRows;
    const Import &i = *r.import;
    const uint64_t length = uint64_t(size[0]) * t.bpp;
    const TexelRowsParts parts =
        texelRowsParts(i.layout, r.offset + uint64_t(origin[0]) * t.bpp, r.bytesPerRow, length);
    if (!texelRowsAddressable(parts))
        return TextureTransferResult::Unsupported;
    if (!write && parts.held < length)
        memset(bytes, 0, length);
    for (uint32_t k = 0; k < parts.count; k++) {
        const TexelRowsPart &p = parts.part[k];
        TextureTransfer region;
        region.texture = sideOf(rowsSide(i, p.unit), p.column / p.unit, p.row, 0);
        region.width = p.bytes / p.unit;
        region.height = p.rows;
        region.planes = 1;
        region.bpp = p.unit;
        if (!transfers_.copy(k_, set_, lock_, region, static_cast<uint8_t *>(bytes) + p.at, r.bytesPerRow, 0, write,
                             owner))
            return TextureTransferResult::Failed;
    }
    return TextureTransferResult::Complete;
}

void moveTexelRows(BlitEncoder &b, const Texture &t, bool fill)
{
    const TexelRows &r = *t.texelRows;
    const TexelRowsParts &parts = r.parts;
    const uint64_t bytes = uint64_t(t.width) * t.bpp;
    if (fill && parts.held < bytes) {
        b.fillBytes(t.va, bytes, 0);
        b.barrier();
    }
    Texture lines;
    lines.pitch = r.bytesPerRow;
    lines.storage = t.storageRange();
    for (uint32_t k = 0; k < parts.count; k++) {
        const TexelRowsPart &p = parts.part[k];
        const Texture rows = rowsSide(*r.import, p.unit);
        lines.va = t.va + p.at;
        lines.bpp = uint8_t(p.unit);
        const uint32_t x = p.column / p.unit, w = p.bytes / p.unit;
        if (fill)
            b.copyTexels(rows, x, p.row, 0, lines, 0, 0, 0, w, p.rows);
        else
            b.copyTexels(lines, 0, 0, 0, rows, x, p.row, 0, w, p.rows);
    }
}

}  // namespace gm20b
