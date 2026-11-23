// SPDX-License-Identifier: MIT
// replaceRegion: and getBytes: of a raw texture: its memory reached by the CPU in place, through the copy engine, or nothing
// moved with one line
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalDecoded.h"
#import "TegraGM20BMetalFresh.h"
#import "TegraGM20BMetalMultisample.h"
#import "TegraGM20BMetalXrPlanes.h"
#include "texture_cpu_range.h"
#include "ycbcr.h"
#include <mach/vm_page_size.h>

namespace {
// A region of `t`'s texels in its elements: a block-compressed texture's blocks (Metal's region of one is whole blocks but at
// its edges), any other's texels
MTLRegion elementRegion(const gm20b::Texture &t, MTLRegion r)
{
    const NSUInteger bw = std::max<NSUInteger>(t.blockW, 1), bh = std::max<NSUInteger>(t.blockH, 1);
    if (bw == 1 && bh == 1)
        return r;
    MTLRegion e = r;
    e.origin.x = r.origin.x / bw;
    e.origin.y = r.origin.y / bh;
    e.size.width = (r.origin.x + r.size.width + bw - 1) / bw - e.origin.x;
    e.size.height = (r.origin.y + r.size.height + bh - 1) / bh - e.origin.y;
    return e;
}

class TextureCpuAccess {
public:
    TextureCpuAccess(SwitchMetalTexture *texture, MTLRegion region, NSUInteger level, NSUInteger slice, bool write)
    {
        if (!texture->_gm20bOk || !texture->_raw.cpu)
            return;
        if (texture->_cpuRows) {
            const uint64_t bpp = texture->_raw.bpp;
            const uint8_t *cpu = texture->_cpuRows + region.origin.y * texture->_cpuPitch + region.origin.x * bpp;
            const uint64_t bytes = (region.size.height - 1) * texture->_cpuPitch + region.size.width * bpp;
            if (!gRM->raw->hostAccess(texture->_raw, cpu, bytes, write, access_))
                [NSException raise:NSInternalInconsistencyException format:@"GPU texture CPU ownership failed"];
            native_ = true;
            return;
        }
        const auto &image = texture->_raw;
        if ((image.blockW > 1 || image.blockH > 1) && image.blockLinear)
            return;
        const MTLRegion elements = elementRegion(image, region);
        const gm20b::TextureCpuShape shape = {image.width,
                                              image.height,
                                              image.depth,
                                              image.layers,
                                              image.pitch,
                                              image.surfaceX,
                                              image.surfaceY,
                                              image.bpp,
                                              image.blockDepth,
                                              image.blockLinear,
                                              image.type == gm20b::fx::kTexThreeD,
                                              image.chain ? &image.chain->layout : nullptr,
                                              textureCpuImportLayout(texture),
                                              image.layerBytes};
        const uint64_t origin[] = {elements.origin.x, elements.origin.y, elements.origin.z};
        const uint64_t size[] = {elements.size.width, elements.size.height, elements.size.depth};
        gm20b::TextureCpuRange range;
        const auto planned = gm20b::textureCpuRange(shape, origin, size, level, slice, range);
        if (planned == gm20b::TextureCpuRegion::Unsupported)
            return;
        if (planned != gm20b::TextureCpuRegion::Native || range.offset > UINTPTR_MAX - uintptr_t(image.cpu))
            [NSException raise:NSInvalidArgumentException format:@"GPU texture CPU region is outside its image"];
        const void *cpu = reinterpret_cast<const void *>(uintptr_t(image.cpu) + range.offset);
        while (texture->_kind == SwitchMetalTextureView && texture->_parent)
            texture = texture->_parent;
        const bool acquired = texture->_buffer
                                  ? gRM->raw->hostAccess(texture->_buffer->_raw, cpu, range.bytes, write, access_)
                                  : gRM->raw->hostAccess(texture->_raw, cpu, range.bytes, write, access_);
        if (!acquired)
            [NSException raise:NSInternalInconsistencyException format:@"GPU texture CPU ownership failed"];
        native_ = true;
    }

