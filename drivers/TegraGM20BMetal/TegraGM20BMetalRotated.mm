// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalRotated.h"
#import "TegraGM20BMetalPlainRows.h"
#import "TegraGM20BMetalXrPlanes.h"
#include "format_standin.h"
#include "surface_refusal.h"

NSUInteger descriptorRotation(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    return [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
}

static void reportFirst(const SwitchMetalTexture *t, MTLTextureDescriptor *d)
{
    static std::atomic<bool> said{false};
    if (said.exchange(true))
        return;
    os_log(gm20bLog(),
           "SwitchMetal: renders into a turned plane (first: rotation %u, frame %lux%lu over plane %ux%u, %{public}s)",
           unsigned(t->_raw.rotation), (unsigned long)d.width, (unsigned long)d.height, t->_raw.width, t->_raw.height,
           t->_raw.renderable ? (t->_raw.blockLinear ? "block-linear target" : "pitch target")
                              : "through a block-linear shadow");
}

// An M4 Max's Metal answers such a descriptor with its validation's "a non-zero rotation set but is not marked as
// framebufferOnly", and with framebufferOnly set its driver's "Buffer texture has invalid rotation"
bool refuseTurnedOffSurface(MTLTextureDescriptor *d)
{
    const NSUInteger turn = descriptorRotation(d);
    if (!turn)
        return false;
    SWITCH_REFUSE("gm20b", "tex.turned",
                  "no %lux%lu texture of format %lu and rotation %lu over no surface: nil, as Metal makes none (a "
                  "rotation is a framebuffer-only surface plane's; MTLDevice.h newTextureWithDescriptor:, nullable)",
                  (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)turn);
    return true;
}

// Drawn through stand-ins or store states laid out as their planes, or held in its planes, whatever its format's own
// header
static bool planarFormat(uint32_t format)
{
    return gm20b::isYcbcrFormat(format) || gm20b::packedPlaneChannels(format) || gm20b::twoPlaneXr(format);
}

// Every format Metal renders turns alike, the plane of several of a planar surface as one plane alone (an M4 Max, each
// rotation: m2v-textures4 probes/reads.mm and more.mm landings). A format the GM20B renders none of is drawn turned into its
// stand-in (format_standin.h), a YCbCr one into its stand-in laid out as its planes, which its store states write
// straight; a block-compressed one Metal makes no rotated texture of
bool rotatedShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, gm20b::Rotation &out)
{
    // RGB10A8_2P_XR10 and its sRGB twin turn over planes of any rows as unturned (TegraGM20BMetalXrPlanes.h): rows that overlap,
    // run into the other plane or lie under 16 bytes are held through the planes' pools, each stored by the frame's
    // tiles and quads (an M4 Max: m2v-textures12 probes/f3/RESULT.txt, m2v-textures13 probes/c4misc/RESULT.txt R2-R5)
    return turnedFrame(d, s, plane, out) && (!gm20b::twoPlaneXr(uint32_t(d.pixelFormat)) || xrPlanesStorable(d));
}

// An M4 Max makes a turned texture wherever it makes the unturned one of the plane's extent: over a surface of planes at
// any extent (past the plane's declared width, which IOSurfaceCreate keeps within bytesPerRow, past the surface and its
// allocation), over a surface of none within its rows and its height, its IOGPU aborting otherwise with validation off too
// ("bytesPerRow (256) must be greater or equal to (264) bytes", "Descriptor height*arrayLength > IOSurface height"; the
// turned-extent probe, macOS 27.2.1)
bool turnedFrame(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, gm20b::Rotation &out)
{
    const NSUInteger turn = descriptorRotation(d);
    const uint32_t format = uint32_t(d.pixelFormat);
    const gm20b::FormatInfo *f = gm20b::formatInfo(format);
    const size_t planes = IOSurfaceGetPlaneCount(s);
    if (!turn || turn > 4 || d.textureType != MTLTextureType2D || d.mipmapLevelCount > 1 || d.sampleCount > 1 ||
        d.arrayLength > 1 || d.depth > 1 ||
        (!planarFormat(format) && (!f || !(f->rt || gm20b::formatStandIn(format)) || gm20b::blockCompressed(f))) ||
        (planes > 1 ? plane >= planes : plane != 0))
        return false;
    out.turn = uint8_t(turn);
    out.width = uint32_t(d.width);
    out.height = uint32_t(d.height);
    if (planes)
        return true;
    const bool rows = planarFormat(format) || uint64_t(out.planeWidth()) * f->bpp <= IOSurfaceGetBytesPerRow(s);
    return rows && IOSurfaceGetHeight(s) >= out.planeHeight();
}

MTLTextureDescriptor *planeDescriptor(MTLTextureDescriptor *d, const gm20b::Rotation &r)
{
    MTLTextureDescriptor *flat = [d copy];
    flat.width = r.planeWidth();
    flat.height = r.planeHeight();
    ((void (*)(id, SEL, NSUInteger))objc_msgSend)(flat, sel_registerName("setRotation:"), 0);
    return flat;
}

