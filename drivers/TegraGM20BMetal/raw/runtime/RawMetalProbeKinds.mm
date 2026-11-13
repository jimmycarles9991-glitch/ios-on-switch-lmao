// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// hw_test_app's probes of routes no device run has covered yet (raw::Device::probeMipCandidates, probeZetaKinds and
// probeZetaColour): @YES turns one on for this process, @NO off; each answers what it was. Work recorded while one is on
// keeps its route.
@implementation RMDevice (ProbeKinds)

- (NSNumber *)switchRawProbeMipCandidates:(NSNumber *)on
{
    return @(gRM->raw->probeMipCandidates.exchange(on.boolValue));
}

- (NSNumber *)switchRawProbeZetaKinds:(NSNumber *)on
{
    return @(gRM->raw->probeZetaKinds.exchange(on.boolValue));
}

- (NSNumber *)switchRawProbeZetaColour:(NSNumber *)on
{
    return @(gRM->raw->probeZetaColour.exchange(on.boolValue));
}

@end
#endif
