// SPDX-License-Identifier: MIT
// Block-compressed textures over memory their callers hold (newTextureLayoutWithDescriptor:isHeapOrBufferBacked: and
// newTextureWithBytesNoCopy:length:descriptor:deallocator:). Core Animation's render server decodes a client's compressed
// image (CA::Render::CompressedImage), CoreUI its asset catalogs' ASTC (CUIATECompressedDataFromBuffer) and ImageIO
// PosterBoard's pre-twiddled snapshots (ASTCTwiddler) into memory sized, aligned and filled through the device's layout
// object, which Core Animation (MetalContext::update_texture), RenderBox and PhotosUI then hand to the no-copy texture
// call. The layout here is NIL's block-linear image (fx::blockChainLayout), so the texture is the caller's pages under a
// block-linear header: no copy, no m2v. Memory off the image's alignment (raw_block_base.h) is copied by the copy engine
// into a pool image of the same layout when the texture is made and after each write through Metal (replaceRegion, or
// the layout object into memory a texture already wraps), and a blit into the image is copied back to it. A plain CPU
// store into that memory is the caller's alone: memory without a surface ID is cleaned once, at import, and no lock or
// seed shows a later store (TegraGM20BABI.h BoImport), in place or copied.
#import "RawMetalInternal.h"
#include <unordered_map>
#include "../fx/fx_image.h"

@class RMNoCopyTexture;
static void wroteThroughLayout(const void *memory);

namespace {

// Which device's layout the memory is in; ImageIO remakes a snapshot whose watermark is not the device's
struct RMWatermark {
    uint64_t words[2];
};

constexpr RMWatermark kBlockLinearWatermark = {{0x4c4220423032474dULL, 1}};  // "MG20B BL", version 1

// The NIL layout of a descriptor this device lays out block-linear itself: a block-compressed 2D texture or 2D array of
// one sample
bool blockLayout(MTLTextureDescriptor *d, raw::fx::MipChain &out, const raw::FormatInfo **format)
{
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    const bool array = d.textureType == MTLTextureType2DArray;
    if (!raw::blockCompressed(f) || (d.textureType != MTLTextureType2D && !array) || d.sampleCount > 1 || d.depth > 1)
        return false;
    *format = f;
    return raw::fx::blockChainLayout(uint32_t(d.width), uint32_t(d.height), uint32_t(d.mipmapLevelCount),
                                     uint32_t(array ? std::max<NSUInteger>(d.arrayLength, 1) : 1), f->blockW, f->blockH,
                                     f->bpp, out);
}

// A Metal region (texels) of one image as the elements it covers; false where it is not on the level's blocks
bool regionElements(const raw::fx::MipChain &c, const raw::FormatInfo *f, NSUInteger level, MTLRegion r, uint32_t e[4])
{
    if (level >= c.levels || r.size.depth != 1 || r.origin.z || r.origin.x % f->blockW || r.origin.y % f->blockH)
        return false;
    e[0] = uint32_t(r.origin.x / f->blockW);
    e[1] = uint32_t(r.origin.y / f->blockH);
    e[2] = uint32_t((r.size.width + f->blockW - 1) / f->blockW);
    e[3] = uint32_t((r.size.height + f->blockH - 1) / f->blockH);
    const raw::fx::MipLevel &lv = c.level[level];
    return e[2] && e[3] && e[0] + e[2] <= lv.width && e[1] + e[3] <= lv.height;
}

#if SWITCH_RAW_DIAGNOSTICS
// Where a caller's blocks land against its page (HW Test ui.compressed_image's intermittent swizzle of 2-block groups,
// gpu.astc.layouts): once for each shape and base in a 16 KiB page, so a process's boot-time images do not use up the
// lines before the one that shows the swizzle, and at most 64 shapes and bases a process
class OncePerShape {
public:
    bool first(uint32_t format, uint32_t width, uint32_t height, const void *memory)
    {
        const uint64_t key = uint64_t(format & 0xffff) << 48 | uint64_t(width & 0x3fff) << 34 |
                             uint64_t(height & 0x3fff) << 20 | (uintptr_t(memory) & 0x3fff);
        std::lock_guard<raw::Mutex> l(lock_);
        return seen_.size() < 64 && seen_.insert(key).second;
    }

private:
    raw::Mutex lock_;
    std::set<uint64_t> seen_;
};

void logBlockWrite(OncePerShape &once, const char *caller, uint32_t format, const raw::fx::MipChain &c,
                   const void *memory, NSUInteger slice, NSUInteger level, const uint32_t e[4], size_t rowStride)
{
    if (!once.first(format, c.width, c.height, memory))
        return;
    const raw::fx::MipLevel &lv = c.level[level];
    const uint64_t first =
        slice * c.arrayStride + lv.offset + tgm_layout_offset_of(lv.rowBytes, lv.yLog2, e[0] * c.bpp, e[1]);
    os_log(
        rawLog(),
        "raw: block layout write by %{public}s, format %u %ux%u blocks: memory & 0x3fff 0x%llx, level %lu slice %lu, "
        "elements %u,%u %ux%u, rows %zu bytes apart, first element at +0x%llx (%u-GOB blocks, %u GOBs across)",
        caller, format, c.width, c.height, (unsigned long long)(uintptr_t(memory) & 0x3fff), (unsigned long)level,
        (unsigned long)slice, e[0], e[1], e[2], e[3], rowStride, (unsigned long long)first, 1u << lv.yLog2,
        lv.rowBytes / 64);
}

void logImport(const void *bytes, NSUInteger length, MTLTextureDescriptor *d, const raw::fx::MipChain &c,
               const raw::Texture &t)
{
    static OncePerShape once;
    if (!once.first(uint32_t(d.pixelFormat), uint32_t(d.width), uint32_t(d.height), bytes))
        return;
    os_log(rawLog(),
           "raw: no-copy texture %lux%lu format %lu over %lu bytes: cpu & 0x3fff 0x%llx, gpu va 0x%llx (& 0x3fff "
           "0x%llx), %u-GOB blocks, %u GOBs across, %llu aligned, %{public}s",
           (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)length,
           (unsigned long long)(uintptr_t(bytes) & 0x3fff), (unsigned long long)t.va,
           (unsigned long long)(t.va & 0x3fff), 1u << c.yLog2, c.level[0].rowBytes / 64, (unsigned long long)c.align,
           t.callerPages ? "copied into a pool image" : "in place, kind 0xfe");
}
#endif

}  // namespace