    bool native() const
    {
        return native_;
    }

private:
    gm20b::HostAccess access_;
    bool native_ = false;
};

// A plane at Metal's GPU layout whose CPU rows are its own (SwitchMetalTexture _cpuRows): one level, one slice
void planeRows(SwitchMetalTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes, NSUInteger row,
               bool write)
{
    if (level || slice || r.origin.z || r.size.depth != 1 || r.origin.x + r.size.width > t->_raw.width ||
        r.origin.y + r.size.height > t->_raw.height)
        return;
    const size_t line = r.size.width * t->_raw.bpp;
    // an M4 Max's CPU copy moves no row past the surface's last page (the plain-rows probe, macOS 27.2.1)
    if (IOSurfaceRef s = t->_surface) {
        const uint8_t *end = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(s)) +
                             ((IOSurfaceGetAllocSize(s) + vm_page_size - 1) & ~size_t(vm_page_size - 1));
        while (r.size.height &&
               t->_cpuRows + (r.origin.y + r.size.height - 1) * t->_cpuPitch + r.origin.x * t->_raw.bpp + line > end)
            r.size.height--;
        if (!r.size.height)
            return;
    }
    TextureCpuAccess access(t, r, level, slice, write);
    for (NSUInteger y = 0; y < r.size.height; y++) {
        uint8_t *at = t->_cpuRows + (r.origin.y + y) * t->_cpuPitch + r.origin.x * t->_raw.bpp;
        if (write)
            memcpy(at, bytes + y * row, line);
        else
            memcpy(bytes + y * row, at, line);
    }
}

// replaceRegion/getBytes of a two-plane YCbCr texture over a surface: its luma codes alone, a code a texel at its plane's
// own offset and rows (ycbcr.h ycbcrCpuLuma), nothing of a packed 10-bit one. False: not such a texture
bool ycbcrLumaRows(SwitchMetalTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice, uint8_t *bytes,
                   NSUInteger row, bool write)
{
    uint32_t unit = 0;
    const gm20b::Texture *luma = t->_gm20bOk && t->_surface ? t->_raw.luma : nullptr;
    if (!luma || !gm20b::ycbcrCpuLuma(uint32_t(t->_descriptor.pixelFormat), unit))
        return false;
    if (!unit || level || slice || r.origin.z || r.size.depth != 1 || r.origin.x + r.size.width > t->_raw.width ||
        r.origin.y + r.size.height > t->_raw.height)
        return true;
    const size_t line = r.size.width * unit;
    if (row && row < line) {
        // an M4 Max's driver asserts "bytes_per_row >= used_bytes_per_row" and moves nothing (probes/c4misc P4)
        SWITCH_REFUSE_DETAIL(
            "gm20b", "texture.cpu.unreached", row,
            "%s of %lux%lu of a format %lu texture with bytesPerRow %lu under its %zu-byte row: nothing "
            "moved (Metal's validation stops it)",
            write ? "replaceRegion" : "getBytes", (unsigned long)r.size.width, (unsigned long)r.size.height,
            (unsigned long)t->_descriptor.pixelFormat, (unsigned long)row, line);
        return true;
    }
    IOSurfaceRef s = t->_surface;
    const bool planes = IOSurfaceGetPlaneCount(s) > 0;
    uint8_t *plane =
        static_cast<uint8_t *>(planes ? IOSurfaceGetBaseAddressOfPlane(s, t->_plane) : IOSurfaceGetBaseAddress(s));
    const size_t pitch = planes ? IOSurfaceGetBytesPerRowOfPlane(s, t->_plane) : IOSurfaceGetBytesPerRow(s);
    uint8_t *first = plane + r.origin.y * pitch + r.origin.x * unit;
    gm20b::HostAccess access;
    if (!gRM->raw->hostAccess(*luma, first, (r.size.height - 1) * pitch + line, write, access))
        [NSException raise:NSInternalInconsistencyException format:@"GPU texture CPU ownership failed"];
    for (NSUInteger y = 0; y < r.size.height; y++) {
        uint8_t *codes = first + y * pitch;
        if (write)
            memcpy(codes, bytes + y * row, line);
        else
            memcpy(bytes + y * row, codes, line);
    }
    return true;
}

