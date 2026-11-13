// SPDX-License-Identifier: MIT
// The words a direct record takes of its layout, samplers, views and compute programs, made from the data the bridge
// hands over (NativeMetalDirect.h) by the GM20B kit's encoders and NVK's layout rules (raw_set_layout.h).
#pragma once
#include <cstdint>
#include <vector>
#include "../../../../kits/SwitchGM20B/SwitchGM20BQmd.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20BSampler.h"
#include "../../../../kits/SwitchGM20B/SwitchGM20BTexture.h"
#include "bridge/NativeMetalDirect.h"
#include "raw_direct_nvk.h"
#include "raw_pack.h"
#include "raw_set_layout.h"

namespace raw {

// A set 0 layout's slots and fixed size, as NVK lays out the bridge's bindings
struct Set0 {
    std::vector<SetSlot> slots;
    uint32_t fixedBytes = 0;
    bool valid = false;

    Set0(const NmbDirectBinding *bindings, uint32_t count)
    {
        std::vector<SetBinding> b(count);
        for (uint32_t i = 0; i < count; i++)
            b[i] = {bindings[i].binding, bindings[i].type, bindings[i].count, bindings[i].flags};
        valid = setSlots(b.data(), count, slots, fixedBytes);
    }

    bool slot(uint32_t binding, SetSlot &out) const
    {
        if (!valid || binding >= slots.size())
            return false;
        out = slots[binding];
        return true;
    }

    uint32_t bytes() const
    {
        return setBytes(fixedBytes);
    }
};

// The first `arrays` heap arrays of `layout`'s set 1 (m2v's layouts hold their sets in set order) as NVK lays out the
// bridge's bindings, each the raw heap's 4-byte elements, and the set's bytes; null: why not
inline const char *heapSetArrays(const NmbDirectLayout *layout, uint32_t arrays, const uint32_t (&heapClass)[8],
                                 PackHeapArray (&out)[kPackHeapArrays], uint32_t &bytes)
{
    if (!layout || layout->setCount < 2)
        return "heap arrays without their set";
    if (arrays > kPackHeapArrays)
        return "more set-1 heap arrays than a record holds";
    const NmbDirectSet &set = layout->sets[1];
    std::vector<SetBinding> b(set.bindingCount);
    for (uint32_t i = 0; i < set.bindingCount; i++)
        b[i] = {set.bindings[i].binding, set.bindings[i].type, set.bindings[i].count, set.bindings[i].flags};
    std::vector<SetSlot> slots;
    uint32_t fixed = 0;
    if (!setSlots(b.data(), set.bindingCount, slots, fixed))
        return "a set-1 descriptor type NVK lays out by another rule";
    for (uint32_t a = 0; a < arrays; a++) {
        if (!heapClass[a])
            return "a set-1 heap array of a class the bridge cannot word";
        const SetBinding *binding = nullptr;
        for (const SetBinding &x : b)
            binding = x.binding == a ? &x : binding;
        if (!binding || slots[a].stride != 4 || binding->count != kPackHeapElements)
            return "a set-1 heap array off the raw heap's 4-byte elements";
        out[a] = {heapClass[a], slots[a].offset};
    }
    bytes = setBytes(fixed);
    return nullptr;
}

// A variant's layout as nvk_switch_offline.h takes it: every set laid out by raw_set_layout.h
struct NakLayoutData {
    std::vector<std::vector<NakBinding>> bindings;
    std::vector<NakSet> sets;
    NakLayout layout = {};
    bool valid = true;

