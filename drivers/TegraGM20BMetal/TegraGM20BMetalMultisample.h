// SPDX-License-Identifier: MIT
#pragma once
#import "TegraGM20BMetalInternal.h"

// texture_shape.h storedSamples
using SwitchMetalStoredSamples = gm20b::shapes::StoredSamples;

SwitchMetalStoredSamples storedMultisampleKind(MTLTextureDescriptor *d);

// The pass's own `count` sample positions (getSamplePositions:nil count:0) for a pass of `samples` samples
// (sample_positions.h); false: ones the driver keeps Metal's refusal of
bool passSamplePositions(MTLRenderPassDescriptor *pass, NSUInteger count, uint32_t samples,
                         gm20b::SamplePositions &out);
