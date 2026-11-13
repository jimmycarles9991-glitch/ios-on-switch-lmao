// SPDX-License-Identifier: BSD-2-Clause-Patent
// Selector 6 and the plumbing its probes share (SwitchVICKernelTest.h), debug kernels only.
#include "SwitchVICKernelTest.h"

#if TEGRA_VIC_STATS
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include "SwitchVICJobs.h"
#include "SwitchVICComposeProbe.h"
#include "SwitchVICFractionProbe.h"
#include "SwitchVICLargeProbe.h"
#include "SwitchVICLayoutProbe.h"
#include "SwitchVICPackedProbe.h"
#include "SwitchVICPitchProbe.h"
#include "SwitchVICTransposeProbe.h"

namespace {
struct Probe {
    uint32_t magic, replyBytes;
    int (*run)(void *reply);
};

constexpr Probe kProbes[] = {
    {SWITCH_VIC_PITCH_PROBE_MAGIC, sizeof(SwitchVICPitchProbeReply), switch_vic_pitch_probe_run},
    {SWITCH_VIC_LAYOUT_PROBE_MAGIC, sizeof(SwitchVICLayoutProbeReply), switch_vic_layout_probe_run},
    {SWITCH_VIC_FRACTION_PROBE_MAGIC, sizeof(SwitchVICFractionProbeReply), switch_vic_fraction_probe_run},
    {SWITCH_VIC_PACKED_PROBE_MAGIC, sizeof(SwitchVICPackedProbeReply), switch_vic_packed_probe_run},
    {SWITCH_VIC_TRANSPOSE_PROBE_MAGIC, sizeof(SwitchVICTransposeProbeReply), switch_vic_transpose_probe_run},
    {SWITCH_VIC_LARGE_PROBE_MAGIC, sizeof(SwitchVICLargeProbeReply), switch_vic_large_probe_run},
    {SWITCH_VIC_COMPOSE_PROBE_MAGIC, sizeof(SwitchVICComposeProbeReply), switch_vic_compose_probe_run},
    {SWITCH_VIC_COMPOSITE_ARM_MAGIC, SWITCH_VIC_COMPOSITE_REPLY_BYTES, switch_vic_composite_arm_run},
    {SWITCH_VIC_COMPOSITE_REPORT_MAGIC, SWITCH_VIC_COMPOSITE_REPLY_BYTES, switch_vic_composite_report_run},
};

bool gBusy;
char gRunning[32];

uint8_t *bytesOf(const SwitchVICBounceBuffer &buffer)
{
    auto *memory = OSDynamicCast(IOBufferMemoryDescriptor, buffer.memory);
    return memory ? static_cast<uint8_t *>(memory->getBytesNoCopy()) : nullptr;
}

const Probe *probeFor(uint32_t magic)
{
    for (const Probe &probe : kProbes) {
        if (probe.magic == magic)
            return &probe;
    }
    return nullptr;
}
}  // namespace

int switch_vic_probe_buffers(uint64_t bytes, SwitchVICProbeBuffers *out)
{
    *out = {};
    constexpr uint32_t kRowBytes = 4096;
    TegraVICSurfaceDesc desc = {};
    desc.fourcc = TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
    desc.width = kRowBytes / 4;
    desc.height = static_cast<uint32_t>((bytes + kRowBytes - 1) / kRowBytes);
    desc.planes = 1;
    desc.planeBytesPerRow[0] = kRowBytes;
    desc.planeWidth[0] = desc.width;
    desc.planeHeight[0] = desc.height;
    desc.length = uint64_t(desc.height) * kRowBytes;
    if (switch_vic_bounce_acquire(&desc, &desc, &out->slot))
        return -12;
    for (uint32_t i = 0; i < 2; i++) {
        out->bytes[i] = bytesOf(out->slot->buffers[i]);
        out->address[i] = switch_vic_mapping_address(out->slot->buffers[i].mapping);
    }
    out->size = bytes;
    return out->bytes[0] && out->bytes[1] ? 0 : -12;
}

void switch_vic_probe_buffers_release(SwitchVICProbeBuffers *buffers)
{
    switch_vic_bounce_release(buffers->slot);
    *buffers = {};
}

int switch_vic_probe_cpu_begin(const SwitchVICProbeBuffers &buffers, uint32_t which, uint32_t access)
{
    return switch_gpu_backing_cpu_acquire(buffers.slot->buffers[which].backing, 0, buffers.size, access);
}

int switch_vic_probe_cpu_end(const SwitchVICProbeBuffers &buffers, uint32_t which, uint32_t access)
{
    return switch_gpu_backing_cpu_release(buffers.slot->buffers[which].backing, 0, buffers.size, access);
}

int switch_vic_probe_submit(const SwitchVICProbeBuffers &buffers, const TegraVICJob &job, uint32_t probe,
                            uint32_t index)
{
    switch_gpu_backing_use uses[] = {{buffers.slot->buffers[0].backing, SWITCH_GPU_BACKING_READ, 0},
                                     {buffers.slot->buffers[1].backing, SWITCH_GPU_BACKING_WRITE, 0}};
    const SwitchVICOwnership ownership = {uses, 2, nullptr, 0, nullptr, 0};
    const char name[5] = {char(probe >> 24), char(probe >> 16), char(probe >> 8), char(probe), 0};
    snprintf(gRunning, sizeof(gRunning), "probe %s case %u", name, index);
    uint32_t fence = 0;
    int error = switch_vic_submit_checked(&job, 1, nullptr, ownership, nullptr, nullptr, &fence);
    if (!error)
        error = switch_vic_wait(fence);
    gRunning[0] = 0;
    return error;
}

const char *switch_vic_probe_running()
{
    return gRunning;
}

int switch_vic_kernel_test_call(const IOExternalMethodArguments *a)
{
    const auto *request = static_cast<const uint32_t *>(a->structureInput);
    if (a->scalarInputCount || a->scalarOutputCount || a->structureInputSize != 8 || !request || request[1] ||
        !a->structureOutput)
        return kIOReturnBadArgument;
    const Probe *probe = probeFor(request[0]);
    if (!probe || a->structureOutputSize != probe->replyBytes)
        return kIOReturnBadArgument;
    if (!tegra_vic_ready() || !tegra_vic_traits())
        return kIOReturnNotReady;
    if (__atomic_exchange_n(&gBusy, true, __ATOMIC_ACQUIRE))
        return kIOReturnBusy;
    void *reply = IOMallocZeroData(probe->replyBytes);
    const int error = reply ? probe->run(reply) : -12;
    if (reply) {
        memcpy(a->structureOutput, reply, probe->replyBytes);
        IOFreeData(reply, probe->replyBytes);
    }
    __atomic_store_n(&gBusy, false, __ATOMIC_RELEASE);
    return error == -12 ? kIOReturnNoMemory : kIOReturnSuccess;
}
#endif
