// SPDX-License-Identifier: BSD-2-Clause-Patent
// The real TegraHost1x of switch_host1x_vic_failure_test: channels, fences, the general interrupt, on a register file.
#include "switch_host1x_fence_stubs.h"
#include "switch_host1x_vic_failure.h"
#define SWITCH_DCACHE_OP(op, address) ((void)(address))
#define SWITCH_DCACHE_BARRIER()       ((void)0)
#include "../TegraHost1x.cpp"
#include "../SwitchHost1xChannels.cpp"
#include "../SwitchHost1xFence.cpp"
#include "../SwitchHost1xErrors.cpp"

namespace {
alignas(16384) uint8_t gHostRegisters[kHost1xSize];
alignas(4096) uint8_t gMcRegisters[0x1000];
alignas(4096) uint8_t gChannelBuffers[2 * TEGRA_HOST1X_CHANNELS][SWITCH_HOST1X_ARENA_WORDS * 4];
uint32_t gChannelAllocated;

// TRM 14.9.5, 14.9.7 and 14.6: HINTSTATUS, HINTSTATUS_EXT and each CPU0 threshold status word clear where a one is
// written; a SYNCPT_CPU_INCR word increments each syncpoint whose bit is set.
bool clearsOnOne(uint32_t offset)
{
    return offset == 0x020 || offset == 0x028 || (offset >= 0xe80 && offset < 0xe80 + kSyncpts / 8);
}

bool increments(uint32_t offset)
{
    return offset >= 0xf60 && offset < 0xf60 + kSyncpts / 8;
}
}  // namespace

extern "C" uint32_t switch_mmio_model_read32(uintptr_t address)
{
    return *reinterpret_cast<uint32_t *>(address);
}

extern "C" void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    const uintptr_t sync = reinterpret_cast<uintptr_t>(gHostRegisters) + kSyncOffset;
    const uint32_t offset = static_cast<uint32_t>(address - sync);
    auto &word = *reinterpret_cast<uint32_t *>(address);
    if (address < sync || address >= reinterpret_cast<uintptr_t>(gHostRegisters) + sizeof(gHostRegisters)) {
        word = value;
    } else if (clearsOnOne(offset)) {
        word &= ~value;
    } else if (increments(offset)) {
        for (uint32_t bit = 0; bit < 32; ++bit)
            if (value & (1u << bit))
                ++switch_test_sync(0xf80 + 4 * ((offset - 0xf60) / 4 * 32 + bit));
    } else {
        word = value;
    }
}

uint32_t &switch_test_sync(uint32_t offset)
{
    return *reinterpret_cast<uint32_t *>(gHostRegisters + kSyncOffset + offset);
}

bool switch_test_host1x_start()
{
    return tegra_host1x_start() == TEGRA_HOST1X_OK;
}

extern "C" uintptr_t ml_io_map(uintptr_t pa, uintptr_t)
{
    return reinterpret_cast<uintptr_t>(pa == kHost1xPA ? gHostRegisters : gMcRegisters);
}

extern "C" bool switch_car_clock_enabled(unsigned)
{
    return true;
}

extern "C" bool switch_car_reset_asserted(unsigned)
{
    return false;
}

extern "C" uint32_t switch_car_read(uint32_t)
{
    return SOURCE_PLLP | HOST1X_DIV_2_5;
}

extern "C" void switch_car_write(uint32_t, uint32_t)
{}

extern "C" void switch_car_clock_enable(unsigned)
{}

extern "C" void switch_car_reset_deassert(unsigned)
{}

bool switch_host1x_channels_confine(char *line, size_t size)
{
    snprintf(line, size, "no SMMU in the host test");
    return false;
}

bool switch_host1x_channels_confine_channel(const switch_dma_buffer *, const switch_dma_buffer *)
{
    return true;
}

extern "C" void tegra_mc_hot_reset(uint32_t, uint32_t, bool)
{}

bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t, IOOptionBits)
{
    if (gChannelAllocated == 2 * TEGRA_HOST1X_CHANNELS || size > sizeof(gChannelBuffers[0]))
        return false;
    out->va = gChannelBuffers[gChannelAllocated++];
    out->pa = 0x80000000u + gChannelAllocated * 0x10000u;
    out->size = size;
    return true;
}

IOService *switch_interrupt_nub(uint32_t, bool)
{
    static IOService nub;
    return &nub;
}

void switch_interrupt_describe(uint32_t, char *buffer, size_t size)
{
    snprintf(buffer, size, "host test");
}

extern "C" void switch_emc_gpu_submit(void)
{}

extern "C" bool switch_nvdec_host1x_cancelled(uint32_t, uint32_t)
{
    return false;
}

extern "C" void switch_gpu_bridge_gate_close(void)
{}
