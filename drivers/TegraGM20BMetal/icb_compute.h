// SPDX-License-Identifier: MIT
// An indirect command buffer of dispatches: each command is a slot of push words beside its regions' QMDs, root tables
// and set 0 in GPU memory, written when the CPU encodes the command, and a range of commands is one run of words the
// channel fetches by reference (ComputeEncoder::executeSlots). A command is its barrier (the serial encoder's
// shader-data flush, or no-ops), the gate macro called with its index (mme.h kIcbGate), then one launch per region
// of its grid (dispatch_regions.h) and per slice of a region past the QMD's grid fields (dispatch_bands.h
// qmdSlice). The gate sets the global render enable from the range in the macros' scratch, which the compute class's
// launches obey, so a command outside the range launches nothing; the execute's tail sets it true again. The call is at
// the same words in every command, so a copy to another index rewrites one word (kComputeIndexWord), as a draw's does.
// A buffer's slots are kComputeSlotWords long until a command launches more than they hold: then every slot of the
// buffer is computeSlotWords of the most it launches.
// A buffer that inherits the encoder's buffers is written the same way, its set-0 buffer descriptors unbound, and each
// command also has a bind slot: calls of kIcbBind with each descriptor's address, its Metal buffer index, the kernel's
// window of it, its residual's root word and the word the record leaves there, then each StageInStride root word of a
// stage-in kernel and its Metal buffer index, then each root table entry of a kernel that reads its buffer slots
// through its root table and its Metal buffer index. An execute writes the encoder's buffers and bind strides into the
// macros' scratch and runs the bind slots of its commands by reference before their slots, so the GPU writes their
// descriptors, strides and entries with no CPU write per command. Bind slots grow as command slots do
// (computeBindSlotWords).
// A buffer that inherits its pipeline has its commands written the same way once for each pipeline it is executed under
// (TegraGM20BMetalIndirectCompute.h SwitchMetalICBVariant), again only after a command changed: a QMD names its kernel's program.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include "../../kits/SwitchGM20B/SwitchGM20BCompute.h"
#include "icb.h"

