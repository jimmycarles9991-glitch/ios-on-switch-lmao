// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "../fx/fx_image.h"
#include "raw_xr_words.h"

bool transferTexture(RMTexture *texture, MTLRegion region, NSUInteger level, NSUInteger slice, void *bytes,
                     NSUInteger row, NSUInteger image, bool write)
{
    if (!texture->_rawOk || texture->_kind == RMTextureBuffer || texture->_kind == RMTextureView ||
        (texture->_heap && texture->_heap->_descriptor.storageMode == MTLStorageModeShared && !texture->_raw.zeta))
        return false;
    if (!region.size.width || !region.size.height || !region.size.depth)
        return true;
    for (const NSUInteger value : {region.origin.x, region.origin.y, region.origin.z, region.size.width,
                                   region.size.height, region.size.depth, level, slice})
        if (value > UINT32_MAX)
            return false;
    const uint32_t origin[] = {uint32_t(region.origin.x), uint32_t(region.origin.y), uint32_t(region.origin.z)};
    const uint32_t size[] = {uint32_t(region.size.width), uint32_t(region.size.height), uint32_t(region.size.depth)};
    if (write)
        declareGpuWrites(texture);
    auto owner = std::shared_ptr<void>((__bridge_retained void *)texture, [](void *held) {
        CFRelease(held);
    });
    const auto result = gRM->raw->transferTexture(texture->_raw, uint32_t(level), uint32_t(slice), origin, size, bytes,
                                                  row, image, write, std::move(owner));
    // A failed write published nothing and a failed read touched only its staging: the CPU paths take either over
    if (result != raw::TextureTransferResult::Complete)
        return false;
    if (write)
        texture->_gpuDirty.store(true, std::memory_order_release);
    return true;
}

bool refuseZetaCpu(RMTexture *texture, MTLRegion region, NSUInteger level, NSUInteger slice, bool write)
{
    if (!texture->_rawOk || !texture->_raw.zeta)
        return false;
    SWITCH_REFUSE_DETAIL(
        "raw", "tex.zeta_cpu", texture->_descriptor.pixelFormat,
        "%s of %lux%lu at (%lu, %lu) level %lu slice %lu of a format %lu depth/stencil %s: the copy "
        "engine made no copy (MTLTexture.h replaceRegion:/getBytes: name a region inside the level)",
        write ? "replaceRegion" : "getBytes", (unsigned long)region.size.width, (unsigned long)region.size.height,
        (unsigned long)region.origin.x, (unsigned long)region.origin.y, (unsigned long)level, (unsigned long)slice,
        (unsigned long)texture->_descriptor.pixelFormat, texture->_kind == RMTextureView ? "view" : "texture");
    return true;
}

void cacheLines(const uint8_t *from, uint64_t bytes, bool invalidate)
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

const tgm_layout *textureCpuImportLayout(RMTexture *texture)
{
    while (texture) {
        if (texture->_raw.import && texture->_raw.import->kernelTiled())
            return &texture->_raw.import->layout;
        texture = texture->_kind == RMTextureView ? texture->_parent : nil;
    }
    return nullptr;
}

// The caller holds CPU backing ownership through every read or partial write.
void cpuTiled(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write)
{
    tgm_layout l;
    if (!r.size.width || !r.size.height)
        return;
    if (const auto *import = textureCpuImportLayout(t))
        l = *import;
    else if (!tgm_layout_tiled(t->_raw.width, t->_raw.height, t->_raw.bpp, &l))
        return;
    r.origin.x += t->_raw.surfaceX;
    r.origin.y += t->_raw.surfaceY;
    if (write) {
        tgm_layout_write(&l, t->_raw.cpu, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                         uint32_t(r.size.height), bytes, row);
        return;
    }
    tgm_layout_read(&l, t->_raw.cpu, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                    uint32_t(r.size.height), bytes, row);
}

