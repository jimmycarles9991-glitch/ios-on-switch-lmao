// SPDX-License-Identifier: BSD-2-Clause-Patent
// The slot composition probe (SwitchVICComposeProbe.h), debug kernels only.
#include "SwitchVICComposeProbe.h"
#include "SwitchVICKernelTest.h"

#if TEGRA_VIC_STATS
#include <IOKit/IOLib.h>

namespace {
using namespace switch_vic_compose_probe;

int runCase(uint32_t index, const SwitchVICProbeBuffers &buffers, SwitchVICComposeResult *r)
{
    const Case &c = kCases[index];
    *r = {};
    r->slots = c.slots;
    int error = switch_vic_probe_cpu(buffers, 0, SWITCH_GPU_BACKING_WRITE, [](uint8_t *bytes) {
        fillSource(bytes);
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
        error = switch_vic_probe_submit(buffers, job, SWITCH_VIC_COMPOSE_PROBE_MAGIC, index);
    if (!error)
        error = switch_vic_probe_cpu(buffers, 1, SWITCH_GPU_BACKING_READ, [&](uint8_t *bytes) {
            compare(bytes, c, r);
        });
    if (error)
        r->status = error;
    return error;
}
}  // namespace

extern "C" int switch_display_composite_probe(uint32_t arm, uint32_t *composited) __attribute__((weak));

namespace {
int composite(void *out, uint32_t arm)
{
    auto *reply = static_cast<uint32_t *>(out);
    reply[0] = arm ? SWITCH_VIC_COMPOSITE_ARM_MAGIC : SWITCH_VIC_COMPOSITE_REPORT_MAGIC;
    reply[1] = arm;
    const int error = switch_display_composite_probe ? switch_display_composite_probe(arm, &reply[2]) : -19;
    reply[3] = uint32_t(error);
    return 0;
}
}  // namespace

int switch_vic_composite_arm_run(void *out)
{
    return composite(out, 1);
}

int switch_vic_composite_report_run(void *out)
{
    return composite(out, 0);
}

int switch_vic_compose_probe_run(void *out)
{
    auto *reply = static_cast<SwitchVICComposeProbeReply *>(out);
    SwitchVICProbeBuffers buffers;
    int error = switch_vic_probe_buffers(kBufferBytes, &buffers);
    for (uint32_t i = 0; !error && i < SWITCH_VIC_COMPOSE_PROBE_CASES; i++, reply->count++)
        error = runCase(i, buffers, &reply->results[i]);
    switch_vic_probe_buffers_release(&buffers);
    reply->magic = SWITCH_VIC_COMPOSE_PROBE_MAGIC;
    summarise(reply);
    IOLog("TegraVIC: compose probe: %u cases, control %s, clean cases %#x, error %d\n", reply->count,
          reply->controlsClean ? "clean" : "NOT clean", reply->cleanCases, error);
    return error;
}
#endif
