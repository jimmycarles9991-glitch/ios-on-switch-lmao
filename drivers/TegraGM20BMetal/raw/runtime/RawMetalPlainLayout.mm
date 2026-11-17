// SPDX-License-Identifier: MIT
// The texture layout object (Apple SPI, newTextureLayoutWithDescriptor:isHeapOrBufferBacked:) of a texture the raw path lays
// out no block-linear image of in caller memory (RawMetalNoCopy.mm takes block-compressed 2D ones): the plain layout m2v's
// frontend answered with (NativeBridgeMetalFrontend.mm NBTextureLayout), each slice its levels in turn, each level its
// images, each image its rows of blocks packed, watermark zero. No GPU reads that memory: newTextureWithBytesNoCopy: of
// such a texture is refused, as it was.
#import "RawMetalInternal.h"
#include <algorithm>

namespace {

constexpr uint64_t kPlainMaxLevels = 16;

uint64_t blocksOf(uint64_t texels, uint64_t block)
{
    return (texels + block - 1) / block;
}

struct PlainLayout {
    uint64_t blockBytes, blockW, blockH;
    uint64_t width, height, depth, levels, slices, sliceBytes;
    uint64_t levelOffset[kPlainMaxLevels];

    uint64_t rowBytes(uint64_t level) const
    {
        return blocksOf(std::max<uint64_t>(1, width >> level), blockW) * blockBytes;
    }

    uint64_t rows(uint64_t level) const
    {
        return blocksOf(std::max<uint64_t>(1, height >> level), blockH);
    }

    uint64_t images(uint64_t level) const
    {
        return std::max<uint64_t>(1, depth >> level);
    }

    // copy(offset in the texture's memory, offset in the linear bytes, bytes) for each row of blocks of `region` (texels) of
    // slice/level, linear rows `rowStride` apart and images `imageStride` apart (0: packed); false outside the level
    template <typename Copy>
    bool eachRow(const MTLRegion &region, uint64_t slice, uint64_t level, uint64_t rowStride, uint64_t imageStride,
                 Copy copy) const
    {
        const uint64_t x = region.origin.x / blockW, y = region.origin.y / blockH, z = region.origin.z;
        const uint64_t length = blocksOf(region.size.width, blockW) * blockBytes,
                       height = blocksOf(region.size.height, blockH), count = region.size.depth;
        if (slice >= slices || level >= levels || x * blockBytes + length > rowBytes(level) ||
            y + height > rows(level) || z + count > images(level))
            return false;
        const uint64_t row = rowBytes(level), image = row * rows(level);
        const uint64_t base = slice * sliceBytes + levelOffset[level] + z * image + y * row + x * blockBytes;
        rowStride = rowStride ? rowStride : length;
        imageStride = imageStride ? imageStride : height * rowStride;
        for (uint64_t i = 0; i < count; i++)
            for (uint64_t r = 0; r < height; r++)
                copy(base + i * image + r * row, i * imageStride + r * rowStride, length);
        return true;
    }
};

struct RMPlainWatermark {
    uint64_t words[2];
};

void refusedCopy(NSUInteger slice, NSUInteger level, const MTLRegion &r)
{
    os_log_error(rawLog(),
                 "raw: texture layout copy outside its texture: slice %lu level %lu region (%lu,%lu,%lu) %lux%lux%lu",
                 (unsigned long)slice, (unsigned long)level, (unsigned long)r.origin.x, (unsigned long)r.origin.y,
                 (unsigned long)r.origin.z, (unsigned long)r.size.width, (unsigned long)r.size.height,
                 (unsigned long)r.size.depth);
}

}  // namespace

@interface RMPlainLayout : RMObject {
@public
    PlainLayout _layout;
}
@end

@implementation RMPlainLayout

+ (Class)twinClass
{
    return NSClassFromString(@"NBTextureLayout");
}

- (NSUInteger)size
{
    return NSUInteger(_layout.sliceBytes * _layout.slices);
}

- (NSUInteger)alignment
{
    return 16;
}

- (MTLSize)copyGranularity
{
    return MTLSizeMake(_layout.blockW, _layout.blockH, 1);
}

- (RMPlainWatermark)watermark
{
    return RMPlainWatermark{};
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
    const uint8_t *from = static_cast<const uint8_t *>(bytes) + offset;
    uint8_t *to = static_cast<uint8_t *>(memory);
    if (!_layout.eachRow(region, slice, level, rowStride, imageStride, [&](uint64_t at, uint64_t linear, uint64_t n) {
            memcpy(to + at, from + linear, n);
        }))
        refusedCopy(slice, level, region);
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
    const uint8_t *from = static_cast<const uint8_t *>(memory);
    uint8_t *to = static_cast<uint8_t *>(bytes) + offset;
    if (!_layout.eachRow(region, slice, level, rowStride, imageStride, [&](uint64_t at, uint64_t linear, uint64_t n) {
            memcpy(to + linear, from + at, n);
        }))
        refusedCopy(slice, level, region);
}

@end

id rmPlainLayout(id device, MTLTextureDescriptor *d)
{
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    if (!f || !f->bpp || d.sampleCount > 1 || d.textureType == MTLTextureType2DMultisample ||
        d.textureType == MTLTextureType2DMultisampleArray || d.textureType == MTLTextureTypeTextureBuffer ||
        !d.mipmapLevelCount || d.mipmapLevelCount > kPlainMaxLevels || !d.width || !d.height || !d.depth ||
        !d.arrayLength)
        return nil;
    RMPlainLayout *made = [RMPlainLayout new];
    made->_device = device;
    PlainLayout &l = made->_layout;
    l.blockBytes = f->bpp;
    l.blockW = f->blockW;
    l.blockH = f->blockH;
    l.width = d.width;
    l.height = d.height;
    l.depth = d.textureType == MTLTextureType3D ? d.depth : 1;
    l.levels = d.mipmapLevelCount;
    const bool cube = d.textureType == MTLTextureTypeCube || d.textureType == MTLTextureTypeCubeArray;
    l.slices = (d.textureType == MTLTextureType3D ? 1 : d.arrayLength) * (cube ? 6 : 1);
    uint64_t offset = 0;
    for (uint64_t level = 0; level < l.levels; level++) {
        l.levelOffset[level] = offset;
        offset += l.rowBytes(level) * l.rows(level) * l.images(level);
    }
    l.sliceBytes = offset;
    return made;
}