// A selector it lacks is answered zero (RMObject's forwardInvocation:), with the m2v frontend's layout class as the one asked
@interface RMBlockLayout : RMObject {
@public
    raw::fx::MipChain _layout;
    const raw::FormatInfo *_format;
}
@end

@implementation RMBlockLayout

+ (Class)twinClass
{
    return NSClassFromString(@"NBTextureLayout");
}

- (NSUInteger)size
{
    return NSUInteger(_layout.bytes);
}

// NVK's image alignment (raw::kBlAlign), a multiple of every block's
- (NSUInteger)alignment
{
    return NSUInteger(std::max<uint64_t>(_layout.align, raw::kBlAlign));
}

- (MTLSize)copyGranularity
{
    return MTLSizeMake(_format->blockW, _format->blockH, 1);
}

- (RMWatermark)watermark
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
        SWITCH_REFUSE("raw", "layout.copy", "a block layout copy outside the image: refused");
        return;
    }
#if SWITCH_RAW_DIAGNOSTICS
    static OncePerShape once;
    logBlockWrite(once, "copyFromLinearBytes", _format->mtl, _layout, memory, slice, level, e,
                  rowStride ? rowStride : size_t(e[2]) * _layout.bpp);
#endif
    raw::fx::writeElements(_layout, static_cast<uint8_t *>(memory), uint32_t(slice), uint32_t(level), e[0], e[1], e[2],
                           e[3], static_cast<const uint8_t *>(bytes) + offset,
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
        SWITCH_REFUSE("raw", "layout.copy", "a block layout copy outside the image: refused");
        return;
    }
    raw::fx::readElements(_layout, static_cast<const uint8_t *>(memory), uint32_t(slice), uint32_t(level), e[0], e[1],
                          e[2], e[3], static_cast<uint8_t *>(bytes) + offset,
                          rowStride ? rowStride : size_t(e[2]) * _layout.bpp);
}

@end

// A block-compressed texture over its caller's pages. Its blocks go in and out through the layout; the deallocator runs once
// the texture's import is gone.
@interface RMNoCopyTexture : RMTexture {
@public
    void *_bytes;
    NSUInteger _length;
    void (^_deallocator)(void *, NSUInteger);
    raw::fx::MipChain _layout;
    const raw::FormatInfo *_format;
}
@end

namespace {

// The live no-copy textures by the memory they wrap, for the writes into it through the layout object
class WrappedMemory {
public:
    void add(const void *memory, RMNoCopyTexture *t)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        textures_.emplace(memory, t);
    }

    // The entries over `memory` whose texture is gone, a deallocating one included
    void prune(const void *memory)
    {
        std::lock_guard<raw::Mutex> l(lock_);
        auto [from, to] = textures_.equal_range(memory);
        while (from != to)
            from = from->second ? std::next(from) : textures_.erase(from);
    }

