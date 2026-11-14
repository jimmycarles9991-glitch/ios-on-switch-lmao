// SPDX-License-Identifier: MIT
// A render state's variant as a draw asks for it (RawMetalCubic.mm renderVariant): its cubic binds (raw_cubic.h), test
// class, two-plane colours (raw_xr_pass.h " | xr2p"), two-plane textures sampled (" | xrs="), and textures written in
// other formats than its record's (raw_written_textures.h " | wtex="). The client names the variant's key with name(), a
// compile request carries the whole variant (raw_compile_request.h), and the shader compiler names the key it checks
// with the same name(), so a form a request leaves out cannot key the same.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "raw_cubic.h"
#include "raw_written_textures.h"
#include "raw_xr_pass.h"

namespace raw {

struct RenderVariant {
    CubicBinds cubic;
    uint8_t fragmentTests = 0;
    bool twoPlane = false;
    uint16_t xrSlots = 0;
    std::vector<WrittenTexture> written;

    // The name after the state's key: " | cubic ...", " | tests=<n>", " | xr2p", " | xrs=<slots>", " | wtex=...", in
    // raw_pack.h variantNameAt's order; "" for the state itself
    std::string name() const
    {
        return (cubic.empty() ? std::string() : cubic.key()) +
               (fragmentTests ? " | tests=" + std::to_string(fragmentTests) : std::string()) +
               (twoPlane ? std::string(kXrPlanesVariant) : std::string()) +
               (xrSlots ? xrSampledVariant(xrSlots) : std::string()) + writtenVariant(written);
    }

    // The same variant without its test class: what draws where no record holds the class
    RenderVariant untested() const
    {
        RenderVariant v = *this;
        v.fragmentTests = 0;
        return v;
    }
};

}  // namespace raw
