// SPDX-License-Identifier: MIT
// Indirect dispatch commands on the raw channel (icb_compute.h): a command region's QMD, root table and set 0 as a
// dispatch's (compute.cpp), in memory the command keeps, and an execute of commands by reference.
#include "compute.h"
#include "../../kits/SwitchGM20B/SwitchGM20B.h"
#include "icb_compute.h"
#include "residual.h"

#include <algorithm>
#include <cstring>

namespace gm20b {
namespace {

constexpr uint8_t kConstexprSamplerSlot = 0x80;
constexpr uint32_t kRootPush = 0x700;
// A GPFIFO entry's length field is 21 bits (NV906F_GP_ENTRY1_LENGTH)
constexpr uint64_t kEntryWords = (1u << 21) - 1;

// `count` slots of `stride` words at `va` by reference: entries end between slots, or inside a slot longer than an
// entry, whose pieces before its end are Incomplete so that no submission ends inside its methods
void appendSlots(PushBatches &segments, uint64_t va, uint64_t count, uint64_t stride)
{
    if (stride <= kEntryWords) {
        const uint64_t perEntry = kEntryWords / stride;
        for (uint64_t at = 0; at < count; at += perEntry)
            segments.append(
                {va + at * stride * 4, uint32_t(std::min(perEntry, count - at) * stride), TegraGM20BPushNoPrefetch},
                false);
        return;
    }
    for (uint64_t slot = 0; slot < count; slot++)
        for (uint64_t word = 0; word < stride; word += kEntryWords) {
            const uint64_t words = std::min(kEntryWords, stride - word);
            segments.append({va + (slot * stride + word) * 4, uint32_t(words),
                             TegraGM20BPushNoPrefetch | (word + words < stride ? TegraGM20BPushIncomplete : 0u)},
                            false);
        }
}

}  // namespace

bool ComputeEncoder::icbRegion(Device &dev, const ComputeState &k, const ComputeRegion &region,
                               const std::array<uint32_t, 3> &grid, const std::array<uint32_t, 3> &nominal,
                               const IcbBuffer (&buffers)[31], const StageIn &stageIn, uint8_t *block, uint64_t va,
                               const char **why)
{
    return icbRegion(dev, k, region, {{}, region.groups}, grid, nominal, buffers, stageIn, block, va, why);
}

bool ComputeEncoder::icbRegion(Device &dev, const ComputeState &k, const ComputeRegion &region,
                               const DispatchPiece &slice, const std::array<uint32_t, 3> &grid,
                               const std::array<uint32_t, 3> &nominal, const IcbBuffer (&buffers)[31],
                               const StageIn &stageIn, uint8_t *block, uint64_t va, const char **why)
{
    for (uint8_t a = 0; a < 3; ++a)
        if (slice.groups[a] > kQmdGroups[a]) {
            *why = "a launch past the QMD's grid fields";
            return false;
        }
    if (const char *refusal = icbRefusal(k)) {
        *why = refusal;
        return false;
    }
    if (__atomic_load_n(&k.heapMissing, __ATOMIC_ACQUIRE)) {  // its writer binds them first (icbArgumentTables)
        *why = "no memory for the tables a kernel reads argument buffers through";
        return false;
    }
    const PackKernel &r = *k.rec;
    if (r.spillEntry && !k.spill) {
        *why = "an argument table whose words the kernel's record does not hold";
        return false;
    }
    const uint32_t setBytes = icb::computeSetBytes(r.setBytes);
    const IcbRegionLayout layout = icbRegionLayout(r);
    const uint64_t rootVa = va + icb::kComputeQmdBytes, setVa = rootVa + icb::kComputeRootBytes;
    uint32_t *qmd = reinterpret_cast<uint32_t *>(block);
    uint32_t *root = qmd + icb::kComputeQmdBytes / 4;
    uint32_t *set = root + icb::kComputeRootBytes / 4;
    memset(root, 0, icb::kComputeRootBytes);
    memset(set, 0, setBytes);
    std::array<uint32_t, 32> residuals{};
    uint32_t residualMask = 0;
    SpillResiduals spilled;
    for (uint32_t i = 0; i < r.entries; i++) {
        const PackKernelEntry &e = r.entry[i];
        uint32_t *at = set + e.offset / 4;
        if (e.kind == KernelBuffer) {
            const IcbBuffer &b = e.slot < 31 ? buffers[e.slot] : IcbBuffer{};
            BufferDescriptor desc;
            kernelBufferDescriptor(e, b.va, b.size, desc);
            if (spilledWord(desc.word)) {
                spilled.set(desc.word, desc.residual);
            } else if (desc.word >= 0) {
                residuals[desc.word] = desc.residual;
                residualMask |= 1u << desc.word;
            }
            const uint64_t base = b.va ? desc.base : dev.zero_.va;
            at[0] = uint32_t(base);
            at[1] = uint32_t(base >> 32);
            at[2] = b.va ? desc.range : 0;
        } else if (e.kind == KernelTextureTable || e.kind == KernelSamplerTable) {
            tableDescriptor(dev, e, at);
        } else if (e.kind == KernelSampler) {
            uint32_t tsc = dev.neutralTsc_;
            if (e.slot & kConstexprSamplerSlot) {
                const uint32_t index = e.slot & ~kConstexprSamplerSlot;
                if (index >= dev.constexprCount_) {
                    *why = "a kernel's constexpr sampler the pack holds no header of";
                    return false;
                }
                tsc = dev.constexprTsc_[index];
            }
            at[0] |= tsc << 20;
        }
    }
    uint32_t words[8] = {};
    if (r.regionWord >= 0 && !computeRegionWords(region.base, grid, nominal, words)) {
        *why = "a threadgroup m2v's dispatch region cannot name";
        return false;
    }
    rootWords(r, slice.base.data(), region.groups.data(), setVa, residualMask, residuals, words, root);
    argumentWords(k, r, root);
    if (r.rootSlots && r.rootTableWord && r.rootTableWord <= 31) {
        RootSlot slot[32];
        for (uint32_t n = 0; n < 31; n++)
            if (r.rootSlots >> n & 1)
                slot[n] = {buffers[n].va, buffers[n].size, 0};
        const uint64_t tableVa = va + layout.slots;
        writeDrawRoots(reinterpret_cast<RootEntry *>(block + layout.slots), tableVa, r.rootSlots, slot,
                       dev.heapTables_.sinkVa(), dev.heapTables_.identityVa(), dev.zero_.va);
        root[kRootPush / 4 + r.rootTableWord - 1] = uint32_t(tableVa);
        root[kRootPush / 4 + r.rootTableWord] = uint32_t(tableVa >> 32);
    }
    if (r.spillEntry && r.spillEntry <= r.entries) {
        composeSpill(k.spill, r.spillWords, spilled, reinterpret_cast<uint32_t *>(block + layout.spill));
        spillDescriptor(va + layout.spill, r.spillWords, set + r.entry[r.spillEntry - 1].offset / 4);
    }
    stageInWords(r, stageIn, root + kRootPush / 4);
    return qmdWords(k, slice.groups.data(), rootVa, setVa, qmd, why);
}

ComputeEncoder::IcbRegionLayout ComputeEncoder::icbRegionLayout(const PackKernel &r)
{
    const auto aligned = [](uint32_t bytes) {
        return (bytes + icb::kComputeBlockAlign - 1) / icb::kComputeBlockAlign * icb::kComputeBlockAlign;
    };
    IcbRegionLayout l;
    l.slots = icb::computeRegionBytes(r.setBytes);
    l.spill = l.slots + aligned(r.rootSlots ? drawRootEntries(r.rootSlots) * uint32_t(sizeof(RootEntry)) : 0);
    l.bytes = l.spill + (r.spillEntry ? spillRange(r.spillWords) : 0);
    return l;
}

uint32_t ComputeEncoder::icbRegionBytes(const PackKernel &r)
{
    return icbRegionLayout(r).bytes;
}

void ComputeEncoder::inheritBuffers(IcbBuffer (&out)[31], std::array<uint32_t, 31> &strides)
{
    strides = stageIn_.strides;
    for (uint32_t slot = 0; slot < 31; slot++) {
        const Bound &b = bufs_[slot];
        out[slot] = {b.va, b.size};
        if (b.buffer) {
            cb_.use(*b.buffer, true);
            if (b.buffer->hostVisible && !b.buffer->wc && b.buffer->cpu)
                cb_.clean(b.buffer->cpu + b.offset, b.size);
        } else if (b.va && !b.kernelMemory) {
            cb_.useAddress(b.va, b.size, true);
        }
    }
}

bool ComputeEncoder::executeSlots(const IcbExecute &x, const char **why)
{
    if (!x.count)
        return true;
    const uint64_t slotBytes = uint64_t(x.slotWords) * 4, offset = uint64_t(x.at) * slotBytes,
                   bytes = uint64_t(x.count) * slotBytes;
    if (!x.slots || !x.slots->va || offset > x.slots->length || x.slots->length - offset < bytes) {
        *why = "indirect command buffer commands outside its memory";
        return false;
    }
    if (x.range && (!x.range->va || (x.rangeOffset & 3) || x.rangeOffset > x.range->length ||
                    x.range->length - x.rangeOffset < 8)) {
        *why = "an indirect command buffer's execution range outside its buffer or not four-byte aligned";
        return false;
    }
    if (x.binds && x.binds->length < (uint64_t(x.at) + x.count) * x.bindWords * 4) {
        *why = "indirect command buffer bind slots outside its memory";
        return false;
    }
    cb_.bufferWrites_.anything();
    cb_.use(*x.slots, false);
    if (serial_ && dispatched_)
        barrier();
    dispatched_ = true;
    if (x.binds) {
        IcbBuffer inherited[31];
        std::array<uint32_t, 31> strides;
        inheritBuffers(inherited, strides);
        uint64_t va[31] = {};
        uint32_t size[31] = {}, stride[31] = {};
        for (uint32_t s = 0; s < 31; s++) {
            // a bind without a stride reads at 0, as Metal's does (stage_in.h)
            stride[s] = strides[s] == kNoBindStride ? 0 : strides[s];
            // Metal takes any stride (an M4 Max under API validation: 2^24 and past); the scratch word holds 24 bits
            // (SwitchGM20BMacroSlots.h: 128 MME scratch words, 3 a slot, the address-high word's top 24 bits free below
            // X1's 2^40). Past them every thread but the first reads past a buffer no longer than the largest it holds,
            // as it does at that stride, so that one stands in.
            if ((x.strideSlots >> s & 1) && stride[s] > icb::kComputeStrideLimit) {
                if (inherited[s].size > icb::kComputeStrideLimit) {
                    *why = "a bind stride past the 24 bits an execute's scratch word holds, over a buffer longer than "
                           "them";
                    return false;
                }
                stride[s] = icb::kComputeStrideLimit;
            }
            va[s] = inherited[s].va;
            size[s] = inherited[s].size;
        }
        cb_.use(*x.binds, false);
        room(8 + 3 * 31);
        icb::computeBindHead(p_, va, size, stride, x.bindSlots | x.strideSlots);
        cb_.segment(start_, p_.at);
        appendSlots(cb_.segments_, x.binds->va + uint64_t(x.at) * x.bindWords * 4, x.count, x.bindWords);
        start_ = p_.at;
        room(4);
        icb::computeBindTail(p_);
    }
    room(16);
    icb::computeExecuteHead(p_, x.written || x.binds, x.range != nullptr, x.first, x.count);
    if (x.range) {
        cb_.use(*x.range, false);
        if (x.range->hostVisible && !x.range->wc && x.range->cpu)
            cb_.clean(x.range->cpu + x.rangeOffset, 8);
        cb_.segment(start_, p_.at);
        cb_.segments_.append({x.range->va + x.rangeOffset, 2, TegraGM20BPushNoPrefetch}, false);
        start_ = p_.at;
    }
    cb_.segment(start_, p_.at);
    appendSlots(cb_.segments_, x.slots->va + offset, x.count, x.slotWords);
    start_ = p_.at;
    room(4);
    icb::computeExecuteTail(p_);
    SWITCH_METAL_DIAGNOSTIC(dev_.stats.dispatches += x.count);
    return true;
}

}  // namespace gm20b
