// SPDX-License-Identifier: MIT
// A layered pass (renderTargetArrayLength 2 or more): every color and zeta target bound with that many layers from its
// attachment's slice on (raw_attachment_target.h), as nvk_CmdBeginRendering binds a view of layer_count layers, its clears
// in every layer (CLEAR_SURFACE's RT_ARRAY_INDEX, NVK's layer loop macro). What it stores needs no work of its own: the
// targets are the textures' own memory. Over raw::PassDesc and raw::Texture (raw_gpu.h), or any types shaped alike.
#pragma once
#include <algorithm>
#include <cstdint>

namespace raw {

// Why the raw path does not render `pass` with its layers (null: it does, or the pass has one layer). A memoryless
// attachment gets a backing of the pass's layers (Device::backing). Neither identity has a 2D multisample array
// (supportsTexture2DMultisampleArray answers NO on 21H461 and 24B119), so no layered pass is multisampled.
template <class Pass> const char *layeredRefusal(const Pass &pass, uint8_t samples, bool turned)
{
    const uint32_t layers = std::max(pass.layers, 1u);
    if (layers == 1)
        return nullptr;
    if (turned)
        return "a layered pass of a turned surface plane";
    if (samples > 1)
        return "a layered multisampled pass";
    for (uint32_t j = 0; j < pass.count && j < 8; j++) {
        const auto *t = pass.color[j].texture;
        if (!t || t->memoryless)
            continue;
        if (!t->blockLinear || t->shadowTarget)
            return "a layered pass of a pitch color target";
        if (t->layers < layers)
            return "a layered pass past a color attachment's layers";
    }
    const auto *z = pass.depthStencil.texture;
    if (z && !z->memoryless && z->layers < layers)
        return "a layered pass past its depth/stencil attachment's layers";
    return nullptr;
}

// What a draw of a state whose record is `arrayLengthWord`'s in a pass of `layers` layers leaves no renderer for (null:
// none): a state whose vertex function chooses its slice, made for one layer (kLayerDropped), has no layered form in
// m2v either (its plan refuses the same fragment function in a layered pass)
inline const char *layeredDrawMissing(uint8_t arrayLengthWord, uint32_t layers, uint8_t layerDropped)
{
    if (layers <= 1 || arrayLengthWord != layerDropped)
        return nullptr;
    return "a state whose vertex function chooses its layer and which m2v has no layered form of, in a layered pass";
}

}  // namespace raw
