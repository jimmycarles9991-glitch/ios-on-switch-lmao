// SPDX-License-Identifier: MIT
// An indirect command buffer of dispatches: each command is kComputeSlotWords push words beside its regions' QMDs, root
// tables and set 0 in GPU memory, written when the CPU encodes the command, and a range of commands is one run of words the
// channel fetches by reference (ComputeEncoder::executeSlots). A command is its barrier (the serial encoder's shader-data
// flush, or no-ops), the gate macro called with its index (raw_mme.h kIcbGate), then one launch per region of its grid
// (raw_dispatch_regions.h). The gate sets the global render enable from the range in the macros' scratch, which the compute
// class's launches obey, so a command outside the range launches nothing; the execute's tail sets it true again. The call is
// at the same words in every command, so a copy to another index rewrites one word (kComputeIndexWord), as a draw's does.
// A buffer that inherits the encoder's buffers is written the same way, its set-0 buffer descriptors zero, and each command
// also has a bind slot: a call of kIcbBind with each descriptor's address, its Metal buffer index, the kernel's window of
// it and its residual's root word, then each StageInStride root word of a stage-in kernel and its Metal buffer index. An
// execute writes the encoder's buffers and bind strides into the macros' scratch and runs the bind slots of its commands by
// reference before their slots, so the GPU writes their descriptors and strides with no CPU write per command.
// A buffer that inherits its pipeline has its commands written the same way once for each pipeline it is executed under
// (RawMetalIndirectCompute.h RMICBVariant), again only after a command changed: a QMD names its kernel's program.
#pragma once
#include <cstdint>
#include "../../../../kits/SwitchGM20B/SwitchGM20BCompute.h"
#include "raw_icb.h"

