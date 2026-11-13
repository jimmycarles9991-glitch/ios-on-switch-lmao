// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>

namespace raw {

constexpr uint8_t kShaderTestDepth = 1, kShaderTestStencil = 2;

struct ShaderTestShape {
    uint32_t width = 0, height = 0;
    bool packedStencil = false;
};

enum class ShaderTestImage : uint8_t {
    attachment,
    depth,
    stencil,
    temporary
};

struct ShaderTestTransfer {
    ShaderTestImage source{}, destination{};
    uint32_t remap = 0, bytes = 0;
    bool ordered = false;
};

struct ShaderTestTransfers {
    std::array<ShaderTestTransfer, 3> transfers{};
    uint8_t count = 0;
};

inline ShaderTestTransfers shaderTestTransfers(uint8_t flags, bool packed, bool toAttachment)
{
    using Image = ShaderTestImage;
    const auto remap = [](uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t size, uint32_t source,
                          uint32_t destination) {
        return x | y << 4 | z << 8 | w << 12 | (size - 1) << 16 | (source - 1) << 20 | (destination - 1) << 24;
    };
    ShaderTestTransfers plan;
    auto append = [&](Image from, Image to, uint32_t word, uint32_t bytes, bool ordered = false) {
        plan.transfers[plan.count++] = {from, to, word, bytes, ordered};
    };
    constexpr uint32_t preserve = 6;
    if (flags & kShaderTestDepth) {
        const uint32_t source = packed && !toAttachment ? 2 : 1;
        const uint32_t destination = packed && toAttachment ? 2 : 1;
        append(toAttachment ? Image::depth : Image::attachment, toAttachment ? Image::attachment : Image::depth,
               remap(0, preserve, preserve, preserve, 4, source, destination), 4);
    }
    if (flags & kShaderTestStencil) {
        if (!packed) {
            append(toAttachment ? Image::stencil : Image::attachment, toAttachment ? Image::attachment : Image::stencil,
                   remap(0, preserve, preserve, preserve, 1, 1, 1), 1);
        } else if (toAttachment) {
            append(Image::stencil, Image::temporary, remap(0, preserve, preserve, preserve, 1, 1, 2), 1);
            append(Image::temporary, Image::attachment, remap(preserve, preserve, 0, preserve, 2, 1, 4), 2, true);
        } else {
            append(Image::attachment, Image::temporary, remap(2, preserve, preserve, preserve, 2, 4, 1), 2);
            append(Image::temporary, Image::stencil, remap(0, preserve, preserve, preserve, 1, 2, 1), 1, true);
        }
    }
    return plan;
}

inline bool shaderTestShape(uint32_t format, uint32_t samples, uint32_t width, uint32_t height, uint8_t flags,
                            ShaderTestShape &out)
{
    if (!flags || (flags & ~(kShaderTestDepth | kShaderTestStencil)) || !width || !height || width > 16384 ||
        height > 16384)
        return false;
    if ((flags & kShaderTestDepth) && format != 252 && format != 260)
        return false;
    if ((flags & kShaderTestStencil) && format != 253 && format != 260)
        return false;
    uint32_t x = 1, y = 1;
    switch (samples) {
    case 1:
        break;
    case 2:
        x = 2;
        break;
    case 4:
        x = 2;
        y = 2;
        break;
    case 8:
        x = 4;
        y = 2;
        break;
    default:
        return false;
    }
    out = {width * x, height * y, format == 260};
    return true;
}

}  // namespace raw
