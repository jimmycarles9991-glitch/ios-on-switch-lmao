// SPDX-License-Identifier: MIT
// MTLDepthStencilState: the device's own object, its register values made at creation (raw_depth_stencil_state.h).
#import "RawMetalInternal.h"

static raw::StencilFace stencilFace(MTLStencilDescriptor *s)
{
    raw::StencilFace face;
    face.compare = uint8_t(s.stencilCompareFunction);
    face.fail = uint8_t(s.stencilFailureOperation);
    face.depthFail = uint8_t(s.depthFailureOperation);
    face.pass = uint8_t(s.depthStencilPassOperation);
    face.readMask = s.readMask;
    face.writeMask = s.writeMask;
    return face;
}

static bool faceWritesStencil(const raw::StencilFace &f)
{
    return f.writeMask && (f.fail || f.depthFail || f.pass);
}

@implementation RMDepthStencil

+ (Class)twinClass
{
    return NSClassFromString(@"NBDepthStencil");
}

// Only m2v's replay of a command buffer the raw path refused asks for it
- (id)rmTwin
{
    @synchronized(self) {
        if (_twin)
            return _twin;
    }
    id<MTLDevice> m2v = RM_M2V("a depth/stencil state's twin");
    return twinPublish(self, _twin, [m2v newDepthStencilStateWithDescriptor:_descriptor]);
}

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
    const raw::DepthStencilDesc &d = _raw.desc();
    return d.front.compare != MTLCompareFunctionAlways || d.back.compare != MTLCompareFunctionAlways;
}

- (BOOL)writesStencil
{
    const raw::DepthStencilDesc &d = _raw.desc();
    return faceWritesStencil(d.front) || faceWritesStencil(d.back);
}

@end

@implementation RMDevice (DepthStencil)

- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d
{
    SWITCH_API(ApiObject);
    raw::DepthStencilDesc desc;
    desc.depthCompare = uint8_t(d.depthCompareFunction);
    desc.depthWrite = d.depthWriteEnabled;
    if (MTLStencilDescriptor *s = d.frontFaceStencil)
        desc.front = stencilFace(s);
    if (MTLStencilDescriptor *s = d.backFaceStencil)
        desc.back = stencilFace(s);
    RMDepthStencil *o = [RMDepthStencil new];
    o->_device = self;
    o->_raw = raw::DepthStencilState(desc);
    gRM->raw->placeSegments(o->_raw);
    o->_descriptor = [d copy];
    o.label = d.label;
    return (id<MTLDepthStencilState>)o;
}

@end
