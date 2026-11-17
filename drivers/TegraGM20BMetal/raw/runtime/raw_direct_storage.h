// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include <cstring>
#include "raw_direct_abi.h"
#include "raw_storage.h"

namespace raw {

inline bool storageDepthQualified(const char *key, bool earlyTests = false, uint32_t tests = 0)
{
    const char *field = strstr(key, "ds=");
    unsigned depth = 0, stencil = 0;
    return field && sscanf(field, "ds=%u/%u", &depth, &stencil) == 2 &&
           ((!depth && !stencil) || earlyTests || tests == 1);
}

// The stand-ins of a state with no storage attachment whose program tests the stencil (m2v ShadowStencilUse, `tests` 2) or
// the depth and stencil (a [[stencil]] output's in-shader tests, 3) itself: null when each draw binds each once, as a 2D
// storage image of the fragment stage (a 2D array of the pass's layers in a slice-choosing state's layered form)
inline const char *standInWrites(const NmbDirectState &state, uint32_t tests)
{
    constexpr int32_t kViewType2D = 1, kViewType2DArray = 5;
    for (const NmbDirectDraw &draw : state.draws) {
        uint32_t bound = 0;
        const int32_t view = layeredRecord(uint8_t(draw.arrayLengthWord)) ? kViewType2DArray : kViewType2D;
        for (uint32_t i = 0; i < draw.writeCount; i++) {
            const NmbDirectWrite &w = draw.writes[i];
            if (w.source != NmbDirectDepthStandIn && w.source != NmbDirectStencilStandIn)
                continue;
            const uint32_t bit = w.source == NmbDirectDepthStandIn ? 1u : 2u;
            if (w.stage != 2 || w.set || w.type != 3 || w.viewType != view || (bound & bit))
                return "in-shader tests: invalid stand-in descriptor";
            bound |= bit;
        }
        if (bound != tests)
            return "in-shader tests: a draw's stand-ins are not its program's";
    }
    return nullptr;
}

// A state with no storage attachment whose program runs the depth and stencil tests (m2v lower/fragment/stencil.rs
// plan_tests: a [[stencil]] output, compiled for a draw whose stencil write follows its depth test, its fragmentTests
// StencilWrite)
template <class Header> const char *qualifyShaderTests(const NmbDirectState &state, Header header, PackPipeline &out)
{
    const NmbDirectStorage &s = state.storage;
    if (s.fragmentTests != 4 || s.testPlanes || s.depthFormat != 100 || s.stencilFormat != 13 ||
        !nativeSamples(s.testSamples))
        return "in-shader tests: a form other than a [[stencil]] output's depth and stencil tests";
    if (const char *refused = standInWrites(state, 3))
        return refused;
    PackShaderTests tests = {};
    if (!header(s.depthFormat, tests.depthTic) || !tests.depthTic || !header(s.stencilFormat, tests.stencilTic) ||
        !tests.stencilTic)
        return "in-shader tests: native depth/stencil storage view unavailable";
    tests.flags = 3;
    out.shaderTests = tests;
    out.storageTests = uint8_t(s.fragmentTests);
    return nullptr;
}

template <class Header> const char *qualifyStorage(const NmbDirectState &state, Header header, PackPipeline &out)
{
    const NmbDirectStorage &s = state.storage;
    if (state.storageForm != bool(s.mask) || (s.mask & ~255u) || (s.writes & ~s.mask) || (s.blendMask & ~s.writes) ||
        (s.blendModel & ~(kRopBlendAssumptions >> kRopBlendShift)))
        return "storage form: inconsistent attachment masks";
    if (s.shadowFormat && (s.shadowFormat != 13 || s.depthFormat || s.stencilFormat || !nativeSamples(s.shadowSamples)))
        return "shader-side stencil: a stand-in of another format or samples, or beside the in-shader tests";
    if (!s.mask) {
        if (s.depthFormat)
            return qualifyShaderTests(state, header, out);
        if (!s.shadowFormat)
            return nullptr;
        if (const char *refused = standInWrites(state, 2))
            return refused;
        PackShaderTests tests = {};
        if (!header(s.shadowFormat, tests.stencilTic) || !tests.stencilTic)
            return "shader-side stencil: native stencil storage view unavailable";
        tests.flags = 2;
        out.shaderTests = tests;
        return nullptr;
    }
    if (!nativeSamples(s.samples) || s.samplePlanes)
        return "storage form: sample planes or multisampled storage require a qualified native view";
    if (s.fragmentTests > 4 || s.testPlanes ||
        (s.depthFormat && (s.depthFormat != 100 || s.testSamples != s.samples)) ||
        (s.stencilFormat && (s.stencilFormat != 13 || !s.depthFormat)))
        return "storage form: unsupported depth/stencil storage view";
    const uint32_t stencilFormat = s.stencilFormat ? s.stencilFormat : s.shadowFormat;
    constexpr int32_t kViewType2D = 1, kViewType2DArray = 5;
    for (const NmbDirectDraw &draw : state.draws) {
        uint32_t bound = 0;
        uint32_t tests = 0;
        // a slice-choosing state's layered form (raw_pack.h layeredRecord) binds its attachments and its depth and stencil
        // stand-ins as arrays of the pass's layers, every other record as one 2D layer
        const int32_t attachmentView = layeredRecord(uint8_t(draw.arrayLengthWord)) ? kViewType2DArray : kViewType2D;
        for (uint32_t i = 0; i < draw.writeCount; i++) {
            const NmbDirectWrite &w = draw.writes[i];
            if (w.source == NmbDirectDepthStandIn || w.source == NmbDirectStencilStandIn) {
                if (w.stage != 2 || w.set || w.type != 3 || w.viewType != attachmentView)
                    return "storage form: invalid depth/stencil descriptor";
                tests |= w.source == NmbDirectDepthStandIn ? 1u : 2u;
                continue;
            }
            if (w.source != NmbDirectStorageAttachment)
                continue;
            if (w.slot >= 8 || w.stage != 2 || w.set != 0 || w.type != 3 || !(s.mask & (1u << w.slot)))
                return "storage form: attachment descriptor does not match its storage view";
            if (w.viewType != attachmentView)
                return w.viewType == kViewType2DArray
                           ? "storage form: a layered storage view in a record not of the layered form"
                           : "storage form: a record of the layered form whose storage view is one 2D layer";
            bound |= 1u << w.slot;
        }
        if (bound != s.mask)
            return "storage form: probe variant does not bind every storage attachment";
        if (tests != (uint32_t(bool(s.depthFormat)) | uint32_t(bool(stencilFormat)) << 1))
            return "storage form: depth/stencil bindings do not match the shader contract";
    }
    uint32_t words[8] = {};
    PackShaderTests tests = {};
    if (s.depthFormat) {
        if (!header(s.depthFormat, tests.depthTic) || !tests.depthTic)
            return "storage form: native depth storage view unavailable";
        tests.flags = 1;
    }
    if (stencilFormat) {
        if (!header(stencilFormat, tests.stencilTic) || !tests.stencilTic)
            return "storage form: native stencil storage view unavailable";
        tests.flags |= 2;
    }
    for (uint32_t i = 0; i < 8; i++)
        if ((s.mask >> i & 1) && (!s.formats[i] || !header(s.formats[i], words[i]) || !words[i]))
            return "storage form: NVK cannot represent the attachment storage format";
    out.flags |= PipelineStorageMetadata;
    out.storageMask = uint8_t(s.mask);
    out.storageWriteMask = uint8_t(s.writes);
    out.storageBlendMask = uint8_t(s.blendMask);
    out.storageBlendModel = uint8_t(s.blendModel);
    out.storageSamples = uint8_t(s.samples);
    out.storageTests = uint8_t(s.fragmentTests);
    out.shaderTests = tests;
    for (uint32_t i = 0; i < 8; i++)
        out.storageTic[i] = words[i];
    return nullptr;
}

}  // namespace raw
