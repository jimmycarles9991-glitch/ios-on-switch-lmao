// SPDX-License-Identifier: BSD-2-Clause-Patent
// The row-pitch probe (SwitchVICPitchProbe.h), debug kernels only.
#include "SwitchVICPitchProbe.h"
#include "SwitchVICKernelTest.h"

#if TEGRA_VIC_STATS
#include <IOKit/IOLib.h>

namespace {
using namespace switch_vic_pitch_probe;

TegraVICImage bgra(uint64_t address, uint32_t pixels, uint32_t pitch)
{
    TegraVICImage image = {};
    image.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    image.width = pixels;
    image.height = kRows;
    image.pitch[0] = pitch;
    image.rows[0] = kRows;
    image.base[0] = address;
    image.span[0] = kBufferBytes;
    return image;
}

// 0, or the error that stops the probe (a job the engine failed: the next one would not tell anything more).
int runCase(uint32_t index, const SwitchVICProbeBuffers &buffers, SwitchVICPitchResult *r)
{
    const Case &c = kCases[index];
    const uint32_t pixels = width(c);
    *r = {c.sourcePitch, c.destinationPitch, pixels, 0, 0, 0, 0, 0, 0};
    int error = switch_vic_probe_cpu(buffers, 0, SWITCH_GPU_BACKING_WRITE, [&](uint8_t *bytes) {
        fillSource(bytes, c.sourcePitch, pixels);
    });
    if (!error)
        error = switch_vic_probe_cpu(buffers, 1, SWITCH_GPU_BACKING_WRITE, [](uint8_t *bytes) {
            memset(bytes, kPoison, kBufferBytes);
        });
    TegraVICTraits traits = *tegra_vic_traits();
    traits.sourcePitchAlign = 64;
    const TegraVICImage in = bgra(buffers.address[0], pixels, c.sourcePitch);
    const TegraVICImage out = bgra(buffers.address[1], pixels, c.destinationPitch);
    TegraVICOp op = {};
    op.srcW = uint64_t(pixels) << 16;
    op.srcH = uint64_t(kRows) << 16;
    op.dstW = pixels;
    op.dstH = kRows;
    op.nearest = 1;
    TegraVICJob job;
    if (!error &&
        tegra_vic_build(&in, &out, &op, &traits, tegra_vic_config_address(), tegra_vic_filter_address(), &job)) {
        r->status = -95;
        return 0;
    }
    if (!error)
        error = switch_vic_probe_submit(buffers, job, SWITCH_VIC_PITCH_PROBE_MAGIC, index);
    if (!error)
        error = switch_vic_probe_cpu(buffers, 1, SWITCH_GPU_BACKING_READ, [&](uint8_t *bytes) {
            compare(bytes, c.destinationPitch, pixels, r);
        });
    if (error)
        r->status = error;
    return error;
}
}  // namespace

int switch_vic_pitch_probe_run(void *out)
{
    auto *reply = static_cast<SwitchVICPitchProbeReply *>(out);
    SwitchVICProbeBuffers buffers;
    int error = switch_vic_probe_buffers(kBufferBytes, &buffers);
    for (uint32_t i = 0; !error && i < SWITCH_VIC_PITCH_PROBE_CASES; i++, reply->count++)
        error = runCase(i, buffers, &reply->results[i]);
    switch_vic_probe_buffers_release(&buffers);
    reply->magic = SWITCH_VIC_PITCH_PROBE_MAGIC;
    summarise(reply);
    IOLog("TegraVIC: pitch probe: %u cases, controls %s, clean cases %#x, error %d\n", reply->count,
          reply->controlsClean ? "clean" : "NOT clean", reply->cleanCases, error);
    return error;
}
#endif
