// SPDX-License-Identifier: BSD-2-Clause-Patent
// The odd-size probe (SwitchVICOddProbe.h), debug kernels only.
#include "SwitchVICOddProbe.h"
#include "SwitchVICKernelTest.h"

#if TEGRA_VIC_STATS
#include <IOKit/IOLib.h>

namespace {
using namespace switch_vic_odd_probe;

int runCase(uint32_t index, const SwitchVICProbeBuffers &buffers, SwitchVICOddResult *r)
{
    const Case &c = kCases[index];
    *r = {};
    r->fourcc = c.fourcc;
    r->width = c.width;
    r->height = c.height;
    r->transform = c.transform;
    int error = switch_vic_probe_cpu(buffers, 0, SWITCH_GPU_BACKING_WRITE, [&](uint8_t *bytes) {
        fillSource(bytes, c);
    });
    if (!error)
        error = switch_vic_probe_cpu(buffers, 1, SWITCH_GPU_BACKING_WRITE, [](uint8_t *bytes) {
            memset(bytes, kPoison, kBufferBytes);
        });
    TegraVICJob job;
    if (!error && build(c, buffers.address[0], buffers.address[1], tegra_vic_config_address(),
                        tegra_vic_filter_address(), &job)) {
        r->status = -95;
        return 0;
    }
    if (!error)
        error = switch_vic_probe_submit(buffers, job, SWITCH_VIC_ODD_PROBE_MAGIC, index);
    if (!error)
        error = switch_vic_probe_cpu(buffers, 1, SWITCH_GPU_BACKING_READ, [&](uint8_t *bytes) {
            compare(bytes, c, r);
        });
    if (error)
        r->status = error;
    return error;
}
}  // namespace

int switch_vic_odd_probe_run(void *out)
{
    auto *reply = static_cast<SwitchVICOddProbeReply *>(out);
    SwitchVICProbeBuffers buffers;
    int error = switch_vic_probe_buffers(kBufferBytes, &buffers);
    for (uint32_t i = 0; !error && i < SWITCH_VIC_ODD_PROBE_CASES; i++, reply->count++)
        error = runCase(i, buffers, &reply->results[i]);
    switch_vic_probe_buffers_release(&buffers);
    reply->magic = SWITCH_VIC_ODD_PROBE_MAGIC;
    summarise(reply);
    IOLog("TegraVIC: odd-size probe: %u cases, controls %s, clean cases %#x, error %d\n", reply->count,
          reply->controlsClean ? "clean" : "NOT clean", reply->cleanCases, error);
    return error;
}
#endif