// replaceRegion/getBytes of level `level` of slice `slice` of a shared mipmapped texture: in its chain (a layer's levels
// arrayStride on, a 3D level's planes in its own blocks), in the CPU's view of kind-0xFE memory
void cpuLevel(RMTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes, NSUInteger row,
              NSUInteger image, bool write)
{
    const raw::Texture::Chain &c = *t->_raw.chain;
    if (level >= c.layout.levels || slice >= c.layout.layers)
        return;
    const raw::fx::MipLevel &lv = c.layout.level[level];
    if (r.origin.x + r.size.width > lv.width || r.origin.y + r.size.height > lv.height ||
        r.origin.z + r.size.depth > lv.depth)
        return;
    if (t->_raw.type == raw::fx::kTexThreeD) {
        const uint32_t origin[3] = {uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.origin.z)},
                       size[3] = {uint32_t(r.size.width), uint32_t(r.size.height), uint32_t(r.size.depth)};
        const size_t planeBytes = image ? image : size_t(row) * r.size.height;
        if (write)
            raw::fx::writeVolume(c.layout, t->_raw.cpu, uint32_t(level), origin, size, bytes, row, planeBytes);
        else
            raw::fx::readVolume(c.layout, t->_raw.cpu, uint32_t(level), origin, size, bytes, row, planeBytes);
        return;
    }
    uint8_t *chain = t->_raw.cpu + slice * c.layout.arrayStride;
    if (write) {
        raw::fx::writeLevel(c.layout, chain, uint32_t(level), uint32_t(r.origin.x), uint32_t(r.origin.y),
                            uint32_t(r.size.width), uint32_t(r.size.height), bytes, row);
    } else {
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
        return;
    }
    raw::fx::readVolume(c, x.cpu, 0, origin, size, bytes, row, planeBytes);
}

// replaceRegion/getBytes of a rectangle of a layer of a 2D array or cube: the layer's 2D image, layerBytes on (a view of a
// mipmapped array's: its chain's stride)
void cpuLayer(RMTexture *t, MTLRegion r, NSUInteger slice, uint8_t *bytes, NSUInteger row, bool write)
{
    const raw::Texture &x = t->_raw;
    raw::fx::MipChain c;
    if (!raw::fx::arrayLayout(x.width, x.height, x.layers, 1, x.bpp, c))
        return;
    uint8_t *layer = x.cpu + slice * (x.layerBytes ? x.layerBytes : c.arrayStride);
    if (write) {
        raw::fx::writeLevel(c, layer, 0, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                            uint32_t(r.size.height), bytes, row);
        return;
    }
    raw::fx::readLevel(c, layer, 0, uint32_t(r.origin.x), uint32_t(r.origin.y), uint32_t(r.size.width),
                       uint32_t(r.size.height), bytes, row);
}

// replaceRegion/getBytes of whole blocks of a block-compressed texture in memory of its own: NIL's image (its chain's
// levels and layers, or a 3D one's planes of blocks, `image` bytes apart in Metal's), in the CPU's view of kind-0xFE
// memory, under the kernel's CPU access to its pages. False: no such region (an origin off a block, a level, slice or
// plane past the image's)
bool cpuBlocks(RMTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes, NSUInteger row,
               NSUInteger image, bool write)
{
    const raw::Texture &x = t->_raw;
    const raw::FormatInfo *f = raw::formatInfo(x.format);
    const bool volume = x.type == raw::fx::kTexThreeD;
    raw::fx::MipChain made;
    const raw::fx::MipChain *c = x.chain ? &x.chain->layout : &made;
    if (!f || !x.cpu ||
        (!x.chain && !(volume ? raw::fx::volumeLayout(x.texelWidth, x.texelHeight, x.depth, 1, f->bpp, false, made,
                                                      f->blockW, f->blockH)
                              : raw::fx::blockChainLayout(x.texelWidth, x.texelHeight, 1, std::max(x.layers, 1u),
                                                          f->blockW, f->blockH, f->bpp, made))))
        return false;
    const raw::fx::MipLevel &lv = c->level[level < c->levels ? level : 0];
    const bool planes = volume ? !slice && r.origin.z + r.size.depth <= lv.depth : !r.origin.z && r.size.depth == 1;
    if (level >= c->levels || slice >= c->layers || !planes || r.origin.x % f->blockW || r.origin.y % f->blockH)
        return false;
    const uint32_t x0 = uint32_t(r.origin.x / f->blockW), y0 = uint32_t(r.origin.y / f->blockH),
                   w = uint32_t((r.size.width + f->blockW - 1) / f->blockW),
                   h = uint32_t((r.size.height + f->blockH - 1) / f->blockH);
    if (!w || !h || x0 + w > lv.width || y0 + h > lv.height)
        return false;
    raw::HostAccess access;
    if (!gRM->raw->hostAccess(x, x.cpu, c->bytes, write, access))
        return false;
    if (volume) {
        const uint32_t origin[3] = {x0, y0, uint32_t(r.origin.z)}, size[3] = {w, h, uint32_t(r.size.depth)};
        const size_t planeBytes = image ? image : size_t(row) * h;
        if (write)
            raw::fx::writeVolume(*c, x.cpu, uint32_t(level), origin, size, bytes, row, planeBytes);
        else
            raw::fx::readVolume(*c, x.cpu, uint32_t(level), origin, size, bytes, row, planeBytes);
        return true;
    }
    if (write)
        raw::fx::writeElements(*c, x.cpu, uint32_t(slice), uint32_t(level), x0, y0, w, h, bytes, row);
    else
        raw::fx::readElements(*c, x.cpu, uint32_t(slice), uint32_t(level), x0, y0, w, h, bytes, row);
    return true;
}

