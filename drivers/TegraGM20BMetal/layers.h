// SPDX-License-Identifier: MIT
// A layered pass (renderTargetArrayLength 2 or more): every color and zeta target bound with that many layers from its
// attachment's slice on (attachment_target.h), as nvk_CmdBeginRendering binds a view of layer_count layers, its clears
// in every layer (CLEAR_SURFACE's RT_ARRAY_INDEX, NVK's layer loop macro). What it stores needs no work of its own: the
// targets are the textures' own memory, or for an attachment of fewer layers than the pass (a linear one has one) a
// backing of the pass's layers whose own layers the copy engine fills and drains (CommandBuffer::render): Apple's GPU
// draws every layer an attachment has and drops the rest. Over gm20b::PassDesc (pass_desc.h) and gm20b::Texture
// (resources.h), or any types shaped alike.
#pragma once
#include <algorithm>
#include <cstdint>

namespace gm20b {

// Why the driver does not render `pass` with its layers (null: it does, or the pass has one layer). A memoryless
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
        return "a layered multisampled pass, which has no attachment: the device makes no "
               "MTLTextureType2DMultisampleArray (MTLTexture.h)";
    return nullptr;
}

// What a draw of a state whose record is `arrayLengthWord`'s in a pass of `layers` layers leaves no renderer for (null:
// none): a state whose vertex function chooses its slice, made for one layer (kLayerDropped). The translation has a
// layered form of every fragment function the driver records but a multisampled storage form's, and no layered pass
// is multisampled (layeredRefusal), so a record that meets one is the driver's error
inline const char *layeredDrawMissing(uint8_t arrayLengthWord, uint32_t layers, uint8_t layerDropped)
{
    if (layers <= 1 || arrayLengthWord != layerDropped)
        return nullptr;
    return "a one-layer record of a state whose vertex function chooses its layer, in a layered pass";
}

}  // namespace gm20b
