// SPDX-License-Identifier: MIT
#pragma once
#include "bridge/NativeMetalDirect.h"
#include "raw_dynamic_push.h"

namespace raw {

inline bool validDynamicPush(const NmbDirectState &state)
{
    for (const NmbDirectDraw &d : state.draws) {
        const DynamicField fields[] = {
            {d.blendColorWord, 8}, {d.halfBlendColorWord, 4}, {d.depthTestWord, 4}, {d.stencilTestWord, 4}};
        if (!dynamicFieldsPlaced(fields, d.pushMask, d.spillWords[1]) ||
            d.blendColorWord != state.draws[0].blendColorWord ||
            d.halfBlendColorWord != state.draws[0].halfBlendColorWord ||
            d.depthTestWord != state.draws[0].depthTestWord || d.stencilTestWord != state.draws[0].stencilTestWord ||
            bool(d.depthTestWord) != bool(state.storage.depthFormat) ||
            bool(d.stencilTestWord) != bool(state.storage.stencilFormat))
            return false;
    }
    return true;
}

// The push words a draw's addresses fill (raw_heap_tables.h): each stage's root-table and ExecutionStatus address, two words
// each
inline uint32_t addressPushWords(const NmbDirectDraw &d)
{
    uint32_t mask = 0;
    for (uint32_t k = 0; k < 2; k++)
        mask |= pushFieldMask(d.rootTableWord[k], 2) | pushFieldMask(d.statusWord[k], 2);
    return mask;
}

// The words of a tessellation state's control stage (m2v control::PUSH_FIELDS), which every patch draw writes
constexpr uint32_t kControlWords = 6;

inline uint32_t controlPushMask(const NmbDirectState &state)
{
    return pushFieldMask(state.tess.controlWord, kControlWords);
}

// The words a raw draw writes itself: each amplification's id and offsets, the draw's amplification count, the pass's
// layer count, the encoder's viewport count and each dynamic pulled layout's stride
inline uint32_t drawnPushMask(const NmbDirectDraw &d)
{
    uint32_t mask = pushFieldMask(d.amplificationIdWord, 1) | pushFieldMask(d.amplificationCountWord, 1) |
                    pushFieldMask(d.amplificationLayerWord, 1) | pushFieldMask(d.amplificationViewportWord, 1) |
                    pushFieldMask(d.arrayLengthWord, 1) | pushFieldMask(d.viewportCountWord, 1);
    for (const auto &pulled : d.pulledStrides)
        mask |= pushFieldMask(pulled[1], 1);
    return mask;
}

// The bind-offset draw changes every buffer residual; only words shared by all probe draws can be folded. An address hole is
// 0 in every probe draw and the draw's address in a real one.
inline uint32_t constantPushWords(const NmbDirectState &state)
{
    uint32_t mask = state.draws[0].pushMask & ~controlPushMask(state) & ~drawnPushMask(state.draws[0]);
    for (const NmbDirectDraw &d : state.draws) {
        mask &= ~(pushFieldMask(d.blendColorWord, 8) | pushFieldMask(d.halfBlendColorWord, 4) |
                  pushFieldMask(d.depthTestWord, 4) | pushFieldMask(d.stencilTestWord, 4) | addressPushWords(d));
        for (uint32_t i = 0; i < 32; i++)
            if (!(d.pushMask >> i & 1) || d.push[i] != state.draws[0].push[i])
                mask &= ~(1u << i);
    }
    return mask;
}

}  // namespace raw