// A memoryless texture holds nothing between passes: Apple's driver leaves getBytes' bytes as they were and drops
// replaceRegion's (an M-series Mac with API validation off), where its twin would open m2v
bool cpuReachesNothing(SwitchMetalTexture *t)
{
    if (t->_descriptor.storageMode != MTLStorageModeMemoryless)
        return false;
    SWITCH_REFUSE("gm20b", "texture.cpu.memoryless",
                  "a CPU copy of a memoryless %lux%lu texture: nothing moved, as Metal",
                  (unsigned long)t->_descriptor.width, (unsigned long)t->_descriptor.height);
    return true;
}

// A texture made plain for want of compressed memory, ahead of its CPU copy: a want still there is withdrawn on the submit
// worker between its submissions, where no move of the texture is under way (private_comp.h compressedFromPlain), and it
// stays plain for life, as one with a raw view does. A move that came first wrote the texture's memory fields under the
// device lock, which wantsCompressed takes, or on the worker this waits for: the copy reads them as they then stay.
void settleMovable(SwitchMetalTexture *t)
{
    if (!t->_movableBytes || !gRM->raw->wantsCompressed(t->_raw))
        return;
    gm20bExcludingSubmissions([t] {
        gRM->raw->keepPlain(t->_raw);
        return true;
    });
}

// A raw texture's CPU copy no path took: the copy engine's transfer of one with no CPU mapping (compressed memory) failed,
// or the level, slice or depth asked is not the texture's, which Metal's validation stops (MTLTexture.h replaceRegion:,
// getBytes:: a region inside the level)
void cpuUnreached(SwitchMetalTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice)
{
    SWITCH_REFUSE_DETAIL("gm20b", "texture.cpu.unreached", t->_descriptor.storageMode,
                         "a CPU copy of %lux%lux%lu at level %lu slice %lu of a %lux%lu format %lu texture of storage "
                         "mode %lu: nothing moved (its transfer failed, or no such part of it)",
                         (unsigned long)r.size.width, (unsigned long)r.size.height, (unsigned long)r.size.depth,
                         (unsigned long)level, (unsigned long)slice, (unsigned long)t->_descriptor.width,
                         (unsigned long)t->_descriptor.height, (unsigned long)t->_descriptor.pixelFormat,
                         (unsigned long)t->_descriptor.storageMode);
}

// getBytes and replaceRegion of R10Unorm_PACKED or RG10Unorm_PACKED over a surface plane move nothing, at every level
// (an M4 Max, validation on and off: m2v-textures11 probes/h4/mips.txt, m2v-textures13 probes/pmips/pmips.txt)
bool packedPlaneMovesNothing(SwitchMetalTexture *t)
{
    return t->_gm20bOk && t->_raw.ycbcr == gm20b::YcbcrPackedPlane;
}

// The texture a view's CPU copy is made on, at the view's first level and slice on: its levels owner, of the same texels
// (a view's format is of its texture's texel size), which the owner's own CPU paths reach. Not a depth/stencil view's:
// X32_Stencil8 names one aspect of its texture's texels.
SwitchMetalTexture *cpuOwner(SwitchMetalTexture *t)
{
    SwitchMetalTexture *o = t->_levelsOwner;
    return t->_kind == SwitchMetalTextureView && t->_gm20bOk && !t->_raw.zeta && o && o != t && o->_gm20bOk ? o : nil;
}
}  // namespace

@implementation SwitchMetalTexture (CpuCopies)

- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
        bytesPerImage:(NSUInteger)image
{
    if (!cpuXrWords(self, r, level, slice, (uint8_t *)bytes, row, image, true))
        [self gm20bReplaceRegion:r mipmapLevel:level slice:slice withBytes:bytes bytesPerRow:row bytesPerImage:image];
}

- (void)gm20bReplaceRegion:(MTLRegion)r
               mipmapLevel:(NSUInteger)level
                     slice:(NSUInteger)slice
                 withBytes:(const void *)bytes
               bytesPerRow:(NSUInteger)row
             bytesPerImage:(NSUInteger)image
{
    if (!r.size.width || !r.size.height || !r.size.depth || cpuReachesNothing(self))
        return;
    settleMovable(self);
    if (SwitchMetalTexture *owner = cpuOwner(self)) {
        [(id<MTLTexture>)owner replaceRegion:r
                                 mipmapLevel:_levelsBase + level
                                       slice:_slicesBase + slice
                                   withBytes:bytes
                                 bytesPerRow:row
                               bytesPerImage:image];
        return;
    }
    if (gm20bDecodedReplace(self, r, level, slice, bytes, row, image))
        return;
#if SWITCH_METAL_DIAGNOSTICS
    _raw.probeWritten = true;
#endif
    if (_surfaceWrites)
        noteSurfaceWrite(_surfaceWrites);
    freshBeforeTransfer(self);
    if (xrPlaneWords(self, r, level, slice, (uint8_t *)bytes, row, true))
        return;
    if (ycbcrLumaRows(self, r, level, slice, (uint8_t *)bytes, row, true))
        return;
    if (_cpuRows) {
        planeRows(self, r, level, slice, (uint8_t *)bytes, row, true);
        return;
    }
    if (transferTexture(self, r, level, slice, const_cast<void *>(bytes), row, image, true) ||
        refuseZetaCpu(self, r, level, slice, true))
        return;
    if (_gm20bOk && _raw.blockW * _raw.blockH > 1 && _raw.blockLinear) {
        cpuBlocks(self, r, level, slice, (uint8_t *)bytes, row, image, true);
        return;
    }
    if (_gm20bOk && _raw.samples > 1 && !_raw.zeta) {
        cpuSamples(self, r, (uint8_t *)bytes, row, true);
        return;
    }
    TextureCpuAccess access(self, r, level, slice, true);
    if (!access.native()) {
        cpuUnreached(self, r, level, slice);
        return;
    }
    if (packedPlaneMovesNothing(self))
        return;
    if (_gm20bOk && _raw.chain) {
        cpuLevel(self, r, level, slice, (uint8_t *)bytes, row, image, true);
        return;
    }
    if (_gm20bOk && _raw.type == gm20b::fx::kTexThreeD && !level && !slice && r.origin.x + r.size.width <= _raw.width &&
        r.origin.y + r.size.height <= _raw.height && r.origin.z + r.size.depth <= _raw.depth) {
        cpuVolume(self, r, (uint8_t *)bytes, row, image, true);
        return;
    }
    if (_gm20bOk && _raw.layers > 1 && !level && slice < _raw.layers && r.size.depth <= 1 &&
        r.origin.x + r.size.width <= _raw.width && r.origin.y + r.size.height <= _raw.height) {
        cpuLayer(self, r, slice, (uint8_t *)bytes, row, true);
        return;
    }
    const MTLRegion e = elementRegion(_raw, r);
    if (e.origin.x + e.size.width > _raw.width || e.origin.y + e.size.height > _raw.height)
        return;
    if (_raw.blockLinear) {
        cpuTiled(self, r, (uint8_t *)bytes, row, true);
        return;
    }
    const size_t line = e.size.width * _raw.bpp;
    for (NSUInteger y = 0; y < e.size.height; y++) {
        uint8_t *to = _raw.cpu + (e.origin.y + y) * _raw.pitch + e.origin.x * _raw.bpp;
        memcpy(to, (const uint8_t *)bytes + y * row, line);
    }
}

- (void)replaceRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)row
{
    [self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:0];
}

