// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include "resources.h"
#include "sample_positions.h"

namespace gm20b {

class Device;

// A render pass's attachments (Core Animation: color 0 the target, color 1 a memoryless attachment, writeMask 0 or 15)
struct ColorAttachment {
    Texture *texture = nullptr;
    // A level or slice of `image` (attachment_target.h): what the pass's other views of the attachment are made of
    const Texture *image = nullptr;
    uint32_t level = 0, slice = 0;

    enum Load : uint8_t {
        DontCare,
        Load,
        Clear
    } load = DontCare;

    bool store = true;  // Store or StoreAndMultisampleResolve
    float clear[4] = {};
};

// The pass's depth/stencil attachment: one zeta texture as each aspect its format has (memoryless: neither loaded nor stored)
struct DepthStencilAttachment {
    Texture *texture = nullptr;
    bool clearDepth = false, clearStencil = false;  // the aspect's load is Clear (else DontCare)
    float depth = 1;
    uint32_t stencil = 0;
    bool loaded = true, stored = true;  // an aspect is loaded, or stored: its contents outside the pass matter
    bool loadedLater = false;           // a later pass of the command buffer loads this depth
};

// A pass whose depth and stencil attachments are two textures, a Depth32Float and a Stencil8 of one extent and sample count:
// it renders a Depth32Float_Stencil8 backing of theirs (depthStencil names the depth texture), each aspect copied in from
// its texture where loaded and back where stored; an aspect left DontCare keeps its texture's texels, as Metal leaves them
// (metal2vulkan-runtime depth_stencil_scratch.rs, measured on an M4)
struct SeparateZeta {
    const Texture *depth = nullptr, *stencil = nullptr;
    bool loadDepth = false, loadStencil = false, storeDepth = false, storeStencil = false;
};

struct PassDesc {
    ColorAttachment color[8];
    uint32_t count = 0;
    DepthStencilAttachment depthStencil;
    SeparateZeta separate;
    // the bits of the UNORM depth the zeta stands in for (16: a Depth16Unorm pair's Depth32Float_Stencil8 scratch), whose
    // units a depth bias counts; 0: the zeta's own format's
    uint8_t biasDepthBits = 0;
    // renderTargetWidth / renderTargetHeight: the render area's bounds where nonzero (m2v render_pass.rs `area`: the
    // attachments' smallest extent, each axis limited by its target size)
    uint32_t width = 0, height = 0;
    // colors a draw of the pass binds as storage images (a pipeline's storage form): their memoryless backings take no
    // compression, which the surface unit cannot read through
    uint8_t storageMask = 0;
    // of those, the colors a layered record's draws bind as 2D arrays (pack.h layeredRecord): a pitch one renders
    // through a block-linear backing, as a pitch header has no array form
    uint8_t arrayedStorageMask = 0;
    SamplePositions positions;
    // no depth/stencil attachment, and a draw's state tests depth or stencil (testsHiddenZeta): the pass renders over a hidden
    // Depth32Float_Stencil8 backing of its own, cleared to 1.0 and 0, as plain Metal tests such a draw
    bool hiddenZeta = false;
    // renderTargetArrayLength: every target binds this many layers from its attachment's slice (a 3D level's planes from its
    // depth plane), each cleared; 0 and 1 render one
    uint32_t layers = 1;
    // a pass of another one's clears alone: its Clear loads reach past the render area as that pass's do, which has a draw
    // call (turned_pass.cpp)
    bool drawn = false;
    // a pass of Metal's draws: each depth they store is bounded as Metal bounds it (depth_bounds.h); the device's own
    // passes store depth unbounded, as they copy it
    bool metalDepth = false;

    // RGB10A8_2P_XR10 textures held in their two planes that the pass's draws store into, each through an RG32Uint image of
    // its texels (pack/xr.metal xr_planes_merge_frag before the pass, xr_planes_split_frag after it)
    struct WrittenPlanes {
        const Texture *texture = nullptr, *image = nullptr;
    };

    // the 8 colours' and one draw's 16 written slots' (pack.h kWrittenSlots) inline, any more a pass's draws write in
    // `morePlanes` (the range encode's, read through the pass); the images' scratch tags TegraGM20BMetalXrPlanes.h xrPlanesImageTag
    static constexpr uint32_t kWrittenPlanes = 24;
    WrittenPlanes writtenPlanes[kWrittenPlanes];
    const WrittenPlanes *morePlanes = nullptr;
    uint32_t morePlaneCount = 0;

    const Texture *writtenImage(const Texture &t) const
    {
        for (const WrittenPlanes &w : writtenPlanes)
            if (w.texture == &t)
                return w.image;
        for (uint32_t k = 0; k < morePlaneCount; k++)
            if (morePlanes[k].texture == &t)
                return morePlanes[k].image;
        return nullptr;
    }
};

// `t`'s layers from `slice` on as a texture of its own (an attachment's slice of a 2D array or cube)
inline Texture layersFrom(const Texture &t, uint32_t slice)
{
    Texture part = t;
    part.va += slice * t.layerBytes;
    part.layers -= slice;
    part.tic = 0;
    return part;
}

}  // namespace gm20b
