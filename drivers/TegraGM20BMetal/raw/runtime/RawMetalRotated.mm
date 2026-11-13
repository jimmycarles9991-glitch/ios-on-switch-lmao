// SPDX-License-Identifier: MIT
#import "RawMetalRotated.h"

static NSUInteger descriptorRotation(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    return [d respondsToSelector:rotation] ? ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation) : 0;
}

static void reportFirst(const RMTexture *t, MTLTextureDescriptor *d)
{
    static std::atomic<bool> said{false};
    if (said.exchange(true))
        return;
    os_log(rawLog(),
           "raw: renders into a turned plane (first: rotation %u, frame %lux%lu over plane %ux%u, %{public}s)",
           unsigned(t->_raw.rotation), (unsigned long)d.width, (unsigned long)d.height, t->_raw.width, t->_raw.height,
           t->_raw.renderable ? (t->_raw.blockLinear ? "block-linear target" : "pitch target")
                              : "through a block-linear shadow");
}

bool rotatedShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, raw::Rotation &out)
{
    const NSUInteger turn = descriptorRotation(d);
    const raw::FormatInfo *f = raw::formatInfo(uint32_t(d.pixelFormat));
    if (!turn || turn > 4 || d.textureType != MTLTextureType2D || d.mipmapLevelCount > 1 || d.sampleCount > 1 ||
        d.arrayLength > 1 || d.depth > 1 || !f || !f->rt || raw::blockCompressed(f) ||
        raw::xrForm(uint32_t(d.pixelFormat)) != raw::XrForm::None || plane || IOSurfaceGetPlaneCount(s) > 1)
        return false;
    out.turn = uint8_t(turn);
    out.width = uint32_t(d.width);
    out.height = uint32_t(d.height);
    const bool one = IOSurfaceGetPlaneCount(s) == 1;
    return (one ? IOSurfaceGetWidthOfPlane(s, 0) : IOSurfaceGetWidth(s)) >= out.planeWidth() &&
           (one ? IOSurfaceGetHeightOfPlane(s, 0) : IOSurfaceGetHeight(s)) >= out.planeHeight();
}

bool createRotated(RMTexture *t, MTLTextureDescriptor *d, IOSurfaceRef s, const raw::Rotation &r)
{
    const bool one = IOSurfaceGetPlaneCount(s) == 1;
    void *base = IOSurfaceGetBaseAddress(s);
    const uint64_t offset = one ? uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, 0)) - uintptr_t(base) : 0;
    const size_t pitch = one ? IOSurfaceGetBytesPerRowOfPlane(s, 0) : IOSurfaceGetBytesPerRow(s);
    if (!gRM->raw->createSurfaceTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset, r.planeWidth(),
                                        r.planeHeight(), uint32_t(pitch), uint32_t(d.pixelFormat), t->_raw, true))
        return false;
    t->_raw.rotation = r.turn;
    reportFirst(t, d);
    return true;
}

raw::RotatedRefusal rotatedPassRefusal(const RMPass &pass)
{
    raw::PassDesc p;
    const auto rawOf = [](id x) -> raw::Texture * {
        RMTexture *t = (RMTexture *)x;
        return [x isKindOfClass:[RMTexture class]] && t->_rawOk ? &t->_raw : nullptr;
    };
    bool any = false;
    for (unsigned c = 0; c < 8; c++) {
        const RMPass::Color &a = pass.color[c];
        if (!a.texture)
            continue;
        p.color[c].texture = rawOf(a.texture);
        p.color[c].load = a.load == MTLLoadActionLoad    ? raw::ColorAttachment::Load
                          : a.load == MTLLoadActionClear ? raw::ColorAttachment::Clear
                                                         : raw::ColorAttachment::DontCare;
        p.color[c].store = a.store != MTLStoreActionDontCare;
        p.count = c + 1;
        any |= p.color[c].texture && p.color[c].texture->rotation;
    }
    if (!any)
        return raw::RotatedRefusal::None;
    p.depthStencil.texture = rawOf(pass.zeta[0].texture ?: pass.zeta[1].texture);
    p.depthStencil.loaded = pass.zeta[0].load == MTLLoadActionLoad || pass.zeta[1].load == MTLLoadActionLoad;
    p.depthStencil.stored = (pass.zeta[0].texture && pass.zeta[0].store != MTLStoreActionDontCare) ||
                            (pass.zeta[1].texture && pass.zeta[1].store != MTLStoreActionDontCare);
    return raw::turnablePass(p);
}

uint8_t passTurn(const RMPass &pass)
{
    for (const RMPass::Color &a : pass.color) {
        RMTexture *t = (RMTexture *)a.texture;
        if ([a.texture isKindOfClass:[RMTexture class]] && t->_rawOk && t->_raw.rotation)
            return t->_raw.rotation;
    }
    return 0;
}
