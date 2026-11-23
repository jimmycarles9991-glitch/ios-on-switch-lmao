// SPDX-License-Identifier: MIT
// The draw calls in a word stream the driver pushed (push.h's encodings), with the 3D state in force at each: the pixel
// program, the pixel stage's and the rasteriser's switches (stage_switches.h) and scissor 0. A draw call is a call of
// a macro in the draw range or of an indirect command buffer's command macros (mme.h).
#pragma once
#include <cstdint>
#include <vector>
#include "../mme.h"
#include "../push.h"
#include "../stage_switches.h"

namespace harness {

constexpr uint32_t kPixelProgramMethod = 0x2144, kScissorXMethod = 0x0e04, kScissorYMethod = 0x0e08;

struct DrawCall {
    uint32_t pixelProgram = 0, pixelStage = gm20b::kPixelStageOn, raster = 1, scissorX = 0, scissorY = 0;
};

// `end`: the state after the last word
inline std::vector<DrawCall> drawCalls(const std::vector<uint32_t> &words, DrawCall *end = nullptr)
{
    namespace mme = gm20b::mme;
    DrawCall at;
    std::vector<DrawCall> out;
    const auto method = [&](uint32_t m, uint32_t v) {
        if (m == kPixelProgramMethod)
            at.pixelProgram = v;
        else if (m == gm20b::kPixelStageSwitch)
            at.pixelStage = v;
        else if (m == gm20b::kRasterSwitch)
            at.raster = v;
        else if (m == kScissorXMethod)
            at.scissorX = v;
        else if (m == kScissorYMethod)
            at.scissorY = v;
    };
    for (size_t i = 0; i < words.size();) {
        const uint32_t h = words[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                       m = (h & 0xfff) << 2;
        if (op == 4) {
            if (subc == gm20b::SUBC_3D)
                method(m, count);
            continue;
        }
        if (subc == gm20b::SUBC_3D && m >= mme::kMacroCall) {
            const uint32_t slot = (m - mme::kMacroCall) / 8;
            if (mme::countedAsDraw(slot) || slot == mme::kIcbDraw || slot == mme::kIcbDrawIndexed ||
                slot == mme::kIcbPatches)
                out.push_back(at);
        } else if (subc == gm20b::SUBC_3D) {
            for (uint32_t k = 0; k < count && i + k < words.size(); k++)
                method(op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m, words[i + k]);
        }
        i += count;
    }
    if (end)
        *end = at;
    return out;
}

}  // namespace harness