namespace gm20b::icb {

constexpr uint32_t kComputeCallWord = 4, kComputeIndexWord = kComputeCallWord + 1, kComputeLaunchWords = 3,
                   kComputeSlotWords = 32,
                   kComputeRegions = (kComputeSlotWords - kComputeIndexWord - 1) / kComputeLaunchWords;

// A region's QMD, root table and set 0, in that order in its block: each at the 256 bytes a QMD address and a constant
// buffer's take (a kernel whose root table holds buffer slots has that table after them: ComputeEncoder::icbRegionBytes)
constexpr uint32_t kComputeQmdBytes = 256, kComputeRootBytes = 0x800, kComputeBlockAlign = 256;

inline uint32_t computeSetBytes(uint32_t recordSetBytes)
{
    return recordSetBytes ? (recordSetBytes + 255) & ~255u : 256;
}

inline uint32_t computeRegionBytes(uint32_t recordSetBytes)
{
    return kComputeQmdBytes + kComputeRootBytes + computeSetBytes(recordSetBytes);
}

// A buffer's slot size for commands that need `words`: `least`, or the power of two past it that holds them, so a
// buffer whose commands need more grows a few times
inline uint64_t grownWords(uint64_t words, uint64_t least)
{
    uint64_t size = least;
    while (size < words)
        size *= 2;
    return size;
}

// The slot size of a buffer whose commands launch at most `launches` QMDs
inline uint64_t computeSlotWords(uint64_t launches)
{
    return grownWords(kComputeIndexWord + 1 + launches * kComputeLaunchWords, kComputeSlotWords);
}

inline uint64_t computeSlotLaunches(uint64_t words)
{
    return words > kComputeIndexWord + 1 ? (words - kComputeIndexWord - 1) / kComputeLaunchWords : 0;
}

struct ComputeCommand {
    std::vector<uint64_t> qmd;  // each launch's QMD address, 256-byte aligned
    bool barrier = false;       // setBarrier: the commands before it have written before it launches
};

// An inherited buffer's set-0 descriptor in a command's region: its three words' address, the root push word that takes
// the residual of its base (0: none), the Metal buffer index it binds, the kernel's window of it in bytes (0: the whole
// buffer) and the word the record leaves in the residual's word, which a base on 16 bytes keeps
struct ComputeBind {
    uint64_t descriptor, residual;
    uint32_t slot, window, recorded;
};

// A stage-in kernel's StageInStride root word in a command's region and the Metal buffer index whose bind stride it takes
struct ComputeStride {
    uint64_t word;
    uint32_t slot;
};

// A root table entry of a kernel's buffer slot in a command's region (ComputeEncoder::icbRegionLayout's slot table) and
// the Metal buffer index it holds
struct ComputeRoot {
    uint64_t entry;
    uint32_t slot;
};

constexpr uint32_t kComputeBindSlotWords = 128;
// A macro call's data words: its ONE_INC header's count field is 13 bits (NV906F_DMA_ONEINCR_COUNT)
constexpr uint32_t kComputeBindCallWords = 0x1fff;
// the largest bind stride an execute's scratch word holds above an address (SWITCH_GM20B_ICB_STRIDE_SHIFT)
constexpr uint32_t kComputeStrideLimit = (1u << (32 - SWITCH_GM20B_ICB_STRIDE_SHIFT)) - 1;

struct ComputeBindCall {
    uint32_t descriptors, strides, roots;
};

// The next call of a bind slot that has `n` descriptors, `m` strides and `k` root entries left to send: as many as one
// call's data words hold, in that order
inline ComputeBindCall bindCall(uint64_t n, uint64_t m, uint64_t k)
{
    uint32_t room = kComputeBindCallWords - SWITCH_GM20B_ICB_BIND_WORDS(0, 0, 0);
    ComputeBindCall c;
    c.descriptors = uint32_t(std::min<uint64_t>(n, room / SWITCH_GM20B_ICB_BIND_DESCRIPTOR_WORDS));
    room -= c.descriptors * SWITCH_GM20B_ICB_BIND_DESCRIPTOR_WORDS;
    c.strides = uint32_t(std::min<uint64_t>(m, room / SWITCH_GM20B_ICB_BIND_STRIDE_WORDS));
    room -= c.strides * SWITCH_GM20B_ICB_BIND_STRIDE_WORDS;
    c.roots = uint32_t(std::min<uint64_t>(k, room / SWITCH_GM20B_ICB_BIND_ROOT_WORDS));
    return c;
}

// The words of the calls a bind slot of `n` descriptors, `m` strides and `k` root entries makes
inline uint64_t bindWords(uint64_t n, uint64_t m, uint64_t k)
{
    uint64_t words = 0;
    while (n || m || k) {
        const ComputeBindCall c = bindCall(n, m, k);
        words += 1 + SWITCH_GM20B_ICB_BIND_WORDS(c.descriptors, c.strides, c.roots);
        n -= c.descriptors;
        m -= c.strides;
        k -= c.roots;
    }
    return words;
}

inline uint64_t computeBindSlotWords(uint64_t n, uint64_t m, uint64_t k)
{
    return grownWords(bindWords(n, m, k), kComputeBindSlotWords);
}

// False: more calls than a bind slot of `words` holds
inline bool binds(uint32_t *slot, const ComputeBind *b, uint32_t n, const ComputeStride *s = nullptr, uint32_t m = 0,
                  const ComputeRoot *r = nullptr, uint32_t k = 0, uint32_t words = kComputeBindSlotWords)
{
    if (bindWords(n, m, k) > words)
        return false;
    Push p{slot, slot + words};
    uint32_t i = 0, j = 0, l = 0;
    while (i < n || j < m || l < k) {
        const ComputeBindCall c = bindCall(n - i, m - j, k - l);
        mme::icbBindHeader(p, c.descriptors, c.strides, c.roots);
        *p.at++ = c.descriptors;
        for (const uint32_t end = i + c.descriptors; i < end; i++) {
            *p.at++ = uint32_t(b[i].descriptor >> 32);
            *p.at++ = uint32_t(b[i].descriptor);
            *p.at++ = 3 * b[i].slot;
            *p.at++ = b[i].window;
            *p.at++ = uint32_t(b[i].residual >> 32);
            *p.at++ = uint32_t(b[i].residual);
            *p.at++ = b[i].recorded;
        }
        *p.at++ = c.strides;
        for (const uint32_t end = j + c.strides; j < end; j++) {
            *p.at++ = uint32_t(s[j].word >> 32);
            *p.at++ = uint32_t(s[j].word);
            *p.at++ = 3 * s[j].slot;
        }
        *p.at++ = c.roots;
        for (const uint32_t end = l + c.roots; l < end; l++) {
            *p.at++ = uint32_t(r[l].entry >> 32);
            *p.at++ = uint32_t(r[l].entry);
            *p.at++ = 3 * r[l].slot;
        }
    }
    while (p.at < p.end)
        *p.at++ = kNop;
    return true;
}

inline void bindsEmpty(uint32_t *slot, uint32_t words = kComputeBindSlotWords)
{
    for (uint32_t i = 0; i < words; i++)
        slot[i] = kNop;
}

// An execute's inherited buffers (`va`, `size`, bind `stride` by Metal buffer index; a stride at most kComputeStrideLimit, 0
// where none) of `slots` (a mask) into the macros' scratch, after the dispatches before have read the descriptors the bind
// slots write. A slot of no buffer is address 0 and size 0 there, of which the bind macro writes the zero page's
// descriptor and root table entry (the scratch word mme.h install() writes), as icbRegion does.
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
        *p.at++ = va[s] ? size[s] : 0;
    }
}

// After the bind slots: their inline writes have landed (the execute head then invalidates the constant caches)
inline void computeBindTail(Push &p)
{
    p.m(NVB197_WAIT_FOR_IDLE, 0);
}

inline void computeEmpty(uint32_t *slot, uint32_t words = kComputeSlotWords)
{
    for (uint32_t i = 0; i < words; i++)
        slot[i] = kNop;
}

// Command `index` into a slot of `words`, which its buffer sized to hold its launches
inline void dispatch(uint32_t *slot, uint32_t index, const ComputeCommand &c, uint32_t words = kComputeSlotWords)
{
    Push p{slot, slot + words};
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
    const uint64_t launches = std::min<uint64_t>(c.qmd.size(), computeSlotLaunches(words));
    for (uint64_t r = 0; r < launches; r++) {
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

}  // namespace gm20b::icb
