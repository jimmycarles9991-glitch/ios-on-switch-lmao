// SPDX-License-Identifier: MIT
// gpuResourceID of the raw path's textures and samplers. The low 32 bits of a texture's ID ORed with a sampler's are the
// bindless handle NVK's programs sample through ({texture header index:20, sampler header index:12}, nvk_descriptor_types.h
// nvk_sampled_image_descriptor). Bit 32 keeps every raw ID out of m2v's handle tables, whose IDs are Metal's small pool
// numbers (metal2vulkan-runtime resource_ids.rs): an argument buffer an m2v draw reads finds no other texture under a raw ID.
#pragma once
#include <cstdint>

namespace raw {

constexpr uint64_t kResourceIdTag = uint64_t(1) << 32;
constexpr uint32_t kTextureHeaderBits = 20, kSamplerHeaderBits = 12;

// 0 for a texture with no header (not sampleable) and a sampler with none
constexpr uint64_t textureResourceId(uint32_t tic)
{
    return tic && tic < (1u << kTextureHeaderBits) ? kResourceIdTag | tic : 0;
}

constexpr uint64_t samplerResourceId(uint32_t tsc)
{
    return tsc && tsc < (1u << kSamplerHeaderBits) ? kResourceIdTag | uint64_t(tsc) << kTextureHeaderBits : 0;
}

constexpr uint32_t bindlessHandle(uint64_t texture, uint64_t sampler)
{
    return uint32_t(texture | sampler);
}

}  // namespace raw
