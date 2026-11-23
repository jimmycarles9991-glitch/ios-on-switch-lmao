// SPDX-License-Identifier: MIT
// A texture over a surface the driver does not make: Metal's, the identity's or GM20B's nil, or the step that failed, with
// one line
#import "TegraGM20BMetalSurfaceRefusals.h"
#import "TegraGM20BMetalRotated.h"
#import "TegraGM20BMetalXrPlanes.h"
#include "surface_refusal.h"
#include "texture_shape.h"
#include "ycbcr.h"
#include "zeta.h"

void planeAt(IOSurfaceRef s, NSUInteger plane, uint64_t &offset, size_t &pitch)
{
    const bool planes = IOSurfaceGetPlaneCount(s) > 0;
    offset = planes ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, plane)) - uintptr_t(IOSurfaceGetBaseAddress(s)) : 0;
    pitch = planes ? IOSurfaceGetBytesPerRowOfPlane(s, plane) : IOSurfaceGetBytesPerRow(s);
}

namespace {

NSString *surfaceFacts(IOSurfaceRef s, NSUInteger plane)
{
    uint64_t offset;
    size_t pitch;
    planeAt(s, plane, offset, pitch);
    const OSType fourcc = IOSurfaceGetPixelFormat(s);
    return
        [NSString stringWithFormat:@"surface %u %ux%u '%c%c%c%c' bytesPerRow %lu planes %lu plane %lu alloc %lu "
                                   @"base&0xfff 0x%lx",
                                   IOSurfaceGetID(s), unsigned(IOSurfaceGetWidth(s)), unsigned(IOSurfaceGetHeight(s)),
                                   char(fourcc >> 24), char(fourcc >> 16), char(fourcc >> 8), char(fourcc),
                                   (unsigned long)pitch, (unsigned long)IOSurfaceGetPlaneCount(s), (unsigned long)plane,
                                   (unsigned long)IOSurfaceGetAllocSize(s),
                                   (unsigned long)(uintptr_t(IOSurfaceGetBaseAddress(s)) & 0xfff)];
}

// Metal's stop or nil for a texture over a surface, null where Metal makes one: an IOSurface texture is 2D (or a texture
// buffer) of one level, sample and layer, of a rotation Metal names and no depth, stencil or undescribed format (an M4
// Max's validation: "IOSurface texture: must be of type MTLTextureType2D", "cannot be mipmapped", "cannot create
// compressed, depth, or stencil textures", "rotation is not a valid MTLTextureRotation"). RGB10A8_2P_XR10 and its sRGB twin
// pass the level rule, and their mipmapped texture reads none of the surface's codes (probes/xrsurf.mm): no texture either.
// R10Unorm_PACKED and RG10Unorm_PACKED pass it too, and an M4 Max makes them of 2 and 3 levels, every lod reading level 0
// (m2v-textures11 probes/h4/mips.txt).
const char *surfaceMetalRefusal(MTLTextureDescriptor *d)
{
    const uint32_t format = uint32_t(d.pixelFormat);
    if (!d.width || !d.height)
        return "a texture of no width or height (its descriptor's validation stops it)";
    if (d.textureType != MTLTextureType2D && d.textureType != MTLTextureTypeTextureBuffer)
        return "an IOSurface texture is a 2D texture or a texture buffer";
    const uint32_t chain = 32u - uint32_t(__builtin_clz(uint32_t(std::max(d.width, d.height))));
    const bool levels = gm20b::packedPlaneChannels(format) ? d.mipmapLevelCount > chain : d.mipmapLevelCount > 1;
    if (levels || d.sampleCount > 1 || d.arrayLength > 1 || d.depth > 1)
        return "an IOSurface texture is of one level, sample, layer and plane";
    if (descriptorRotation(d) > 4)
        return "a rotation that is no MTLTextureRotation";
    if (const char *why = gm20b::shapes::formatRefusal(format))
        return why;
    if (gm20b::zetaFormatIndex(format) < gm20b::kZetaFormats || format == 255 || format == 261 || format == 262)
        return "an IOSurface texture is of no depth or stencil format";
    // an M4 Max makes none of any usage (m2v-textures4 probes/forms.mm, more.mm); a rotated texture is a framebuffer's
    if (gm20b::blockCompressed(gm20b::formatInfo(format)) &&
        (descriptorRotation(d) || (d.usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite))))
        return "a block-compressed IOSurface texture is never rendered into or written";
    return nullptr;
}

}  // namespace

