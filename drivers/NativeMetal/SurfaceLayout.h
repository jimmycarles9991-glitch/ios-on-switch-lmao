// The layout of an IOSurface the GPU uses, which the GM20B's kernel decides once for every process at the surface's first
// GPU import that asks (drivers/TegraGM20B/TegraGM20BABI.h method 16, TegraGM20BLayout.h): block-linear when that importer
// writes it and is its only import, rows otherwise. A block-linear surface's texture is the device's optimal image over its
// memory (texture_tiled_in_place), at the import's own VA, which the import bound under the kind its texels are written in.
// A texture of another shape over a block-linear surface is refused, never drawn in the wrong layout.
// NativeBridgeMetalFrontend.mm includes this once.
#pragma once
#include <stdint.h>
#include "bridge/NativeMetalObjects.h"

namespace surface_layout {

// TegraGM20BABI.h's method 16 flags and replies and TegraGM20BLayout.h's TGM_LAYOUT_KIND (the frontend builds without the
// kernel's headers); kUnknown: not asked, or no answer
constexpr uint32_t kWrites = 1u << 0, kPitch = 0, kTiled = 1, kOtherShape = 2, kUnknown = UINT32_MAX, kKind = 0xfe;

struct Shape {
    uint32_t width = 0, height = 0, bpp = 0, rowBytes = 0, flags = 0;

    bool operator==(const Shape &o) const
    {
        return width == o.width && height == o.height && bpp == o.bpp && rowBytes == o.rowBytes;
    }
};

// What a surface import was answered: the import's handle on the process's GPU connection, its layout, and the shape the
// layout was asked for (width 0: none, as for the raw GM20B path's import, whose kind says block-linear or not)
struct Import {
    uint64_t handle = 0;
    uint32_t layout = kUnknown;
    Shape asked;
};

// NVK's calls (nvkmd_darwin.h), looked up through `symbol` once
struct Nvk {
    void (*importShape)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
    uint32_t (*importLayout)(uint64_t *) = nullptr;
    uint32_t (*layout)(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
};

inline const Nvk &nvk(void *(*symbol)(const char *))
{
    static const Nvk calls = [symbol] {
        Nvk n;
        n.importShape = reinterpret_cast<decltype(n.importShape)>(symbol("switch_nvk_import_surface_shape"));
        n.importLayout = reinterpret_cast<decltype(n.importLayout)>(symbol("switch_nvk_import_surface_layout"));
        n.layout = reinterpret_cast<decltype(n.layout)>(symbol("switch_nvk_surface_layout"));
        return n;
    }();
    return calls;
}

// The shape a w x h image of `format` asks, rows `rowBytes` apart; writes only for a single 2D image (the one shape the kernel
// lays out). False: a format of blocks, or of no texel size.
inline bool shapeOf(NmbDevice *device, uint64_t format, uint64_t width, uint64_t height, uint64_t rowBytes, bool writes,
                    Shape &out)
{
    uint64_t block[4] = {};
    if (!nmb_device_pixel_format_block(device, format, block) || block[1] != 1 || block[2] != 1 || block[3] != 1 ||
        width > UINT32_MAX || height > UINT32_MAX || rowBytes > UINT32_MAX)
        return false;
    out = {uint32_t(width), uint32_t(height), uint32_t(block[0]), uint32_t(rowBytes), writes ? kWrites : 0};
    return true;
}

// The layout an image of `shape` over the import takes: kPitch, kTiled (at that shape), or kOtherShape/kUnknown (refused)
inline uint32_t layoutFor(const Nvk &nvk, const Import &import, const Shape &shape)
{
    if (import.layout == kPitch || import.layout == kUnknown)
        return import.layout;
    if (import.layout == kTiled && import.asked == shape)
        return kTiled;
    return nvk.layout && import.handle
               ? nvk.layout(import.handle, shape.width, shape.height, shape.bpp, shape.rowBytes, shape.flags)
               : kUnknown;
}

}  // namespace surface_layout
