// SPDX-License-Identifier: MIT
// Copyright © 2022 Collabora Ltd. and Red Hat Inc.
// Descriptor set 0 as NVK lays it out on GM20B (nvk_descriptor_set_layout.c, nvk_descriptor_types.h): each binding's
// byte offset and stride in the set, the set's size as its pool places it, and the size word of a buffer descriptor.
// The records' layouts are made in these terms, which raw's descriptor writes follow.
#pragma once
#include <cstdint>
#include <vector>

namespace raw {

// A binding of a set layout: its VkDescriptorType, count and VkDescriptorBindingFlags
struct SetBinding {
    uint32_t binding;
    int32_t type;
    uint32_t count, flags;
};

struct SetSlot {
    uint32_t offset = 0, stride = 0;
};

namespace setlayout {

constexpr int32_t kInlineUniformBlock = 1000138000;
constexpr uint32_t kVariableCount = 0x8;  // VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
constexpr uint32_t kCbufAlignment = 256;  // nvk_min_cbuf_alignment below Turing
constexpr uint32_t kSsboSizeAlignment = 4;

// stride and alignment by VkDescriptorType (nvk_descriptor_stride_align_for_type): 4-byte image, sampler and texel
// buffer handles, 16-byte buffer addresses, nothing for dynamic buffers (the root table holds them)
inline bool strideAlign(int32_t type, uint32_t &stride, uint32_t &alignment)
{
    if (type >= 0 && type <= 5)
        stride = alignment = 4;
    else if (type == 6 || type == 7)
        stride = alignment = 16;
    else if (type == 8 || type == 9)
        stride = alignment = 0;
    else if (type == 10)
        stride = alignment = 4;
    else if (type == kInlineUniformBlock)
        stride = 1, alignment = kCbufAlignment;
    else
        return false;
    return true;
}

}  // namespace setlayout

// Each binding number's slot, up to the highest the set has; false for a type NVK lays out by another rule
inline bool setSlots(const SetBinding *bindings, uint32_t count, std::vector<SetSlot> &slots, uint32_t &bytes)
{
    uint32_t top = 0;
    for (uint32_t i = 0; i < count; i++)
        top = bindings[i].binding + 1 > top ? bindings[i].binding + 1 : top;
    slots.assign(top, SetSlot());
    std::vector<const SetBinding *> byNumber(top, nullptr);
    for (uint32_t i = 0; i < count; i++)
        byNumber[bindings[i].binding] = &bindings[i];
    bytes = 0;
    for (const SetBinding *b : byNumber) {
        if (!b || !b->count)
            continue;
        uint32_t stride, alignment;
        if (!setlayout::strideAlign(b->type, stride, alignment))
            return false;
        if (!stride)
            continue;
        bytes = (bytes + alignment - 1) / alignment * alignment;
        slots[b->binding] = {bytes, stride};
        if (!(b->flags & setlayout::kVariableCount))
            bytes += stride * b->count;
    }
    return true;
}

// The set's size as nvk_descriptor_set_addr hands it to the root table: its fixed part, aligned as a constant buffer
inline uint32_t setBytes(uint32_t fixedBytes)
{
    return (fixedBytes + setlayout::kCbufAlignment - 1) / setlayout::kCbufAlignment * setlayout::kCbufAlignment;
}

// The size word NVK writes in a buffer descriptor of `range` bytes: a uniform buffer's whole constant-buffer blocks, a
// storage buffer's whole words; 0 for another type
inline uint32_t bufferDescriptorBytes(int32_t type, uint64_t range)
{
    // Mesa's align() takes and gives 32 bits
    const auto align = [](uint32_t v, uint32_t to) {
        return (v + to - 1) & ~(to - 1);
    };
    if (type == 6 || type == 8)
        return align(uint32_t(range), setlayout::kCbufAlignment);
    if (type == 7 || type == 9)
        return align(uint32_t(range), setlayout::kSsboSizeAlignment);
    return 0;
}

}  // namespace raw
