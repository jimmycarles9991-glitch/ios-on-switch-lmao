// SPDX-License-Identifier: MIT
// Block-compressed textures over memory their callers hold (newTextureLayoutWithDescriptor:isHeapOrBufferBacked: and
// newTextureWithBytesNoCopy:length:descriptor:deallocator:). Core Animation's render server decodes a client's compressed
// image (CA::Render::CompressedImage), CoreUI its asset catalogs' ASTC (CUIATECompressedDataFromBuffer) and ImageIO
// PosterBoard's pre-twiddled snapshots (ASTCTwiddler) into memory sized, aligned and filled through the device's layout
// object, which Core Animation (MetalContext::update_texture), RenderBox and PhotosUI then hand to the no-copy texture
// call. The layout here is NIL's block-linear image (fx::blockChainLayout), so the texture is the caller's pages under a
// block-linear header: no copy, no m2v. Memory off the image's alignment (block_base.h) is copied by the copy engine
// into a pool image of the same layout when the texture is made and after each write through Metal (replaceRegion, or
// the layout object into memory a texture already wraps), and a blit into the image is copied back to it. A plain CPU
// store into that memory is the caller's alone: memory without a surface ID is cleaned once, at import, and no lock or
// seed shows a later store (TegraGM20BABI.h BoImport), in place or copied.
#import "TegraGM20BMetalInternal.h"
#include <unordered_map>
#include "fx/fx_image.h"

@class SwitchMetalNoCopyTexture;
static void wroteThroughLayout(const void *memory);

namespace {

// Which device's layout the memory is in; ImageIO remakes a snapshot whose watermark is not the device's
struct SwitchMetalWatermark {
    uint64_t words[2];
};

constexpr SwitchMetalWatermark kBlockLinearWatermark = {{0x4c4220423032474dULL, 1}};  // "MG20B BL", version 1

// The NIL layout of a descriptor this device lays out block-linear itself: a block-compressed 2D texture or 2D array of
// one sample
bool blockLayout(MTLTextureDescriptor *d, gm20b::fx::MipChain &out, const gm20b::FormatInfo **format)
{
    const gm20b::FormatInfo *f = gm20b::formatInfo(uint32_t(d.pixelFormat));
    const bool array = d.textureType == MTLTextureType2DArray;
    if (!gm20b::blockCompressed(f) || (d.textureType != MTLTextureType2D && !array) || d.sampleCount > 1 || d.depth > 1)
        return false;
    *format = f;
    return gm20b::fx::blockChainLayout(uint32_t(d.width), uint32_t(d.height), uint32_t(d.mipmapLevelCount),
                                       uint32_t(array ? std::max<NSUInteger>(d.arrayLength, 1) : 1), f->blockW,
                                       f->blockH, f->bpp, out);
}

// A Metal region (texels) of one image as the elements it covers; false where it is not on the level's blocks
bool regionElements(const gm20b::fx::MipChain &c, const gm20b::FormatInfo *f, NSUInteger level, MTLRegion r,
                    uint32_t e[4])
{
    if (level >= c.levels || r.size.depth != 1 || r.origin.z || r.origin.x % f->blockW || r.origin.y % f->blockH)
        return false;
    e[0] = uint32_t(r.origin.x / f->blockW);
    e[1] = uint32_t(r.origin.y / f->blockH);
    e[2] = uint32_t((r.size.width + f->blockW - 1) / f->blockW);
    e[3] = uint32_t((r.size.height + f->blockH - 1) / f->blockH);
    const gm20b::fx::MipLevel &lv = c.level[level];
    return e[2] && e[3] && e[0] + e[2] <= lv.width && e[1] + e[3] <= lv.height;
}

}  // namespace

// A selector it lacks is answered zero (SwitchMetalObject's forwardInvocation:)
@interface SwitchMetalBlockLayout : SwitchMetalObject {
@public
    gm20b::fx::MipChain _layout;
    const gm20b::FormatInfo *_format;
}
@end

@implementation SwitchMetalBlockLayout

- (NSUInteger)size
{
    return NSUInteger(_layout.bytes);
}

// NVK's image alignment (gm20b::kBlAlign), a multiple of every block's
- (NSUInteger)alignment
{
    return NSUInteger(std::max<uint64_t>(_layout.align, gm20b::kBlAlign));
}

- (MTLSize)copyGranularity
{
    return MTLSizeMake(_format->blockW, _format->blockH, 1);
}

- (SwitchMetalWatermark)watermark
{
    return kBlockLinearWatermark;
}

- (void)initializeTextureMemory:(void *)memory
{
    (void)memory;
}