namespace raw::icb {

constexpr uint32_t kComputeRegions = 8, kComputeCallWord = 4, kComputeIndexWord = kComputeCallWord + 1,
                   kComputeLaunchWords = 3, kComputeSlotWords = 32;
static_assert(kComputeIndexWord + 1 + kComputeRegions * kComputeLaunchWords <= kComputeSlotWords);

// A region's QMD, root table and set 0, in that order in its block: each at the 256 bytes a QMD address and a constant
// buffer's take
constexpr uint32_t kComputeQmdBytes = 256, kComputeRootBytes = 0x800, kComputeBlockAlign = 256;

inline uint32_t computeSetBytes(uint32_t recordSetBytes)
{
    return recordSetBytes ? (recordSetBytes + 255) & ~255u : 256;
}

inline uint32_t computeRegionBytes(uint32_t recordSetBytes)
{
    return kComputeQmdBytes + kComputeRootBytes + computeSetBytes(recordSetBytes);
}

struct ComputeCommand {
    uint64_t qmd[kComputeRegions];  // each region's QMD address, 256-byte aligned
    uint32_t regions;
    bool barrier;  // setBarrier: the commands before it have written before it launches
};

// An inherited buffer's set-0 descriptor in a command's region: its three words' address, the root push word that takes the
// residual of its base (0: none), the Metal buffer index it binds and the kernel's window of it in bytes (0: the whole buffer)
struct ComputeBind {
    uint64_t descriptor, residual;
    uint32_t slot, window;
};

// A stage-in kernel's StageInStride root word in a command's region and the Metal buffer index whose bind stride it takes
struct ComputeStride {
    uint64_t word;
    uint32_t slot;
};

constexpr uint32_t kComputeBindSlotWords = 128, kComputeBindsMax = (kComputeBindSlotWords - 3) / 6;
// the largest bind stride an execute's scratch word holds above an address (SWITCH_GM20B_ICB_STRIDE_SHIFT)
constexpr uint32_t kComputeStrideLimit = (1u << (32 - SWITCH_GM20B_ICB_STRIDE_SHIFT)) - 1;

inline bool bindsFit(uint32_t descriptors, uint32_t strides)
{
    return 3 + 6 * descriptors + 3 * strides <= kComputeBindSlotWords;
}

// False: more descriptors and strides than a bind slot holds
inline bool binds(uint32_t *slot, const ComputeBind *b, uint32_t n, const ComputeStride *s = nullptr, uint32_t m = 0)
{
    if (!bindsFit(n, m))
        return false;
    Push p{slot, slot + kComputeBindSlotWords};
    if (n || m) {
        mme::icbBindHeader(p, n, m);
        *p.at++ = n;
        for (uint32_t i = 0; i < n; i++) {
            *p.at++ = uint32_t(b[i].descriptor >> 32);
            *p.at++ = uint32_t(b[i].descriptor);
            *p.at++ = 3 * b[i].slot;
            *p.at++ = b[i].window;
            *p.at++ = uint32_t(b[i].residual >> 32);
            *p.at++ = uint32_t(b[i].residual);
        }
        *p.at++ = m;
        for (uint32_t i = 0; i < m; i++) {
            *p.at++ = uint32_t(s[i].word >> 32);
            *p.at++ = uint32_t(s[i].word);
            *p.at++ = 3 * s[i].slot;
        }
    }
    while (p.at < p.end)
        *p.at++ = kNop;
    return true;
}

inline void bindsEmpty(uint32_t *slot)
{
    for (uint32_t i = 0; i < kComputeBindSlotWords; i++)
        slot[i] = kNop;
}

// An execute's inherited buffers (`va`, `size`, bind `stride` by Metal buffer index; a stride at most kComputeStrideLimit, 0
// where none) of `slots` (a mask) into the macros' scratch, after the dispatches before have read the descriptors the bind
// slots write
inline void computeBindHead(Push &p, const uint64_t (&va)[31], const uint32_t (&size)[31], const uint32_t (&stride)[31],
                            uint32_t slots)
{
    p.m(NVB1C0_WAIT_FOR_IDLE, 0, SUBC_COMPUTE);
    if (!slots)
        return;
    const uint32_t lo = uint32_t(__builtin_ctz(slots)), hi = 31u - uint32_t(__builtin_clz(slots));
    *p.at++ = Push::hdr(1, SUBC_3D, NVB197_SET_MME_SHADOW_SCRATCH(mme::kScratchBuffers + 3 * lo), 3 * (hi - lo + 1));
    for (uint32_t s = lo; s <= hi; s++) {
        *p.at++ = uint32_t(va[s]);
        *p.at++ = uint32_t(va[s] >> 32) | stride[s] << SWITCH_GM20B_ICB_STRIDE_SHIFT;
        *p.at++ = size[s];
    }
}

// After the bind slots: their inline writes have landed (the execute head then invalidates the constant caches)
inline void computeBindTail(Push &p)
{
    p.m(NVB197_WAIT_FOR_IDLE, 0);
}

inline void computeEmpty(uint32_t *slot)
{
    for (uint32_t i = 0; i < kComputeSlotWords; i++)
        slot[i] = kNop;
}

inline void dispatch(uint32_t *slot, uint32_t index, const ComputeCommand &c)
{
    Push p{slot, slot + kComputeSlotWords};
    if (c.barrier) {
        p.m(NVB1C0_INVALIDATE_SHADER_CACHES,
            NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, DATA, TRUE) |
                NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE) |
                NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE),
            SUBC_COMPUTE);
        p.m(NVB1C0_INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI, 0, SUBC_COMPUTE);
    }
    while (p.at < slot + kComputeCallWord)
        *p.at++ = kNop;
    mme::icbGateHeader(p);
    *p.at++ = index;
    for (uint32_t r = 0; r < c.regions && r < kComputeRegions; r++) {
        p.inc(NVB1C0_SEND_PCAS_A, {uint32_t(c.qmd[r] >> 8)}, SUBC_COMPUTE);
        p.m(NVB1C0_SEND_SIGNALING_PCAS_B,
            NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, INVALIDATE, TRUE) |
                NVDEF(NVB1C0, SEND_SIGNALING_PCAS_B, SCHEDULE, TRUE),
            SUBC_COMPUTE);
    }
    while (p.at < p.end)
        *p.at++ = kNop;
}

// Before an execute's commands: what a copy wrote of their words reaches the command fetch first, then the range into the
// macros' scratch (the CPU's), or the call whose two data words the caller appends as a GPFIFO continuation at the range
// buffer (kIcbRange)
inline void computeExecuteHead(Push &p, bool written, bool rangeFromMemory, uint32_t first, uint32_t count)
{
    if (written || rangeFromMemory) {
        p.m(NVB1C0_WAIT_FOR_IDLE, 0, SUBC_COMPUTE);
        p.m(NVB1C0_INVALIDATE_SHADER_CACHES,
            NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, FLUSH_DATA, TRUE) |
                NVDEF(NVB1C0, INVALIDATE_SHADER_CACHES, CONSTANT, TRUE),
            SUBC_COMPUTE);
        // NV906F_SET_REFERENCE, as RenderEncoder::indirect: the fetches wait for the writes before them
        p.m(0x0050, 0);
    }
    if (rangeFromMemory)
        mme::icbRangeHeader(p);
    else
        mme::icbRange(p, first, count);
}

// After them: launches obey the render enable again as the channel left it before the first gate
inline void computeExecuteTail(Push &p)
{
    p.m(NVB1C0_SET_GLOBAL_RENDER_ENABLE_C, NVB1C0_SET_GLOBAL_RENDER_ENABLE_C_MODE_TRUE, SUBC_COMPUTE);
}

}  // namespace raw::icb
