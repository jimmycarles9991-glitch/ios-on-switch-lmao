// SPDX-License-Identifier: MIT
// MTLDepthStencilState: the device's own object, its register values made at creation (depth_stencil_state.h).
#import "TegraGM20BMetalInternal.h"

static gm20b::StencilFace stencilFace(MTLStencilDescriptor *s)
{
    gm20b::StencilFace face;
    face.compare = uint8_t(s.stencilCompareFunction);
    face.fail = uint8_t(s.stencilFailureOperation);
    face.depthFail = uint8_t(s.depthFailureOperation);
    face.pass = uint8_t(s.depthStencilPassOperation);
    face.readMask = s.readMask;
    face.writeMask = s.writeMask;
    return face;
}

static bool faceWritesStencil(const gm20b::StencilFace &f)
{
    return f.writeMask && (f.fail || f.depthFail || f.pass);
}

@implementation SwitchMetalDepthStencil

- (BOOL)readsDepth
{
    return _raw.desc().depthCompare != MTLCompareFunctionAlways;
}

- (BOOL)writesDepth
{
    return _raw.desc().depthWrite;
}

- (BOOL)readsStencil
{
    const gm20b::DepthStencilDesc &d = _raw.desc();
    return d.front.compare != MTLCompareFunctionAlways || d.back.compare != MTLCompareFunctionAlways;
}

- (BOOL)writesStencil
{
    const gm20b::DepthStencilDesc &d = _raw.desc();
    return faceWritesStencil(d.front) || faceWritesStencil(d.back);
}

@end

@implementation SwitchMetalDevice (DepthStencil)

- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d
{
    SWITCH_API(ApiObject);
    gm20b::DepthStencilDesc desc;
    desc.depthCompare = uint8_t(d.depthCompareFunction);
    desc.depthWrite = d.depthWriteEnabled;
    if (MTLStencilDescriptor *s = d.frontFaceStencil)
        desc.front = stencilFace(s);
    if (MTLStencilDescriptor *s = d.backFaceStencil)
        desc.back = stencilFace(s);
    SwitchMetalDepthStencil *o = [SwitchMetalDepthStencil new];
    o->_device = self;
    o->_raw = gm20b::DepthStencilState(desc);
    gRM->raw->placeSegments(o->_raw);
    o.label = d.label;
    return (id<MTLDepthStencilState>)o;
}

@end