// The caller's word that it wrote the memory, however it wrote it
- (void)finalizeTextureMemory:(void *)memory
{
    wroteThroughLayout(memory);
}

- (void)copyFromLinearBytes:(const void *)bytes
               linearOffset:(NSUInteger)offset
          linearBytesPerRow:(NSUInteger)rowStride
        linearBytesPerImage:(NSUInteger)imageStride
            toTextureMemory:(void *)memory
               textureSlice:(NSUInteger)slice
               textureLevel:(NSUInteger)level
              textureRegion:(MTLRegion)region
{
    (void)imageStride;
    uint32_t e[4];
    if (slice >= _layout.layers || !regionElements(_layout, _format, level, region, e)) {
        SWITCH_REFUSE("gm20b", "layout.copy", "a block layout copy outside the image: refused");
        return;
    }
    gm20b::fx::writeElements(_layout, static_cast<uint8_t *>(memory), uint32_t(slice), uint32_t(level), e[0], e[1],
                             e[2], e[3], static_cast<const uint8_t *>(bytes) + offset,
                             rowStride ? rowStride : size_t(e[2]) * _layout.bpp);
    wroteThroughLayout(memory);
}

- (void)copyFromTextureMemory:(const void *)memory
                 textureSlice:(NSUInteger)slice
                 textureLevel:(NSUInteger)level
                textureRegion:(MTLRegion)region
                toLinearBytes:(void *)bytes
                 linearOffset:(NSUInteger)offset
            linearBytesPerRow:(NSUInteger)rowStride
          linearBytesPerImage:(NSUInteger)imageStride
{
    (void)imageStride;
    uint32_t e[4];
    if (slice >= _layout.layers || !regionElements(_layout, _format, level, region, e)) {
        SWITCH_REFUSE("gm20b", "layout.copy", "a block layout copy outside the image: refused");
        return;
    }
    gm20b::fx::readElements(_layout, static_cast<const uint8_t *>(memory), uint32_t(slice), uint32_t(level), e[0], e[1],
                            e[2], e[3], static_cast<uint8_t *>(bytes) + offset,
                            rowStride ? rowStride : size_t(e[2]) * _layout.bpp);
}

@end

// A block-compressed texture over its caller's pages. Its blocks go in and out through the layout; the deallocator runs once
// the texture's import is gone.
@interface SwitchMetalNoCopyTexture : SwitchMetalTexture {
@public
    void *_bytes;
    NSUInteger _length;
    void (^_deallocator)(void *, NSUInteger);
    gm20b::fx::MipChain _layout;
    const gm20b::FormatInfo *_format;
}
@end

namespace {

// The live no-copy textures by the memory they wrap, for the writes into it through the layout object
class WrappedMemory {
public:
    void add(const void *memory, SwitchMetalNoCopyTexture *t)
    {
        std::lock_guard<gm20b::UnfairMutex> l(lock_);
        textures_.emplace(memory, t);
    }

    // The entries over `memory` whose texture is gone, a deallocating one included
    void prune(const void *memory)
    {
        std::lock_guard<gm20b::UnfairMutex> l(lock_);
        auto [from, to] = textures_.equal_range(memory);
        while (from != to)
            from = from->second ? std::next(from) : textures_.erase(from);
    }

    std::vector<SwitchMetalNoCopyTexture *> over(const void *memory)
    {
        std::vector<SwitchMetalNoCopyTexture *> found;
        std::lock_guard<gm20b::UnfairMutex> l(lock_);
        auto [from, to] = textures_.equal_range(memory);
        for (; from != to; ++from)
            if (SwitchMetalNoCopyTexture *t = from->second)
                found.push_back(t);
        return found;
    }

private:
    gm20b::UnfairMutex lock_;
    std::unordered_multimap<const void *, __weak SwitchMetalNoCopyTexture *> textures_;
};

WrappedMemory &wrapped()
{
    static WrappedMemory *memory = new WrappedMemory;
    return *memory;
}

// The texture held until the copy completed: its image and the caller's pages stay until then
bool copyCallerPages(SwitchMetalNoCopyTexture *t)
{
    if (!t->_raw.callerPages)
        return true;
    auto owner = std::shared_ptr<void>((__bridge_retained void *)t, [](void *held) {
        CFRelease(held);
    });
    return gRM->raw->copyCallerPages(t->_raw, std::move(owner));
}

// The CPU wrote the texture's memory through Metal: the lines go to memory, and a copied texture's image is copied again
void wrote(SwitchMetalNoCopyTexture *t, const char *by)
{
    cacheLines(static_cast<const uint8_t *>(t->_bytes), t->_layout.bytes, false);
    if (!copyCallerPages(t))
        SWITCH_REFUSE("gm20b", "texture.bytesnocopy.copy",
                      "a no-copy texture's copy into its image after %s: refused (the GPU reads the blocks before it)",
                      by);
}

}  // namespace

