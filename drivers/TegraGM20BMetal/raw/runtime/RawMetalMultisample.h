// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"

// A stored multisampled texture the raw path keeps: private, 2D multisampled at a count the device answers yes for (2, 4), one
// level, no rotation; a colour format it renders (Device::createStoredMultisample), or a depth/stencil one only rendered and
// sampled (Device::createOwnedZeta)
enum class RMStoredSamples : uint8_t {
    None,
    Colour,
    Zeta
};

RMStoredSamples storedMultisampleKind(MTLTextureDescriptor *d);
bool makeStoredMultisample(MTLTextureDescriptor *d, RMStoredSamples kind, raw::Texture &t);

// A stored one-sample depth/stencil texture (zetaDescriptor's, not memoryless) in zeta memory of its own, no m2v twin
bool makeStoredZeta(MTLTextureDescriptor *d, raw::Texture &t);
// m2v's twin of stored one-sample depth/stencil texture `t`, made at the first replay that needs it: NVK's image, with
// `t`'s texels copied into it and `t` over it from then on (raw::Device::moveOwnedZeta); nil, *why, where it cannot be
id storedZetaTwin(RMTexture *t, id<MTLDevice> m2v, const char **why);
// Before a range replays on m2v and m2v is ordered after the raw path's work (rmRangePlain): `t`'s twin, if it is a stored
// one-sample depth/stencil texture, so its move's copy is work m2v is ordered after
void rmStoredZetaForM2v(RMTexture *t);

// The pass's own `count` sample positions (getSamplePositions:nil count:0) for a pass of `samples` samples
// (raw_sample_positions.h); false: ones the raw path keeps Metal's refusal of
bool passSamplePositions(MTLRenderPassDescriptor *pass, NSUInteger count, uint32_t samples, raw::SamplePositions &out);

// The first stored multisampled depth/stencil texture, or depth/stencil texture of an MTLHeap (its arena's zeta view), range
// [from, to) of `cb` loads, stores, binds or copies: m2v's twin of
// it is a texture of its own memory, so a replay of the range fails by name, before m2v encodes any of it, rather than read
// or keep depth the raw path's memory does not hold. A range that only attaches it, cleared or not loaded and not stored,
// replays on the twin.
RMTexture *ownedZetaIn(RMCommandBuffer *cb, size_t from, size_t to);
void failOwnedZeta(RMCommandBuffer *cb, RMTexture *t);
