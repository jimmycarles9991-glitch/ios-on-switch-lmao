// SPDX-License-Identifier: MIT
// RGB8_444_2P (542) textures as render targets (Texture::planar): a pass renders into an RGBA16Float stand-in, which the pack's
// two-plane load state fills from the codes image and its store state draws into the planes, luma (R8Unorm over plane 0) and
// chroma (RG8Unorm over plane 1). One made with render-target usage holds its own (Device::createPlanarTarget); any other
// usage's pass takes the stand-in from the command buffer's scratch and renders the planes as targets of no header, so a
// texture only sampled (the widget snapshots Core Animation samples) holds no pool memory or header for passes it may never get.
#pragma once
#include "gpu.h"
#include "ycbcr_load.h"

namespace gm20b {

constexpr uint32_t kPlanarStandInFormat = 115;  // RGBA16Float
// A slot's target is a loaded YCbCr texture (kYcbcrLoadTag + slot) or an RGB8_444_2P one, never both in one pass
constexpr uint32_t kPlanarScratchTag = kYcbcrLoadTag;

// Plane 1 (`chroma`) or plane 0 of planar texture `t` as a render target: rendered as it lies where NVK's linear-render rule
// takes its rows, else through a backing the pass fills and drains (Texture::shadowTarget), as the texture's own plane
// targets are (Device::createPlanarTarget). Nothing to free: it holds the texture's import and no header.
inline void planarPlaneTarget(const Texture &t, bool chroma, Texture &out)
{
    constexpr uint32_t kR8Unorm = 10, kRG8Unorm = 30;
    out = {};
    shape(t.width, t.height, chroma ? kRG8Unorm : kR8Unorm, out);
    out.va = chroma ? t.planes.chroma : t.planes.luma;
    out.pitch = chroma ? t.planes.chromaPitch : t.planes.lumaPitch;
    out.import = t.import;
    out.storage = {out.va, uint64_t(out.pitch) * out.height, !t.import->id};
    const bool linear = !((out.va | out.pitch) & 127);
    out.shadowTarget = !linear;
    out.renderable = linear;
}

}  // namespace gm20b