static void wroteThroughLayout(const void *memory)
{
    for (SwitchMetalNoCopyTexture *t : wrapped().over(memory))
        wrote(t, "a layout write");
}

@implementation SwitchMetalNoCopyTexture

- (void)dealloc
{
    wrapped().prune(_bytes);
    gm20bHeapForgetTexture(self);
    if (_gm20bOk && gRM)
        gRM->raw->destroyTexture(_raw);
    _gm20bOk = false;
    if (_deallocator)
        _deallocator(_bytes, _length);
}

- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
        bytesPerImage:(NSUInteger)image
{
    (void)image;
    uint32_t e[4];
    if (slice >= _layout.layers || !regionElements(_layout, _format, level, r, e))
        return;
    gm20b::fx::writeElements(_layout, static_cast<uint8_t *>(_bytes), uint32_t(slice), uint32_t(level), e[0], e[1],
                             e[2], e[3], static_cast<const uint8_t *>(bytes), row);
    wrote(self, "replaceRegion");
}

- (void)getBytes:(void *)bytes
      bytesPerRow:(NSUInteger)row
    bytesPerImage:(NSUInteger)image
       fromRegion:(MTLRegion)r
      mipmapLevel:(NSUInteger)level
            slice:(NSUInteger)slice
{
    (void)image;
    uint32_t e[4];
    if (slice >= _layout.layers || !regionElements(_layout, _format, level, r, e))
        return;
    if (_gpuDirty.exchange(false, std::memory_order_acq_rel))
        cacheLines(static_cast<const uint8_t *>(_bytes), _layout.bytes, true);
    gm20b::fx::readElements(_layout, static_cast<const uint8_t *>(_bytes), uint32_t(slice), uint32_t(level), e[0], e[1],
                            e[2], e[3], static_cast<uint8_t *>(bytes), row);
}

@end

@implementation SwitchMetalDevice (NoCopy)

- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d isHeapOrBufferBacked:(BOOL)backed
{
    gm20b::fx::MipChain layout;
    const gm20b::FormatInfo *f = nullptr;
    // nil as m2v answered: a heap's or a buffer's texture is laid out by the device when it is made there
    if (backed)
        return nil;
    if (!blockLayout(d, layout, &f))
        return gm20bPlainLayout(self, d);
    SwitchMetalBlockLayout *l = [SwitchMetalBlockLayout new];
    l->_device = self;
    l->_layout = layout;
    l->_format = f;
    return l;
}

- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)bytes
                                     length:(NSUInteger)length
                                 descriptor:(MTLTextureDescriptor *)d
                                deallocator:(void (^)(void *, NSUInteger))deallocator
{
    SWITCH_API(ApiObject);
    gm20b::fx::MipChain layout;
    const gm20b::FormatInfo *f = nullptr;
    if (!blockLayout(d, layout, &f)) {
        os_log_error(
            gm20bLog(),
            "SwitchMetal: %{public}s: no no-copy texture of format %lu %lux%lu type %lu: only block-compressed 2D "
            "images are laid out in caller memory",
            getprogname(), (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height,
            (unsigned long)d.textureType);
        return nil;
    }
    SwitchMetalNoCopyTexture *t = [SwitchMetalNoCopyTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    t->_kind = SwitchMetalTexturePool;
    t->_layout = layout;
    t->_format = f;
    if (!gRM->raw->createBlockTextureOver(bytes, length, uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat),
                                          layout, t->_raw)) {
        SWITCH_REFUSE_DETAIL("gm20b", "texture.bytesnocopy", uintptr_t(d.pixelFormat),
                             "no-copy texture of format %lu %lux%lu, %lu levels, over %lu bytes at 0x%llx: refused",
                             (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height,
                             (unsigned long)d.mipmapLevelCount, (unsigned long)length,
                             (unsigned long long)uintptr_t(bytes));
        return nil;
    }
    t->_gm20bOk = true;
    t->_bytes = bytes;
    t->_length = length;
    if (!copyCallerPages(t)) {
        SWITCH_REFUSE_DETAIL("gm20b", "texture.bytesnocopy.copy", uintptr_t(d.pixelFormat),
                             "no-copy texture of format %lu %lux%lu over %lu bytes at 0x%llx: the copy engine's copy "
                             "into its image refused",
                             (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height,
                             (unsigned long)length, (unsigned long long)uintptr_t(bytes));
        return nil;
    }
    t->_deallocator = [deallocator copy];
    wrapped().add(bytes, t);
    return (id<MTLTexture>)t;
}

@end
