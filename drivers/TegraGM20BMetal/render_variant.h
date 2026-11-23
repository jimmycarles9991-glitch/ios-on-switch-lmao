// SPDX-License-Identifier: MIT
// A render state's variant as a draw asks for it (TegraGM20BMetalCubic.mm renderVariant): its cubic binds (cubic.h), test
// class, two-plane textures sampled (xr_pass.h " | xrs="), and textures written in other formats than its record's
// (written_textures.h " | wtex="). The client names the variant's key with name(), a compile request
// carries the whole variant (compile_request.h), and the shader compiler names the key it checks with the same
// name(), so a form a request leaves out cannot key the same.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "cubic.h"
#include "written_textures.h"
#include "xr_pass.h"

namespace gm20b {

struct RenderVariant {
    CubicBinds cubic;
    uint8_t fragmentTests = 0;
    uint16_t xrSlots = 0;
    std::vector<WrittenTexture> written;

    // The name after the state's key: " | cubic ...", " | tests=<n>", " | xrs=<slots>", " | wtex=...", in pack.h
    // variantNameAt's order; "" for the state itself
    std::string name() const
    {
        return (cubic.empty() ? std::string() : cubic.key()) +
               (fragmentTests ? " | tests=" + std::to_string(fragmentTests) : std::string()) +
               (xrSlots ? xrSampledVariant(xrSlots) : std::string()) + writtenVariant(written);
    }
};

}  // namespace gm20b
