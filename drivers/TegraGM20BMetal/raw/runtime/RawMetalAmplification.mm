// SPDX-License-Identifier: MIT
// setVertexAmplificationCount:viewMappings: as the encoder's raster state (raw_amplification.cpp draws it): the count and its
// first kAmplificationMappings view mappings, nil ones as offsets of 0, as Metal reads them.
#import "RawMetalInternal.h"

@interface RMRenderEncoder (Amplification)
@end
@implementation RMRenderEncoder (Amplification)

- (void)setVertexAmplificationCount:(NSUInteger)count viewMappings:(const MTLVertexAmplificationViewMapping *)mappings
{
    SWITCH_API(ApiState);
    Cmd m = cmd(OpAmplification);
    m.a = uint32_t(std::min<NSUInteger>(count, UINT32_MAX));
    m.x = _cb->_extra.size();
    for (uint32_t k = 0; k < kAmplificationMappings; k++) {
        const bool mapped = mappings && k < count;
        _cb->_extra.push_back(mapped ? mappings[k].viewportArrayIndexOffset : 0);
        _cb->_extra.push_back(mapped ? mappings[k].renderTargetArrayIndexOffset : 0);
    }
    [_cb record:m];
}

@end
