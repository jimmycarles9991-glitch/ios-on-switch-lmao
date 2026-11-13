// SPDX-License-Identifier: MIT
// A shared texture's IOSurface (RawMetalSharedTextures.mm). Its creation properties hold the texture's descriptor under
// the keys _MTLDevice's newSharedTextureWithDescriptor: writes and its newSharedTextureWithHandle: needs every one of,
// per kind (the kinds' Metal.framework; each key's string is its name, as Metal's exported constants hold), and the
// device's kMetalRegistryID, by which MTLSharedTextureHandle finds the receiving process's device. Its rows are the raw
// path's for a pitch surface, and its bytes leave room for the kernel's block-linear layout of the image: the kernel
// lays a surface out once, at its first GPU import (TegraGM20BLayout.h), so every process opens it in the layout the
// creator's import decided.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>
#include "MetalABI.h"
#include "TegraGM20BLayout.h"

namespace raw::shared {

inline constexpr std::string_view kRegistryKey = "kMetalRegistryID";
// AGX's copyIOSurfaceSharedTextureProperties: writes it beside kIOSurfaceAllocSize, and its newTextureWithDescriptor:
// iosurface:plane: opens a surface that holds it as one in the device's layout of the descriptor (21H461 0x21c8d6530,
// 24B119 0x18c1486c0): here a surface of raw_surface_image.h's image, uncompressed
inline constexpr std::string_view kLayoutKey = "kAGXCompressionMode";

// A descriptor property under its key. Swizzle is the four channels packed red first, a byte each
// (MTLTextureSwizzleChannelsToKey); Private ones are SPI the descriptor may not answer (0 then). All go through KVC,
// which boxes each at its own width.
enum class Property : uint8_t {
    Public,
    Swizzle,
    Private
};

struct Key {
    std::string_view key, property;
    Property how;
};

inline constexpr Key kKeys[] = {
    {"kMetalTextureType", "textureType", Property::Public},
    {"kMetalTexturePixelFormat", "pixelFormat", Property::Public},
    {"kMetalTextureWidth", "width", Property::Public},
    {"kMetalTextureHeight", "height", Property::Public},
    {"kMetalTextureDepth", "depth", Property::Public},
    {"kMetalTextureMipmapLevelCount", "mipmapLevelCount", Property::Public},
    {"kMetalTextureSampleCount", "sampleCount", Property::Public},
    {"kMetalTextureArrayLength", "arrayLength", Property::Public},
    {"kMetalTextureResourceOptions", "resourceOptions", Property::Public},
    {"kMetalTextureUsage", "usage", Property::Public},
    {"kMetalTextureSparseValue", "sparseSurfaceDefaultValue", Property::Private},
    {"kMetalTextureGPUOptimization", "allowGPUOptimizedContents", Property::Public},
    {"kMetalTextureSwizzleKey", "swizzle", Property::Swizzle},
    {"kMetalTextureRotation", "rotation", Property::Private},
    {"kMetalTextureFootprint", "compressionFootprint", Property::Private},
    {"kMetalTextureResourceIndex", "resourceIndex", Property::Private},
    {"kMetalTextureCSCMatrix", "colorSpaceConversionMatrix", Property::Private},
    {"kMetalTextureWriteAccessPattern", "writeAccessPattern", Property::Private},
};

// IOSurface's kIOSurfaceProtectionOptions, written beside the keys and never needed by an opener
inline constexpr Key kProtection = {"IOSurfaceProtectionOptions", "protectionOptions", Property::Private};

struct Kind {
    std::string_view kind;
    bool writeAccessPattern;  // its Metal writes and needs kMetalTextureWriteAccessPattern
    bool resourceIndexOpen;   // its _MTLDevice has newSharedTextureWithHandle:withResourceIndex:
};

inline constexpr Kind kKinds[] = {
    {"21H461", false, false},
    {"24B119", true, true},
};

inline constexpr bool keyOf(const Kind &k, const Key &key)
{
    return k.writeAccessPattern || key.key != "kMetalTextureWriteAccessPattern";
}

inline constexpr size_t kindIndex()
{
    for (size_t i = 0; i < std::size(kKinds); i++)
        if (kKinds[i].kind == SWITCH_METAL_KIND)
            return i;
    return std::size(kKinds);
}

static_assert(kindIndex() < std::size(kKinds), "no shared texture keys for this kind (raw_shared_texture.h)");
inline constexpr const Kind &kThisKind = kKinds[kindIndex()];

// Rows a multiple of 128 bytes: the raw path samples them in place (32) and renders into them as a pitch target (128)
inline constexpr uint32_t kSurfaceRowAlignment = 128;

struct SurfaceShape {
    uint32_t bytesPerRow;
    uint64_t allocBytes;
};

// The surface of a one-level 2D image of `bpp`-byte texels: false for none (an extent of 0 or past the GPU's)
inline bool surfaceShape(uint32_t width, uint32_t height, uint32_t bpp, SurfaceShape &out)
{
    if (!width || !height || !bpp || width > TGM_LAYOUT_MAX_EXTENT || height > TGM_LAYOUT_MAX_EXTENT)
        return false;
    out.bytesPerRow = (width * bpp + kSurfaceRowAlignment - 1) & ~(kSurfaceRowAlignment - 1);
    out.allocBytes = uint64_t(out.bytesPerRow) * height;
    tgm_layout tiled;
    if (tgm_layout_tiled(width, height, bpp, &tiled))
        out.allocBytes = std::max(out.allocBytes, tiled.bytes);
    return true;
}

}  // namespace raw::shared
