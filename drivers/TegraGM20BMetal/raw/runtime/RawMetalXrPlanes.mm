// SPDX-License-Identifier: MIT
#import "RawMetalXrPlanes.h"

bool xrPlanesShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    return raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::Planes && plane == 0 &&
           IOSurfaceGetPlaneCount(s) == 2 && d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 &&
           d.sampleCount == 1 && !turned && d.usage &&
           !(d.usage & ~(MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget)) &&
           IOSurfaceGetWidthOfPlane(s, 0) == d.width && IOSurfaceGetHeightOfPlane(s, 0) == d.height &&
           IOSurfaceGetWidthOfPlane(s, 1) == d.width && IOSurfaceGetHeightOfPlane(s, 1) == d.height &&
           IOSurfaceGetBytesPerElementOfPlane(s, 0) == 4 && IOSurfaceGetBytesPerElementOfPlane(s, 1) == 1;
}

bool makeXrPlanes(RMTexture *t, IOSurfaceRef s)
{
    void *base = IOSurfaceGetBaseAddress(s);
    const uintptr_t at = uintptr_t(base);
    MTLTextureDescriptor *d = t->_descriptor;
    const uint64_t word = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - at,
                   alpha = uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 1)) - at;
    const uint32_t wordPitch = uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 0)),
                   alphaPitch = uint32_t(IOSurfaceGetBytesPerRowOfPlane(s, 1));
    const char *why = "";
    if (!gRM->raw->createXrPlaneTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, word, wordPitch, alpha,
                                        alphaPitch, uint32_t(d.width), uint32_t(d.height), t->_raw, &why)) {
        SWITCH_REFUSE_DETAIL(
            "raw", "xr.planes", 0,
            "no raw texture over a %lux%lu two-plane surface (planes at %llu and %llu, rows %u and %u, "
            "base&0xfff 0x%lx): %s: m2v's",
            (unsigned long)d.width, (unsigned long)d.height, (unsigned long long)word, (unsigned long long)alpha,
            wordPitch, alphaPitch, (unsigned long)(at & 0xfff), why);
        return false;
    }
    keepExtendedRange(d, t->_raw, false);
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
        const raw::Texture &w = *t->_raw.luma, &a = *t->_raw.chroma;
        os_log(rawLog(),
               "raw: two-plane XR on (%ux%u '%c%c%c%c', drawn in its planes: rows %u and %u, va&127 %llu and %llu, %s "
               "and %s)",
               unsigned(d.width), unsigned(d.height), char(IOSurfaceGetPixelFormat(s) >> 24),
               char(IOSurfaceGetPixelFormat(s) >> 16), char(IOSurfaceGetPixelFormat(s) >> 8),
               char(IOSurfaceGetPixelFormat(s)), w.pitch, a.pitch, (unsigned long long)(w.va & 127),
               (unsigned long long)(a.va & 127), w.renderable ? "direct" : "shadowed",
               a.renderable ? "direct" : "shadowed");
    }
    return true;
}