// A YCbCr texture planned at its plane's extent: its stand-in and store states are laid out as the planes, and a turned
// pass draws into the stand-in turned
static bool createTurnedYcbcr(SwitchMetalTexture *t, MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane,
                              const gm20b::Rotation &r)
{
    gm20b::YcbcrPlan plan;
    if (ycbcrPlan(planeDescriptor(d, r), s, plane, plan))
        return false;
    if (!gRM->raw->createYcbcrTexture(IOSurfaceGetID(s), IOSurfaceGetBaseAddress(s), IOSurfaceGetAllocSize(s), s, plan,
                                      r.planeWidth(), r.planeHeight(), t->_raw)) {
        gm20b::surfaceRefusal = "the import of its planes, or no memory or pack state for its image";
        return false;
    }
    if (plan.target)
        makeYcbcrTarget(t->_raw, uint32_t(d.pixelFormat), plan.store);
    return true;
}

static bool createTurnedXr(SwitchMetalTexture *t, IOSurfaceRef s, NSUInteger plane, const gm20b::Rotation &r)
{
    return xrLastPlane(s, plane) ? makeXrWordPlane(t, s, r.planeWidth(), r.planeHeight())
                                 : makeXrPlanes(t, s, r.planeWidth(), r.planeHeight());
}

bool createRotated(SwitchMetalTexture *t, MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane,
                   const gm20b::Rotation &r)
{
    const size_t planes = IOSurfaceGetPlaneCount(s);
    void *base = IOSurfaceGetBaseAddress(s);
    const uint64_t offset = planes ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, plane)) - uintptr_t(base) : 0;
    const size_t pitch = planes ? IOSurfaceGetBytesPerRowOfPlane(s, plane) : IOSurfaceGetBytesPerRow(s);
    const uint32_t format = uint32_t(d.pixelFormat);
    // a planar surface's planes stay rows (TegraGM20BLayout.h), so only a surface of one plane asks to be laid out tiled
    bool made;
    if (gm20b::twoPlaneXr(format))
        made = createTurnedXr(t, s, plane, r);
    else if (planarFormat(format))
        made = createTurnedYcbcr(t, d, s, plane, r);
    else if (plainRowsShape(planeDescriptor(d, r), s, plane))
        made = makePlainRows(t, s, plane, r.planeWidth(), r.planeHeight());
    else
        made =
            gRM->raw->createSurfaceTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset, r.planeWidth(),
                                           r.planeHeight(), uint32_t(pitch), format, t->_raw, planes <= 1);
    if (!made)
        return false;
    t->_raw.rotation = r.turn;
    reportFirst(t, d);
    return true;
}

gm20b::RotatedRefusal rotatedPassRefusal(const SwitchMetalPass &pass)
{
    gm20b::PassDesc p;
    const auto gm20bOf = [](id x) -> gm20b::Texture * {
        SwitchMetalTexture *t = (SwitchMetalTexture *)x;
        return [x isKindOfClass:[SwitchMetalTexture class]] && t->_gm20bOk ? &t->_raw : nullptr;
    };
    bool any = false;
    for (unsigned c = 0; c < 8; c++) {
        const SwitchMetalPass::Color &a = pass.color[c];
        if (!a.texture)
            continue;
        p.color[c].texture = gm20bOf(a.texture);
        p.color[c].load = a.load == MTLLoadActionLoad    ? gm20b::ColorAttachment::Load
                          : a.load == MTLLoadActionClear ? gm20b::ColorAttachment::Clear
                                                         : gm20b::ColorAttachment::DontCare;
        p.color[c].store = a.store != MTLStoreActionDontCare;
        p.count = c + 1;
        any |= p.color[c].texture && p.color[c].texture->rotation;
    }
    if (!any)
        return gm20b::RotatedRefusal::None;
    p.depthStencil.texture = gm20bOf(pass.zeta[0].texture ?: pass.zeta[1].texture);
    p.depthStencil.loaded = pass.zeta[0].load == MTLLoadActionLoad || pass.zeta[1].load == MTLLoadActionLoad;
    p.depthStencil.stored = (pass.zeta[0].texture && pass.zeta[0].store != MTLStoreActionDontCare) ||
                            (pass.zeta[1].texture && pass.zeta[1].store != MTLStoreActionDontCare);
    return gm20b::turnablePass(p);
}

uint8_t passTurn(const SwitchMetalPass &pass)
{
    for (const SwitchMetalPass::Color &a : pass.color) {
        SwitchMetalTexture *t = (SwitchMetalTexture *)a.texture;
        if ([a.texture isKindOfClass:[SwitchMetalTexture class]] && t->_gm20bOk && t->_raw.rotation)
            return t->_raw.rotation;
    }
    return 0;
}