- (void)getBytes:(void *)bytes
      bytesPerRow:(NSUInteger)row
    bytesPerImage:(NSUInteger)image
       fromRegion:(MTLRegion)r
      mipmapLevel:(NSUInteger)level
            slice:(NSUInteger)slice
{
    if (!cpuXrWords(self, r, level, slice, (uint8_t *)bytes, row, image, false))
        [self gm20bGetBytes:bytes bytesPerRow:row bytesPerImage:image fromRegion:r mipmapLevel:level slice:slice];
}

- (void)gm20bGetBytes:(void *)bytes
          bytesPerRow:(NSUInteger)row
        bytesPerImage:(NSUInteger)image
           fromRegion:(MTLRegion)r
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
{
    if (!r.size.width || !r.size.height || !r.size.depth || cpuReachesNothing(self))
        return;
    settleMovable(self);
    if (SwitchMetalTexture *owner = cpuOwner(self)) {
        [(id<MTLTexture>)owner getBytes:bytes
                            bytesPerRow:row
                          bytesPerImage:image
                             fromRegion:r
                            mipmapLevel:_levelsBase + level
                                  slice:_slicesBase + slice];
        return;
    }
    if (packedPlaneMovesNothing(self) || gm20bDecodedGetBytes(self, r, level, slice, bytes, row, image))
        return;
    freshBeforeTransfer(self);
    if (xrPlaneWords(self, r, level, slice, (uint8_t *)bytes, row, false))
        return;
    if (ycbcrLumaRows(self, r, level, slice, (uint8_t *)bytes, row, false))
        return;
    if (_cpuRows) {
        planeRows(self, r, level, slice, (uint8_t *)bytes, row, false);
        return;
    }
    if (transferTexture(self, r, level, slice, bytes, row, image, false) || refuseZetaCpu(self, r, level, slice, false))
        return;
    if (_gm20bOk && _raw.blockW * _raw.blockH > 1 && _raw.blockLinear) {
        cpuBlocks(self, r, level, slice, (uint8_t *)bytes, row, image, false);
        return;
    }
    if (_gm20bOk && _raw.samples > 1 && !_raw.zeta) {
        cpuSamples(self, r, (uint8_t *)bytes, row, false);
        return;
    }
    TextureCpuAccess access(self, r, level, slice, false);
    if (!access.native()) {
        cpuUnreached(self, r, level, slice);
        return;
    }
    if (_gm20bOk && _raw.chain) {
        cpuLevel(self, r, level, slice, (uint8_t *)bytes, row, image, false);
        return;
    }
    if (_gm20bOk && _raw.type == gm20b::fx::kTexThreeD && !level && !slice && r.origin.x + r.size.width <= _raw.width &&
        r.origin.y + r.size.height <= _raw.height && r.origin.z + r.size.depth <= _raw.depth) {
        cpuVolume(self, r, (uint8_t *)bytes, row, image, false);
        return;
    }
    if (_gm20bOk && _raw.layers > 1 && !level && slice < _raw.layers && r.size.depth <= 1 &&
        r.origin.x + r.size.width <= _raw.width && r.origin.y + r.size.height <= _raw.height) {
        cpuLayer(self, r, slice, (uint8_t *)bytes, row, false);
        return;
    }
    const MTLRegion e = elementRegion(_raw, r);
    if (e.origin.x + e.size.width > _raw.width || e.origin.y + e.size.height > _raw.height)
        return;
    if (_raw.blockLinear) {
        cpuTiled(self, r, (uint8_t *)bytes, row, false);
        return;
    }
    const size_t line = e.size.width * _raw.bpp;
    for (NSUInteger y = 0; y < e.size.height; y++)
        memcpy((uint8_t *)bytes + y * row, _raw.cpu + (e.origin.y + y) * _raw.pitch + e.origin.x * _raw.bpp, line);
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
{
    [self getBytes:bytes bytesPerRow:row bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
}

@end