// A texture buffer over a surface of no planes longer than its rows, which an M4 Max's Metal asserts on with validation off
// too ("bytesPerRow (320) must be greater or equal to (324) bytes"), counting 2 bytes a texel of RGB10A8_2P_XR10 and its
// sRGB twin and every other format's own (R8, BGRA8, BGR10_XR, RGBA16Float, BGRA10_XR, RGBA32Float: the texel-buffer row
// probe, macOS 27.2.1); over a plane it makes one of any length
const char *texelBufferRowsRefusal(MTLTextureDescriptor *d, IOSurfaceRef s)
{
    const uint32_t format = uint32_t(d.pixelFormat);
    const gm20b::FormatInfo *f = gm20b::formatInfo(format);
    if (d.textureType != MTLTextureTypeTextureBuffer || IOSurfaceGetPlaneCount(s) || !f ||
        gm20b::isYcbcrFormat(format) || gm20b::packedPlaneChannels(format))
        return nullptr;
    const uint64_t bytes = uint64_t(d.width) * (gm20b::twoPlaneXr(format) ? 2 : f->bpp);
    return bytes > IOSurfaceGetBytesPerRow(s) ? "an IOSurface texture buffer is no longer than its surface's rows"
                                              : nullptr;
}

// No texture over `s`: one line naming whose answer the nil is, or the step that failed
id<MTLTexture> unmadeSurfaceTexture(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    NSString *extra = surfaceFacts(s, plane);
    const char *shape = extra.UTF8String;
    const uint32_t format = uint32_t(d.pixelFormat);
    gm20b::shapes::TextureShape texture;
    texture.format = format;
    if (const char *why = gm20b::surfaceRefusal ? nullptr : surfaceMetalRefusal(d) ?: texelBufferRowsRefusal(d, s)) {
        SWITCH_REFUSE_DETAIL("gm20b", "tex.metal", uintptr_t(why),
                             "no %lux%lu texture of format %lu, type %lu over IOSurface %u: nil, as Metal makes none "
                             "(%s; MTLDevice.h newTextureWithDescriptor:iosurface:plane:, nullable): %s",
                             (unsigned long)d.width, (unsigned long)d.height, (unsigned long)format,
                             (unsigned long)d.textureType, IOSurfaceGetID(s), why, shape);
        return nil;
    }
    const char *identity = gm20b::shapes::surfaceFormatRefusal(format);
    if (!identity)
        identity = gm20b::shapes::identityRefusal(texture, gm20b::answers::kThisKind);
    if (identity) {
        SWITCH_REFUSE_DETAIL("gm20b", "tex.identity", uintptr_t(identity),
                             "no texture of format %lu over IOSurface %u: nil, as the identity's Metal makes none (%s; "
                             "MTLDevice.h newTextureWithDescriptor:iosurface:plane:, nullable): %s",
                             (unsigned long)format, IOSurfaceGetID(s), identity, shape);
        return nil;
    }
    if (const char *decoded = gm20b::shapes::decodedSurfaceRefusal(format)) {
        SWITCH_REFUSE_DETAIL("gm20b", "tex.decoded", uintptr_t(decoded),
                             "no texture of format %lu over IOSurface %u: nil (%s; MTLDevice.h "
                             "newTextureWithDescriptor:iosurface:plane:, nullable): %s",
                             (unsigned long)format, IOSurfaceGetID(s), decoded, shape);
        return nil;
    }
    if (const char *gm20b = gm20b::packedPlaneChannels(format) ? nullptr : gm20b::shapes::hardwareRefusal(texture)) {
        SWITCH_REFUSE_DETAIL("gm20b", "tex.gm20b", uintptr_t(gm20b),
                             "no texture of format %lu over IOSurface %u: nil (%s; MTLDevice.h "
                             "newTextureWithDescriptor:iosurface:plane:, nullable): %s",
                             (unsigned long)format, IOSurfaceGetID(s), gm20b, shape);
        return nil;
    }
    gm20b::YcbcrPlan unused;
    gm20b::Rotation turned;
    if (descriptorRotation(d) && !turnedFrame(d, s, plane, turned)) {
        SWITCH_REFUSE_DETAIL(
            "gm20b", "tex.metal", uintptr_t(descriptorRotation(d)),
            "no %lux%lu texture of format %lu and rotation %lu over IOSurface %u: nil, as Metal makes "
            "none (a turned frame past the rows or the height of a surface of no planes, of a type or a "
            "format no framebuffer is): %s",
            (unsigned long)d.width, (unsigned long)d.height, (unsigned long)format,
            (unsigned long)descriptorRotation(d), IOSurfaceGetID(s), shape);
        return nil;
    }
    // a turned one of a planar format is planned and held at its plane's extent (TegraGM20BMetalRotated.mm)
    MTLTextureDescriptor *flat = descriptorRotation(d) ? planeDescriptor(d, turned) : d;
    const bool xr = gm20b::twoPlaneXr(format);
    const bool xrTaken =
        xr && (gm20b::surfaceRefusal || xrPlanesShape(flat, s, plane) || xrWordPlaneShape(flat, s, plane));
    const bool hasPlane = plane < std::max<size_t>(IOSurfaceGetPlaneCount(s), 1);
    if ((gm20b::isYcbcrFormat(format) || gm20b::packedPlaneChannels(format)) && !gm20b::surfaceRefusal && hasPlane &&
        ycbcrPlan(flat, s, plane, unused) == gm20b::kRowsPastWithoutSparse)
        gm20b::surfaceRefusal = gm20b::kRowsPastWithoutSparse;
    if (xr && !xrTaken && !xrPlanesStorable(d)) {
        SWITCH_REFUSE_DETAIL(
            "gm20b", "tex.surf.pack", uintptr_t(d.usage),
            "no %lux%lu texture of format %lu and usage 0x%lx over IOSurface %u: nil: the pack holds no "
            "state merging and splitting RGB10A8_2P_XR10 planes, which a fragment program may store to "
            "(a pack made without them): %s",
            (unsigned long)d.width, (unsigned long)d.height, (unsigned long)format, (unsigned long)d.usage,
            IOSurfaceGetID(s), shape);
        return nil;
    }
    uint64_t offset;
    size_t pitch;
    planeAt(s, plane, offset, pitch);
    const size_t planes = IOSurfaceGetPlaneCount(s);
    const gm20b::FormatInfo *f = gm20b::formatInfo(format);
    const uint64_t packed = f ? (uint64_t(flat.width) + f->blockW - 1) / f->blockW * f->bpp : 0;
    const uint64_t rows = f ? (uint64_t(flat.height) + f->blockH - 1) / f->blockH : flat.height;
    const bool pastAllocation = uint64_t(pitch) * rows > IOSurfaceGetAllocSize(s);
    if (plane >= std::max<size_t>(planes, 1)) {
        os_log_error(gm20bLog(),
                     "SwitchMetal: %{public}s: no texture over IOSurface %u: it has no plane %lu: %{public}@",
                     getprogname(), IOSurfaceGetID(s), (unsigned long)plane, extra);
    } else if (!xrTaken && !planes && ((pitch & 15) || pitch < packed)) {
        // an M4 Max asserts with validation off too: "bytesPerRow (256) must be greater or equal to (264) bytes" (the
        // turned-extent probe, macOS 27.2.1)
        SWITCH_REFUSE_DETAIL("gm20b", "tex.metal", uintptr_t(pitch),
                             "no %lux%lu texture of format %lu over IOSurface %u: nil, as Metal makes none (rows of a "
                             "surface of no planes off 16 bytes or under the texture's row): %s",
                             (unsigned long)flat.width, (unsigned long)flat.height, (unsigned long)format,
                             IOSurfaceGetID(s), shape);
    } else if (!xrTaken && !planes && pastAllocation) {
        os_log_error(
            gm20bLog(),
            "SwitchMetal: %{public}s: no texture over IOSurface %u: %lu rows of %zu bytes past its %zu: %{public}@",
            getprogname(), IOSurfaceGetID(s), (unsigned long)flat.height, pitch, IOSurfaceGetAllocSize(s), extra);
    } else {
        SWITCH_REFUSE_DETAIL("gm20b", "tex.surf.imp", 0,
                             "no texture over IOSurface %u: %s (the connection's last error: %s): nil (MTLDevice.h "
                             "newTextureWithDescriptor:iosurface:plane:, nullable): %s",
                             IOSurfaceGetID(s), gm20b::surfaceRefusal ?: "a step that names none",
                             gRM->raw->kernel().error().c_str(), shape);
    }
    return nil;
}
