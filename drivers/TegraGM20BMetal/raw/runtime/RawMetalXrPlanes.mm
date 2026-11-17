// SPDX-License-Identifier: MIT
#import "RawMetalXrPlanes.h"

bool xrPlanesShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    const NSUInteger turned =
        [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
    // a draw stores into such a texture through the pack's merge and split states (mergeXrPlanes)
    const bool unstorable = (d.usage & MTLTextureUsageShaderWrite) &&
                            !(gRM->raw->xrPlanesImagePipeline(false) && gRM->raw->xrPlanesImagePipeline(true));
    return raw::xrForm(uint32_t(d.pixelFormat)) == raw::XrForm::Planes && plane == 0 && !unstorable &&
           IOSurfaceGetPlaneCount(s) == 2 && d.textureType == MTLTextureType2D && d.mipmapLevelCount == 1 &&
           d.sampleCount == 1 && !turned && d.usage &&
           !(d.usage & ~(MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget)) &&
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

static int planesPass(raw::CommandBuffer *rc, const raw::PassDesc &p, raw::Pipeline *state, const raw::Texture &read0,
                      const raw::Texture *read1, uint32_t width, uint32_t height, const char **why)
{
    raw::RenderEncoder *e = rc->render(p);
    if (!e) {
        *why = "a two-plane RGB10A8_2P_XR10 texture's image or planes the raw path cannot render";
        return 1;
    }
    e->setPipeline(state);
    e->setTexture(0, &read0);
    if (read1)
        e->setTexture(1, read1);
    e->setViewport(0, 0, width, height, 0, 1);
    e->setScissor(0, 0, width, height);
    if (!e->draw(4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4, 1, 0, why))
        return 2;
    e->end();
    return 0;
}

int mergeXrPlanes(raw::CommandBuffer *rc, const raw::Texture &t, uint32_t k, raw::Texture *&image, const char **why)
{
    constexpr uint32_t kRG32Uint = 103;
    image = rc->turnScratch(kRG32Uint, t.width, t.height, kXrPlanesImageTag + k);
    raw::Pipeline *merge = gRM->raw->xrPlanesImagePipeline(false);
    if (!image || !merge) {
        *why = !image ? "no image for a written two-plane RGB10A8_2P_XR10 texture"
                      : "the pack has no state merging a two-plane RGB10A8_2P_XR10 texture's planes";
        return 1;
    }
    raw::PassDesc p;
    p.color[0].texture = image;
    p.color[0].load = raw::ColorAttachment::DontCare;
    p.color[0].store = true;
    p.count = 1;
    return planesPass(rc, p, merge, *t.luma, t.chroma, t.width, t.height, why);
}

int splitXrPlanes(raw::CommandBuffer *rc, const raw::Texture &t, const raw::Texture &image, const char **why)
{
    raw::Pipeline *split = gRM->raw->xrPlanesImagePipeline(true);
    if (!split) {
        *why = "the pack has no state splitting a two-plane RGB10A8_2P_XR10 texture's image";
        return 1;
    }
    raw::PassDesc p;
    p.color[0].texture = t.luma;
    p.color[1].texture = t.chroma;
    for (raw::ColorAttachment &a : p.color) {
        a.load = raw::ColorAttachment::DontCare;
        a.store = true;
    }
    p.count = 2;
    return planesPass(rc, p, split, image, nullptr, t.width, t.height, why);
}