    std::vector<RMNoCopyTexture *> over(const void *memory)
    {
        std::vector<RMNoCopyTexture *> found;
        std::lock_guard<raw::Mutex> l(lock_);
        auto [from, to] = textures_.equal_range(memory);
        for (; from != to; ++from)
            if (RMNoCopyTexture *t = from->second)
                found.push_back(t);
        return found;
    }

private:
    raw::Mutex lock_;
    std::unordered_multimap<const void *, __weak RMNoCopyTexture *> textures_;
};

WrappedMemory &wrapped()
{
    static WrappedMemory *memory = new WrappedMemory;
    return *memory;
}

// The texture held until the copy completed: its image and the caller's pages stay until then
bool copyCallerPages(RMNoCopyTexture *t)
{
    if (!t->_raw.callerPages)
        return true;
    auto owner = std::shared_ptr<void>((__bridge_retained void *)t, [](void *held) {
        CFRelease(held);
    });
    return gRM->raw->copyCallerPages(t->_raw, std::move(owner));
}

// The CPU wrote the texture's memory through Metal: the lines go to memory, and a copied texture's image is copied again
void wrote(RMNoCopyTexture *t, const char *by)
{
    cacheLines(static_cast<const uint8_t *>(t->_bytes), t->_layout.bytes, false);
    if (!copyCallerPages(t))
        SWITCH_REFUSE("raw", "texture.bytesnocopy.copy",
                      "a no-copy texture's copy into its image after %s: refused (the GPU reads the blocks before it)",
                      by);
}

}  // namespace

static void wroteThroughLayout(const void *memory)
{
    for (RMNoCopyTexture *t : wrapped().over(memory))
        wrote(t, "a layout write");
}

@implementation RMNoCopyTexture

- (void)dealloc
{
    wrapped().prune(_bytes);
    rmHeapForgetTexture(self);
    if (_rawOk && gRM)
        gRM->raw->destroyTexture(_raw);
    _rawOk = false;
    if (_deallocator)
        _deallocator(_bytes, _length);
}

// No m2v texture over these pages: a command buffer that goes to m2v for another reason samples nothing here
- (id)rmTwin
{
    return nil;
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
#if SWITCH_RAW_DIAGNOSTICS
    static OncePerShape once;
    logBlockWrite(once, "replaceRegion", _format->mtl, _layout, _bytes, slice, level, e, row);
#endif
    raw::fx::writeElements(_layout, static_cast<uint8_t *>(_bytes), uint32_t(slice), uint32_t(level), e[0], e[1], e[2],
                           e[3], static_cast<const uint8_t *>(bytes), row);
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
    raw::fx::readElements(_layout, static_cast<const uint8_t *>(_bytes), uint32_t(slice), uint32_t(level), e[0], e[1],
                          e[2], e[3], static_cast<uint8_t *>(bytes), row);
}

@end

@implementation RMDevice (NoCopy)

- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d isHeapOrBufferBacked:(BOOL)backed
{
    raw::fx::MipChain layout;
    const raw::FormatInfo *f = nullptr;
    if (backed || !blockLayout(d, layout, &f)) {
        id<MTLDevice> m2v = RM_M2V("a texture layout the raw path does not take");
        return ((id (*)(id, SEL, id, BOOL))objc_msgSend)(m2v, _cmd, d, backed);
    }
    RMBlockLayout *l = [RMBlockLayout new];
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
    raw::fx::MipChain layout;
    const raw::FormatInfo *f = nullptr;
    if (!blockLayout(d, layout, &f)) {
        id<MTLDevice> m2v = RM_M2V("a no-copy texture the raw path does not take");
        return ((id (*)(id, SEL, void *, NSUInteger, id, id))objc_msgSend)(m2v, _cmd, bytes, length, d, deallocator);
    }
    RMNoCopyTexture *t = [RMNoCopyTexture new];
    t->_device = self;
    t->_descriptor = [d copy];
    t->_kind = RMTexturePool;
    t->_layout = layout;
    t->_format = f;
    if (!gRM->raw->createBlockTextureOver(bytes, length, uint32_t(d.width), uint32_t(d.height), uint32_t(d.pixelFormat),
                                          layout, t->_raw)) {
        SWITCH_REFUSE_DETAIL("raw", "texture.bytesnocopy", uintptr_t(d.pixelFormat),
                             "no-copy texture of format %lu %lux%lu, %lu levels, over %lu bytes at 0x%llx: refused",
                             (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height,
                             (unsigned long)d.mipmapLevelCount, (unsigned long)length,
                             (unsigned long long)uintptr_t(bytes));
        return nil;
    }
#if SWITCH_RAW_DIAGNOSTICS
    logImport(bytes, length, d, layout, t->_raw);
#endif
    t->_rawOk = true;
    t->_bytes = bytes;
    t->_length = length;
    if (!copyCallerPages(t)) {
        SWITCH_REFUSE_DETAIL("raw", "texture.bytesnocopy.copy", uintptr_t(d.pixelFormat),
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