    explicit NakLayoutData(const NmbDirectLayout *in)
    {
        if (!in)
            return;
        bindings.resize(in->setCount);
        for (uint32_t s = 0; s < in->setCount; s++) {
            const NmbDirectSet &set = in->sets[s];
            std::vector<SetBinding> b(set.bindingCount);
            for (uint32_t i = 0; i < set.bindingCount; i++)
                b[i] = {set.bindings[i].binding, set.bindings[i].type, set.bindings[i].count, set.bindings[i].flags};
            std::vector<SetSlot> slots;
            uint32_t fixed = 0;
            valid = valid && setSlots(b.data(), set.bindingCount, slots, fixed);
            for (const SetBinding &x : b) {
                const SetSlot slot = x.binding < slots.size() ? slots[x.binding] : SetSlot();
                bindings[s].push_back({x.binding, x.type, x.count, x.flags, slot.offset, slot.stride});
            }
            sets.push_back({bindings[s].data(), uint32_t(bindings[s].size()), set.flags});
        }
        layout = {sets.data(), uint32_t(sets.size()), 0, in->push, in->pushCount};
    }
};

inline bool samplerHeader(const NmbDirectSamplerState &s, uint32_t header[8])
{
    switch_gm20b_sampler_state k = {};
    k.mag_filter = s.magFilter;
    k.min_filter = s.minFilter;
    k.mipmap_mode = s.mipmapMode;
    for (int i = 0; i < 3; i++)
        k.address[i] = s.address[i];
    k.mip_lod_bias = s.mipLodBias;
    k.min_lod = s.minLod;
    k.max_lod = s.maxLod;
    k.anisotropy = s.anisotropy;
    k.max_anisotropy = s.maxAnisotropy;
    k.compare = s.compare;
    k.compare_op = s.compareOp;
    k.border_color = s.borderColor;
    k.reduction = s.reduction;
    for (int i = 0; i < 4; i++)
        k.border_components[i] = s.borderComponents[i];
    k.unnormalized = s.unnormalized;
    return switch_gm20b_sampler_header(&k, header);
}

// The immutable sampler of set 0's `binding`
inline bool immutableSamplerHeader(const NmbDirectBinding *bindings, uint32_t count, uint32_t binding,
                                   uint32_t header[8])
{
    for (uint32_t i = 0; i < count; i++)
        if (bindings[i].binding == binding)
            return bindings[i].immutable && samplerHeader(bindings[i].sampler, header);
    return false;
}

inline bool viewWord0(uint32_t vkFormat, const uint32_t components[4], bool storage, uint32_t &word0)
{
    return switch_gm20b_view_word0(vkFormat, components, storage, &word0);
}

inline switch_gm20b_qmd_info qmdInfo(const NvkStage &stage)
{
    switch_gm20b_qmd_info q = {};
    q.num_gprs = stage.numGprs;
    q.num_barriers = stage.numBarriers;
    for (int i = 0; i < 3; i++)
        q.local_size[i] = stage.localSize[i];
    q.slm_size = stage.slmSize;
    q.crs_size = stage.crsSize;
    return q;
}

constexpr uint32_t kRootTableBytes = 0x800, kMaxCbufBytes = 1u << 16;

// The QMD of a dispatch of `stage` with set 0 of `setBytes` and set 1 (argument buffers' heap arrays) of `heapBytes`
// (nvk_cmd_upload_qmd's): its program, grid and constant buffers at 0. False where it reads a constant buffer other
// than the root table, those sets and its shader data.
inline bool computeQmd(const NvkStage &stage, uint32_t setBytes, uint32_t heapBytes, uint32_t qmd[64])
{
    switch_gm20b_qmd_info q = qmdInfo(stage);
    q.smem_size = stage.smemSize;
    const auto aligned = [](uint32_t bytes) {
        const uint32_t a =
            (bytes + setlayout::kCbufAlignment - 1) / setlayout::kCbufAlignment * setlayout::kCbufAlignment;
        return a < kMaxCbufBytes ? a : kMaxCbufBytes;
    };
    q.cbuf_count = stage.cbufCount;
    if (q.cbuf_count > SWITCH_GM20B_QMD_CBUFS)
        return false;
    for (uint32_t c = 0; c < stage.cbufCount; c++) {
        switch (stage.cbufTypes[c]) {
        case 0:
            break;
        case CbufRoot:
            q.cbuf_size[c] = aligned(kRootTableBytes);
            break;
        case CbufShaderData:
            q.cbuf_size[c] = stage.dataBytes ? aligned(stage.dataBytes) : 0;
            break;
        case CbufSet:
            if (stage.cbufSets[c] != 0)
                return false;
            q.cbuf_size[c] = setBytes ? aligned(setBytes) : 0;
            break;
        case kCbufHeapSet:
            if (stage.cbufSets[c] != 1 || !heapBytes)
                return false;
            q.cbuf_size[c] = aligned(heapBytes);
            break;
        default:
            return false;
        }
    }
    return switch_gm20b_qmd_fill(&q, qmd);
}

// The QMD of a fragment split's prologue (nvk_switch_split.c fill_qmd): one workgroup, 256 bytes of each constant buffer
inline bool splitQmd(const NvkStage &prologue, uint32_t qmd[64])
{
    switch_gm20b_qmd_info q = qmdInfo(prologue);
    q.grid[0] = q.grid[1] = q.grid[2] = 1;
    q.cbuf_count = prologue.cbufCount;
    if (q.cbuf_count > SWITCH_GM20B_QMD_CBUFS)
        return false;
    for (uint32_t c = 0; c < q.cbuf_count; c++)
        q.cbuf_size[c] = 256;
    return switch_gm20b_qmd_fill(&q, qmd);
}

}  // namespace raw
