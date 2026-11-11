// SPDX-License-Identifier: MIT
// Block-compressed textures over memory their callers hold (newTextureLayoutWithDescriptor:isHeapOrBufferBacked: and
// newTextureWithBytesNoCopy:length:descriptor:deallocator:). Core Animation's render server decodes a client's compressed
// image (CA::Render::CompressedImage), CoreUI its asset catalogs' ASTC (CUIATECompressedDataFromBuffer) and ImageIO
// PosterBoard's pre-twiddled snapshots (ASTCTwiddler) into memory sized, aligned and filled through the device's layout
// object, which Core Animation (MetalContext::update_texture), RenderBox and PhotosUI then hand to the no-copy texture
// call. The layout here is NIL's block-linear image (fx::blockChainLayout), so the texture is the caller's pages under a
// block-linear header: no copy, no m2v. Other formats and heap- or buffer-backed layouts keep the m2v frontend's answer.
#import "RawMetalInternal.h"
#include "../fx/fx_image.h"

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

- (void)finalizeTextureMemory:(void *)memory
{
    (void)memory;
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
    raw::fx::writeElements(_layout, static_cast<uint8_t *>(memory), uint32_t(slice), uint32_t(level), e[0], e[1], e[2],
                           e[3], static_cast<const uint8_t *>(bytes) + offset,
                           rowStride ? rowStride : size_t(e[2]) * _layout.bpp);
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

@implementation RMNoCopyTexture

- (void)dealloc
{
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
    raw::fx::writeElements(_layout, static_cast<uint8_t *>(_bytes), uint32_t(slice), uint32_t(level), e[0], e[1], e[2],
                           e[3], static_cast<const uint8_t *>(bytes), row);
    cacheLines(static_cast<const uint8_t *>(_bytes), _layout.bytes, false);
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
    raw::fx::readElements(_layout, static_cast<const uint8_t *>(_bytes), uint32_t(slice), uint32_t(level), e[0], e[1],
                          e[2], e[3], static_cast<uint8_t *>(bytes), row);
}

@end

@implementation RMDevice (NoCopy)

- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d isHeapOrBufferBacked:(BOOL)backed
{
    raw::fx::MipChain layout;
    const raw::FormatInfo *f = nullptr;
    if (backed || !blockLayout(d, layout, &f))
        return ((id (*)(id, SEL, id, BOOL))objc_msgSend)(gRM->fallback, _cmd, d, backed);
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
    if (!blockLayout(d, layout, &f))
        return ((id (*)(id, SEL, void *, NSUInteger, id, id))objc_msgSend)(gRM->fallback, _cmd, bytes, length, d,
                                                                           deallocator);
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
    t->_rawOk = true;
    t->_bytes = bytes;
    t->_length = length;
    t->_deallocator = [deallocator copy];
    return (id<MTLTexture>)t;
}

@end
