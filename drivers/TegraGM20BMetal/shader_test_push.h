// SPDX-License-Identifier: MIT
#pragma once
#include "dynamic_push.h"
#include "fragment_tests.h"
#include "pack.h"
#include "shader_tests.h"
#include "zeta.h"
#include <cstring>

namespace gm20b {

inline uint32_t shaderStencilWord(const StencilFace &face)
{
    return (face.compare & 7u) | (face.fail & 7u) << 3 | (face.pass & 7u) << 6 | (face.readMask & 255u) << 9 |
           (face.writeMask & 255u) << 17 | (face.depthFail & 7u) << 25;
}

// A record of m2v's shader-side stencil (ShadowStencilUse): a fragment function's [[stencil]] output is both faces' reference
// of the test its program runs on the stencil stand-in (kShaderTestStencil, none in a pass without a stencil attachment) inside
// the interlock; the depth test stays fixed-function. PackShaderTests::reserved holds its ShadowStencilState push word + 1.
inline bool shadowStencil(const PackShaderTests &tests)
{
    return tests.reserved != 0;
}

// Where the stencil a shader-side stencil writes depends on the depth test, which the hardware runs after its program: such
// a draw takes its state's " | tests=4" variant, whose program runs both tests (drawnTests)
inline bool shadowDependsOnDepth(const DepthStencilDesc &d, bool depthAttachment)
{
    const auto depends = [](const StencilFace &f) {
        return (f.writeMask & 255u) && f.depthFail != f.pass;
    };
    return depthAttachment && d.depthCompare != 7 && (depends(d.front) || depends(d.back));
}

// The test class a draw under `d` in a pass of depth/stencil format `zeta` takes of a record of `tests`, its " | tests=<class>"
// variant (0: the record itself): a state that `fetches` with a depth attachment and no storage form, by its tests; a
// shader-side stencil, StencilWrite where its stencil write follows the depth test
inline uint8_t drawnTests(bool fetches, const PackShaderTests &tests, const DepthStencilDesc &d, uint32_t zeta)
{
    if (fetches)
        return uint8_t(fragmentTests(d, zeta));
    return shadowStencil(tests) && shadowDependsOnDepth(d, zetaHasDepth(zeta)) ? uint8_t(FragmentTests::StencilWrite)
                                                                               : 0;
}

inline uint32_t shadowStencilWord(const StencilFace &face, bool attached)
{
    return (face.compare & 7u) | (face.fail & 7u) << 3 | (face.pass & 7u) << 6 | (face.readMask & 255u) << 9 |
           (face.writeMask & 255u) << 17 | (attached ? 1u << 31 : 0u);
}

inline void patchShaderTestPush(const PackShaderTests &tests, const DepthStencilDesc &state,
                                const uint32_t references[2], const float depthRange[2], bool clamp, uint32_t *at,
                                uint32_t first = 0, uint32_t count = 32)
{
    if (uint32_t *words = fieldWords(at, first, count, tests.depthWord, 4)) {
        words[0] = (state.depthCompare & 7u) | uint32_t(state.depthWrite) << 3 | uint32_t(clamp) << 4;
        memcpy(words + 1, depthRange, 2 * sizeof(float));
        words[3] = 0;
    }
    if (uint32_t *words = fieldWords(at, first, count, tests.stencilWord, 4)) {
        words[0] = shaderStencilWord(state.front);
        words[1] = shaderStencilWord(state.back);
        words[2] = (references[0] & 255u) | (references[1] & 255u) << 8;
        words[3] = 0;
    }
    if (uint32_t *words = fieldWords(at, first, count, tests.reserved, 2)) {
        const bool attached = tests.flags & kShaderTestStencil;
        words[0] = shadowStencilWord(state.front, attached);
        words[1] = shadowStencilWord(state.back, attached);
    }
}

}  // namespace gm20b
