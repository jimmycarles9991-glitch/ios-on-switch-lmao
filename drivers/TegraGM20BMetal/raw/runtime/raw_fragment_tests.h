// SPDX-License-Identifier: MIT
#pragma once
#include "raw_zeta.h"

namespace raw {

enum class FragmentTests : uint8_t {
    Any,
    Pass,
    ReadOnly,
    DepthWrite,
    StencilWrite
};

inline FragmentTests fragmentTests(const DepthStencilDesc &state, uint32_t format)
{
    const bool depthDrops = zetaHasDepth(format) && state.depthCompare != 7;
    const bool stencilDrops = zetaHasStencil(format) && (state.front.compare != 7 || state.back.compare != 7);
    if (!depthDrops && !stencilDrops)
        return FragmentTests::Pass;
    const bool depthWrites = zetaHasDepth(format) && state.depthWrite;
    const auto writes = [&](const StencilFace &face) {
        return face.writeMask && ((face.compare != 7 && face.fail) || (depthDrops && face.depthFail) || face.pass);
    };
    const bool stencilWrites = zetaHasStencil(format) && (writes(state.front) || writes(state.back));
    if (!depthWrites && !stencilWrites)
        return FragmentTests::ReadOnly;
    if (!stencilDrops && !stencilWrites)
        return FragmentTests::DepthWrite;
    return FragmentTests::StencilWrite;
}

inline bool fragmentTestsMatch(uint8_t required, const DepthStencilDesc &state, uint32_t format)
{
    return !required || required == uint8_t(fragmentTests(state, format));
}

}  // namespace raw
