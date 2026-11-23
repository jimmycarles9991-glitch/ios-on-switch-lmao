// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's probes of routes no device run has covered yet, and of the routes a test compares one with
// (gm20b::Device::probeMipCandidates, probeZetaKinds, probeStencilBits and probeZetaDirect), and plain memory for a probe's
// uncompressed reference (probePlainMemory): @YES turns one on for this process, @NO off; each answers what it was. Work
// recorded while one is on keeps its route.
@implementation SwitchMetalDevice (ProbeKinds)

- (NSNumber *)switchMetalProbeMipCandidates:(NSNumber *)on
{
    return @(gRM->raw->probeMipCandidates.exchange(on.boolValue));
}

- (NSNumber *)switchMetalProbeZetaKinds:(NSNumber *)on
{
    return @(gRM->raw->probeZetaKinds.exchange(on.boolValue));
}

- (NSNumber *)switchMetalProbePlainMemory:(NSNumber *)on
{
    const bool was = gRM->raw->probePlainMemory.load(std::memory_order_relaxed);
    gRM->raw->probePlainMemory.store(on.boolValue, std::memory_order_relaxed);
    return @(was);
}

- (NSNumber *)switchMetalProbeStencilBits:(NSNumber *)on
{
    return @(gRM->raw->probeStencilBits.exchange(on.boolValue));
}

- (NSNumber *)switchMetalProbeZetaDirect:(NSNumber *)on
{
    return @(gRM->raw->probeZetaDirect.exchange(on.boolValue));
}

@end
#endif
