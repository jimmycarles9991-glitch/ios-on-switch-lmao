// SPDX-License-Identifier: BSD-2-Clause-Patent
// The fractional source offset and alpha override probe (SwitchVICFractionProbe.h), debug kernels only.
#include "SwitchVICFractionProbe.h"
#include "SwitchVICKernelTest.h"

#if TEGRA_VIC_STATS
#include <IOKit/IOLib.h>

namespace {
using namespace switch_vic_fraction_probe;

int runCase(uint32_t index, const SwitchVICProbeBuffers &buffers, SwitchVICFractionResult *r)
{
    const Case &c = kCases[index];
    *r = {};
    r->fractionX = c.fractionX;
    r->fractionY = c.fractionY;
    r->mode = c.mode;
    int error = switch_vic_probe_cpu(buffers, 0, SWITCH_GPU_BACKING_WRITE, [&](uint8_t *bytes) {
        fillSource(bytes, c);
    });
    if (!error)
        error = switch_vic_probe_cpu(buffers, 1, SWITCH_GPU_BACKING_WRITE, [](uint8_t *bytes) {
            memset(bytes, kPoison, kBufferBytes);
        });
    const TegraVICImage in = image(buffers.address[0], false), out = image(buffers.address[1], true);
    const TegraVICOp op = operation(c);
    TegraVICJob job;
    if (!error && tegra_vic_build(&in, &out, &op, tegra_vic_traits(), tegra_vic_config_address(),
                                  tegra_vic_filter_address(), &job)) {
        r->status = -95;
        return 0;
    }
    if (c.mode == kBackgroundAlpha)
        overrideAlpha(&job, kOverride);
    if (!error)
        error = switch_vic_probe_submit(buffers, job, SWITCH_VIC_FRACTION_PROBE_MAGIC, index);
    if (!error)
        error = switch_vic_probe_cpu(buffers, 1, SWITCH_GPU_BACKING_READ, [&](uint8_t *bytes) {
            compare(bytes, c, r);
        });
    if (error)
        r->status = error;
    return error;
}
}  // namespace

int switch_vic_fraction_probe_run(void *out)
{
    auto *reply = static_cast<SwitchVICFractionProbeReply *>(out);
    SwitchVICProbeBuffers buffers;
    int error = switch_vic_probe_buffers(kBufferBytes, &buffers);
    for (uint32_t i = 0; !error && i < SWITCH_VIC_FRACTION_PROBE_CASES; i++, reply->count++)
        error = runCase(i, buffers, &reply->results[i]);
    switch_vic_probe_buffers_release(&buffers);
    reply->magic = SWITCH_VIC_FRACTION_PROBE_MAGIC;
    summarise(reply);
    IOLog("TegraVIC: fraction probe: %u cases, controls %s, clean cases %#x, error %d\n", reply->count,
          reply->controlsClean ? "clean" : "NOT clean", reply->cleanCases, error);
    return error;
}
#endif
