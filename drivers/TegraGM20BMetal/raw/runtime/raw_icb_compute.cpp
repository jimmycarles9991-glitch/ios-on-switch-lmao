// SPDX-License-Identifier: MIT
// Indirect dispatch commands on the raw channel (raw_icb_compute.h): a command region's QMD, root table and set 0 as a
// dispatch's (raw_compute.cpp), in memory the command keeps, and an execute of commands by reference.
#include "raw_compute.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "raw_icb_compute.h"
#include "raw_residual.h"

#include <cstring>

namespace raw {
namespace {

constexpr uint8_t kConstexprSamplerSlot = 0x80;
constexpr uint32_t kRootPush = 0x700;

}  // namespace

bool ComputeEncoder::icbRegion(Device &dev, const ComputeState &k, const ComputeRegion &region,
                               const std::array<uint32_t, 3> &grid, const std::array<uint32_t, 3> &nominal,
                               const IcbBuffer (&buffers)[31], const StageIn &stageIn, uint8_t *block, uint64_t va,
                               const char **why)
{
    if (const char *refusal = icbRefusal(k)) {
        *why = refusal;
        return false;
    }
    const PackKernel &r = *k.rec;
    const uint32_t setBytes = icb::computeSetBytes(r.setBytes);
    const uint64_t rootVa = va + icb::kComputeQmdBytes, setVa = rootVa + icb::kComputeRootBytes;
    uint32_t *qmd = reinterpret_cast<uint32_t *>(block);
    uint32_t *root = qmd + icb::kComputeQmdBytes / 4;
    uint32_t *set = root + icb::kComputeRootBytes / 4;
    memset(root, 0, icb::kComputeRootBytes);
    memset(set, 0, setBytes);
    std::array<uint32_t, 32> residuals{};
    uint32_t residualMask = 0;
    for (uint32_t i = 0; i < r.entries; i++) {
        const PackKernelEntry &e = r.entry[i];
        uint32_t *at = set + e.offset / 4;
        if (e.kind == KernelBuffer) {
            const IcbBuffer &b = e.slot < 31 ? buffers[e.slot] : IcbBuffer{};
            BufferDescriptor desc;
            if (!bufferDescriptor(e, b.va, b.size, desc)) {
                *why = "a buffer bound at an offset that is not a multiple of 16";
                return false;
            }
            if (desc.word >= 0) {
                residuals[desc.word] = desc.residual;
                residualMask |= 1u << desc.word;
            }
            at[0] = uint32_t(desc.base);
            at[1] = uint32_t(desc.base >> 32);
            at[2] = b.va ? desc.range : 0;
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
    const uint32_t base[3] = {};  // an indirect dispatch is never cut into pieces
    rootWords(r, base, region.groups.data(), setVa, residualMask, residuals, words, root);
    stageInWords(r, stageIn, root + kRootPush / 4);
    return qmdWords(k, region.groups.data(), rootVa, setVa, qmd, why);
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
        } else if (b.va) {
            cb_.useAddress(b.va, b.size, true);
        }
    }
}

bool ComputeEncoder::executeSlots(const IcbExecute &x, const char **why)
{
    if (!x.count)
        return true;
    const uint64_t slotBytes = icb::kComputeSlotWords * 4, offset = uint64_t(x.at) * slotBytes,
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
    if (x.binds && x.binds->length < uint64_t(x.at + x.count) * icb::kComputeBindSlotWords * 4) {
        *why = "indirect command buffer bind slots outside its memory";
        return false;
    }
    cb_.buffersWritten_ = true;
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
            if ((x.alignedSlots >> s & 1) && (inherited[s].va & 15)) {
                *why = "a buffer bound at an offset that is not a multiple of 16";
                return false;
            }
            // a bind without a stride reads at 0, as Metal's does (raw_stage_in.h)
            stride[s] = strides[s] == kNoBindStride ? 0 : strides[s];
            if ((x.strideSlots >> s & 1) && stride[s] > icb::kComputeStrideLimit) {
                *why = "a bind stride past the 24 bits an execute's scratch word holds";
                return false;
            }
            va[s] = inherited[s].va;
            size[s] = inherited[s].size;
        }
        cb_.use(*x.binds, false);
        room(8 + 3 * 31);
        icb::computeBindHead(p_, va, size, stride, x.bindSlots | x.strideSlots);
        cb_.segment(start_, p_.at);
        cb_.segments_.append({x.binds->va + uint64_t(x.at) * icb::kComputeBindSlotWords * 4,
                              x.count * icb::kComputeBindSlotWords, TegraGM20BPushNoPrefetch},
                             false);
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
    cb_.segments_.append({x.slots->va + offset, uint32_t(bytes / 4), TegraGM20BPushNoPrefetch}, false);
    start_ = p_.at;
    room(4);
    icb::computeExecuteTail(p_);
    RAW_DIAGNOSTIC(dev_.stats.dispatches += x.count);
    return true;
}

}  // namespace raw