// replaceRegion/getBytes of a shared multisampled texture: Metal's rows of each pixel's samples in turn (fx::writeSamples)
bool cpuSamples(RMTexture *t, MTLRegion r, uint8_t *bytes, NSUInteger row, bool write)
{
    const raw::Texture &x = t->_raw;
    raw::fx::MipChain c;
    if (!x.cpu || x.samples < 2 || !raw::fx::msLayout(x.width, x.height, x.samples, x.bpp, c) || r.origin.z ||
        r.size.depth != 1 || r.origin.x + r.size.width > x.width || r.origin.y + r.size.height > x.height)
        return false;
    raw::HostAccess access;
    if (!gRM->raw->hostAccess(x, x.cpu, c.bytes, write, access))
        return false;
    const uint32_t at[2] = {uint32_t(r.origin.x), uint32_t(r.origin.y)},
                   size[2] = {uint32_t(r.size.width), uint32_t(r.size.height)};
    if (write)
        raw::fx::writeSamples(c, x.cpu, x.samples, at, size, bytes, row);
    else
        raw::fx::readSamples(c, x.cpu, x.samples, at, size, bytes, row);
    return true;
}

// replaceRegion/getBytes of an RGB10A8_2P_XR10 texture of its own memory: its texels' colour words alone, through its
// texels (raw_xr_words.h). False: another texture, which the copies move as it is
bool cpuXrWords(RMTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes, NSUInteger row,
                NSUInteger image, bool write)
{
    if (!t->_rawOk || t->_kind != RMTexturePool || t->_raw.xrPlanes ||
        raw::xrForm(t->_raw.format) != raw::XrForm::Planes || !r.size.width || !r.size.height || !r.size.depth)
        return false;
    const uint32_t w = uint32_t(r.size.width), h = uint32_t(r.size.height), d = uint32_t(r.size.depth);
    const size_t texelRow = size_t(w) * raw::kXrTexelBytes, texelImage = texelRow * h;
    const size_t wordImage = image ? image : size_t(row) * h;
    std::vector<uint8_t> texels(texelImage * d);
    [t rmGetBytes:texels.data()
          bytesPerRow:texelRow
        bytesPerImage:texelImage
           fromRegion:r
          mipmapLevel:level
                slice:slice];
    if (!write) {
        raw::xrWordsFromTexels(texels.data(), texelRow, texelImage, bytes, row, wordImage, w, h, d);
        return true;
    }
    raw::xrWordsIntoTexels(bytes, row, wordImage, texels.data(), texelRow, texelImage, w, h, d);
    [t rmReplaceRegion:r
           mipmapLevel:level
                 slice:slice
             withBytes:texels.data()
           bytesPerRow:texelRow
         bytesPerImage:texelImage];
    return true;
}
