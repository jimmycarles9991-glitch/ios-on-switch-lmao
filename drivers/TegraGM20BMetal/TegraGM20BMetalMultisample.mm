// SPDX-License-Identifier: MIT
#include "lock.h"
#import "TegraGM20BMetalMultisample.h"

SwitchMetalStoredSamples storedMultisampleKind(MTLTextureDescriptor *d)
{
    return gm20b::shapes::storedSamples(textureShape(d));
}

bool passSamplePositions(MTLRenderPassDescriptor *pass, NSUInteger count, uint32_t samples, gm20b::SamplePositions &out)
{
    const auto read = [pass](float *xy, uint32_t n) {
        MTLSamplePosition at[8];
        [pass getSamplePositions:at count:n];
        for (uint32_t i = 0; i < n; i++) {
            xy[2 * i] = at[i].x;
            xy[2 * i + 1] = at[i].y;
        }
    };
    if (!gm20b::readSamplePositions(uint32_t(std::min<NSUInteger>(count, 9)), samples, read, out))
        return false;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        os_log(
            gm20bLog(),
            "SwitchMetal: a pass's own sample positions on (first: %u of %u samples, x | y << 4 %02x %02x %02x %02x)",
            unsigned(out.count), samples, out.xy[0], out.xy[1], out.xy[2], out.xy[3]);
    return true;
}
